# Linux OBS validation — 2026-09-05

These are measured results, not a guarantee for every GPU, encoder, network, or OBS distribution. The test machine ran Ubuntu 24.04, Ryzen 5 5500 (6 cores/12 threads), 32 GB RAM, Xvfb and software llvmpipe/EGL rendering. OBS, Chromium and instrumentation shared this machine. Media was synthetic; signaling and browser code used `https://vdo.ninja/alpha/`.

The GUI was the official OBS 32.2.2 Ubuntu binary, with the packaged plugin loaded from `/tmp/ninja-system-package/lib/obs-plugins/obs-vdoninja.so`. Tests used an isolated OBS profile and authenticated OBS WebSocket. They created temporary scenes, changed video settings, streamed/recorded real media, collected browser `getStats()` and OBS `GetStats`, and restored settings afterward.

## Publishing and encoder/rendering measurements

The initial matrix used the plugin built before the subsequent RTP repair commits, at the `b284267` source baseline. Packaging-only changes through `cdc960b` did not change its streaming code. Bitrates are requested x264 video rates. FPS, jitter, drops and freezes are browser receiver measurements over the stabilized interval; OBS skipped-frame counters use interval deltas, excluding historical startup skips.

| Case | Soak | Bitrate | Decoded fps | Max reported jitter | Mean OBS render time | OBS render skips | Encoder/output skips |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 720p30 | 60 s | 3 Mbps | 30.00 | 17 ms | 9.05 ms | 0/1800 | 0 |
| 1080p30 + simultaneous native OBS receiver | 120 s | 6 Mbps | 29.99 | 22 ms | 18.79 ms | 13/3600 | 0 |
| 720p60 | 120 s | 6 Mbps | 59.99 | 10 ms | 12.63 ms | 47/7200 | 0 |
| 1080p60 | 120 s | 10 Mbps | 59.96 | 12 ms | 19.83 ms | 1096/7199 | 0 |

All four browser counter intervals had zero new reported video drops, packet loss and freezes. That does **not** prove unique, smoothly rendered frames: OBS can send duplicated content at the requested encoder cadence. The 1080p60 render time exceeds its 16.67 ms frame budget.

A deterministic binary frame-counter source made that distinction visible. Its offline source validation passed all 4500 frames. The 1080p60 live decoded-pixel test failed: 1765 decoded frames contained 512 duplicates and 548 skipped markers, with no invalid or backward markers. OBS reported 555 render skips while the pixel probe was active. A direct OBS recording of the same source, without VDO publishing or a browser viewer, still had 14 duplicate/skipped markers in 1798 recorded frames. This establishes some rendering/source-cadence loss outside the plugin; it does not attribute the entire streaming deficit to OBS. Shared software-rendering and probe load materially affect these results.

The rebuilt `21183cc` binary also passed a 30-second 720p30 decoded-counter retest: all 900 frames decoded, with zero invalid, duplicate, backward or skipped markers, zero OBS render/output skips, and zero new audio concealment. Browser presentation was weaker: `requestVideoFrameCallback` observed four presented-frame jumps. Thus the decoded media and OBS performance gates passed, while the separately reported presentation analysis failed on the headless desktop.

## Native reception and recovery

A Chromium H.264 publisher at 1280×720/30 fps fed the real native OBS source. The source harness passed moving-image and audio-activity checks. A 60-second OBS recording contained 1799 video frames; FFmpeg `freezedetect=n=-50dB:d=0.5` reported no freezes of at least 500 ms. OBS averaged 7.69 ms render time with zero interval render skips. Changing the native source to an offline stream ID and restoring the original ID recovered moving video, verified by two different PNG hashes.

Publisher samples while frames were advancing measured 5524 encoded frames over 184.318 aggregate active peer-seconds: 29.97 fps and 2.45 ms encoding time per frame. Chromium reported both `none` and `bandwidth` quality limitation states during the run. The synthetic browser audio reached full scale, so this audio-activity check is not a clean-audio quality claim. Use the separate PCM audio-continuity harness for that criterion.

Chrome's default fake camera supplied 20 fps and could not satisfy the alpha site's requested exact 30 fps camera constraint. `PUBLISH_FAKE_CAMERA_FPS=30` fixed the fixture. An initial black-image test correctly failed before this was corrected.

After rebuilding at `21183cc`, another 30-second native recording and stream-ID recovery test passed, with zero OBS render skips and no detected freezes of at least 500 ms. The native decoder negotiated 640×360 in this run. The installed binary SHA-256 was `89be81897949026dce050c0f26b2d93e99612d32892f82eb8d1a6125ab3fd0c6`; its actual loaded path was checked in the OBS process mappings.

## Controlled network impairment

A dedicated Docker TURN server, with no published host ports, carried both selected ICE candidates as `relay`. `tc netem` affected only that container's `eth0`, not the host interface. This is a local relay stress test, not cellular or public-WAN evidence. A single randomized run per setting is not a statistically controlled comparison.

| Binary / condition | Soak | Decoded fps | New video freezes | Audio concealed samples |
| --- | ---: | ---: | ---: | ---: |
| Initial binary, relay without impairment | 45 s | 29.98 | 0 | 654 |
| Initial binary, constant 25 ms delay + 0.5% loss | 45 s | 30.00 | 0 | 9052 |
| Initial binary, 25 ± 8 ms delay + 0.5% loss | 60 s | 25.95 | 11 | 14003 |
| Initial binary, variable delay/loss + High video protection, audio RED, 100 ms view buffer | 60 s | 27.58 | 72 | 0 |
| Updated `21183cc`, variable delay/loss | 60 s | 29.90 | 20 (4.132 s total) | 12802 |

Variable per-packet delay creates reordering as well as jitter. Sender logs showed thousands of NACK requests, repair queue expiry and roughly 500 ms maximum pacing delay, despite approximately 0.5% actual container packet drops. The updated binary includes stale-repair expiry and repeated-NACK prioritization from `32e7ff9`. Its retest had zero OBS rendering/output skips and much better average decoded FPS, but still **failed** video continuity (34.35 ms inter-frame standard deviation and 20 freezes). These results do not justify declaring impaired-network quality fixed or enabling protection defaults globally. Protection plus RED eliminated measured audio concealment in one earlier run but did not fix its video freezes.

Chrome's cumulative video `packetsLost` can decrease when late packets arrive; the updated jitter run had a -1 interval delta. This is not negative physical network loss. Prefer the complete delay/freeze/repair evidence over that single counter.

## Installer, compatibility and package validation

- Thirteen isolated installer/uninstaller regression tests pass, including both system prefixes, private dependencies, legacy copies, incompatible OBS versions, and a custom `XDG_CONFIG_HOME` containing spaces.
- A fresh Ubuntu 24.04 container installed the actual official OBS 32.2.2 `.deb`. Root and non-root archive install, reinstall and plugin initialization passed. Default uninstall preserved data; `--remove-data` removed it. The custom XDG user path loaded successfully in OBS.
- The Arch development recipe built against Arch OBS 32.2.2 and current system libdatachannel/FFmpeg. All nine linked media tests passed. `namcap` reported no errors; pacman install, reinstall, file checks, real OBS plugin initialization and removal passed. The recipe is ready for review at `packaging/arch`; it has **not** been submitted to public AUR.
- The same release package initializes and registers native sources against official OBS 32.2.0, 32.2.1 and 32.2.2 runtimes; 32.1.2 and 32.0.4 reject it as expected. Snap is explicitly unsupported by this native archive installer. Flatpak runtime packaging and physical desktop installation were not exercised here.
- On the pulled `21183cc` source, 718 C++ tests pass normally and with AddressSanitizer/UndefinedBehaviorSanitizer, and all nine OBS-linked native media tests pass. Performance/continuity measurement tests run in CI alongside the platform package jobs.
- The initial long-running GUI session completed module-global RTC cleanup and reported zero OBS memory leaks at shutdown. This is an OBS allocation report, not a whole-process leak proof.

## Reproducing the media tests

Use an isolated OBS profile, enable authenticated OBS WebSocket, and select x264. Install Node/Playwright dependencies as documented in the other publishing guides. Export `OBS_WEBSOCKET_URL` and `OBS_WEBSOCKET_PASSWORD` for that profile. Synthetic MP4 input must outlast startup plus measurement, or explicitly enable looping for ordinary motion footage (do not loop a frame counter).

```bash
VDONINJA_BASE_URL=https://vdo.ninja/alpha/ \
VDONINJA_SOURCE_MODE=media-sequence \
VDONINJA_MEDIA_SEQUENCE_PATH=/absolute/path/synthetic.mp4 \
VDONINJA_VIDEO_WIDTH=1280 VDONINJA_VIDEO_HEIGHT=720 \
VDONINJA_VIDEO_FPS_NUMERATOR=30 VDONINJA_VIDEO_FPS_DENOMINATOR=1 \
VDONINJA_VIDEO_BITRATE_KBPS=3000 \
VDONINJA_SOAK_MS=60000 VDONINJA_VIEWER_STABILIZE_MS=5000 \
VDONINJA_REQUIRE_STABLE_VIDEO=1 VDONINJA_REQUIRE_OBS_PERFORMANCE=1 \
VDONINJA_OUTPUT_DIR=artifacts/linux-e2e/retest \
node scripts/obs-websocket-vdoninja-publish-check.cjs UNIQUE_TEST_STREAM false
```

Add `VDONINJA_NATIVE_VIEWER=1` for a simultaneous native receiver. For a counter generated by `tests/tools/generate-publish-sequence-video.cjs`, add `VDONINJA_REQUIRE_VISUAL_SEQUENCE=1`; validate the input offline first with `tests/tools/analyze-publish-sequence-video.cjs`. The optional OBS performance gate rejects any render/output skips by default. Explicit skip budgets are available through `VDONINJA_MAX_RENDER_SKIPPED_FRAMES` and `VDONINJA_MAX_OUTPUT_SKIPPED_FRAMES`; reports always retain the actual counts.

For local TURN testing, set `VDONINJA_FORCE_TURN=1`, native `VDONINJA_ICE_SERVERS='turn:CONTAINER_IP:3478|USER|PASSWORD'`, and matching `VDONINJA_VIEWER_ICE_SERVERS_JSON` (an RTCIceServer array). Confirm relay candidates in the report. Apply `tc qdisc ... netem` only inside an isolated test container with `NET_ADMIN`, then remove the qdisc and stop the server. Measurement-only runs without strict flags may exit successfully despite a failed continuity analysis; inspect `videoContinuityAnalysis`, audio concealment and `obsPerformanceAnalysis` separately.

`PUBLISH_FAKE_CAMERA_FPS=30 PUBLISH_STATS_PATH=artifacts/publisher.jsonl` adds encoder telemetry to `scripts/playwright-vdo-publish-session.cjs`. `scripts/obs-linux-e2e-record.cjs file MEDIA_FILE` records a direct OBS control; `native STREAM_ID` records a native receiver and verifies recovery. Set `E2E_DURATION_MS`, `E2E_WIDTH`, `E2E_HEIGHT`, `E2E_FPS` and `E2E_OUTPUT_DIR` as needed. The recorder waits for asynchronous output shutdown before restoring video settings.

Raw screenshots, recordings and JSON reports remain in the local ignored `artifacts/linux-e2e/` directory. Hardware encoders, GPU-rendered high-frame-rate performance, physical audio devices, multi-hour sessions and public-network fault recovery remain unvalidated by this run.
