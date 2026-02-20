# Repository Guidelines

## Project Structure & Module Organization
`src/` contains plugin code:
- `plugin-main.*` registers the OBS module and service.
- `vdoninja-output.*` handles publishing.
- `vdoninja-source.*` handles viewing.
- `vdoninja-signaling.*`, `vdoninja-peer-manager.*`, and `vdoninja-data-channel.*` implement signaling/WebRTC flow.
- `vdoninja-utils.*` holds shared helpers (hashing, JSON, formatting).

`tests/` contains GoogleTest suites (`test-*.cpp`) and `tests/stubs/` OBS API stubs for test-only builds.  
`data/locale/en-US.ini` stores localization strings.  
`.github/workflows/` defines CI/build/format pipelines.  
`scripts/` provides OS-specific install helpers.

## Build, Test, and Development Commands
```bash
# Tests only (no OBS SDK needed)
cmake -B build -DBUILD_TESTS=ON -DBUILD_PLUGIN=OFF -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target vdoninja-tests
ctest --test-dir build --output-on-failure
```

```bash
# Plugin build (requires OBS SDK, libdatachannel, OpenSSL)
cmake -B build -DCMAKE_BUILD_TYPE=Release -DOBS_SDK_PATH=/path/to/obs-sdk
cmake --build build
cmake --install build --prefix install
```

Use `scripts/install-linux.sh`, `scripts/install-macos.sh`, or `scripts/install-windows.ps1` for local installation.

## Coding Style & Naming Conventions
- Language level: C++17 (non-MSVC), C++20 (MSVC).
- Format with `.clang-format` (LLVM-based, tabs for indentation, 120-column limit, Linux brace style).
- CI style check command:
```bash
find src tests -name "*.cpp" -o -name "*.h" | xargs clang-format-14 --dry-run --Werror
```
- Conventions: `vdoninja-*.{h,cpp}` filenames, `PascalCase` types, `camelCase` functions, trailing `_` for private members.

## Testing Guidelines
- Framework: GoogleTest + GMock (via CMake `FetchContent`).
- Add tests under `tests/test-*.cpp`, grouped by module behavior.
- For OBS-dependent interactions, use `tests/stubs/` instead of linking full OBS in unit tests.
- No enforced coverage threshold today; new logic should ship with focused unit tests.

## Commit & Pull Request Guidelines
- Match existing history: short, imperative commit subjects (for example, `Fix Windows build: ...`, `Add macOS framework support`).
- Keep each commit scoped to one logical change.
- PRs should include: problem summary, key changes, commands run locally, and platform-specific notes when relevant.
- Ensure CI is green (unit tests + format check) before merge.
