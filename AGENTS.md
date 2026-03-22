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

## OBS Compatibility And Release Baseline
- OBS plugin binaries are version-gated by the OBS/libobs SDK they are built against; the plugin's own `PLUGIN_VERSION` is not the compatibility key.
- Build release artifacts against the oldest OBS version in the compatibility band you intend to support.
- For the current OBS 32.x line, the release baseline is OBS `32.0.4`.
- Do not bump the release workflow to a newer OBS SDK just because it exists; doing so can prevent the plugin from loading on older OBS 32.x installs.
- Only introduce separate OBS-version-specific release artifacts if the code starts requiring newer-only OBS APIs.
- Prefer one obvious Windows installer/ZIP pair for users unless a real compatibility split is technically necessary.
- When changing OBS targets, update `.github/workflows/build.yml`, release notes, and install/docs text together.

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

## Windows Build/Test Memory
- On this machine, do not assume regular OBS is testing the repo DLL. Validate with portable OBS and verify the loaded module path from the running `obs64` process.
- Portable OBS should be launched with working directory `C:\Users\steve\Code\ninja-plugin\_obs-portable\bin\64bit`, otherwise startup can exit early.
- When building against the local OBS source/build tree, include both:
  - `C:\Users\steve\Code\obs-studio\build_x64\config`
  - `C:\Users\steve\Code\obs-studio\deps\w32-pthreads`
- Keep libdatachannel headers and libraries from the same build family. During the `2026-03-21` validation, the working local pairing was:
  - package config / static library: `C:\Users\steve\.codex\memories\ldc-static-config`
  - header override during build: `C:\Users\steve\Code\gpt\vst\build\webrtc_vst_win\_deps\libdatachannel-src\include`
- After changing libdatachannel config, header precedence, or Windows include flags, rebuild with `--clean-first` to avoid stale objects hiding ABI/header mismatches.
- The normal-path TLS warning `TLS certificate verification with root CA is not supported on Windows` was observed during successful connects. Treat it as informational unless the signaling connection actually fails.
- Fallback regression should be verified with the hosts-file fault injection documented in `docs/windows-obs32-build-and-validation.md`, not just by reading the code.
- OBS/plugin logs do not prove the phone's selected ICE candidate type. For hard cellular / `srflx` proof, capture phone-side `getStats()` or add temporary candidate-pair logging.
