#!/usr/bin/env python3
"""Exercise deployment in a disposable filesystem with fake privileged commands.

The copied script uses temporary installation paths and a simulated EUID. No
command in this suite installs a service or creates an account on the host.
"""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import unittest

REPOSITORY = Path(__file__).resolve().parents[1]
FAKE_COMMAND = r'''#!/usr/bin/python3
import json, os, pathlib, subprocess, sys, time
root = pathlib.Path(os.environ['DEPLOY_TEST_ROOT'])
name = pathlib.Path(sys.argv[0]).name
args = sys.argv[1:]
state_file = root / 'state.json'
state = json.loads(state_file.read_text()) if state_file.exists() else {}
with (root / 'commands.jsonl').open('a') as f:
    f.write(json.dumps([name, *args]) + '\n')
def save(): state_file.write_text(json.dumps(state))
def fail_once(key):
    if os.environ.get(key) and not state.get(key):
        state[key] = True; save(); sys.exit(1)
unit = root / 'system/etc/systemd/system/keyboardeyes.service'
if name == 'ps': print('systemd')
elif name == 'getent':
    category, entry = args
    if category == 'group' and entry == 'input':
        if os.environ.get('NO_INPUT_GROUP'): sys.exit(2)
        print('input:x:107:')
    elif state.get('account'):
        if category == 'group': print('keyboardeyes:x:990:')
        else:
            shell = '/bin/bash' if os.environ.get('ACCOUNT_CONFLICT') else '/usr/sbin/nologin'
            print('keyboardeyes:x:990:990::/nonexistent:' + shell)
    else: sys.exit(2)
elif name == 'id': print('keyboardeyes')
elif name == 'useradd': state['account'] = True; save()
elif name == 'stat': print('0')
elif name == 'sudo':
    env = dict(os.environ, TEST_IS_ROOT='1')
    os.execve(args[0], args, env)
elif name == 'install':
    if any('.tmp.' in x for x in args): fail_once('FAIL_INSTALL')
    if os.environ.get('FAIL_ROLLBACK') and pathlib.Path(args[-2]).name.isdigit(): sys.exit(1)
    filtered = []
    i = 0
    while i < len(args):
        if args[i] in ('-o', '-g'): i += 2
        else: filtered.append(args[i]); i += 1
    sys.exit(subprocess.call(['/usr/bin/install', *filtered]))
elif name == 'cmake':
    if os.environ.get('FAIL_BUILD'): sys.exit(1)
    if '-B' in args: pathlib.Path(args[args.index('-B') + 1]).mkdir(parents=True, exist_ok=True)
    if '--build' in args:
        binary = pathlib.Path(args[args.index('--build') + 1]) / 'KeyboardEyes'
        binary.write_text('#!/bin/sh\n# ' + os.environ.get('DEPLOY_VERSION', 'v1') + '\nexit 0\n')
        binary.chmod(0o755)
elif name == 'ctest':
    if os.environ.get('FAIL_TESTS'): sys.exit(1)
elif name == 'ldd':
    print('libsqlite3.so.0 => ' + ('not found' if os.environ.get('MISSING_LIBRARY') else '/lib/libsqlite3.so.0'))
elif name == 'runuser':
    if os.environ.get('DENY_DEVICE') and '-r' in args: sys.exit(1)
    command = args[args.index('--') + 1:]
    if '-r' in command and os.environ.get('TEST_PRESENT_DEVICE'):
        command[-1] = os.environ['TEST_PRESENT_DEVICE']
    sys.exit(subprocess.call(command))
elif name == 'systemd-analyze': fail_once('FAIL_VERIFY')
elif name == 'journalctl': print('simulated service journal')
elif name == 'sleep': pass  # Record observation steps without spending real seconds.
elif name == 'systemctl':
    op = args[0]
    if op == 'show':
        prop = next(a.split('=', 1)[1] for a in args if a.startswith('--property='))
        properties = {
            'FragmentPath': str(unit) if unit.exists() else '',
            'DropInPaths': 'custom.conf' if os.environ.get('HAS_DROPIN') else '',
            'ActiveState': state.get('active', 'inactive'),
            'MainPID': str(state.get('pid', 0)),
            'NRestarts': str(state.get('restarts', 0)),
        }
        print(properties[prop])
    elif op == 'is-enabled':
        enabled = state.get('enabled', 'disabled' if unit.exists() else 'not-found')
        if '--quiet' not in args: print(enabled)
        sys.exit(0 if enabled == 'enabled' else 1)
    elif op == 'is-active':
        if os.environ.get('FAIL_OBSERVATION') and not state.get('FAIL_OBSERVATION'):
            state['FAIL_OBSERVATION'] = True
            state['pid'] = state.get('pid', 100) + 1
            state['restarts'] = state.get('restarts', 0) + 1
            save()
        sys.exit(0 if state.get('active') == 'active' else 3)
    elif op == 'enable': state['enabled'] = 'enabled'; save()
    elif op == 'disable': state['enabled'] = 'disabled'; save()
    elif op == 'stop': state['active'] = 'inactive'; state['pid'] = 0; save()
    elif op == 'start':
        if os.environ.get('BLOCK_START'):
            (root / 'start-blocked').touch()
            while not (root / 'release-start').exists(): time.sleep(.02)
        fail_once('FAIL_START')
        if state.get('active') == 'failed': sys.exit(1)  # Simulate an exhausted start limit.
        state['active'] = 'active'; state['pid'] = state.get('generation', 100) + 1
        state['generation'] = state['pid']; save()
        directory = root / 'system/var/lib/keyboardeyes'; directory.mkdir(parents=True, exist_ok=True)
        (directory / 'stats.db').touch(exist_ok=True)
    elif op == 'reset-failed':
        # Inactive units can be unloaded even when their unit file exists.
        if state.get('active', 'inactive') == 'inactive':
            print('Failed to reset failed state of unit keyboardeyes.service: Unit keyboardeyes.service not loaded.', file=sys.stderr)
            sys.exit(1)
        fail_once('FAIL_RESET')
        if state.get('active') == 'failed': state['active'] = 'inactive'
        state['restarts'] = 0; save()
    elif op not in ('daemon-reload', 'list-units', 'status'):
        raise RuntimeError('Unsupported systemctl command: ' + repr(args))
'''


class DeploymentTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='keyboardeyes-deploy-test-')
        self.root = Path(self.temporary.name)
        self.checkout = self.root / 'checkout with spaces'
        (self.checkout / 'scripts').mkdir(parents=True)
        (self.checkout / 'deploy').mkdir()
        self.system = self.root / 'system'
        script = (REPOSITORY / 'scripts/deploy.sh').read_text()
        # Only isolation boundaries are changed; deployment control flow is original.
        for variable, path in {
            'BINARY': '/usr/local/bin/KeyboardEyes',
            'UNIT': '/etc/systemd/system/keyboardeyes.service',
            'CONFIG': '/etc/default/keyboardeyes',
            'LOCK_DIRECTORY': '/run/keyboardeyes-deploy',
        }.items():
            script = script.replace('readonly ' + variable + '=' + path,
                                    'readonly ' + variable + '=' + str(self.system) + path)
        script = script.replace('EUID == 0', 'TEST_IS_ROOT == 1')
        # Map a synthetic present device to /dev/null for read-permission preflight.
        script = script.replace('[[ -e $device ]]', '[[ -e ${TEST_PRESENT_DEVICE:-$device} ]]')
        script = script.replace('[[ -c $device ]]', '[[ -c ${TEST_PRESENT_DEVICE:-$device} ]]')
        self.script = self.checkout / 'scripts/deploy.sh'
        self.script.write_text(script)
        self.script.chmod(0o755)
        shutil.copy(REPOSITORY / 'deploy/keyboardeyes.service', self.checkout / 'deploy')
        commands = self.root / 'commands'
        commands.mkdir()
        dispatcher = commands / 'dispatcher'
        dispatcher.write_text(FAKE_COMMAND)
        dispatcher.chmod(0o755)
        for name in ('ps', 'getent', 'id', 'useradd', 'stat', 'sudo', 'install', 'cmake',
                     'ctest', 'ldd', 'runuser', 'systemd-analyze', 'journalctl', 'sleep', 'systemctl'):
            (commands / name).symlink_to(dispatcher)
        (self.root / 'tmp').mkdir()
        self.env = dict(os.environ, PATH=str(commands) + ':' + os.environ['PATH'],
                        DEPLOY_TEST_ROOT=str(self.root), TEST_IS_ROOT='0', TMPDIR=str(self.root / 'tmp'))
        self.device = '/dev/input/by-id/test keyboard-"quoted"-\\-$literal%-event-kbd'
        self.binary = self.system / 'usr/local/bin/KeyboardEyes'
        self.unit = self.system / 'etc/systemd/system/keyboardeyes.service'
        self.config = self.system / 'etc/default/keyboardeyes'
        self.database = self.system / 'var/lib/keyboardeyes/stats.db'

    def tearDown(self):
        self.temporary.cleanup()

    def run_deploy(self, *, expect=0, args=None, **environment):
        result = subprocess.run([str(self.script), *(args if args is not None else ['--device', self.device])],
                                cwd='/', env=dict(self.env, **environment), text=True,
                                capture_output=True, timeout=10)
        self.assertEqual(result.returncode, expect, result.stdout + result.stderr)
        self.assertNotIn('unbound variable', result.stderr)
        return result

    def state(self):
        path = self.root / 'state.json'
        return json.loads(path.read_text()) if path.exists() else {}

    def commands(self):
        path = self.root / 'commands.jsonl'
        return [json.loads(s) for s in path.read_text().splitlines()] if path.exists() else []

    def test_install_update_escaping_and_data_preservation(self):
        self.run_deploy()
        escaped = self.device.replace('\\', '\\\\').replace('"', '\\"')
        self.assertIn('KEYBOARDEYES_DEVICE="' + escaped + '"\n', self.config.read_text())
        self.assertEqual(self.binary.stat().st_mode & 0o777, 0o755)
        self.assertEqual(self.unit.stat().st_mode & 0o777, 0o644)
        self.assertEqual(self.state()['active'], 'active')
        self.assertEqual(self.state()['enabled'], 'enabled')
        self.assertEqual(sum(c == ['sleep', '1'] for c in self.commands()), 3)
        self.database.write_bytes(b'preserved database')
        self.run_deploy(DEPLOY_VERSION='v2')
        self.assertIn('v2', self.binary.read_text())
        self.assertEqual(self.database.read_bytes(), b'preserved database')
        self.assertEqual(sum(c[0] == 'useradd' for c in self.commands()), 1)
        self.assertFalse(list((self.root / 'tmp').iterdir()))
        self.assertFalse(list((self.system / 'run/keyboardeyes-deploy').glob('backup.*')))

    def test_preflight_failure_does_not_stop_existing_service(self):
        self.run_deploy()
        previous = self.binary.read_bytes()
        for failure in ('FAIL_BUILD', 'FAIL_TESTS', 'MISSING_LIBRARY', 'NO_INPUT_GROUP'):
            with self.subTest(failure=failure):
                before = len(self.commands())
                self.run_deploy(expect=1, **{failure: '1'})
                self.assertEqual(self.binary.read_bytes(), previous)
                self.assertEqual(self.state()['active'], 'active')
                self.assertNotIn(['systemctl', 'stop', 'keyboardeyes.service'], self.commands()[before:])

    def test_redeploy_recovers_failed_service(self):
        self.run_deploy()
        state = self.state()
        state.update(active='failed', pid=0, restarts=5)
        (self.root / 'state.json').write_text(json.dumps(state))
        self.run_deploy(DEPLOY_VERSION='v2')
        self.assertEqual(self.state()['active'], 'active')
        self.assertEqual(self.state()['restarts'], 0)
        self.assertIn('v2', self.binary.read_text())

    def test_reset_failure_rolls_back_deployment(self):
        self.run_deploy()
        previous = [p.read_bytes() for p in (self.binary, self.unit, self.config)]
        state = self.state()
        state.update(active='failed', pid=0, restarts=5)
        (self.root / 'state.json').write_text(json.dumps(state))
        result = self.run_deploy(expect=1, DEPLOY_VERSION='v2', FAIL_RESET='1')
        self.assertIn('Restoring previous installation', result.stderr)
        self.assertEqual([p.read_bytes() for p in (self.binary, self.unit, self.config)], previous)
        self.assertEqual(self.state()['enabled'], 'enabled')

    def test_rollback_restores_files_and_service_state(self):
        self.run_deploy()
        old = [p.read_bytes() for p in (self.binary, self.unit, self.config)]
        self.database.write_bytes(b'original counts')
        for failure in ('FAIL_INSTALL', 'FAIL_VERIFY', 'FAIL_START', 'FAIL_OBSERVATION'):
            with self.subTest(failure=failure):
                result = self.run_deploy(expect=1, DEPLOY_VERSION='v2', **{failure: '1'})
                self.assertIn('Restoring previous installation', result.stderr)
                self.assertNotIn('Rollback incomplete', result.stderr)
                self.assertEqual([p.read_bytes() for p in (self.binary, self.unit, self.config)], old)
                self.assertEqual(self.database.read_bytes(), b'original counts')
                self.assertEqual(self.state()['active'], 'active')
                self.assertEqual(self.state()['enabled'], 'enabled')

    def test_failed_first_install_removes_files_but_retains_account(self):
        self.run_deploy(expect=1, FAIL_START='1')
        self.assertFalse(self.binary.exists())
        self.assertFalse(self.unit.exists())
        self.assertFalse(self.config.exists())
        self.assertTrue(self.state()['account'])
        self.assertNotEqual(self.state().get('active'), 'active')
        self.assertNotEqual(self.state().get('enabled'), 'enabled')

    def test_root_install_and_present_device(self):
        self.run_deploy(TEST_IS_ROOT='1', TEST_PRESENT_DEVICE='/dev/null')
        self.assertFalse(any(c[0] == 'sudo' for c in self.commands()))
        self.assertTrue(any(c[0] == 'runuser' and '-r' in c for c in self.commands()))
        self.assertEqual(self.state()['active'], 'active')

    def test_failed_rollback_keeps_recovery_files(self):
        self.run_deploy()
        old_binary = self.binary.read_bytes()
        result = self.run_deploy(expect=1, DEPLOY_VERSION='v2', FAIL_START='1', FAIL_ROLLBACK='1')
        self.assertIn('Rollback incomplete', result.stderr)
        backups = list((self.system / 'run/keyboardeyes-deploy').glob('backup.*'))
        self.assertEqual(len(backups), 1)
        self.assertEqual((backups[0] / '0').read_bytes(), old_binary)
        self.assertEqual(self.state()['active'], 'inactive')
        self.assertEqual(self.state()['enabled'], 'disabled')

    def test_rollback_restores_disabled_stopped_state(self):
        self.run_deploy()
        state = self.state()
        state.update(active='inactive', enabled='disabled', pid=0)
        (self.root / 'state.json').write_text(json.dumps(state))
        self.run_deploy(expect=1, FAIL_START='1')
        self.assertEqual(self.state()['active'], 'inactive')
        self.assertEqual(self.state()['enabled'], 'disabled')

    def test_conflicts_and_permissions_are_rejected_before_stop(self):
        self.run_deploy()
        for options in ({'ACCOUNT_CONFLICT': '1'}, {'HAS_DROPIN': '1'},
                        {'DENY_DEVICE': '1', 'TEST_PRESENT_DEVICE': '/dev/null'}):
            with self.subTest(options=options):
                before = len(self.commands())
                self.run_deploy(expect=1, **options)
                self.assertNotIn(['systemctl', 'stop', 'keyboardeyes.service'], self.commands()[before:])
        self.config.write_text('unmanaged configuration\n')
        self.run_deploy(expect=1)
        self.assertEqual(self.config.read_text(), 'unmanaged configuration\n')
        self.assertEqual(self.state()['active'], 'active')

    def test_arguments_and_unmanaged_binary(self):
        for args in ([], ['--unknown'], ['--device'], ['--device', 'relative'],
                     ['--device', self.device, '--device', self.device], ['--device', '/dev/input/bad\npath']):
            with self.subTest(args=args):
                self.run_deploy(expect=1, args=args)
        self.run_deploy(args=['--help'])
        self.binary.parent.mkdir(parents=True)
        self.binary.write_bytes(b'unmanaged binary')
        self.run_deploy(expect=1)
        self.assertEqual(self.binary.read_bytes(), b'unmanaged binary')
        self.assertFalse(self.state().get('account'))

    def test_deployment_locks(self):
        first = subprocess.Popen([str(self.script), '--device', self.device], cwd='/',
                                 env=dict(self.env, BLOCK_START='1'), text=True,
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            deadline = time.monotonic() + 8
            while not (self.root / 'start-blocked').exists():
                if first.poll() is not None or time.monotonic() > deadline:
                    out, err = first.communicate(timeout=1)
                    self.fail(out + err)
                time.sleep(.02)
            result = self.run_deploy(expect=1)
            self.assertIn('Another deployment from this checkout', result.stderr)
            stage = self.root / 'second-stage'
            stage.mkdir()
            shutil.copy(self.binary, stage / 'KeyboardEyes')
            shutil.copy(self.unit, stage / 'keyboardeyes.service')
            shutil.copy(self.config, stage / 'keyboardeyes.env')
            result = self.run_deploy(expect=1, args=['--install-stage', str(stage), self.device], TEST_IS_ROOT='1')
            self.assertIn('Another system deployment', result.stderr)
        finally:
            (self.root / 'release-start').touch()
            out, err = first.communicate(timeout=5)
        self.assertEqual(first.returncode, 0, out + err)


if __name__ == '__main__':
    unittest.main(verbosity=2)
