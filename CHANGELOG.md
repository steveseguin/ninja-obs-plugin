# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Fixed
- Bundle the Linux libdatachannel runtime privately and remove build-machine library search paths from release packages.
- Remove Linux plugin copies from multiarch and legacy system directories during uninstall, including the private runtime.
- Reject incompatible native OBS versions, Snap installs, and unresolved runtime dependencies before installing the Linux package.
- Restored native reception from browser publishers that negotiate a separate media peer over an existing data channel, while retaining stale-session checks.
- Prevented recurring low-bitrate NVENC keyframe freezes by retaining a 4 Mbps minimum RTP pacing rate without changing the encoder bitrate or shared packet-burst limit.
- Repaired VP9 alpha publisher test headers, optional alpha-plane writes, and RTP SSRC signaling, and prevented duplicate OBS module loading in the source stress harness.

### Added
- Run the Linux release build on main and pull requests, with isolated install/uninstall regression tests and an extracted-package ELF loading and OBS API compatibility gate.

## [1.1.66] - 2026-09-05

### Fixed
- Fixed long-running RTP timestamp conversion and large alpha-plane scaling arithmetic.
- Bounded retransmission-cache storage when sequence numbers are reused, preserved expiration with out-of-order timestamps, and capped NACK expansion.
- Corrected ICE candidate expiration, bitrate recovery cooldowns, shared pacer accounting, and compound REMB telemetry.
- Hardened JSON escaping and parsing, signaling-result reuse, malformed audio RED/VP9 handling, and module cleanup failure detection.
- Made password hashing and URL encoding independent of locale settings, and preserved supported VDO.Ninja viewer aliases and fragment-based playback links.
- Corrected SDP media-section parsing and RTX associations, and prevented invalid playback-hint aliases from hiding supported URLs.
- Added focused regression coverage for the corrected behavior.

## [1.1.65] - 2026-08-09

### Added
- Added release-linked Windows gates for the real statically linked native-media and module-lifecycle boundaries, including owner-lifetime, unload/reload, PE architecture, dependency, and OBS/FFmpeg ABI checks.
- Added a packet-loss protection reference covering automatic NACK repair, paced packet duplication, Audio RED, adaptive bitrate, native-receiver limitations, diagnostics, and per-viewer bandwidth costs.

### Changed
- Strengthened native peer, track, data-channel, signaling, and VP9 alpha ownership across replacement, reconnect, teardown, and delayed-callback races.
- Replaced broad OBS 32.2+ compatibility wording with an exact platform/package matrix for OBS 32.2.x and the Windows-only OBS 32.0.x-32.1.x legacy builds.

### Fixed
- Made the VDO.Ninja Studio dock vertically scrollable so docking it no longer forces OBS beyond the monitor work area and every control remains reachable in a shorter window.
- Coordinated process-global libdatachannel logger and cleanup state with live OBS source, output, and control-center instances so unload and reload cannot race active native objects.
- Made the documented `vdoninja-tests` build target include the separately registered module-lifecycle executable, preventing clean Linux and sanitizer CTest runs from referencing an unbuilt test.
- Made VP9 alpha pairing generation- and timestamp-safe across track changes, late frames, duplicate frames, RTP timestamp wraparound, and mismatched dimensions.

## [1.1.64] - 2026-08-01

### Added
- Expanded sanitizer-backed fuzz coverage for H.264 payload parsing, audio RED construction, compound RTCP feedback, NACK parsing, and SDP normalization with 160,000 deterministic randomized cases.

### Fixed
- Restored plugin loading on OBS 32.2 after OBS moved its bundled runtime from FFmpeg 7 to FFmpeg 8. Current release packages now build against OBS 32.2.0, while Windows also ships clearly labeled OBS 32.0-32.1 legacy artifacts.
- Added release-time Windows import validation for the expected FFmpeg DLL family so an OBS runtime mismatch fails packaging instead of reaching users.

## [1.1.63] - 2026-07-28

### Added
- Extended the BrowserStack publishing probe with decoded/presented-frame telemetry, optional pixel-marker analysis, explicit lightweight-observer overrides, playback-clock and zero-audio-concealment gates, and native iPhone Safari WebDriver support that distinguishes visible playback from transport-only decoding.
- Added browser-side publishing controls for custom signaling hosts, repeatable media-sequence looping, raw audio continuity, and remote viewer validation without extending the local measurement window while a cloud device allocates.

### Fixed
- Added bounded RTP packetization/keyframe-burst headroom to High video protection and included that repair allowance in the aggregate pacer rate. Under deterministic 0.5% TURN-path loss, every scheduled copy now meets its 250 ms deadline instead of roughly 2% expiring.
- Made decoded PCM the authoritative audio continuity gate when Opus RED is active, while retaining an opt-in zero-concealment/statistics gate; receivers discarding redundant RED blocks no longer create a false audio failure when the decoded waveform is intact.
- Made BrowserStack probe failures fail closed even when diagnostic screenshot collection also fails.

## [1.1.62] - 2026-07-28

### Added
- Added decoded-pixel publishing validation with deterministic per-frame markers, browser presentation-cadence telemetry, multi-viewer encoder checks, forced-TURN candidate proof, and a deterministic UDP loss proxy for repeatable macOS stress testing.

### Fixed
- Prevented VideoToolbox bitrate overshoot at very low configured rates from accumulating seconds of RTP-pacer latency. The pacer now retains a 2 Mbps minimum drain rate while continuing to smooth bursts; a 1080p60/500 kbps high-motion regression improved from 43.98 decoded FPS and 2.1 seconds of queue delay to 59.88 FPS and 39 ms without changing the encoded bitrate.

## [1.1.61] - 2026-07-27

### Added
- Added strict browser-side publishing continuity analysis for sustained frame rate, sub-second stalls, inter-frame variance, RTP jitter, decoder drops, packet loss, and freezes, with focused tests that catch recovered-average and modest sustained-cadence failures.

### Fixed
- Disabled VideoToolbox's encoder-specific B-frame setting in addition to the x264/NVENC setting. Hardware-encoded Mac streams no longer deliver every frame but decode only around periodic keyframes, which appeared as a roughly two-second freeze-and-jump cycle in browser viewers.
- Restored the libdatachannel WebRTC-compatible H.264 SDP offer while retaining live encoder-profile diagnostics. Advertising the encoder's High profile prevented some VDO.Ninja browser viewers from completing publisher negotiation.
- Pinned local release formatting verification to clang-format 14, matching CI, and added a WSL fallback on Windows so a newer Visual Studio formatter cannot approve a tree that the required CI formatter rejects.

## [1.1.60] - 2026-07-26

### Added
- Added per-peer RTCP diagnostics for NACK, cache hits/misses, paced retransmissions, PLI/FIR, receiver loss, jitter, RTT, malformed feedback, and REMB, and correlated them with keyframe and pacer measurements in the rolling `Publish:` summary.
- Added negotiated, opt-in RFC 2198 audio RED carrying one previous Opus frame. Plain Opus remains the default, and viewers that do not select the offered RED mapping fall back per peer without losing audio.
- Added default-off `Low`, `Medium`, and `High` paced video packet-duplication modes. Copies are delayed, bandwidth-limited, subordinate to live media, and allowed to expire; the UI identifies this as duplication rather than RED/ULPFEC and states its possible extra upload.
- Added default-off REMB-driven adaptive bitrate for encoders advertising dynamic-bitrate support, with a configured floor, minimum-fresh-estimate-across-all-viewers policy, staged decreases, conservative increases, cooldowns, and original-bitrate restoration.
- Added controlled pacer, recovery-gate, RTCP, retransmission-cache, RED-negotiation, H.264 profile, bitrate-controller, and protection-policy tests, plus portable-OBS gates for real Browser Source playback, native-viewer REMB, protection traffic, and audio fallback.
- Added an optional BrowserStack real-browser/real-device viewer probe to the portable-OBS publishing harness. It records negotiated codecs, selected ICE candidate types, playback progress, WebRTC loss/repair/FEC/freeze counters, and whether requested BrowserStack network settings measurably reached the media path.

### Changed
- Replaced the 10x video sender drain rate with frame-aware 2 ms token-bucket pacing at twice the encoder bitrate and a shared 4 KB aggregate burst allowance across viewers. Large IDRs are packet-spaced while ordinary frames retain bounded latency.
- Replaced the direct 512-packet video NACK responder with a two-second, 2048-packet/4 MiB history and deadline-limited retransmissions routed through the common pacer with a separate repair allowance.
- Derived the offered H.264 `profile-level-id` from encoder SPS data, with a resolution/frame-rate fallback until live SPS data is available.

### Fixed
- Made decoder recovery generation-safe: cached keyframes are only for initial paint, a complete live keyframe opens the gate, known local frame loss suppresses dependent deltas, and a PLI reuses an already pending live keyframe without deleting its valid GOP.
- Prevented adaptive bitrate decreases from turning pre-change encoder output into seconds of pacer backlog. Encoder reductions are staged and existing pacers retain their prior drain rate briefly before settling; the 8 Mbps to 720 kbps real-viewer regression now stays bounded without freezes, drops, or transport errors.
- Prevented recovery purges from removing a partially transmitted RTP frame or racing a frame selected while waiting on the shared multi-viewer pacing budget.
- Reclaimed RTP sequence numbers assigned to unsent tail frames when PLI or local-loss recovery purges the pacer. The next live keyframe no longer exposes artificial sequence gaps that receivers NACK even though those packets never reached the retransmission cache.
- Kept live deltas flowing after an established viewer sends PLI while waiting for the next bounded-interval IDR. PLI alone no longer closes a healthy sender gate and creates a guaranteed freeze; known local frame loss and transport failure still suppress dependent deltas until a complete live keyframe.
- Prevented optional duplicate packets from taking pacing slots while any primary video frame is queued. Protection copies now use only true idle capacity and expire when none is available, so `High` protection cannot repeatedly add a frame of latency during large keyframes.

## [1.1.59] - 2026-07-26

### Added
- Extended the rolling publish summary with encoded-audio RTP cadence, media-queue peak and audio delay, audio queue drops, successful packet sends, and transport send errors.
- Added deterministic published-audio continuity tests: byte-exact Opus RTP coverage, timestamp/send-result accounting, PCM click/dropout detection, and a portable-OBS soak that validates raw decoded Chromium audio alongside Chrome WebRTC counters and an optional local recording.

### Fixed
- Moved rolling publish-summary collection and OBS logfile writes off libobs's serialized encoded A/V callback. OBS synchronously flushes each log line, so a slow disk or logfile lock could pause both audio and video packet intake every 30 seconds.
- Counted libdatachannel `Track::send()` false returns as transport failures for audio and paced video instead of silently reporting those rejected packets as successfully sent.
- Prevented long portable-OBS validation reports from deadlocking on a full redirected-output pipe, and excluded deliberate second-viewer startup/screenshot work from steady-state freeze counts.

## [1.1.58] - 2026-07-25

### Added
- Extended the rolling publish summary with RTP-pacer batch size, peak queue, packet delay, dropped-frame, and send-error diagnostics.
- Added a high-complexity 1080p60 portable-OBS smoke mode that verifies large keyframes are split, checks Chrome's freeze/loss/recovery counters, and confirms playback advances in an actual OBS Browser Source.

### Fixed
- Paced each viewer's H.264 RTP packets in bounded 5 ms batches instead of pushing every fragment of a large keyframe back-to-back on the shared audio/video send path. Audio can now pass between video batches, and an overloaded pacer drops a complete encoded frame rather than corrupting a GOP with a partial send.
- Kept initial cached-keyframe priming separate from decoder recovery state. RTCP PLI previously re-enabled the v1.1.57 cache guard immediately before the cache was checked, so synchronized viewers could still receive the stale keyframe that the guard was meant to reject.
- Made portable-OBS validation synchronize and hash-check every DLL discovery path so an older installation copy cannot silently register the plugin before the build under test.

## [1.1.57] - 2026-07-25

### Added
- The OBS log now carries a one-line publish summary every 30 seconds while streaming, plus a final one when the stream stops: encoded frame rate, video and audio bitrate, measured keyframe interval, average and largest keyframe size and how many times larger the largest is than an average frame, viewer count, send-queue depth, dropped frames, and how many keyframes viewers asked for. Diagnosing the stutter fixed below required inferring most of that from indirect evidence, because nothing in the log stated the encoder's real cadence or what viewers were asking for.
- Repeated viewer keyframe requests no longer get a log line each. A viewer on a lossy link asks about once a second for as long as the loss lasts, which buried the rest of the log; the first request is still logged with its peer id and transport, and the rest are counted into the publish summary.
- The publisher now warns in the OBS log when the encoder is emitting keyframes far less often than VDO.Ninja publishing can tolerate, naming the measured interval and the setting to change. The clamp below normally prevents this, but OBS's advanced "ignore streaming service setting recommendations" option bypasses it, and until now that left no trace in the log.
- Added byte-level fuzz coverage for the VP9 RTP payload descriptor parser, which consumes untrusted packet bytes from remote peers and previously had none. It sweeps all 256 mandatory descriptor bytes against every truncation length, drives scalability-structure lengths (`N_S`, `N_G`, `R`) to their maximums, and checks random payloads, asserting the invariant both callers depend on: a valid result never reports a `payloadOffset` past the end, which would underflow `payloadSize - payloadOffset` into a huge value.
- Added a CI job that runs the unit tests under AddressSanitizer and UndefinedBehaviorSanitizer. The suite already contained fuzzers for data-channel and signaling message parsing, but with no sanitizer in CI they could only catch hard crashes, not out-of-bounds reads or undefined behavior.

### Fixed
- Fixed a periodic freeze of both audio and video on published streams, reported as a stutter every 5-10 seconds. The VDO.Ninja service now clamps the stream encoder's keyframe interval to 2 seconds, honoring a tighter value if one is set. OBS leaves the interval at the encoder's own default of 250 frames — 8.3 seconds at 30fps — whenever "Keyframe Interval" is 0/auto, which is the default for both x264 and NVENC. That matters more here than for RTMP: libobs exposes no way to request an on-demand keyframe, so the GOP length is also the longest a viewer can stay broken after losing a packet, and each oversized keyframe arrives as a burst many times the per-frame bitrate budget. The `keyint` value recommended in the injected services catalog only ever applied to the RTMP-catalog entry, not to the plugin's own service, so nothing previously constrained it.
- Stopped replaying the cached keyframe at viewers that had already decoded one. A viewer recovery request (RTCP PLI, or the data-channel equivalent) re-sent a keyframe still carrying the RTP timestamp of the GOP it was captured in, which rewound that viewer's timestamp while the encoder's live frames kept advancing — and could not repair the decode anyway, because the delta frames that followed still referenced the encoder's real frame sequence. The result was a self-sustaining loop of requests, with one observed log showing a viewer asking for a keyframe roughly once a second for the better part of a minute. Viewers that have not yet decoded a keyframe are still primed immediately, so connect time is unchanged.
- Stopped a rewound RTP timestamp from forcing an RTCP sender report. The "is a sender report due" check subtracted timestamps as unsigned values, so a backwards timestamp wrapped to nearly 2^32 and always read as long overdue. That emitted a sender report pairing a rewound RTP timestamp with the current NTP time, corrupting the mapping viewers use to keep audio and video in sync — which is why the stall above affected both tracks rather than video alone.
- Stopped a tolerated OBS SDK install hiccup from failing the whole Windows release build. The step already continued past a non-fatal `cmake --install` failure once the required import libraries existed, but left `$LASTEXITCODE` non-zero; since every following command is a PowerShell cmdlet and GitHub appends `exit $LASTEXITCODE` to `pwsh` steps, the step still failed. Upstream OBS trips this intermittently via a `libobs-opengl.dll` install race, which is what broke the v1.1.56 Windows job.
- Made GitHub release titles consistent. The workflow titled new releases "Release v1.2.3" while all 41 earlier releases used the bare tag, so the releases page appeared to use two naming schemes; it now always titles a release with its tag, and the existing outliers were renamed to match.
- Guaranteed a tagged build never leaves a release sitting as a draft. The publish step now explicitly clears the draft flag after every asset is uploaded, and marks non-prerelease builds as the latest release.

## [1.1.56] - 2026-07-24

### Fixed
- Moved the bundled macOS CA-certificate store from `Contents/MacOS` into `Contents/Resources/data`, where the plugin already looks for it. `codesign` treats every file in a bundle's `MacOS` directory as a nested code object, so the stray `.pem` failed bundle signing with "code object is not signed at all" and blocked the 1.1.55 macOS package.
- Fixed `install.sh` failing with "Could not find plugin binaries in package." on Debian/Ubuntu. The Linux tarball places the plugin under `lib/x86_64-linux-gnu/obs-plugins` (multiarch `CMAKE_INSTALL_LIBDIR`), which the installer did not probe; it now also handles `lib64`, multiarch, and portable layouts, and reports the paths it searched when it genuinely finds nothing. Root installs now prefer an `obs-plugins` directory OBS already scans instead of hardcoding `/usr/lib/obs-plugins`. ([#15](https://github.com/steveseguin/ninja-obs-plugin/issues/15))

### Added
- macOS installer validation now fails on any non-code file left in `Contents/MacOS` and reports whether the CA bundle shipped, so the signing regression above cannot recur silently.

## [1.1.55] - 2026-07-25

### Fixed
- Shipped a CA-certificate bundle inside the macOS plugin and pointed the signaling TLS client at it — honoring an `SSL_CERT_FILE` override and falling back to common system trust stores — so WebSocket signaling no longer fails with "TLS connection failed" on Macs that lack a Homebrew OpenSSL trust store.

## [1.1.54] - 2026-07-19

### Fixed
- Prevented Windows PowerShell from treating normal Git push progress on stderr as a release-script failure.
- Retried GitHub release assets independently so one failed upload cannot invalidate binaries that already succeeded.

## [1.1.53] - 2026-07-19

### Fixed
- Made VirusTotal submission advisory and added explicit GitHub CLI retries plus idempotent asset uploads so transient external-service failures cannot block release publication.

## [1.1.52] - 2026-07-19

### Added
- Added bounded, session-aware buffering for remote ICE candidates that arrive before peer creation or remote SDP, including expiry and global memory limits.
- Added automatic inbound room-listing reconciliation with a removal grace period and custom signaling-server propagation for auto-created sources.

### Changed
- Increased the native viewer target bitrate ceiling for full-resolution streams and kept established media active while signaling reconnects.
- Required every GitHub macOS release build to be Developer ID signed, notarized, stapled, and validated before artifacts are uploaded.

### Fixed
- Rebuilt publisher peer connections for ICE restart requests while preserving UUID, session, RTP sequence/timestamp, and media-send state, and isolated candidate bundles by peer generation to prevent stale signaling races.
- Forwarded RTCP keyframe requests to the output keyframe cache and made sender-report timing use the correct audio/video RTP clock rates.
- Tightened VP9 alpha-frame matching and scaling so delayed or differently sized alpha tracks do not reuse future or unrelated masks.
- Refused plaintext signaling fallback when encrypted SDP or ICE candidate generation fails.
- Serialized MSVC program-database writes for parallel unit-test builds to prevent intermittent `C1041` release-verification failures.

## [1.1.51] - 2026-07-06

### Added
- Added portable OBS chaos stress and seeded replay harnesses covering source mutation, scene churn, duplicate sources, OBS restart, publisher outage/recovery, publisher track/data-channel churn, and crash dump capture.
- Added Game Capture Spout2 VP9-alpha smoke validation, including deterministic Spout sender support, live VTube Studio window churn, OBS alpha-composite pixel checks, and local-control diagnostics capture.
- Documented the Game Capture Spout2 to VP9 alpha to OBS native receiver transparency workflow in README, Quick Start, hosted docs, and in-app source guidance.

### Fixed
- Hardened native receiver reconnect handling so media-less transport peers no longer cancel view retry before accepted video, alpha, or audio media arrives.
- Recreated native viewer peers when a publisher rotates session IDs or an existing peer is terminal/retired, preventing stale peers from blocking recovered offers.
- Deferred replacement video, alpha, and audio tracks while another peer owns active media, then adopted the deferred tracks after the old peer disconnects.
- Ignored extra inbound audio tracks for the same native receiver peer instead of replacing the active audio stream unexpectedly.

## [1.1.50] - 2026-07-01

### Added
- Added a VDO.Ninja dock System CPU readout that samples total host CPU usage and color-codes elevated load.

### Fixed
- Hardened System CPU sampling against invalid OS counter deltas and added focused unit coverage.

## [1.1.49] - 2026-06-30

### Added
- Added an AI-friendly VDO.Ninja workflow map covering signaling, data-channel, native receiver, peer cleanup, retry, media-track, and validation flows.
- Added focused fuzz-style regression coverage for VDO.Ninja signaling/data-channel parsing and JSON array handling.

### Fixed
- Aligned native VDO.Ninja signaling/data-channel handling with official message shapes for ping/pong, cleanup, ICE restart, stats requests, keyframe requests, media controls, recovery controls, and targeted director video state.
- Fixed JSON array parsing so scalar and nested-array entries cannot stall parser consumers.
- Fixed native receiver handling for remote audio mute/unmute and remote video suppression state, including targeted director mute and virtual hangup.
- Hardened publisher/viewer data-channel teardown, stats subscription cleanup, peer cleanup, and malformed message handling to avoid callback-thread crashes and stale state.

## [1.1.48] - 2026-06-28

### Fixed
- Fixed a VDO.Ninja publishing crash/unclean shutdown when duplicate `offerSDP` requests arrived for the same viewer while the peer connection was still negotiating.

## [1.1.47] - 2026-06-10

### Fixed
- Removed committed local Claude session state and ignored generated Claude, build, install, portable OBS, and local libdatachannel output directories.

## [1.1.46] - 2026-06-10

### Added
- Added portable OBS publish and source smoke-test wrappers that verify VDO.Ninja output/viewer connectivity and OBS mixer audio activity.

### Fixed
- Avoided teardown deadlocks and callback-thread crashes by making OBS UI tasks fire-and-forget, serializing VDO.Ninja output start/stop, and deferring WebRTC peer cleanup out of RTC callbacks.
- Hardened signaling and peer negotiation paths so invalid remote descriptions or ICE candidates are logged instead of escaping callback threads.
- Bounded native receiver decode timestamp queues and pending viewer signaling channels to avoid unbounded growth during stalled or abandoned sessions.

## [1.1.45] - 2026-06-08

### Fixed
- Reduced OBS freezes during VDO.Ninja publishing by moving RTP packetization and libdatachannel media sends off the OBS encoder callback thread.
- Hardened libdatachannel, WebSocket, and native receive callbacks so unexpected exceptions are logged instead of escaping callback threads.
- Fixed VDO.Ninja Studio dock cleanup when registration fails or the plugin unloads.

## [1.1.44] - 2026-06-06

### Fixed
- Fixed a Windows OBS crash after long-running VDO.Ninja publish sessions when a viewer lost ICE connectivity. Peer connections are now retired from libdatachannel state callbacks and cleaned up later from normal plugin paths, so `Disconnected`/`Failed`/`Closed` callbacks no longer destroy RTC objects re-entrantly.

## [1.1.43] - 2026-05-03

### Fixed
- Avoid recreating the internal Browser Source for VDO.Ninja Source on ordinary property updates, reducing UI-thread churn when opening or saving source properties during active publish/view workflows.

## [1.1.42] - 2026-05-02

### Fixed
- Fixed the Windows release workflow to continue staging the plugin SDK when OBS 32.0.4 builds the required import libraries but its optional `cmake --install` payload fails while installing unrelated OBS runtime components.

## [1.1.41] - 2026-05-02

### Fixed
- Fixed Windows release builds using OBS-deps `rtc/` headers instead of the libdatachannel headers that match the linked static library. This corrupted `rtc::Configuration` fields and could produce invalid viewer SDP such as huge `a=max-message-size` values, leaving VDO.Ninja view links blank.

## [1.1.40] - 2026-04-17

### Added
- VP9 native receiver support: plugin now negotiates VP9 (preferred) with H.264 fallback when receiving streams via the native receiver path.
- RFC 9628 VP9 RTP payload descriptor parser (`vdoninja-rtp-utils`) with full unit test coverage (B/E frame reassembly, PictureID 7-bit and 15-bit, layer indices, flexible mode P_DIFFs, scalability structure).
- VP9 hardware decode via D3D11VA/DXVA2 with `extra_hw_frames` surface pool workaround (FFmpeg trac #10608); software fallback via `libvpx-vp9`.
- VP9 alpha channel support via dual-track convention: when the publisher sends a second VP9 video m-line, the plugin treats it as the alpha track. The Y-plane of the alpha stream carries 8-bit alpha values; the native receiver combines the primary YUV planes with the alpha Y-plane into YUVA420P before `sws_scale`, producing correct BGRA transparency in OBS.
- Alpha VP9 decode always runs in software (`libvpx-vp9`) so the Y-plane is directly accessible; the primary stream continues to use hardware acceleration.
- `TrackType::AlphaVideo` added to peer-manager; second VP9 video m-line in the remote offer is accepted and routed through the dedicated alpha decode path.
- 6 additional unit tests covering alpha-stream VP9 RTP descriptor scenarios (single-packet frame, first/last fragment, 15-bit PID, Z-flag, scalability structure with V=1).
- `vp9-alpha-publisher` test tool (`tests/tools/vp9-alpha-publisher/`): standalone executable that connects to VDO.Ninja signaling as a publisher, sends two VP9 video tracks (primary YUV + alpha-as-Y), and generates an animated test pattern with variable transparency. Build with `-DBUILD_PUBLISHER_TOOL=ON`. Useful for verifying the native receiver's alpha decode path end-to-end.
- Playwright e2e test for VP9 publish-to-native-receiver path (`npm run test:e2e:vp9`).

## [1.1.39] - 2026-03-22

### Added
- Structured release automation via `scripts/release.ps1` to align version files, promote `CHANGELOG.md`, generate release notes, run format/tests, and optionally commit/tag/push a release.

### Fixed
- Signaling fallback recovery no longer wedges after an initial pre-open signaling failure. The signaling thread now clears its stale run state on exit, and the pre-open error path actively advances fallback instead of waiting passively for a close callback.
- Fixed JSON parser silently corrupting nested objects/arrays when string values contained braces or brackets (e.g. in SDP descriptions), which could cause intermittent WebRTC connection failures.
- Fixed `stopViewing()` deadlock where `peersMutex_` was held while destroying peer connections, which could trigger re-entrant state-change callbacks.
- Fixed `disconnect()` deadlock where WebSocket `close()` was called under `handleMutex_`, but synchronous close callbacks also tried to acquire it.
- Fixed use-after-free risk in WebSocket `onOpen` callback that captured a stack variable (`reconnectAttempts`) by reference.
- Fixed output reference leak in Control Center status updates that prevented proper cleanup on stream stop.
- Fixed `std::isspace()` undefined behavior on non-ASCII input in JSON parser.
- Fixed data races on RTP timestamp state during output stop and on keyframe request timing in the native source receiver.

### Docs
- Documented the validated Windows OBS 32.x build recipe, DLL provenance gotchas, portable-OBS test flow, fallback fault-injection test, and the limits of proving phone-side `srflx` from OBS logs alone.

### Changed
- Tagged GitHub release builds now fail fast if the tag, source/package version files, and changelog entry do not match.

## [1.1.36] - 2026-03-13

### Fixed
- Special-character passwords (containing `$`, `#`, `@`, `&`, `+`, `[`, etc.) now produce the same room/stream hashes as VDO.Ninja's browser JS. Previously, our C++ used the raw password for SHA-256 hashing while VDO.Ninja runs `encodeURIComponent()` first, causing the two sides to land in different signaling rooms and never see each other.
- AES encryption/decryption of SDP and ICE candidates now uses the URI-encoded password to match VDO.Ninja's key derivation, fixing encrypted-session failures with special-character passwords.

## [1.1.35] - 2026-03-12

### Fixed
- Normalized formatting in `vdoninja-output.cpp` and `vdoninja-utils.h` so the release branch passes the enforced `clang-format` check again.

## [1.1.34] - 2026-03-12

### Fixed
- Avoided wrapper-child no-op updates that were forcing unnecessary reconnect churn in `VDO.Ninja Source`, which could make native sources feel sticky while they were connecting or retrying.
- Added an OBS websocket movement benchmark harness to compare source move responsiveness across baseline, browser-backed, and native loading states.

## [1.1.33] - 2026-03-11

### Fixed
- Release builds now use a single explicit libdatachannel clone/build path on macOS and Windows, instead of assuming an untracked vendored checkout is present in CI.
- Release CI is pinned back to the `v0.20.2` libdatachannel baseline that matches the locally validated native-receiver build path, restoring green Linux, macOS, and Windows packaging jobs.

## [1.1.32] - 2026-03-11

### Fixed
- macOS release builds now install libdatachannel as a static library for tagged packaging, avoiding the unresolved-symbol link failure from the shared dylib export surface.
- Windows release builds now install the PowerShell Core version required by vcpkg before dependency resolution, avoiding transient GitHub download failures during tagged packaging.

## [1.1.31] - 2026-03-11

### Fixed
- macOS and Windows release builds now resolve FFmpeg development headers and libraries from the correct OBS dependency roots explicitly, avoiding Qt-only prefixes and missing FFmpeg detection during tagged packaging.

## [1.1.30] - 2026-03-11

### Fixed
- macOS release builds now cache the OBS-bundled FFmpeg include and library paths explicitly during plugin configure, resolving the remaining tagged-build failure after the native source packaging changes.

## [1.1.29] - 2026-03-11

### Fixed
- GitHub release builds now use `libdatachannel v0.24.0` across Linux, macOS, and Windows so CI matches the native receiver headers used by the plugin.
- macOS release builds now pass the OBS-bundled FFmpeg development prefix through to plugin configure, restoring tagged release packaging after the native source work.

## [1.1.28] - 2026-03-11

### Added
- **VDO.Ninja Studio Dock**: A new persistent Qt-based dockable panel for OBS (Qt6 preferred, Qt5 fallback).
  - Dedicated fields for Stream ID, Room ID, and Password (no more piped stream keys).
  - One-click "Go Live" independent of the primary stream button.
  - "Zero-config" experience with automatic secure Stream ID generation.
  - Quick-action buttons to copy View and Push links.
  - Real-time live status, uptime, and viewer telemetry directly in the dock.
- Independent output management allowing VDO.Ninja to function as a surgical multi-stream destination.

### Fixed
- **RTMP/WHIP Compatibility**: Fixed bugs where VDO.Ninja would conflict with other streaming services.
  - Removed aggressive profile-wide overrides that forced the Opus encoder on all services.
  - Service settings are now applied surgically only when the VDO.Ninja output is active.
- Streaming start reliability now enforces Opus on the active stream output audio encoder(s) before encoder init.
- Pre-start validation failures now set a clear last-error without duplicate stop/error loops.
- `VDO.Ninja Source` now defaults to a browser-backed viewer path, while `Use Native Receiver (Experimental)` switches to an explicit opt-in native H.264/Opus receive path.
- Hardened source/output async teardown, malformed JSON parsing, and exception boundaries to reduce silent OBS crashes during shutdown and callback races.
- Browser-backed `VDO.Ninja Source` now reliably syncs lifecycle state to its internal child source, preventing blank default-mode renders in OBS smoke tests and real use.

### Added (Installer)
- Windows GUI installer (`obs-vdoninja-windows-x64-setup.exe`) via Inno Setup, with:
  - OBS install path detection
  - optional launch OBS / open Quick Start on finish
  - Add/Remove Programs uninstall support
- New build helper script: `scripts/build-installer-windows.ps1`.

### Changed
- Release builds now target OBS `32.0.4` instead of OBS `32.1.0-rc1`, so shipped plugin binaries load on current stable OBS `32.x` installs without forcing users to pick version-specific downloads.
- Stream destination UX now avoids the misleading `Get Stream Key` helper button for VDO.Ninja.
- VDO.Ninja server label in OBS Stream settings now explicitly points users to `Tools -> VDO.Ninja Control Center`.
- README/QUICKSTART now clarify that OBS still shows a `Stream Key` field for compatibility.
- Added a new `Tools -> VDO.Ninja Control Center` workflow with one-place publish config, start/stop controls, generated links, and runtime peer telemetry.
- Windows release docs now route users to the setup `.exe` first, with ZIP install scripts as fallback.
- Release workflow now publishes Windows ZIP + setup `.exe`, and release checksums include `.exe` artifacts.
- Inno Setup uninstaller files now live under `data/obs-plugins/obs-vdoninja/_installer` instead of the OBS root.
- Windows installer/script post-install actions now open the GitHub Pages quick-start guide instead of a local Markdown file.
- In-app Stream Service helper links now point to the GitHub Pages guide/home docs.
- Tools menu flow is simplified around `VDO.Ninja Control Center` to reduce duplicate publish/control paths.
- Custom ICE server fields are now compact and consistently documented across Service/Output/Source/Control Center.
- Custom ICE parsing now accepts semicolon-separated entries, and runtime warns when `Force TURN` is enabled without TURN servers.
- Local build artifact hygiene improved (`install/`, setup `.exe`, `_obs-portable/`, temp dirs ignored by git).

### Fixed
- Plugin runtime version banner now correctly reports `1.1.15` (`PLUGIN_VERSION` macro alignment).

## [1.1.27] - 2026-03-08

### Fixed
- Browser-source auto-add now wraps direct playback/WHEP endpoints in a VDO.Ninja page URL using `?whepplay=...`, so OBS Browser Sources still load VDO.Ninja instead of being pointed at raw playback endpoints.
- Pseudo WHEP inputs like `whep://host/path` are normalized into `https://...` endpoints before generating the VDO.Ninja browser URL.

## [1.1.26] - 2026-03-08

### Fixed
- Browser-source auto-add now only accepts VDO.Ninja viewer pages or stream IDs it can convert into `https://vdo.ninja/?view=...` links. Direct playback/WHEP-style URLs are ignored instead of being attached to OBS Browser Sources.
- Auto-inbound scene management now skips unsupported browser-source targets instead of creating sources with invalid URLs.

## [1.1.25] - 2026-03-08

### Fixed
- Auto-added OBS browser sources now always get a full `https://vdo.ninja/?view=...` viewer URL. `whep:<streamId>` inputs no longer collapse to a bare stream ID, and room/solo/password parameters are preserved when configured.

## [1.1.24] - 2026-03-07

### Changed
- Release builds now target OBS `32.0.4` as the OBS 32.x compatibility baseline, keeping the default Windows installer/ZIP compatible with current stable OBS 32.x installs.
- README/INSTALL/release text now explicitly state the OBS `32.0.4` build baseline so users know which OBS line the shipped artifacts target.
- Repository agent guidance now documents the "build against the oldest supported OBS version in the compatibility band" rule for future release work.

## [1.1.23] - 2026-03-02

### Fixed
- Restored green multi-platform release CI by fixing macOS Qt framework header resolution during OBS SDK packaging in GitHub Actions.

## [1.1.22] - 2026-03-02

### Fixed
- Prevented OBS heap corruption crashes on publish stop/shutdown with active viewers by hardening peer teardown and callback shutdown handling.
- Outgoing audio/video RTP sends now use owning `rtc::binary` buffers to avoid transient send-buffer lifetime hazards under load.

## [1.1.21] - 2026-03-01

### Fixed
- Publisher video packetizer path now updates RTP timestamps on every frame before `track->send`, matching OBS `obs-webrtc` send semantics and preventing stalled/frozen playback with only intermittent keyframe refresh.
- Added runtime logging of whether each viewer connection uses libdatachannel H264 packetizer or manual RTP fallback.

## [1.1.20] - 2026-03-01

### Fixed
- Signaling request handling no longer treats `joinroom` as an offer-generation trigger, preventing unnecessary renegotiation loops during active publish sessions.
- Duplicate offer requests for already-active peers are now ignored to avoid repeatedly re-entering keyframe-gated startup behavior (a common cause of stalled/stepwise video playback while audio continues).

## [1.1.19] - 2026-03-01

### Fixed
- OBS publish video now uses explicit H264 RTP packetization (single NAL + FU-A fragmentation) with deterministic marker bits and sequencing.
- Video RTP timestamps now derive from encoder packet timebase (`pts * timebase_num / timebase_den * 90000`) with a fallback path when timebase is unavailable.
- Video transport no longer depends on libdatachannel H264 packetizer behavior, reducing frozen-frame/stalled-playback cases.

## [1.1.18] - 2026-03-01

### Fixed
- Audio publishing now uses explicit manual RTP packetization for OBS Opus frames instead of the libdatachannel Opus packetizer path, improving compatibility with real OBS capture streams.
- Audio RTP timestamps now derive from encoder packet timebase (`pts * timebase_num / timebase_den * 48000`) with a fallback path when timebase is unavailable.

### Changed
- End-to-end Playwright coverage now explicitly requires inbound audio bytes in all publish/view scenarios to catch silent-audio regressions.

## [1.1.17] - 2026-03-01

### Fixed
- Windows release workflow now treats self-signed/private-CA trust warnings during `signtool verify` as non-fatal when Authenticode signature + signer thumbprint checks pass.
- This unblocks signed installer artifact publication in GitHub releases for private certificates.

## [1.1.16] - 2026-03-01

### Fixed
- Publishing now forwards only one selected OBS audio track into the WebRTC output path.
- Added startup track selection (prefers profile stream track, falls back to first available output audio encoder).
- Non-selected OBS audio track packets are explicitly dropped to prevent multi-track packet mixing on a single WebRTC audio sender.

### Tested
- Plugin build: `cmake --build build-plugin --config Release`
- Unit tests: `ctest --test-dir build --output-on-failure` (306 passed, 0 failed)
- E2E web tests: `npm test` (3 Playwright scenarios passed)
- Portable OBS live validation: OBS publish to VDO.Ninja + Playwright viewer stats confirmed inbound audio/video bytes and packets.

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
