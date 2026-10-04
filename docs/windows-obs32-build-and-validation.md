# Windows OBS 32.x Build And Validation Notes

## Windows installer validation (2026-10-03)

Run the isolated ZIP and compiled setup regression suite with Inno Setup 6 installed:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tests/test-windows-package.ps1 -RequireCompiler
```

Use `-IsccPath "C:\path\to\ISCC.exe"` for a portable compiler. The suite uses inert
payloads under `artifacts/windows-installer-tests`, disables test setup registration,
and checks custom paths, cancellation, missing/incomplete OBS roots, per-user installs,
and upgrades. Omitting `-RequireCompiler` permits ZIP-only checks; CI requires both.

Local validation also exercised the interactive executable picker, invalid-folder
error, final destination confirmation, and installation. A production-script installer
using the unchanged 1.1.68 DLL passed fresh install, repair, and uninstall against a
separate OBS 32.2.2 portable copy. The running process loaded `obs-vdoninja.dll` from
the selected custom folder; uninstall preserved OBS and an unrelated sentinel file.
These checks validate installer behavior, not a new media or compatibility claim.

Standalone Windows unit tests now request OBS's 1 ms timer resolution for the test
process and release it on teardown. This avoids false repair-expiration failures
from repeated short waits under coarse Windows timers without relaxing assertions
or changing plugin behavior. All 728 Windows tests passed; the affected pacing test
also passed 20 consecutive runs.

## Earlier OBS 32.x validation

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

## Published Audio Continuity Check

The packet and PCM analyzer tests do not require OBS:

```powershell
npm run test:audio-continuity
npm run test:audio-capture-control
```

For an end-to-end check through portable OBS, the real plugin, VDO.Ninja signaling, and both Chromium and OBS
Browser Source viewers:

```powershell
$env:VDONINJA_SOURCE_MODE = "audio-continuity"
$env:VDONINJA_SOAK_MS = "60000"
$env:VDONINJA_VIEW_BUFFER_MS = "500"
$env:VDONINJA_REQUIRE_ZERO_FREEZES = "1"
$env:VDONINJA_REQUIRE_PACER_SPLIT = "1"
$env:VDONINJA_OBS_BROWSER_VIEWER = "1"
powershell -ExecutionPolicy Bypass -File .\scripts\run-vdoninja-publish-smoke.ps1
```

The harness uses a Browser Source for high-motion video and a separate Media Source for a precomputed 997 Hz
WAV. Keeping the tone out of Chromium makes the sender-side control deterministic: a busy Browser Source can
itself underrun before the plugin receives the audio. The check fails on raw decoded-track dropouts, click-sized
discontinuities, non-forward timestamps, new Chrome concealment, packet loss, media queue drops, transport send
rejection, or video freezes. It saves the source WAV, raw decoded PCM, Web Audio playout PCM, screenshots, and a
JSON report under `artifacts/`.

Use `$env:VDONINJA_RECORD_LOCAL_OUTPUT = "1"` to record the OBS mix at the same time for an upstream control.
The raw decoded `MediaStreamTrack` is the continuity authority. Feeding that track through a second Web Audio
clock can periodically insert or remove samples even when the raw decoded track, RTP cadence, and local recording
are clean; those playout-clock shifts remain in the report as diagnostics.

When the actual OBS Browser Source viewer is enabled, the harness routes its audio through OBS and mutes it so
the viewer cannot feed the published tone back into the mix. It establishes Chrome's freeze-counter baseline
after that viewer starts and after the first OBS screenshot; the PCM capture still spans setup, but steady-state
video assertions do not mistake deliberate setup work for a transport freeze. Decoded-track timestamp gaps over
12 ms are recorded with sample and wall-clock positions for correlation against OBS's 30-second publish lines.

The publish lines themselves are emitted by a dedicated summary worker. Never move their `logInfo` call back
into the encoded-packet callback: OBS takes a global logfile lock and synchronously flushes each line, which can
stall both encoded audio and video delivery.

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

## Publishing Settings Validation (2026-10-03)

Validated with portable OBS 32.2.2, a synthetic 1280x720/60 scene, and the locally built Release DLL. The running process loaded `_obs-portable/obs-plugins/64bit/obs-vdoninja.dll`; its hash matched `build-plugin-release-validation/Release/obs-vdoninja.dll`.

| Check | Result |
| --- | --- |
| Edit dock ID/password, use native Start Streaming and dock Go Live | Both published the entered identity; Copy Viewer Link matched it |
| Restart OBS | Saved ID was reused |
| Common VDO.Ninja service, ApplyServiceSettings enabled, IgnoreRecommended disabled | x264 retained CRF, 12000 kbps CBR, and 24000 kbps CBR; Intel QSV retained ICQ quality 23 |
| Intel QSV compatibility settings | Zero B-frames and a two-second keyframe interval, with ICQ unchanged |
| Go Live after advanced settings changes | Preserved custom salt and adaptive minimum; clearing the password stayed effective |
| Chrome using the copied link | Decoded over 300 frames from Intel ICQ and 24000 kbps CBR, including a custom salt |
| C++ tests and formatting | 737 tests passed; clang-format 14 passed for all `src`/`tests` C++ files |
| Windows packaging | 22 ZIP/compiled-installer scenarios passed; scoped firewall helper passed mocked rule checks |

The 24000 kbps run received about 17005 kbps in the short browser sample, proving delivery above 6/12 Mbps. Its OBS log also recorded pacer queue growth/drops, so this is not a sustained 24 Mbps performance certification. Remote Firefox playback, cellular paths, and changes to the real Windows firewall were not tested in this pass.

Copied signaling overrides use `wss2`, which retains VDO.Ninja's signaling protocol and encryption. With the same password and endpoint, `wss` selected the browser's custom-server protocol and prevented this test viewer from accepting the offer; `wss2` decoded successfully. Legacy `wss` input in an OBS Stream Key is still accepted and converted when generating links.

Local evidence is under `artifacts/publish-settings-runtime/` (OBS logs, viewer statistics, module hashes, and restoration results), with build/test logs under `artifacts/publish-settings-*.log`. The original portable OBS configuration, DLL, and locale were restored and compared against the backup after testing.

### Custom Local UDP Ports

The additional port-selection validation used the same portable OBS version and verified the loaded DLL path/hash again. The publisher keeps separate sockets per peer; UDP multiplexing remains disabled, so a range is recommended for multiple viewers.

- Native Start Streaming with `50000`: Chrome decoded more than 300 frames, and its received SDP contained only host port 50000 from OBS.
- Dock Go Live with `50010-50020`: two concurrent Chrome viewers decoded more than 300 frames each. Their received OBS host ports were 50010 and 50020, respectively.
- Restart retained the custom range. Go Live honored a subsequent service-side range change. Native Start Streaming restored the profile's range after recreating a common VDO.Ninja service containing only the Stream Key/server fields.
- Switching back to Auto restored normal allocation and browser playback. Invalid text could not start through Go Live.
- The linked socket test gathered real candidates for two peers within 42000-42100, then verified that an occupied fixed port failed without allocating a replacement port. The complete native linked suite passed.
- All 741 dependency-light unit tests passed, including automatic allocation, single ports, inclusive ranges, boundary values, and rejection of malformed/descending/out-of-bounds input. The plugin build and clang-format 14 checks passed.

Evidence is under `artifacts/udp-ports-runtime/` and `artifacts/udp-ports-*.log`. The original portable OBS configuration, DLL, and locale were restored and compared against the backup. These local tests do not establish whether a remote viewer will use TURN or whether a router will preserve the chosen external port.

### Remote BrowserStack Validation (2026-10-03)

Used the updated DLL in portable OBS 32.2.2, verified its loaded path and SHA-256 before and after restart, and published only synthetic 1280x720/60 motion. Sessions ran sequentially without BrowserStack Local or network shaping. Each successful receiver sample lasted about 20 seconds. BrowserStack credentials were read from the external `chunkcast/.secrets` file; no credential file was copied here.

| Remote receiver | OBS configuration | Receiver result | Selected ICE pair |
| --- | --- | --- | --- |
| Windows Chrome 154 | x264 CBR 12000; Auto ports; native Start Streaming | 12023 kbps, 1204 decoded frames, zero lost packets and reported freezes | srflx/srflx UDP; 26 ms RTT |
| Windows Chrome 154 | Intel QSV ICQ 23; fixed port 50000; native Start Streaming | 5563 kbps, 1206 decoded frames, zero lost packets; 10 reported freezes totaling 2.118 s | srflx/srflx UDP; 25 ms RTT |
| Pixel 9 Chrome | Intel QSV ICQ 23; range 50010-50020; dock Go Live | 5574 kbps, 1206 decoded frames, zero lost packets; 8 reported freezes totaling 1.470 s | srflx/srflx UDP; 77 ms RTT |
| Windows Playwright Firefox 155 and standard WebDriver Firefox 157 | H.264 capability check / ICQ playback attempt | Neither installation advertised H.264 WebRTC decoding; Firefox playback remains unverified | No selected media pair |

Receiver-side `getStats()` proves the three successful routes were direct UDP. OBS socket snapshots confirmed port 50000 and port 50010 for the corresponding cases. Both start buttons preserved the edited stream ID, and Copy Viewer Link matched the live identity. Encoder logs confirmed ICQ 23, zero B-frames and a two-second keyframe interval. The ICQ cases passed the playback-progress gate, but their freeze counters prevent a smooth-playback claim. OBS logged roughly 570 KB keyframes and around 200 ms keyframe send times; pacing needs follow-up. These short results do not diagnose KRD's NAT/firewall or certify sustained 24 Mbps delivery. The phone used BrowserStack's network, not a cellular route.

The remote harness needed two adjustments: disabling BrowserStack network logging restored signaling after WebSocket HTTP 403 responses, and `networkProfile: "none"` avoided the unsupported desktop network-update API. The failed attempts are retained with the successful reports.

A separate viewer-side reproduction used both `codec=h264` and `bitrate=24000`: `CodecsHandler.setVideoBitrates()` threw before the answer was applied. Captured input contained a bare LF between the video media line and its connection line, while the parser split on CRLF. The failure also reproduced locally. The copied viewer link without the bitrate override worked remotely, and a local check with `bitrate=12000` alone decoded successfully. No deployed viewer code was changed during this validation.

Evidence is under `artifacts/browserstack-publish-settings/`, including raw receiver reports, OBS logs, socket snapshots, module verification and `summary.json`. Remote sessions were closed, and portable OBS's original configuration, DLL and locale were restored and hash-compared against the backup.

### Release Retest After Windows Pacing Correction

Windows condition-variable waits rounded sub-millisecond token waits up, repeatedly exhausting the small shared bucket before the next wakeup. The corrected pacer uses a thread-owned high-resolution waitable timer for these short waits while keeping the existing burst cap and longer interruptible waits. It falls back to the condition variable if the high-resolution timer is unavailable. The same synthetic scene, encoder settings, and 20-second receiver measurements then produced:

| Receiver / mode | Received kbps | Decoded frames | Reported freezes | Lost packets |
| --- | ---: | ---: | ---: | ---: |
| Remote Windows Chrome / ICQ 23, port 50000 | 5653 | 1208 | 0 | 0 |
| Remote Pixel 9 Chrome / ICQ 23, range 50010-50020 | 5790 | 1210 | 0 | 0 |
| Local Firefox / ICQ 23 with corrected viewer helper and explicit codec + bitrate | 5638 | 1212 | 0 | 0 |
| Remote Windows Chrome / CBR 24000, Auto ports | 24043 | 1204 | 0 | 0 |

The CBR 24000 log recorded zero pacing drops and a maximum keyframe send time of 52 ms. All 742 unit tests, the native linked suite, 22 compiled Windows installer scenarios, and mocked firewall checks passed. Real firewall integration is additionally required by the elevated Windows CI runner; the local shell is not elevated. The focused viewer suite passed nine cases in each of local Chrome and Firefox. The viewer correction preserves CRLF when changing codec order; it is maintained separately in the VDO.Ninja web repository.

Evidence is under `artifacts/publish-release-runtime/`. The running portable OBS DLL path/hash was verified, and its configuration, DLL and locale were restored and hash-compared after testing. These are short checks, not long-duration soak results; all playback measurement windows were below one minute.

### Windows High-Bitrate Validation (v1.1.71)

On 2026-10-03, the published v1.1.71 DLL was verified against the module loaded by portable OBS 32.2.2. Tests used 1080p60 CBR, local Chrome, NVIDIA TITAN RTX NVENC, and Intel Core Ultra 7 265K QuickSync. Each playback measurement lasted 20 seconds. The fixed-noise scene deliberately stresses large keyframes.

| Encoder | Configured Mbps | Received Mbps | Reported freezes | Lost packets |
| --- | ---: | ---: | ---: | ---: |
| x264 veryfast | 20 | 20.14 | 9 | 0 |
| x264 veryfast | 40 | 40.12 | 9 | 0 |
| NVIDIA NVENC P5 / HQ | 20 | 20.03 | 0 | 0 |
| NVIDIA NVENC P5 / HQ | 40 | 40.13 | 0 | 0 |
| Intel QuickSync TU4 | 20 | 20.29 | 0 | 0 |
| Intel QuickSync TU4 | 40 | 33.31 | 0 | 0 |

All local cases decoded both test audio tones. Intel's 40 Mbps publisher log reported about 39 Mbps in its final interval; the receiver measurement above must not be presented as sustained 40 Mbps delivery. Saved B-frame requests were deliberately set to three. Recordings using the streaming encoder decoded successfully with zero B-frames for every encoder and bitrate, confirming the runtime compatibility override despite the saved UI value.

With ordinary animation, x264 delivered 40.28 Mbps locally with zero freezes. A remote BrowserStack Windows Chrome receiver measured NVIDIA at 40.29 Mbps with zero freezes, lost packets, or concealed audio samples. The remote fixed-noise case received 40.40 Mbps but reported 16 freezes totaling 3.45 seconds, despite zero loss and a direct `srflx`/`srflx` UDP route. High-bitrate transport works; expensive-keyframe playback still needs investigation, and these checks do not establish its cause.

No plugin or website code changed for this validation. Bitrates above 40 Mbps, AMD encoders, and VPN paths were not tested. Evidence, screenshots, bitstream checks, and a browsable report are under `artifacts/high-bitrate-1.1.71/`. BrowserStack sessions were closed, and portable OBS configuration, DLL, and locale were restored and hash-verified.

### WHIP Review and High-Bitrate Follow-up

The local `../obs-studio/plugins/obs-webrtc/` implementation (encoder-start safeguards at OBS commit `2c9f0b1df`) supplied two useful patterns: tenfold transmission headroom for keyframes, and enforcing encoder compatibility before initialization while checking an already-active recording encoder. The Ninja plugin now follows those patterns, sanitizes custom NVENC `frameIntervalP` overrides, and changes UHQ tuning to HQ because UHQ requires B-frames. Other quality/rate-control choices remain intact. It implements these safeguards without requiring the custom WHIP encoder flag in that OBS checkout.

The shared packet budget now retains 250 microseconds of high-rate pacing tokens within a 4–16 KB bound, avoiding discarded tokens on delayed Windows timer wakeups. Per-viewer retained media stays capped at 8 MB. The pacer permits ten times nominal encoder bitrate up to 500 Mbps; these are transmission headroom limits, not encoder targets or a claim of tested 500 Mbps delivery. RTP payload fragmentation remains 1200 bytes. WHIP's HTTP signaling and simulcast machinery were not imported into the WebSocket publishing path; Ninja already has bounded NACK repair and small RTP fragments.

Same-machine 1080p60 stress comparisons, each using a 20-second receiver sample:

| Receiver / encoder | v1.1.71 freezes | Updated freezes | Updated received Mbps |
| --- | ---: | ---: | ---: |
| Local Chrome / x264 CBR 20 Mbps | 9 | 0 | 20.19 |
| Local Chrome / x264 CBR 40 Mbps | 9 | 0 | 40.22 |
| Local Chrome / NVENC CBR 40 Mbps | 0 | 0 | 39.96 |
| Remote Windows Chrome / NVENC CBR 40 Mbps | 16 | 0 | 39.98 |

All samples reported zero packet loss. Local decoded audio contained both test tones, and the remote sample had zero audio concealment. The remote publisher's maximum keyframe send time fell from 142 ms to 34 ms; maximum frame-send time fell from 237 ms to 56 ms. Intel ICQ 23 also retained its quality mode and passed at 11.44 Mbps with zero reported freezes or packet loss. Two simultaneous local Chrome viewers each received about 40.04 Mbps with verified audio, zero freezes, and zero lost packets.

With OBS service recommendations disabled, a recording started first contained 75 B-frames; streaming was correctly declined while recording continued. Starting streaming first produced zero B-frames and allowed streaming to stop/restart while recording continued. NVENC custom `frameIntervalP=4` was changed to `1` at runtime while preserving `aqStrength=8`; its recorded stream contained zero B-frames. The OBS error guidance and encoder settings were visually checked. Evidence is under `artifacts/whip-publishing-improvements/`. These short samples do not certify all networks, hardware, or long-running sessions.


### BrowserStack Packet-Size Follow-up (2026-10-04 UTC)

Twelve sequential BrowserStack sessions used about 20 seconds of measured playback each (one split into 5/10/5-second phases). Portable OBS published synthetic 1080p60 NVENC video and stereo tones. The published v1.1.72 DLL and matched local builds with 1200-byte and 1000-byte H.264 RTP payload limits were checked by loaded module path and hash. The temporary packet-size change was reverted; released behavior is unchanged.

| Receiver / path | RTP payload limit | Target Mbps | Received Mbps | Reported freezes | Concealed audio ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| Pixel 9, direct UDP, two samples | 1200 | 20 | 20.10-20.20 | 0 | 0 |
| Pixel 9, direct UDP, two samples | 1200 | 40 | 40.28-40.48 | 0 | 0-8 |
| Galaxy S25 Ultra, direct UDP | 1200 | 20 | 20.52 | 0 | 0 |
| Pixel 9, direct UDP | 1000 | 20 | 20.20 | 0 | 0 |
| Pixel 9, fresh direct UDP connections, two samples | 1000 | 40 | 39.69-40.59 | 0-2 | 0-28 |
| Pixel 9, forced TURN UDP, published DLL | 1200 | 20 | 20.26 | 0 | 0 |

All table rows using 1200-byte payloads reported zero packet loss. One fresh 1000-byte/40-Mbps connection reported 69 lost packets, two freezes totaling 1.145 seconds, and sender transport rejections/repair failures; a later fresh connection was clean. An additional 1000-byte repeat retained the preceding viewer in the publisher log, so it is excluded from the controlled comparison above. These short results do not establish packet size as the cause of the variation or justify reducing the default.

The Android presentation callback diagnostic remained uneven with both packet sizes, despite approximately 60 decoded frames per second and often zero reported freezes. The Pixel averaged roughly 53 presented frames per second at 40 Mbps; the S25 averaged about 29 at 20 Mbps in this test environment. Do not equate the transport progress gate with perfect physical display cadence. Nonzero receiver audio energy confirms decoded audio, not physical speaker output.

BrowserStack accepted the requested network profiles, but a diagnostic 1-Mbps cap still allowed 20.212 Mbps over direct UDP. Its loss/delay simulation was not validated for this media path; a few NACKs also occurred without shaping. These checks do not reproduce a VPN MTU restriction. The documented BrowserStack network controls cover bandwidth, latency, and loss: https://www.browserstack.com/docs/automate/selenium/simulate-network-conditions .

The viewer harness now saves decode timing, buffer targets, audio energy/concealment details, and rendered video dimensions. `--capture-decoded-frame 1` optionally writes a PNG after measurement; use it when Android page screenshots omit the video layer. It confirms decoded image content independently of the page screenshot, without claiming display smoothness. `--capture-presentation 1 --expected-fps 60` records presentation diagnostics; `--require-presentation 1` additionally gates success on them.

Evidence, raw metrics, decoded frames, module hashes, and the browsable report are under `artifacts/browserstack-packet-size-1.1.72/`. Portable OBS configuration, DLL, and locale were restored and hash-compared with the backup; the local build was restored to the 1200-byte default. No plugin release or packet-size option was added for these test-only changes.
