# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- Windows GUI installer (`obs-vdoninja-windows-x64-setup.exe`) via Inno Setup, with:
  - OBS install path detection
  - optional launch OBS / open Quick Start on finish
  - Add/Remove Programs uninstall support
- New build helper script: `scripts/build-installer-windows.ps1`.

### Changed
- Stream destination UX now avoids the misleading `Get Stream Key` helper button for VDO.Ninja.
- VDO.Ninja server label in OBS Stream settings now explicitly points users to `Tools -> Configure VDO.Ninja`.
- README/QUICKSTART now clarify that OBS still shows a `Stream Key` field for compatibility.
- Added a new `Tools -> VDO.Ninja Control Center` workflow with one-place publish config, start/stop controls, generated links, and runtime peer telemetry.
- Windows release docs now route users to the setup `.exe` first, with ZIP install scripts as fallback.
- Release workflow now publishes Windows ZIP + setup `.exe`, and release checksums include `.exe` artifacts.
- Build workflow now targets OBS `32.1.0-rc1` to align with current service ABI requirements.
- Inno Setup uninstaller files now live under `data/obs-plugins/obs-vdoninja/_installer` instead of the OBS root.

### Fixed
- Plugin runtime version banner now correctly reports `1.1.11` (`PLUGIN_VERSION` macro alignment).

## [1.1.10] - 2026-02-22

### Added
- Tools menu action: `Configure VDO.Ninja` to activate `vdoninja_service` even though OBS 32.x stream UI does not list third-party service types.
- Activation flow now seeds VDO.Ninja settings from the current stream key (including parsing `https://vdo.ninja/?push=...&password=...&room=...`) and auto-generates a stream ID fallback.
- Automatic `rtmp-services` catalog injection for a `VDO.Ninja` Stream destination entry in OBS settings.
- Stream key envelope parsing for advanced settings:
  - URL form supports `push/view`, `password` (`pasword` alias), `room`, `salt`, and `wss`/`wss_host`/`server`/`signaling`.
  - Compact form supports `stream|password|room|salt|wss`.

### Changed
- Service/output codec declarations aligned to a WHIP-like compatibility baseline:
  - video: `h264`
  - audio: `opus`
- Activation flow now applies Opus defaults to OBS profile output settings (`SimpleOutput`/`AdvOut`) for reliable startup.
- Output settings loader now accepts stream-key URL input and maps `server` to signaling host when using stream service compatibility mode.
- Compatibility service server label now explains custom signaling is overridden via stream-key URL.
- Tools menu action label renamed to `Configure VDO.Ninja` for clearer user intent.
- Stream service metadata now includes direct docs links for the Stream page helper buttons.

### Fixed
- Added `apply_encoder_settings` for VDO.Ninja service to enforce WebRTC-safe encoder options (`bf=0`, `repeat_headers=true`).
- Removed unsupported VP8/VP9 publish selection from output/service properties to avoid mismatched SDP/packetization behavior.

## [1.1.9] - 2026-02-22

### Fixed
- OBS 31 service registration now includes the required `get_protocol` callback for `vdoninja_service`, resolving:
  - `Required value 'get_protocol' for 'vdoninja_service' not found. obs_register_service failed.`
- Added explicit service metadata callbacks for codec support and connect info (`get_supported_video_codecs`, `get_supported_audio_codecs`, `get_connect_info`) to improve compatibility with OBS service handling.

## [1.1.8] - 2026-02-22

### Fixed
- Windows CI guard now validates `CMAKE_PREFIX_PATH` instead of relying on a strict `LibDataChannel_DIR:PATH=` cache key match, avoiding false negatives across runner/CMake variations.
- Keeps the static-linking verification flow intact so the build proceeds to the runtime import gate (`datachannel.dll`/OpenSSL dynamic import checks).

## [1.1.7] - 2026-02-22

### Fixed
- Windows CI workflow now generates a local `LibDataChannelConfig.cmake` backed by `datachannel-static.lib` instead of patching upstream install exports (which broke configure on transitive export dependencies).
- Fixed malformed YAML in the Windows build workflow that prevented GitHub Actions from parsing/running the job.
- Added a Windows CI guard that verifies `CMAKE_PREFIX_PATH` includes `local-libdatachannel`, preventing accidental omission of the local static package path.

## [1.1.6] - 2026-02-22

### Fixed
- Windows CI now patches upstream libdatachannel install exports to include `datachannel-static`, builds that target explicitly, and points CMake at the patched package config.
- Windows plugin linking now requires a static-safe libdatachannel target:
  - prefers `LibDataChannel::LibDataChannelStatic` when available
  - falls back to `LibDataChannel::LibDataChannel` only when it advertises `RTC_STATIC`
  - fails configure otherwise (prevents accidental `datachannel.dll` runtime imports)
- This resolves the failed import gate:
  - `Unexpected dynamic runtime imports in obs-vdoninja.dll: datachannel.dll`

## [1.1.5] - 2026-02-22

### Fixed
- Windows release build now uses static libdatachannel/OpenSSL linkage (`x64-windows-static-md`) to avoid runtime dependency conflicts with OBS-bundled `datachannel.dll`.
- Added a Windows CI import gate that fails the build if `obs-vdoninja.dll` imports `datachannel.dll`, `libcrypto-3-x64.dll`, or `libssl-3-x64.dll`.
- This addresses OBS startup failures where logs showed:
  - `Module '../../obs-plugins/64bit/obs-vdoninja.dll' not loaded`
  - `Service 'vdoninja_service' not found`

### Changed
- Signaling parser now handles request/type field case variants more robustly (for example `OfferSDP`, `videoAddedToRoom`, and `type: "Offer"`).
- Added protocol tests for mixed-case request/type variants.
- Expanded `INSTALL.md` troubleshooting guidance for Windows module-load failures.

## [1.1.4] - 2026-02-22

### Fixed
- Windows release packaging now bundles required runtime DLLs with the plugin:
  - `datachannel.dll`
  - `libcrypto-3-x64.dll` (when available from vcpkg)
  - `libssl-3-x64.dll` and `zlib1.dll` (when available from vcpkg)
- This addresses `Module '../../obs-plugins/64bit/obs-vdoninja.dll' not loaded` on clean installs where dependencies were not resolvable.

### Changed
- Version markers updated to `1.1.4` in plugin/build/package metadata.

## [1.1.3] - 2026-02-22

### Added
- Windows-friendly launcher wrappers included in release packages:
  - `install.cmd` (calls `install.ps1` with process-local execution-policy bypass)
  - `uninstall.cmd` (calls `uninstall.ps1` with process-local execution-policy bypass)
- Release package UX improvements:
  - `install.ps1` / `install.sh` and new uninstall scripts (`uninstall.ps1` / `uninstall.sh`) included in artifacts
  - Expanded `INSTALL.md` with install, update, uninstall, and portable-path guidance
- `TESTING_OBS_MANUAL.md` for real OBS validation matrix
- `SECURITY_AND_TRUST.md` for checksum/signing guidance
- Optional nightly live internet e2e workflow: `.github/workflows/live-e2e.yml`

### Changed
- README and GitHub Pages onboarding content now prioritizes first-time user install flow (`install.cmd` on Windows)
- Project/release/docs links updated to `steveseguin/ninja-obs-plugin`
- Plugin support URL metadata updated to the new repository issues URL
- Windows installer output now omits stale quickstart path text when `QUICKSTART.md` is not present
- Version bumped to `1.1.3`

### Tested
- Local package simulation on Windows:
  - `install.cmd` successfully installed plugin/data into a test OBS root
  - `uninstall.cmd -RemoveData` successfully removed plugin/data
- GitHub Actions on `main` after installer/docs changes:
  - `CI`, `Code Quality`, and `GitHub Pages` all passed

## [1.1.2] - 2026-02-21

### Added
- Package-level installation guide (`INSTALL.md`) included in release archives
- Package installer helpers included in release artifacts:
  - Windows: `install.ps1`
  - Linux/macOS: `install.sh`
- Additional locale files for common OBS user languages:
  - `de-DE`, `es-ES`, `fr-FR`, `it-IT`, `ja-JP`, `ko-KR`, `nl-NL`, `pl-PL`, `pt-BR`, `ru-RU`, `tr-TR`, `zh-CN`
- Firefox receive/playback verification script for Chromium publisher -> Firefox viewer:
  - `scripts/playwright-vdo-firefox-view-check.cjs`

### Changed
- Release packaging workflow now bundles `INSTALL.md` and platform install helper scripts
- E2E scenario builder now supports room/no-password/bitrate matrices with room-safe view semantics (`room + scene + view`)
- README and GitHub Pages content rewritten for clearer product purpose, value proposition, and usage guidance
- Version bumped to `1.1.2`

### Tested
- E2E matrix:
  - Default password/no-room: `npm test` (3 passed)
  - Room + password + high bitrate: reload and multi-view specs passed
  - No-password mode: `npm test` with `VDO_NO_PASSWORD=1` (3 passed)
  - Firefox viewer checks passed (room/password/high bitrate and no-password scenarios)

## [1.1.1] - 2026-02-21

### Added
- Data-channel WHEP URL extraction for nested payload variants (`whepSettings`, `whepScreenSettings`, `info`)
- Initial VDO.Ninja-style `msg.info` data-channel message from publisher to viewers
- Multi-viewer Playwright e2e spec validating one publisher feeding multiple viewers with active media
- Licensing and release compliance docs:
  - `THIRD_PARTY_LICENSES.md`
  - `RELEASE_COMPLIANCE.md`

### Changed
- Viewer limit enforcement now counts pending publisher peers (`New`/`Connecting`/`Connected`), preventing burst over-admission
- Release packaging workflow now includes `LICENSE`, `THIRD_PARTY_LICENSES.md`, and `RELEASE_COMPLIANCE.md` in artifacts
- Metadata/license consistency updates to `AGPL-3.0-only` across build and package files
- Version bumped to `1.1.1`

### Tested
- Unit tests: `ctest --test-dir build-verify --output-on-failure` (290 passed, 0 failed)
- Plugin build: `cmake --build build-plugin -j 8` (Windows DLL built successfully)
- E2E (Playwright):
  - `vdoninja-publish-view-reload.spec.js` passed (playback survives reload with active media)
  - `vdoninja-multi-viewers.spec.js` passed (two simultaneous viewers with active tracks/bytes)
  - Combined run: `2 passed`

## [1.1.0] - 2026-02-20

### Added
- Normalized VDO.Ninja signaling parser for `description`, legacy SDP, offer requests, candidate bundles, listings, and WHEP URL hints
- Auto-inbound scene management to create/update/remove OBS Browser Sources from room events and data channel hints
- Deterministic grid layout helper for auto-managed inbound scenes
- New unit tests for signaling protocol parsing and layout generation

### Changed
- Signaling client now supports explicit offer request handling (`offerSDP` / `sendOffer`) and richer offer/answer envelopes
- Peer manager now responds to offer requests and supports RTP packetizer paths (H264/Opus) with RTCP helpers
- Output lifecycle and encoder/capture start checks improved for safer start/stop behavior
- Build now allows OpenSSL to be optional and includes an internal SHA-256 implementation for testability

### Tested
- Unit tests: `ctest --test-dir build-verify -C Debug --output-on-failure` (276 passed, 0 failed)
- Plugin build: `cmake --build build-plugin --config RelWithDebInfo --target obs-vdoninja`
- Endpoint reachability checks:
  - `https://vdo.ninja/?view=CoatdevdavER`
  - `https://vdo.ninja/?push=u8F2kVV`
  - `https://vdo.ninja/?push=u8F2kVV&room=test123252345`

## [1.0.2] - 2026-02-14

### Added
- AGPLv3 license
- GitHub Actions CI/CD workflows for Linux, Windows, and macOS
- Automated release workflow with artifact publishing
- Unit test suite using Google Test
  - Tests for utility functions (UUID, SHA256, base64, JSON)
  - Tests for data channel message handling
  - Tests for tally state management
- Installer scripts for all platforms
  - `scripts/install-linux.sh`
  - `scripts/install-windows.ps1`
  - `scripts/install-macos.sh`

### Changed
- Updated README with comprehensive installation and testing instructions
- Updated CMakeLists.txt with `BUILD_TESTS` option

## [1.0.0] - 2024-XX-XX

### Added
- Initial release
- VDO.Ninja output for publishing streams from OBS
- VDO.Ninja source for viewing streams in OBS
- Support for H.264, VP8, VP9 video codecs
- Opus audio codec support
- Room-based streaming with password protection
- Data channel support for:
  - Tally light signaling
  - Chat messages
  - Keyframe requests
  - Custom data exchange
- Automatic reconnection on connection loss
- Configurable bitrate and quality settings
- Multi-viewer P2P connections (up to 10 concurrent viewers)
- ICE candidate bundling for faster connections
- TURN server support with force option

### Technical
- WebSocket signaling via `wss://wss.vdo.ninja`
- SHA256-based stream/room ID hashing (VDO.Ninja SDK compatible)
- Built on libdatachannel for WebRTC support
- Cross-platform support (Linux, Windows, macOS)
