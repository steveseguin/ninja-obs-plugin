# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- No unreleased changes yet.

### Changed
- No unreleased changes yet.

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
