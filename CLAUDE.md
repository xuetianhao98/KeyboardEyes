@AGENTS.md

## Claude-specific rules

- After changing code, verify with `cmake --build build && (cd build && ctest --output-on-failure)`; the build must stay free of warnings. Run `python3 tests/deploy_test.py` when touching `scripts/` or `deploy/`.
- Run `ctest` from inside `build/`, never from the repository root (it leaves a stray `Testing/` directory).
- A project hook runs `clang-format-23 -i` on every C++ file edited through Write/Edit; files changed any other way must be formatted manually.
- Never run `scripts/deploy.sh`, `sudo`, or `systemctl`, and never read or write `/var/lib/keyboardeyes/`. Deployment is done by the maintainer; use the mocked `tests/deploy_test.py` instead.
