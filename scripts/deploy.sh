#!/usr/bin/env bash
set -Eeuo pipefail

readonly SERVICE=keyboardeyes.service
readonly BINARY=/usr/local/bin/KeyboardEyes
readonly UNIT=/etc/systemd/system/keyboardeyes.service
readonly CONFIG=/etc/default/keyboardeyes
readonly LOCK_DIRECTORY=/run/keyboardeyes-deploy
readonly MARKER='# Managed by KeyboardEyes scripts/deploy.sh'
SCRIPT_DIRECTORY="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIRECTORY="$(cd -- "$SCRIPT_DIRECTORY/.." && pwd)"
readonly SCRIPT_DIRECTORY PROJECT_DIRECTORY

die() { printf 'Error: %s\n' "$*" >&2; exit 1; }
info() { printf '%s\n' "$*"; }

require_commands() {
    local command_name
    for command_name in "$@"; do
        command -v "$command_name" >/dev/null || die "Missing required command: $command_name"
    done
}

check_system() {
    [[ $(uname -s) == Linux ]] || die 'Deployment requires Linux.'
    [[ $(ps -p 1 -o comm=) == systemd ]] || die 'systemd must be the system service manager.'
    systemctl list-units --no-pager >/dev/null || die 'Cannot contact the system systemd manager.'
    getent group input >/dev/null || die 'The input group is required; configure input-device permissions first.'
}

validate_device() {
    [[ $1 == /dev/input/* && $1 != *$'\n'* && $1 != *$'\r'* ]] ||
        die '--device must be an absolute path under /dev/input/ without line breaks.'
}

check_account() {
    local account_record account_name password uid gid comment home login_shell
    if account_record=$(getent passwd keyboardeyes); then
        IFS=: read -r account_name password uid gid comment home login_shell <<< "$account_record"
        [[ $uid != 0 && $home == /nonexistent && $(id -gn keyboardeyes) == keyboardeyes ]] ||
            die 'Existing keyboardeyes account is incompatible; it will not be modified.'
        case "$login_shell" in
            */nologin|*/false) ;;
            *) die 'Existing keyboardeyes account permits login; it will not be modified.' ;;
        esac
    else
        if getent group keyboardeyes >/dev/null; then
            die 'A keyboardeyes group exists without the service account; resolve this conflict first.'
        fi
        useradd --system --user-group --no-create-home --home-dir /nonexistent \
            --shell "$(command -v nologin)" keyboardeyes
    fi
}

reset_failed_service() {
    local active_state
    active_state=$(systemctl show "$SERVICE" --property=ActiveState --value) || return
    # A new/inactive unit may be unloaded; reset-failed does not load it.
    # Failed units remain loaded and need their start-limit counters cleared.
    if [[ $active_state == failed ]]; then
        systemctl reset-failed "$SERVICE"
    fi
}

# All variables below are local to install_stage and remain in scope for its EXIT trap.
atomic_install() {
    local source=$1 destination=$2 mode=$3 temporary
    temporary=$(mktemp "${destination}.tmp.XXXXXX") || return
    temporary_files+=("$temporary")
    install -o root -g root -m "$mode" -- "$source" "$temporary" || return
    mv -fT -- "$temporary" "$destination" || return
}

rollback() {
    local failed=0 index destination
    info 'Restoring previous installation and service state...' >&2
    if [[ -f $UNIT ]]; then
        systemctl stop "$SERVICE" || failed=1
        systemctl disable "$SERVICE" || failed=1
    fi
    for index in "${!destinations[@]}"; do
        destination=${destinations[$index]}
        if [[ -f $backup_directory/$index ]]; then
            atomic_install "$backup_directory/$index" "$destination" "${modes[$index]}" || failed=1
        else
            rm -f -- "$destination" || failed=1
        fi
    done
    systemctl daemon-reload || failed=1
    # Do not start a mixture of old and new files if restoration was incomplete.
    (( failed == 0 )) || return "$failed"
    if [[ $old_enabled == enabled ]]; then
        systemctl enable "$SERVICE" || failed=1
    fi
    if [[ $old_active == active ]]; then
        reset_failed_service || failed=1
        systemctl start "$SERVICE" || failed=1
        systemctl is-active --quiet "$SERVICE" || failed=1
    fi
    return "$failed"
}

finish_install() {
    local result=$? rollback_failed=0
    trap - EXIT INT TERM
    set +e
    if (( result != 0 )) && [[ $changed == yes ]]; then
        systemctl status "$SERVICE" --no-pager >&2
        journalctl -u "$SERVICE" -n 30 --no-pager >&2
        rollback || rollback_failed=1
    fi
    if ((${#temporary_files[@]})); then
        rm -f -- "${temporary_files[@]}"
    fi
    if [[ -n $backup_directory ]]; then
        if (( rollback_failed )); then
            printf 'Rollback incomplete. Backups retained at %s; inspect systemctl status %s.\n' \
                "$backup_directory" "$SERVICE" >&2
        else
            rm -rf -- "$backup_directory"
        fi
    fi
    if (( result != 0 )); then
        printf 'Deployment failed (exit %s). Database and service account were retained.\n' "$result" >&2
    fi
    exit "$result"
}

install_stage() {
    (( EUID == 0 )) || die 'The internal installation phase requires root.'
    local stage=$1 device=$2 path first_line fragment drop_ins enable_result
    local old_enabled old_active changed=no backup_directory=''
    local -a temporary_files=() destinations=("$BINARY" "$CONFIG" "$UNIT") modes=(0755 0644 0644)
    require_commands install mktemp mv cp rm head stat useradd nologin runuser id \
        systemctl systemd-analyze journalctl getent flock uname ps sleep ldd mkdir
    validate_device "$device"
    check_system
    [[ -x $stage/KeyboardEyes && -f $stage/keyboardeyes.service && -f $stage/keyboardeyes.env ]] ||
        die 'Incomplete deployment staging directory.'

    # A root-owned directory prevents other users from replacing the lock file.
    [[ ! -L $LOCK_DIRECTORY ]] || die "Refusing symbolic link: $LOCK_DIRECTORY"
    install -d -o root -g root -m 0700 -- "$LOCK_DIRECTORY"
    local deployment_lock
    exec {deployment_lock}>"$LOCK_DIRECTORY/lock"
    flock -n "$deployment_lock" || die 'Another system deployment is in progress.'

    fragment=$(systemctl show "$SERVICE" --property=FragmentPath --value)
    [[ -z $fragment || $fragment == "$UNIT" ]] || die "Conflicting service exists at $fragment"
    drop_ins=$(systemctl show "$SERVICE" --property=DropInPaths --value)
    [[ -z $drop_ins ]] || die 'Service drop-ins exist; resolve them before using this deployment script.'
    for path in "$UNIT" "$CONFIG"; do
        if [[ -e $path || -L $path ]]; then
            [[ -f $path && ! -L $path && $(stat -c %u "$path") == 0 ]] ||
                die "Refusing unmanaged file: $path"
            first_line=$(head -n 1 -- "$path")
            [[ $first_line == "$MARKER" ]] || die "Refusing unmanaged configuration: $path"
        fi
    done
    if [[ -e $BINARY || -L $BINARY ]]; then
        [[ -f $UNIT && -f $BINARY && ! -L $BINARY && $(stat -c %u "$BINARY") == 0 ]] ||
            die "Refusing unmanaged executable: $BINARY"
    fi
    if old_enabled=$(systemctl is-enabled "$SERVICE" 2>/dev/null); then
        enable_result=0
    else
        enable_result=$?
    fi
    case "$old_enabled" in
        enabled|disabled|not-found) ;;
        '') [[ $enable_result != 0 && -z $fragment ]] || die 'Cannot determine service enable state.'
            old_enabled=not-found ;;
        *) die "Unsupported service enable state: $old_enabled" ;;
    esac
    old_active=$(systemctl show "$SERVICE" --property=ActiveState --value)
    case "$old_active" in
        active|inactive|failed) ;;
        *) die "Service is in transitional or unknown state: $old_active" ;;
    esac

    check_account
    if [[ -e $device ]]; then
        [[ -c $device ]] || die "Device is not a character device: $device"
        runuser -u keyboardeyes -g keyboardeyes -G input -- test -r "$device" ||
            die "Service account cannot read $device; configure persistent device permissions first."
    else
        info "Device is currently absent; the service will wait for it: $device"
    fi
    # Check dependencies before replacing any existing installation.
    local dependency_output
    dependency_output=$(ldd "$stage/KeyboardEyes") || die 'Cannot inspect runtime dependencies.'
    [[ $dependency_output != *'not found'* ]] || die "Missing runtime dependency: $dependency_output"

    backup_directory=$(mktemp -d "$LOCK_DIRECTORY/backup.XXXXXX")
    trap finish_install EXIT
    trap 'exit 130' INT
    trap 'exit 143' TERM
    local index
    for index in "${!destinations[@]}"; do
        if [[ -f ${destinations[$index]} ]]; then
            cp -p -- "${destinations[$index]}" "$backup_directory/$index"
        fi
    done
    changed=yes
    if [[ $old_active == active ]]; then
        systemctl stop "$SERVICE"
    fi
    mkdir -p -- "${BINARY%/*}" "${CONFIG%/*}" "${UNIT%/*}"
    atomic_install "$stage/KeyboardEyes" "$BINARY" 0755
    atomic_install "$stage/keyboardeyes.env" "$CONFIG" 0644
    atomic_install "$stage/keyboardeyes.service" "$UNIT" 0644
    systemd-analyze verify "$UNIT"
    runuser -u keyboardeyes -g keyboardeyes -G input -- test -x "$BINARY" ||
        die 'Installed executable is inaccessible to the service account.'
    systemctl daemon-reload
    reset_failed_service
    systemctl enable "$SERVICE"
    systemctl start "$SERVICE"

    local initial_pid initial_restarts current_pid current_restarts
    initial_pid=$(systemctl show "$SERVICE" --property=MainPID --value)
    initial_restarts=$(systemctl show "$SERVICE" --property=NRestarts --value)
    [[ $initial_pid =~ ^[1-9][0-9]*$ && $initial_restarts =~ ^[0-9]+$ ]] ||
        die 'Service did not start a main process.'
    for index in 1 2 3; do
        sleep 1
        systemctl is-active --quiet "$SERVICE" || die 'Service failed during startup observation.'
        current_pid=$(systemctl show "$SERVICE" --property=MainPID --value)
        current_restarts=$(systemctl show "$SERVICE" --property=NRestarts --value)
        [[ $current_pid == "$initial_pid" && $current_restarts == "$initial_restarts" ]] ||
            die 'Service restarted during startup observation.'
    done
    systemctl is-enabled --quiet "$SERVICE" || die 'Service is not enabled at boot.'
    changed=no
    info 'KeyboardEyes deployed, running, and enabled at boot.'
    info 'Database: /var/lib/keyboardeyes/stats.db'
    info 'Status: systemctl status keyboardeyes'
    info 'Logs: journalctl -u keyboardeyes -f'
    # Run the EXIT handler before leaving the scope of the backup variables.
    exit 0
}

main() {
    if [[ ${1:-} == --install-stage ]]; then
        [[ $# == 3 ]] || die 'Invalid internal installation arguments.'
        install_stage "$2" "$3"
        return
    fi
    local device='' device_set=no
    while (($#)); do
        case "$1" in
            --help|-h)
                printf 'Usage: %s --device /dev/input/by-id/KEYBOARD-event-kbd\n' "${0##*/}"
                printf 'Build Release, run tests, install and enable/start the system service.\n'
                return ;;
            --device)
                [[ $device_set == no && $# -ge 2 && -n $2 ]] || die '--device must be specified exactly once with a path.'
                device=$2; device_set=yes; shift 2 ;;
            *) die "Unknown argument: $1" ;;
        esac
    done
    [[ $device_set == yes ]] || die 'Missing --device; use --help for usage.'
    validate_device "$device"
    require_commands cmake ninja ctest pkg-config install mktemp cp rm flock ldd \
        systemctl getent uname ps mkdir
    check_system
    if ! (( EUID == 0 )); then
        require_commands sudo
    fi
    local build_directory="$PROJECT_DIRECTORY/build/deploy" build_lock
    mkdir -p -- "$PROJECT_DIRECTORY/build"
    exec {build_lock}>"$PROJECT_DIRECTORY/build/.deploy-build.lock"
    flock -n "$build_lock" || die 'Another deployment from this checkout is in progress.'
    info 'Building and testing Release before changing the system installation...'
    cmake -S "$PROJECT_DIRECTORY" -B "$build_directory" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
    cmake --build "$build_directory"
    (cd -- "$build_directory" && ctest --output-on-failure)
    local dependencies
    dependencies=$(ldd "$build_directory/KeyboardEyes") || die 'Cannot inspect runtime dependencies.'
    [[ $dependencies != *'not found'* ]] || die "Missing runtime dependency: $dependencies"

    local stage escaped_device
    stage=$(mktemp -d)
    # Errors/signals exit while main's local staging path is still in scope.
    trap 'rm -rf -- "$stage"' EXIT
    trap 'exit 130' INT
    trap 'exit 143' TERM
    install -m 0755 -- "$build_directory/KeyboardEyes" "$stage/KeyboardEyes"
    install -m 0644 -- "$PROJECT_DIRECTORY/deploy/keyboardeyes.service" "$stage/keyboardeyes.service"
    escaped_device=${device//\\/\\\\}
    escaped_device=${escaped_device//\"/\\\"}
    printf '%s\nKEYBOARDEYES_DEVICE="%s"\n' "$MARKER" "$escaped_device" > "$stage/keyboardeyes.env"
    if (( EUID == 0 )); then
        "$SCRIPT_DIRECTORY/deploy.sh" --install-stage "$stage" "$device"
    else
        sudo "$SCRIPT_DIRECTORY/deploy.sh" --install-stage "$stage" "$device"
    fi
    rm -rf -- "$stage"
    trap - EXIT INT TERM
}

main "$@"
