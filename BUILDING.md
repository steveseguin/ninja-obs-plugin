# Build Requirements and From-Scratch Setup

This guide is for building the plugin from a fresh clone on a new machine.

## What You Need

Plugin builds (`BUILD_PLUGIN=ON`) require:

1. A C++ toolchain and CMake
2. OBS SDK headers and libraries
3. `libdatachannel` CMake package (or equivalent install prefix)
4. Qt Widgets development files (Qt6 preferred, Qt5 fallback)
5. OpenSSL development libraries

Unit tests only (`BUILD_PLUGIN=OFF`) do not require OBS SDK, Qt, or `libdatachannel`.

## Quick Test-Only Build

```bash
cmake -B build -DBUILD_TESTS=ON -DBUILD_PLUGIN=OFF -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target vdoninja-tests
ctest --test-dir build --output-on-failure
```

## Windows (Recommended From Scratch Flow)

### 1. Install tools

Install:

1. Visual Studio 2022 Build Tools (MSVC + Windows SDK + CMake/Ninja support)
2. CMake
3. Git
4. Node.js (only needed for Playwright e2e)

### 2. Prepare dependency folders

Use any layout you want. Example:

- `D:\deps\obs-sdk`
- `D:\deps\libdatachannel-install`
- `D:\deps\obs-deps-qt6`

### 3. Install OBS SDK and Qt deps

Provide:

1. OBS SDK path with `include/obs/obs-module.h` and `lib/obs.lib`
2. Qt6 path containing `lib/cmake/Qt6/Qt6Config.cmake` (or pass that folder directly as `Qt6_DIR`)

### 4. Build and install `libdatachannel` (example)

```powershell
git clone --recursive --branch v0.20.2 https://github.com/paullouisageneau/libdatachannel.git D:\deps\libdatachannel
cmake -S D:\deps\libdatachannel -B D:\deps\libdatachannel\build -G "Visual Studio 17 2022" -A x64 -DNO_EXAMPLES=ON -DNO_TESTS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build D:\deps\libdatachannel\build --config Release
cmake --install D:\deps\libdatachannel\build --config Release --prefix D:\deps\libdatachannel-install
```

### 5. Verify your dependency paths

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\check-build-requirements-windows.ps1 `
  -ObsSdkPath "D:\deps\obs-sdk" `
  -LibDataChannelPrefix "D:\deps\libdatachannel-install" `
  -Qt6Prefix "D:\deps\obs-deps-qt6"
```

### 6. Configure and build plugin

```powershell
cmake -S . -B build-plugin -G "Visual Studio 17 2022" -A x64 `
  -DBUILD_PLUGIN=ON `
  -DBUILD_TESTS=OFF `
  -DCMAKE_BUILD_TYPE=Release `
  -DOBS_SDK_PATH="D:/deps/obs-sdk" `
  -DCMAKE_PREFIX_PATH="D:/deps/libdatachannel-install;D:/deps/obs-deps-qt6" `
  -DQt6_DIR="D:/deps/obs-deps-qt6/lib/cmake/Qt6"

cmake --build build-plugin --config Release
cmake --install build-plugin --config Release --prefix install
```

## Linux Build Notes

You need:

1. C++ compiler + CMake
2. OBS development package (`libobs` and frontend headers)
3. Qt Widgets development package
4. `libdatachannel` package or custom install prefix
5. OpenSSL development package

If your distro packages all dependencies, plugin build is typically:

```bash
cmake -S . -B build-plugin -DBUILD_PLUGIN=ON -DBUILD_TESTS=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build-plugin
cmake --install build-plugin --prefix install
```

If dependencies are in non-standard paths, pass them via `CMAKE_PREFIX_PATH`.

## macOS Build Notes

You need:

1. Xcode command line tools
2. OBS SDK/framework path
3. Qt Widgets dev files
4. `libdatachannel` install prefix
5. OpenSSL

If not in default lookup paths, pass `OBS_SDK_PATH` and `CMAKE_PREFIX_PATH`.

## Troubleshooting

### `Qt5Config.cmake` / `Qt6Config.cmake` not found

Set `Qt6_DIR` explicitly and include your Qt prefix in `CMAKE_PREFIX_PATH`.

### `libdatachannel not found`

Install `libdatachannel` and pass its prefix through `CMAKE_PREFIX_PATH`.

### `OBS SDK not found`

Set `OBS_SDK_PATH` to the SDK root with `include` and `lib`.

### It worked before, now it fails after cleaning build folders

Old `CMakeCache.txt` may have contained machine-specific paths. Reconfigure with explicit paths above.
