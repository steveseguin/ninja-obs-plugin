# OBS Plugin for VDO.Ninja

[![CI](https://github.com/steveseguin/ninja-obs-plugin/actions/workflows/ci.yml/badge.svg)](https://github.com/steveseguin/ninja-obs-plugin/actions/workflows/ci.yml)
[![License: AGPL-3.0-only](https://img.shields.io/badge/License-AGPL--3.0--only-blue.svg)](LICENSE)

Native OBS Studio plugin for [VDO.Ninja](https://vdo.ninja), with WebRTC publishing and ingest paths integrated directly into OBS workflows.

## What Is VDO.Ninja?

VDO.Ninja is a low-latency WebRTC platform used for live production, remote guests, room-based contribution, and browser-based return feeds. It is commonly used with OBS, often via Browser Sources and links like:

- `https://vdo.ninja/?push=YourStreamID`
- `https://vdo.ninja/?view=YourStreamID`

### Download, Install, and Use

- Download latest build: [Releases](https://github.com/steveseguin/ninja-obs-plugin/releases)
- Install steps: [INSTALL.md](INSTALL.md)
- Linux install: [INSTALL.md#install-linux](INSTALL.md#install-linux)
- macOS install: [INSTALL.md#install-macos](INSTALL.md#install-macos)
- macOS multichannel audio interfaces: [Scarlett 1-2 + 3-4 routing guide](https://steveseguin.github.io/ninja-obs-plugin/macos-multichannel-audio.html)
- macOS installer validation: [docs/macos-installer-validation.md](docs/macos-installer-validation.md)
- Build requirements and source build guide: [BUILDING.md](BUILDING.md)
- Web quick start (post-install): [GitHub Pages Quick Start](https://steveseguin.github.io/ninja-obs-plugin/#quick-start)
- First-run usage guide: [QUICKSTART.md](QUICKSTART.md)
- Full docs: [README Quick Start](#quick-start)
Choose the package that matches the installed OBS version:

| Platform | OBS version | Release package |
| --- | --- | --- |
| Windows | `32.2.x` | `obs-vdoninja-windows-x64-setup.exe` (recommended) or `obs-vdoninja-windows-x64.zip` |
| Windows | `32.0.x` or `32.1.x` | The package whose filename contains `obs32.0-32.1` |
| Linux | `32.2.x` | `obs-vdoninja-linux-x86_64.tar.gz` |
| macOS (Apple silicon) | `32.2.x` | `obs-vdoninja-macos-arm64.pkg` (recommended) or the matching ZIP |

OBS 32.2 changed its bundled FFmpeg runtime, so the Windows binaries for OBS 32.0-32.1 and OBS 32.2 are not interchangeable. This release does not provide prebuilt packages for OBS 31.x or older and does not claim compatibility with future OBS versions beyond 32.2.x.

See [OBS compatibility and release validation](docs/obs-compatibility-and-release-validation.md)
for upgrade policy and the tested Linux runtime versions. Release binaries stay on
the oldest supported SDK in their compatibility band; testing a newer OBS does
not require raising that build baseline.

## Why This Plugin Exists

Using VDO.Ninja only through Browser Sources can be limiting for some production workflows. This plugin adds tighter OBS integration so users can:

- Publish directly from OBS output settings to VDO.Ninja.
- Manage inbound room/view streams with less manual setup.
- Receive compatible VP9 alpha streams for transparent avatars or graphics when paired with Game Capture.
- Configure advanced signaling, salt, and ICE behavior from plugin settings.
- Keep media workflows closer to OBS native controls.

## Core Value

- Faster setup for repeat production workflows.
- Better control of stream/session behavior from OBS.
- Multi-viewer capable publish path.
- Optional data-channel metadata hooks for inbound automation.

## Why Not Just WHIP/WHEP?

WHIP/WHEP is excellent for standards-based ingest/egress, especially for server/CDN pipelines.  
This plugin targets a different primary use case: live interactive VDO.Ninja workflows.

Where this plugin + VDO.Ninja is often stronger:

- **Peer-to-peer first:** very low-latency contribution paths for live production use.
- **One publisher, many direct viewers:** practical multi-viewer room workflows.
- **VDO.Ninja ecosystem support:** room semantics, link-based routing, data-channel metadata patterns.
- **OBS workflow integration:** stream IDs, password/salt behavior, signaling and ICE controls in one place.

Where WHIP/WHEP is often stronger:

- **Simple standards-only server ingest** to a media server/CDN.
- **Centralized distribution architectures** where every viewer goes through infrastructure.
- **Interoperability-first deployments** with minimal platform-specific behavior.

In practice, many teams use both: VDO.Ninja workflows for interactive contribution and WHIP/WHEP for specific distribution paths.

## Current Status

- Publishing (`OBS -> VDO.Ninja`) is the primary stable path.
- Multi-viewer publishing is supported and tested end-to-end.
- Auto-inbound management can create/update Browser Sources from room/data-channel events.
- `VDO.Ninja Source` defaults to a browser-backed viewer path; `Use Native Receiver (Experimental)` switches to an experimental native VP9/H.264/Opus receive path.
- Compatible dual-track VP9 alpha senders can preserve transparency in the native receiver. The tested user-facing path is [Game Capture](https://github.com/steveseguin/game-capture) publishing a Spout2 avatar/graphics source with VP9 alpha enabled.
- Plugin injects a `VDO.Ninja` destination into OBS Stream service list via `rtmp-services` catalog compatibility.
- `Tools -> VDO.Ninja Studio` provides basic publish setup, Go Live/Stop controls, generated links, and runtime peer stats. Advanced service options remain under `Settings -> Stream`.
- Locale fallback to built-in English strings is supported if locale files are missing.
- Remote OBS control is not yet a fully hardened command surface.

## Quick Start

### 1. Install

Download the latest package from [Releases](https://github.com/steveseguin/ninja-obs-plugin/releases).

- Linux, OBS 32.2.x: `obs-vdoninja-linux-x86_64.tar.gz`
- Windows, OBS 32.2.x installer: `obs-vdoninja-windows-x64-setup.exe`
- Windows, OBS 32.2.x portable/manual package: `obs-vdoninja-windows-x64.zip`
- Windows, OBS 32.0.x-32.1.x legacy installer: `obs-vdoninja-windows-x64-obs32.0-32.1-setup.exe`
- Windows, OBS 32.0.x-32.1.x legacy package: `obs-vdoninja-windows-x64-obs32.0-32.1.zip`
- macOS Apple silicon, OBS 32.2.x installer: `obs-vdoninja-macos-arm64.pkg`
- macOS Apple silicon, OBS 32.2.x ZIP fallback: `obs-vdoninja-macos-arm64.zip`

The macOS `.pkg` release artifact should be Developer ID signed, notarized, and stapled. The ZIP fallback is available for manual installs and troubleshooting.

Each release archive includes:

- `INSTALL.md` (quick install instructions)
- `QUICKSTART.md` (offline first-run workflow copy)
- `install.cmd` + `install.ps1` on Windows, or `install.sh` on Linux/macOS
- `uninstall.cmd` + `uninstall.ps1` on Windows, or `uninstall.sh` on Linux/macOS

Windows recommendation:

1. Use the setup `.exe` for normal installs/uninstalls.
2. Use ZIP scripts only for portable/custom-path workflows.
3. For OBS `32.2.x`, use the primary Windows download. For OBS `32.0.x` or `32.1.x`, use the filename containing `obs32.0-32.1`.
4. Use the web quick-start guide after install: `https://steveseguin.github.io/ninja-obs-plugin/#quick-start`.

Portable OBS note: if launching from terminal, start `obs64.exe` from `bin\64bit` (or set `Start-Process -WorkingDirectory` to `bin\64bit`) to avoid `Failed to load theme`.

### 2. Publish to VDO.Ninja

1. OBS -> `Settings` -> `Stream`
2. Service: `VDO.Ninja`
3. `Server` should stay at default (`wss://wss.vdo.ninja:443`) unless self-hosting or troubleshooting signaling; `wss://proxywss.rtc.ninja:443` is available as a fallback.
4. For basic setup, open OBS -> `Tools` -> `VDO.Ninja Studio` and enter the stream ID, password, and optional room.
   - Configure advanced options such as `Signaling Server`, `Salt`, custom ICE/TURN, and packet protection under `Settings -> Stream`; leave optional values blank to use defaults.
5. `Stream Key` remains visible in OBS for compatibility; if you use it directly, set your stream ID or an advanced envelope:
   - URL: `https://vdo.ninja/?push=<StreamID>&password=<Password>&room=<RoomID>&salt=<Salt>&wss=<WSS_URL>`
   - Compact: `<StreamID>|<Password>|<RoomID>|<Salt>|<WSS_URL>`
6. Click `Start Streaming`

Studio note: `Go Live`/`Stop` use the same OBS `Start Streaming`/`Stop Streaming` pipeline and active stream slot. They are not a second parallel output path. If you configured advanced service options, save them under `Settings -> Stream` and use OBS `Start Streaming`; Studio `Go Live` rebuilds the service from the dock's basic fields and default advanced values.

The plugin parses stream-key URLs like:

```text
https://vdo.ninja/?push=<StreamID>&password=<Password>&room=<RoomID>&salt=<Salt>&wss=<WSS_URL>
```

Viewer URL pattern:

```text
https://vdo.ninja/?view=<StreamID>
https://vdo.ninja/?view=<StreamID>&password=<Password>
https://vdo.ninja/?view=<StreamID>&room=<RoomID>&solo
https://vdo.ninja/?view=<StreamID>&room=<RoomID>&solo&password=<Password>
```

### 3. Ingest a VDO.Ninja stream in OBS

1. Recommended today: use Browser Source or room-based auto-inbound.
2. `VDO.Ninja Source` defaults to a browser-backed viewer. Enable `Use Native Receiver (Experimental)` only if you want to test the native VP9/H.264/Opus receive path or receive transparency from a compatible VP9 alpha sender.
3. For room automation, use auto-inbound options in plugin settings.

### Transparent Avatars and Alpha Video

Transparent video in OBS requires the plugin's native receiver and a sender that publishes VDO.Ninja's dual-track VP9 alpha workflow. Browser Sources and normal browser viewers stay standard color video; they do not composite this alpha channel.

The tested path is:

```text
Spout2 avatar/graphics app -> Game Capture -> VDO.Ninja -> OBS VDO.Ninja Source
```

Use it like this:

1. In your avatar or graphics app, enable Spout2 output.
   - VTube Studio has been tested with a `VTubeStudioSpout` sender.
2. In [Game Capture](https://github.com/steveseguin/game-capture), choose `Video Source -> Spout2 (avatar apps)`.
3. Select the Spout2 sender, choose any audio source you need, select `VP9`, and enable the OBS alpha workflow.
4. Start publishing from Game Capture and copy the VDO.Ninja view stream ID or view URL.
5. In OBS, add `VDO.Ninja Source`, set the same stream ID/password, and enable `Use Native Receiver (Experimental)`.

Notes:

- VP9 alpha is CPU-heavy, especially with high-resolution avatar senders. If OBS or Game Capture CPU is high, lower the Game Capture output resolution/FPS or bitrate.
- If the Spout sender is not listed in Game Capture, enable Spout output in the avatar app, keep both apps on the same Windows session/GPU where possible, and refresh the sender list.
- Transparency support is for OBS's native receive path. A regular browser viewer of the same stream should still receive normal color video.

## Key Settings

- `Stream ID`: Primary stream identifier.
- `Password`: Uses VDO.Ninja-compatible hashing behavior.
- `Salt`: Optional; leave blank for default `vdo.ninja` or set for self-hosted/domain-specific setups.
- `Signaling Server`: Optional; leave blank for default `wss://wss.vdo.ninja:443`, try `wss://proxywss.rtc.ninja:443` as a fallback, or set custom signaling.
- `Custom ICE Servers`: Optional custom STUN/TURN list. Use `;` to separate entries.
  - Example: `stun:stun.l.google.com:19302; turn:turn.example.com:3478|user|pass`
- `Force TURN`: Use relay-only path for difficult network environments. Requires at least one TURN server entry.
- `Max Viewers`: Upper bound for simultaneous P2P viewers.
- `Packet Duplication (Experimental)`: Optional paced, delayed copies of selected video RTP packets. NACK repair is
  automatic and remains active in every mode, including `Off`.

  | Mode | Protection | Best-effort extra video traffic | Starting guidance |
  | --- | --- | --- | --- |
  | `Off` | NACK repair only | None while the path is clean | Default; use this first |
  | `Low` | Copies keyframe packets | Up to 20% | Isolated loss with limited spare upload |
  | `Medium` | Copies keyframes plus one quarter of delta packets | Up to 50% | Measured random loss after lowering bitrate |
  | `High` | Can copy every video packet once | Up to 100% | Controlled testing with enough capacity to nearly double video traffic |

  Copies are delayed by 15 ms, yield to live media, and expire after 250 ms instead of extending the stream queue. This
  is packet duplication, not negotiated video RED/ULPFEC, FlexFEC, or RTX.
- `Audio RED (Experimental)`: Adds the previous Opus frame to the current packet using negotiated RFC 2198 RED. It is off by default, and each viewer falls back to ordinary Opus unless its SDP answer selects the offered RED mapping.
- `Adaptive Bitrate from REMB (Experimental)`: Dynamically changes supported OBS encoders and RTP pacing from receiver estimates. It is off by default, uses the lowest fresh estimate across every connected viewer, decreases in stages, increases conservatively, and restores the configured encoder bitrate when streaming stops.
- `Minimum Adaptive Bitrate`: Floor used only while adaptive bitrate is enabled.

See [Packet-Loss Protection Reference](docs/packet-loss-protection.md) for NACK/cache behavior, per-viewer fan-out costs,
Audio RED versus Opus FEC, mode selection, native-receiver limitations, and why the plugin does not advertise H.264
RED/ULPFEC.

The VDO.Ninja stream service keeps the effective keyframe interval at two seconds or less unless OBS's advanced `Ignore streaming service setting recommendations` option bypasses service settings. The plugin cannot request an extra IDR for PLI because libobs has no on-demand keyframe API, so an established stream keeps flowing until the next scheduled IDR. Dependent frames are gated only after the publisher knows it lost or partially failed a frame locally.

Default ICE behavior:
- If `Custom ICE Servers` is empty, plugin uses built-in STUN servers (`stun:stun.l.google.com:19302` and `stun:stun.cloudflare.com:3478`).
- No TURN server is added automatically unless you provide one.

### Packet-loss diagnostics

The OBS log writes a rolling `Publish:` summary with keyframe size/cadence, per-peer pacer delay and drops, NACK/cache/repair counts, PLI/FIR, receiver loss/jitter/RTT, REMB, packet-duplication activity, audio RED activity, and audio continuity. Use that evidence before assuming encoder overload and compare it with the viewer's VDO.Ninja/WebRTC statistics.

For a fixed-rate stream that exceeds a viewer's route, lower bitrate or resolution first. Packet duplication and RED can be useful opt-in tools, but they add traffic and do not create capacity on an already saturated route. Adaptive bitrate is the opt-in path intended to react to fresh receiver estimates.

The plugin has no native ULPFEC/FlexFEC generator, and libdatachannel does not provide one in this publisher path.
Advertising payload types without generating and validating repair packets would be misleading. Upstream libwebrtc also
disables H.264 ULPFEC when NACK is enabled because that combination can require retransmitting FEC packets. The plugin
therefore does not offer H.264 RED/ULPFEC until a native generator passes induced-loss recovery tests in supported
browsers. Audio RED is interoperable, paced duplication remains the default-off H.264 protection option, and FlexFEC is
the preferred future H.264 FEC candidate.

## Testing

### Unit tests

```bash
cmake -B build -DBUILD_TESTS=ON -DBUILD_PLUGIN=OFF -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target vdoninja-tests
ctest --test-dir build --output-on-failure
```

### End-to-end (Playwright)

```bash
npm ci
npm test
```

E2E covers:

- Publish -> view playback validation
- Viewer reload continuity
- One publisher -> multiple concurrent viewers
- Firefox receive validation (Chromium publisher -> Firefox viewer)

Manual OBS test checklist:

- [TESTING_OBS_MANUAL.md](TESTING_OBS_MANUAL.md)

## Build from Source

For a clean-machine setup (dependencies, required paths, and platform-specific commands), use:

- [BUILDING.md](BUILDING.md)

Windows includes a dependency checker script:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\check-build-requirements-windows.ps1 `
  -ObsSdkPath "D:\deps\obs-sdk" `
  -LibDataChannelPrefix "D:\deps\libdatachannel-install" `
  -Qt6Prefix "D:\deps\obs-deps-qt6"
```

## CI and Releases

- `main` pushes run `CI`, `Code Quality`, and `GitHub Pages`.
- Tag pushes matching `v*` run cross-platform build/release packaging.
- Current release workflow auto-builds Linux x86_64, Windows x64 ZIP + setup `.exe`, and macOS arm64 `.pkg` + ZIP; macOS release packages must pass strict signing/notarization validation before upload.
- Both tag-triggered and manually dispatched release builds fail up front if any required macOS signing or notarization secret is missing; the workflow never publishes an unsigned macOS fallback.
- Use `powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\release.ps1 -Action status` to inspect release readiness.
- Supported staged flow: `status` -> `prepare` -> `verify` -> `cut`.
- `prepare` promotes `CHANGELOG.md` and aligns version files for the target release; `verify` runs the release checks against that prepared version; `cut` repeats the full path, then commits/tags/pushes.
- Use `powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\release.ps1 -Action cut -Bump patch -Push` to align version files, promote `CHANGELOG.md`, run release checks, commit, tag, and push in one supported flow.
- The release script owns `CMakeLists.txt`, `src/plugin-main.h`, `package.json`, `package-lock.json`, and `CHANGELOG.md`; tag CI now rejects mismatches instead of building a broken release.
- Before tagging, sync signing secrets with `powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\sync-release-secrets-windows.ps1`.
- Optional nightly live internet e2e matrix is in `.github/workflows/live-e2e.yml`.

## Trust and Security

- Releases include `checksums.txt` for SHA-256 verification.
- See [SECURITY_AND_TRUST.md](SECURITY_AND_TRUST.md) for signing status and verification guidance.

## Project Layout

- `src/`: plugin implementation (`vdoninja-output`, `vdoninja-source`, signaling, peer manager, data channel)
- `tests/`: GoogleTest suites and stubs
- `tests/e2e/`: Playwright end-to-end specs
- `data/locale/`: localization files
- `.github/workflows/`: CI/build/pages pipelines

## License

Licensed under **AGPL-3.0-only**.

- [LICENSE](LICENSE)
- [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md)
- [RELEASE_COMPLIANCE.md](RELEASE_COMPLIANCE.md)
- [SECURITY_AND_TRUST.md](SECURITY_AND_TRUST.md)
