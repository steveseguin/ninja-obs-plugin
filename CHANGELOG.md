# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

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
