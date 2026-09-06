# macOS local release recheck — September 5, 2026

Tested the installed v1.1.67 release on the M1 MacBook Air (8 GB, macOS 26.4.1),
with official OBS 32.2.2. The running process loaded the installed plugin bundle.
These are additional short local checks, not a replacement for the earlier
[two-hour campaign](macos-publishing-validation.md). No plugin media behavior,
quality defaults, protection defaults, or GameCapture features were changed.

## Method

- Marked 1080p60 file, hardware VideoToolbox H.264 CBR at **8 Mbps**, 45 seconds
  per case. High video protection and Opus RED except the unprotected comparison.
- Private local coturn with forced relay, verified using Chromium's selected
  candidate pair. UDP and TCP allocations were exercised. The selected browser
  candidate was `relay`; its `relayProtocol` was respectively `udp` or `tcp`.
  The remote candidate appeared as `prflx`, so browser statistics alone do not
  independently identify the publisher's selected local candidate type.
- UDP proxy loss: every 200th datagram in each direction after a seven-second
  warmup. Reordering: deterministic per-datagram 15–40 ms delay in each direction,
  active from connection setup. Proxy counters establish actual traffic and drops.
  This is a local relay test, not a WAN, cellular, or restrictive-firewall test.
- Textured browser scene: 720p30, actual hardware encoder still **8 Mbps**.
  The harness's 4 Mbps request only changed `SimpleOutput/VBitrate`, not the
  active Advanced Output encoder. OBS encoder and publisher logs establish the
  actual rate. The harness now explicitly says that its bitrate setting affects
  Simple Output only; Advanced Output must be configured separately before testing. Encoder expectations
  now also reject the wrong Output Mode before publishing.
- ScreenCaptureKit window capture: moving marked fixture, scaled to 1080p60;
  separate application-only ScreenCaptureKit capture of a continuous 997 Hz tone.
  Native OBS (640×360 output), OBS Browser Source, and Chromium viewers ran together.
  Three 60-second cases: no synthetic load, three CPU workers at 70% duty plus
  384 MiB allocation and moderate GPU load, then recovery. Screen frame-marker
  uniqueness and compositor cadence were not measured in these three cases;
  receiver statistics, raw audio, OBS performance, and screenshots were checked.
- x264 comparison uses the same textured scene at 720p30 and an independently
  configured **4 Mbps CBR**, veryfast preset, two-second keyframes. Compare normal
  buffer with an explicit 400 kbit / 100 ms buffer; no persistent defaults change.

## Results

“Gate” combines the configured decoded-video, audio, concealment, and applicable
source-marker requirements. It does **not** imply that the separate Web Audio or
compositor presentation checks passed. Zero drops with nonzero freezes is possible:
frames eventually decode, but their delivery is uneven.

| Case | Gate | Decoded fps | Drops | Freeze seconds | Max RTP jitter ms | Raw audio | Concealed samples |
|---|---|---:|---:|---:|---:|---|---:|
| direct | pass | 59.92 | 0 | 0 | 8 | pass | 0 |
| direct-recovery | pass | 59.95 | 0 | 0 | 8 | pass | 0 |
| large-direct | fail | 30.02 | 0 | 8.557 | 23 | pass | 0 |
| large-turn | fail | 29.98 | 0 | 6.303 | 23 | pass | 0 |
| large-turn-loss | fail | 29.75 | 0 | 12.264 | 30 | pass | 0 |
| screen-audio | pass | 59.98 | 0 | 0 | 8 | pass | 0 |
| screen-audio-load | pass | 59.96 | 0 | 0 | 10 | pass | 0 |
| screen-audio-recovery | pass | 59.98 | 0 | 0 | 9 | pass | 0 |
| turn-jitter-delay20 | fail | 59.90 | 0 | 0 | 11 | pass | 0 |
| turn-tcp | pass | 59.96 | 0 | 0 | 8 | pass | 0 |
| turn-clean | pass | 60.00 | 0 | 0 | 8 | pass | 0 |
| turn-jitter | fail | 60.01 | 0 | 0 | 12 | pass | 0 |
| turn-loss | pass | 60.00 | 0 | 0 | 8 | pass | 0 |
| turn-loss-jitter | fail | 59.94 | 1 | 0 | 12 | pass | 241 |
| turn-no-protection | fail | 59.99 | 0 | 0 | 12 | pass | 6211 |
| 100ms-buffer | pass | 29.98 | 0 | 0 | 15 | pass | 0 |
| default-buffer | fail | 29.92 | 0 | 6.757 | 23 | pass | 0 |

## Findings and limits

Clean direct, TURN UDP, TURN TCP, and loss-only marked-video tests passed their
configured gates. Reordering still damages presentation even when every marked
frame decodes correctly: the jitter-only run decoded 60.01 fps with zero marker
errors, but presented only 49.07 fps. OBS had no render or encoder skips. Its
reported two lost RTP packets do not establish two permanently missing frames;
the source-frame sequence remained complete. Combined loss/reordering presented
43.08 fps and dropped one decoded frame.

A single diagnostic repeat with `WebRTC-SendNackDelayMs/20/` improved jitter-only
presentation to 57.22 fps but did not pass the strict checks. This was not a
balanced causal comparison, and it is not a deployable plugin fix: it changes
browser launch behavior. See [receiver timing investigation](linux-gpu-receiver-followup.md).

Large hardware keyframes reached 706 KB and took up to approximately 356 ms to
send. The direct case had 23 freezes totaling 8.557 seconds despite zero decoded
frame drops, zero OBS render/encoder skips, and continuous raw decoded audio.
Clean TURN and impaired TURN also froze. The bounded pacer and repair deadlines
were preserved; increasing burst rates or silently reducing picture quality is
not a validated general fix.

The confirmed x264 comparison reproduced 6.757 seconds of freezes with the
normal 4,000 kbit buffer. A 400 kbit / 100 ms buffer reduced maximum keyframes
from 314 KB to 48 KB and maximum frame-send time from 316 ms to 46 ms. It passed
the configured video and raw-audio gates with zero freezes, dropped frames, or
concealment. This repeats the earlier mitigation; it remains an explicit quality
versus latency choice, not a new default. Web Audio still reported click candidates.

The three short screen/audio checks supplement the earlier long stress test.
Do not interpret a short pass as proof of sustained game-stream performance:
the earlier two-hour campaign failed under prolonged heavy local load. In this
short baseline and loaded pair, OBS-reported memory stayed approximately
402–423 MiB; mean OBS process CPU rose from 18.57% to 23.54%. Those are OBS process
metrics, not whole-system utilization. Loaded p95 frame-render time reached
16.33 ms, close to the 60 fps frame budget.

An initial x264 fixture attempt wrote profile keys with spaces before `=`; OBS
fell back to Simple Output. The old harness incorrectly accepted the Advanced
encoder expectation because it inspected the inactive configuration. Both runs
are retained under `x264/` and excluded from this table. The corrected fixture
uses OBS-compatible INI formatting; `x264-confirmation/` logs verify the active
Advanced x264 encoder and each buffer size. A real negative-mode repeat and four
focused regression tests validate the new harness guard.

Two separate 30-second browser-to-browser audio controls (Opus and Opus RED)
passed both raw PCM and Web Audio checks with zero concealment. This does not
prove that clicks in the heavier publishing tests are measurement artifacts.
Several Web Audio captures reported click candidates despite continuous decoded
PCM; therefore this campaign does not certify click-free speaker output or
acoustic A/V synchronization. Likewise, compositor cadence checks remained
stricter than the decoded-video gate. Windows GameCapture, Windows hardware
codecs, physical microphone capture, macOS 13, and remote viewers are not certified
by these tests. Windows validation remains a separate handoff.

Raw reports, WAVs, proxy counters, OBS logs, loaded paths, configuration backups,
and restoration checks are in the ignored `artifacts/obs-local-recheck-20260905/`
directory. No upstream OBS submissions were made.

## Final validation and cleanup

- 17 valid publishing cases completed; two misconfigured exploratory x264 runs
  were excluded, and a separate wrong-mode negative control failed before publishing
  as expected. Two browser-only audio controls also completed.
- The new mode guard has four focused tests, included in CI. The full JavaScript
  measurement suite passed 53/53 tests; the existing C++ suite passed 729/729.
  `node --check` and `git diff --check` passed.
- All ten original OBS configuration files were restored and compared byte for
  byte. OBS, TURN, fixture apps, and synthetic load processes were stopped.
  The installed v1.1.67 module hash matches its pre-test hash and its bundle
  passes strict/deep code-signature verification. No new release is needed for
  these test-harness and documentation changes.
