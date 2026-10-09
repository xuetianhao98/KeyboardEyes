# Repository Guidelines

## Project Structure & Module Organization

KeyboardEyes is a Linux keyboard counter with SQLite persistence and a separate terminal viewer.
- `src/main.cpp` handles device events, reconnection, signals, and collector locking; `src/database.*` handles persistence.
- `src/viewer/` contains the read-only database reader, table printer, keyboard layout, heatmap renderer, and terminal lifecycle code. Keep rendering independent of database access and terminal operations.
- `tests/` contains C++ unit tests and Python integration tests.
- `cmake/clang.cmake` selects the toolchain; `scripts/deploy.sh` and `deploy/keyboardeyes.service` manage deployment. `docs/images/` holds README screenshots.
- Build artifacts belong in `build/`. Register new sources in the appropriate `CMakeLists.txt` target.

## Build, Test, and Development Commands

Install Clang with matching libc++/libc++abi, CMake ≥ 3.16, Ninja, pkg-config, libevdev development files, SQLite ≥ 3.24 development files, and Python 3 with sqlite3.

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
(cd build && ctest --output-on-failure)
python3 tests/deploy_test.py
```

These configure, build, run registered tests, and separately test deployment with simulated system commands. Tests are enabled by default; use `-DBUILD_TESTING=OFF` for binaries only.

Run the collector with `./build/KeyboardEyes /dev/input/eventX --db ./stats.db`; device read permission is required. Run `./build/KeyboardEyesViewer --db ./stats.db --view table` against an existing database.

## Coding Style & Naming Conventions

Use C++20 and Google style from `.clang-format`: two-space indentation, `snake_case` functions and variables, `PascalCase` types, and trailing underscores for private members. Format changed C++ files with `clang-format-23 -i <file>`. Python uses four-space indentation and `snake_case`. Keep builds free of `-Wall -Wextra -Wpedantic` warnings. `.clangd` reads `build/compile_commands.json`.

## Testing Guidelines

CTest runs standalone C++ assertion tests and Python `unittest` integration suites. Name files `tests/<feature>_test.cpp` or `.py` and test functions `test_<behavior>`. Cover persistence, invalid data, locking, signals, and terminal restoration when relevant. Use temporary databases, pseudo-terminals, and mocked deployment commands. No numeric coverage threshold is configured.

## Commit & Pull Request Guidelines

History favors short imperative subjects such as “Add SQLite persistence…”; occasional `docs:` prefixes appear. Keep commits focused. PRs should explain behavior changes, report validation commands/results, link relevant issues, and include screenshots for terminal UI changes. Update README usage when commands change.

## Configuration & Data Safety

Default `stats.db` resolves from the working directory; service data lives at `/var/lib/keyboardeyes/stats.db`. Never use live service data for tests or replace an active database. Deployment installs and starts a system service; use its isolated test suite for development checks.
