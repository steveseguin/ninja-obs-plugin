# Windows OBS 32.x Build And Validation Notes

Status as of `2026-03-21`: validated on Steve's Windows machine with portable OBS, stream ID `steve12345`, no room ID.

This note captures the actual failures encountered during the TLS/fallback investigation, the code fix that landed, and the local Windows build/test gotchas that made the debugging session harder than it should have been.

## What Went Wrong

### 1. The signaling fallback bug was real, but hidden behind the normal path

The original TLS/fallback patch was intended to recover from pre-open signaling failures, but two state-management bugs meant recovery could still wedge:

1. `VDONinjaSignaling::connect()` could leave `shouldRun_` set after the thread exited on an initial failure, so the next `connect()` saw "thread already running" even though no thread was alive.
2. The pre-open `onError()` path recorded fallback intent, but relied on the WebSocket closing by itself before the reconnect loop could advance to the proxy host.

Result: normal connects looked fine, but an actual pre-open failure could stall fallback or block later reconnect attempts.

### 2. Local build environment drift produced binaries that OBS 32.x could not load

Some local builds were linking against older FFmpeg major versions and produced DLLs that imported:

- `avcodec-60`
- `avutil-58`
- `swscale-7`
- `swresample-4`

The OBS 32.x runtime on this machine expected:

- `avcodec-61`
- `avutil-59`
- `swscale-8`
- `swresample-5`

Result: the plugin build succeeded, but OBS could not load the DLL.

### 3. Multiple OBS/plugin locations made it easy to test the wrong binary

This machine had multiple plugin copies:

- Regular OBS plugin path: `C:\Program Files\obs-studio\obs-plugins\64bit\obs-vdoninja.dll`
- AppData plugin path: `C:\Users\steve\AppData\Roaming\obs-studio\plugins\obs-vdoninja\bin\64bit\obs-vdoninja.dll`
- Portable OBS plugin path used for validation: `C:\Users\steve\Code\ninja-plugin\_obs-portable\obs-plugins\64bit\obs-vdoninja.dll`

Result: "OBS works" did not automatically mean "the repo build is loaded".

### 4. The local Windows rebuild had several machine-specific gotchas

When building against the local OBS source/build tree instead of a packaged SDK, the project also needed:

- `C:\Users\steve\Code\obs-studio\build_x64\config` for `obsconfig.h`
- `C:\Users\steve\Code\obs-studio\deps\w32-pthreads` for `pthread.h`

There was also a libdatachannel header/library mismatch risk:

- The OBS dependency bundle carried an older `rtc` header surface.
- The working static libdatachannel import on this machine came from `C:\Users\steve\.codex\memories\ldc-static-config`.
- The matching headers that worked with that library came from `C:\Users\steve\Code\gpt\vst\build\webrtc_vst_win\_deps\libdatachannel-src\include`.

Result: mixing the wrong headers and libs caused unresolved symbols around `rtc::PeerConnection::setLocalDescription`.

### 5. Portable OBS has a startup gotcha

On this machine, launching portable OBS without setting the working directory to `_obs-portable\bin\64bit` could exit immediately before useful validation happened.

## Code Fix That Landed

Commit: `8e6f5b1` (`Fix signaling fallback recovery`)

File:

- `src/vdoninja-signaling.cpp`

Behavior now encoded:

1. `connect()` joins a dead `wsThread_` before deciding a signaling thread is still running.
2. A pre-open `onError()` now:
   - marks `needsReconnect_ = true`
   - wakes the send loop
   - closes the WebSocket handle if it still exists
3. `wsThreadFunc()` clears `shouldRun_` and `needsReconnect_` on exit.

Result: the initial failure path now advances to the fallback host and later `connect()` attempts are not blocked by stale thread state.

## Validated Outcome

### Normal path

Portable OBS log:

- `_obs-portable/config/obs-studio/logs/2026-03-21 22-03-37.txt`

Observed:

- Plugin loaded from portable path
- Connected to `wss://wss.vdo.ninja`
- Publishing started for `steve12345`
- Viewer connected successfully
- Separate browser probe received audio/video and continuous playback

### Forced fallback path

Portable OBS log:

- `_obs-portable/config/obs-studio/logs/2026-03-21 22-05-36.txt`

Observed after intentionally breaking `wss.vdo.ninja` via `hosts`:

- `WebSocket error from wss://wss.vdo.ninja: TCP connection failed`
- `Signaling connect to wss://wss.vdo.ninja failed before open; trying fallback server`
- `Trying fallback signaling server: wss://proxywss.rtc.ninja:443`
- `WebSocket connected to signaling server: wss://proxywss.rtc.ninja:443`
- Publishing started
- Viewer connected successfully

This is the important regression-proof: fallback was exercised in a real OBS publish run, not just reasoned about from code.

### Direct ICE path seen by browser probe

Browser viewer probe against `https://vdo.ninja/?view=steve12345&cleanoutput=1` showed:

- video/audio tracks present
- playback advancing
- selected ICE pair over `udp`
- local candidate type `prflx`
- remote candidate type `host`

This proves direct UDP media flow for the browser probe. It does not prove the phone's exact candidate type.

## Phone / Cellular / `srflx` Caveat

The plugin logs do not currently prove the phone's selected ICE candidate type.

If a release claim depends on "cellular `srflx` works", capture one of:

1. phone-side `RTCPeerConnection.getStats()` selected candidate pair
2. temporary candidate-pair logging in the plugin

Do not treat OBS log success alone as hard `srflx` proof.

## Local Windows Build Recipe That Worked On This Machine

These are the validated local ingredients on Steve's Windows machine:

- OBS libs:
  - `C:\Users\steve\Code\obs-studio\build_x64\libobs\RelWithDebInfo\obs.lib`
  - `C:\Users\steve\Code\obs-studio\build_x64\UI\obs-frontend-api\RelWithDebInfo\obs-frontend-api.lib`
- OBS headers:
  - `C:\Users\steve\Code\obs-studio\libobs`
  - `C:\Users\steve\Code\obs-studio\frontend\api`
  - `C:\Users\steve\Code\obs-studio\build_x64\config`
  - `C:\Users\steve\Code\obs-studio\deps\w32-pthreads`
- OBS 32.x deps:
  - `C:\Users\steve\Code\obs-studio\.deps\obs-deps-2024-09-12-x64`
  - `C:\Users\steve\Code\obs-studio\.deps\obs-deps-qt6-2024-09-12-x64`
- libdatachannel package config:
  - `C:\Users\steve\.codex\memories\ldc-static-config`
- libdatachannel header override used during build:
  - `C:\Users\steve\Code\gpt\vst\build\webrtc_vst_win\_deps\libdatachannel-src\include`

Configure:

```powershell
cmake -S . -B build-plugin-review-obs32-static -G "Visual Studio 17 2022" -A x64 `
  -DLibDataChannel_DIR="C:/Users/steve/.codex/memories/ldc-static-config" `
  -DCMAKE_C_FLAGS="/IC:/Users/steve/Code/obs-studio/build_x64/config /IC:/Users/steve/Code/obs-studio/deps/w32-pthreads" `
  -DCMAKE_CXX_FLAGS="/IC:/Users/steve/Code/obs-studio/build_x64/config /IC:/Users/steve/Code/obs-studio/deps/w32-pthreads"
```

Build:

```powershell
$env:CL = "/IC:\Users\steve\Code\gpt\vst\build\webrtc_vst_win\_deps\libdatachannel-src\include /IC:\Users\steve\Code\obs-studio\build_x64\config /IC:\Users\steve\Code\obs-studio\deps\w32-pthreads"
cmake --build build-plugin-review-obs32-static --config Release --target obs-vdoninja --clean-first
```

Important:

- Use `--clean-first` after changing any libdatachannel config or include-order override.
- Verify the loaded module path from the running OBS process instead of assuming the rebuilt DLL is actually the one under test.

## Portable OBS Validation Checklist

1. Copy the rebuilt DLL into `_obs-portable/obs-plugins/64bit/obs-vdoninja.dll`.
2. Launch `_obs-portable/bin/64bit/obs64.exe` with working directory set to `_obs-portable/bin/64bit`.
3. Use `--portable --profile Untitled --collection Untitled --startstreaming`.
4. Confirm the loaded plugin module path from the OBS process.
5. Confirm the latest portable OBS log records the expected connect/publish/view sequence.

## Alpha Validation

Status as of `2026-04-12`: validated locally with `game-capture` publishing `VP9 + Alpha` into the OBS native receiver path.

Expected behavior:

- OBS native receiver upgrades to dual-track VP9 alpha and reaches `Native receiver alpha composition active`.
- A normal Chromium viewer can watch the same stream at the same time, but it stays standard color video instead of composited transparency.
- When the publisher exits, OBS clears the native video output instead of holding the last frame.

Tracked motion source used for these checks:

- `tests/tools/gc-motion-demo.html`

Local automation wrappers that now cover the validated path:

### Concurrent OBS + Browser alpha check

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\run-vdoninja-alpha-concurrent-smoke.ps1
```

What it does:

1. syncs the installed plugin DLL into the portable OBS plugin path used on this machine
2. launches the tracked motion-demo window in Chrome
3. launches `game-capture` headless in `VP9 + Alpha`
4. runs a standard Chromium VDO.Ninja viewer against the same stream
5. runs the OBS native receiver smoke against the same stream
6. verifies:
   - browser viewer received exactly one video track with inbound media
   - OBS log reached `Native receiver alpha composition active`
   - OBS log did not hit stale-video timeout or queue-drop storms

### Publisher teardown / clear check

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\run-vdoninja-alpha-teardown-smoke.ps1
```

What it does:

1. syncs the installed plugin DLL into the portable OBS plugin path used on this machine
2. launches the tracked motion-demo window in Chrome
3. launches `game-capture` headless in `VP9 + Alpha`
4. waits long enough for alpha composition to become active
5. lets the publisher exit while OBS stays alive
6. verifies:
   - OBS log reached `Native receiver alpha composition active`
   - OBS log recorded peer disconnect
   - OBS log recorded `Clearing native video output (peer-disconnected)`
   - the end-of-run screenshot from the empty test scene is near-blank

These wrappers intentionally validate against the machine-specific portable plugin path:

- `_obs-portable/config/obs-studio/plugins/obs-vdoninja/bin/64bit/obs-vdoninja.dll`

That avoids the common Windows mistake of testing a stale portable plugin copy while the repo build under `install-obs32` is newer.

## Forced Fallback Test Recipe

Use this only for regression validation, and always restore the file afterward.

1. Back up `C:\Windows\System32\drivers\etc\hosts`.
2. Add:

```text
127.0.0.1 wss.vdo.ninja # ninja-plugin fallback test
```

3. Launch portable OBS and start publishing.
4. Confirm the log shows:
   - primary connect failure
   - signaling diagnostic
   - fallback host selection
   - successful proxy connection
   - successful viewer connection
5. Restore the original `hosts` file.

## Short Gotcha List

- Do not trust a successful compile alone; inspect the DLL import/runtime compatibility.
- Do not trust regular OBS when validating repo builds; use portable OBS and verify the loaded module path.
- Do not mix libdatachannel headers and libraries from different installs.
- Do not skip `build_x64/config` and `deps/w32-pthreads` when building against the local OBS source tree on Windows.
- Do not skip `--clean-first` after changing libdatachannel source/config or header precedence.
- Do not claim hard `srflx` validation without phone-side stats or explicit candidate logging.
