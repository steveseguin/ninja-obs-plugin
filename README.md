# OBS VDO.Ninja Plugin

[![CI](https://github.com/steveseguin/ninja-plugin/actions/workflows/ci.yml/badge.svg)](https://github.com/steveseguin/ninja-plugin/actions/workflows/ci.yml)
[![License: AGPL-3.0-only](https://img.shields.io/badge/License-AGPL--3.0--only-blue.svg)](LICENSE)

Native OBS Studio plugin for [VDO.Ninja](https://vdo.ninja), with WebRTC publishing and ingest paths integrated directly into OBS workflows.

## What Is VDO.Ninja?

VDO.Ninja is a low-latency WebRTC platform used for live production, remote guests, room-based contribution, and browser-based return feeds. It is commonly used with OBS, often via Browser Sources and links like:

- `https://vdo.ninja/?push=YourStreamID`
- `https://vdo.ninja/?view=YourStreamID`

### Download, Install, and Use

- Download latest build: [Releases](https://github.com/steveseguin/ninja-plugin/releases)
- Install steps: [INSTALL.md](INSTALL.md)
- First-run usage guide: [QUICKSTART.md](QUICKSTART.md)
- Full docs: [README Quick Start](#quick-start)

## Why This Plugin Exists

Using VDO.Ninja only through Browser Sources can be limiting for some production workflows. This plugin adds tighter OBS integration so users can:

- Publish directly from OBS output settings to VDO.Ninja.
- Manage inbound room/view streams with less manual setup.
- Configure advanced signaling, salt, and ICE behavior from plugin settings.
- Keep media workflows closer to OBS native controls.

## Core Value

- Faster setup for repeat production workflows.
- Better control of stream/session behavior from OBS.
- Multi-viewer capable publish path.
- Optional data-channel metadata hooks (including WHEP URL hints).

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
- Native decode in `VDO.Ninja Source` is available but still being hardened.
- Locale fallback to built-in English strings is supported if locale files are missing.
- Remote OBS control is not yet a fully hardened command surface.

## Quick Start

### 1. Install

Download the latest package from [Releases](https://github.com/steveseguin/ninja-plugin/releases).

- Linux: `obs-vdoninja-linux-x86_64.tar.gz`
- Windows: `obs-vdoninja-windows-x64.zip`
- macOS: `obs-vdoninja-macos-arm64.zip`

Each release archive includes:

- `INSTALL.md` (quick install instructions)
- `QUICKSTART.md` (first-run workflow)
- `install.cmd` + `install.ps1` on Windows, or `install.sh` on Linux/macOS
- `uninstall.cmd` + `uninstall.ps1` on Windows, or `uninstall.sh` on Linux/macOS

### 2. Publish to VDO.Ninja

1. OBS -> `Settings` -> `Stream`
2. Service: `VDO.Ninja`
3. Set `Stream ID` and optional `Password` / `Room ID`
4. Click `Start Streaming`

Viewer URL pattern:

```text
https://vdo.ninja/?view=<StreamID>
https://vdo.ninja/?view=<StreamID>&password=<Password>
```

### 3. Ingest a VDO.Ninja stream in OBS

1. Add Source -> `VDO.Ninja Source`
2. Enter `Stream ID` (and optional room/password)
3. For room automation, use auto-inbound options in plugin settings

## Key Settings

- `Stream ID`: Primary stream identifier.
- `Password`: Uses VDO.Ninja-compatible hashing behavior.
- `Salt`: Default `vdo.ninja`; change for self-hosted/domain-specific setups.
- `Signaling Server`: Default `wss://wss.vdo.ninja`; can be customized.
- `Custom ICE Servers`: Optional custom STUN/TURN list.
- `Force TURN`: Use relay-only path for difficult network environments.
- `Max Viewers`: Upper bound for simultaneous P2P viewers.

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

## CI and Releases

- `main` pushes run `CI`, `Code Quality`, and `GitHub Pages`.
- Tag pushes matching `v*` run cross-platform build/release packaging.
- Current release workflow auto-builds Linux x86_64, Windows x64, and macOS arm64.
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
