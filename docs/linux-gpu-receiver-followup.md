# Linux GPU and receiver timing follow-up

Testing started from `c0141a9` on 2026-09-05. The cold-start production failure
remains open. These results distinguish real OBS/browser measurements from the
standalone upstream timing experiment.

## Environment

- Ubuntu 24.04.4, kernel `6.17.0-1032-oem`, Intel Core Ultra 5 235, 16 GB RAM.
- Intel Arrow Lake graphics, `/dev/dri/renderD128`, Mesa 25.2.8, Intel iHD 24.1.0.
  OBS logged `Intel Mesa Intel(R) Graphics (ARL)` as its OpenGL adapter.
- Official OBS 32.2.2 Ubuntu package; plugin compiled against the official
  **32.2.0** SDK. No release compatibility baseline was changed.
- Runtime module verified through `/proc/PID/maps` at
  `/home/steve/obs-testing/config/obs-studio/plugins/obs-vdoninja/bin/64bit/obs-vdoninja.so`.
  libdatachannel was built from `v0.20.2`.
- Chromium 145.0.7632.6 and Firefox 146.0.1 from the locked Playwright install.
  Firefox H.264 capability and negotiated H.264 were checked after installing
  Cisco OpenH264 2.6.0 using Mozilla's GMP update manifest and SHA-512 checksum.
- VDO.Ninja viewer: `https://vdo.ninja/alpha/`. Local API source checkout:
  `/home/steve/code/obsninja`, commit `6b018f2822a77f2c2e505b63e12533dde12c4733`.
- Authenticated OBS WebSocket on port 4466 in an isolated `XDG_CONFIG_HOME`.
  Only the disposable coturn container's interface received `netem` changes;
  no host interface was impaired.

Quick Sync was advertised by OBS but failed to initialize, both before and after
installing Ubuntu's `libmfx-gen1.2` 23.2.3. Those attempts are not hardware-encode
passes. VAAPI H.264 worked: OBS logged the actual encoder, render device, CQP/CBR
configuration and dimensions during recording/publishing. Driver capabilities
alone were not treated as proof of encoding.

## Receiver cold-start reproduction

The matched 720p30 trials used marked MP4 input, x264 at a fixed 3 Mbps, a 300 ms
requested viewer buffer, 20 seconds of stabilization and a 30-second measured
interval. The plugin logged its unchanged 6 Mbps video pacer, 1 KB per 2 ms batch
and 4 KB shared aggregate burst. Reordering was `delay 25ms 8ms distribution normal`;
the loss cases additionally specified `loss 0.5%`.

| Condition | Decoded marker result | Presentation result |
| --- | --- | --- |
| Chromium, clean | 900 valid, no omissions | Pass |
| Chromium, cold reordering 1 | 900 valid, no omissions | 17 omitted frames |
| Chromium, cold reordering 2 | 901 valid, no omissions | 25 omitted frames |
| Chromium, clean startup then reordering | 900 valid, no omissions | Pass; maximum cadence deviation 0.47 ms |
| Firefox H.264, clean | Probe unavailable | Pass |
| Firefox H.264, cold reordering | Probe unavailable | Fail; maximum cadence deviation 59.83 ms |
| Chromium, reordering + 0.5% loss | One skipped marker | 82 omissions; two freezes totaling 0.426 s |
| Firefox H.264, reordering + 0.5% loss | Probe unavailable | Fail; one reported freeze, 0.191 s |
| Chromium, cold reordering, 20 ms NACK-delay experiment | 900 valid, no omissions | Pass |
| Three Chromium viewers, clean | 2,701 main / 600 each additional, all consecutive | All pass |
| Three Chromium viewers, cold reordering | 2,700 main / 600 each additional, all consecutive | 69 main / 11 and 21 additional omissions |

The primary multiple-viewer measurement lasted 90 seconds; each independently
measured additional viewer ran for 20 seconds within that interval. The additional
viewers used the same private relay, not unconfigured OBS Browser Sources. An
earlier OBS Browser Source attempt failed during readiness and was excluded from
the measured multiple-viewer comparison.

In the first cold-start trace, **all 1,541 received frames had a NACK**. Median
encoded-ready-to-composition time was 11 ms; sender-report mapping drift peaked
at 2.58 ms. The warm-start control retained a median 246 ms before composition.
These reproduce the earlier symptom on a hardware-rendered OBS host.

The 20 ms NACK-delay diagnostic kept sender bitrate, pacing, quality and loss
recovery enabled. It produced 1,187 non-NACKed frames out of 1,538 received frames
and a median 287 ms encoded-ready-to-composition delay. This supports the timing
initialization explanation without changing the sender's bandwidth policy.

Use `VDONINJA_CHROMIUM_FIELD_TRIALS='WebRTC-SendNackDelayMs/20/'` with the existing
publisher harness to repeat the diagnostic. Reports include browser version,
field-trial configuration and negotiated codec. **50 ms is invalid** at this
WebRTC revision: `GetSendNackDelay` accepts only 1–20 ms and silently uses zero
otherwise. The initial 50 ms trial was therefore an additional default-behavior
control, not evidence against delayed NACK feedback.

This field trial is a browser launch setting, not a plugin-side mitigation. The
plugin cannot configure remote Chromium's NACK timer through the current WebRTC
signaling path. No sender bitrate, pacer budget, buffer default or adaptive mode
was changed. Larger viewer buffers and existing adaptive mode remain unsuccessful
controls from the [earlier investigation](linux-receiver-timing-isolation.md).

## Direct upstream timing experiment

The [standalone probe](../tests/webrtc-timing-probe/README.md) compiles WebRTC's
actual `VCMTiming`, `TimestampExtrapolator` and dependencies from Chromium's pinned
revision. Instrumentation counts incoming timing samples and queries that reach
the uninitialized extrapolator branch. It explicitly models the controller's
retransmission exclusion; it does **not** compile the full controller or browser.

The baseline reproduces immediate rendering for every frame when all incoming
timing samples are excluded. The experimental variant initializes an RTP/time
anchor once and then applies the existing clamped playout delay. Each variant
passed its expected assertions in 24 scenarios of 1,800 frames: cold/warm startup,
300/1000 ms minimum delay, RTP wraparound and repeated resets. With a 300 ms target
and alternating arrival jitter, the candidate retained 284–301 ms lead and
32–34 ms render cadence. The baseline cold case remained at zero added delay.

This directly establishes the timing component's behavior. It is not an
instrumented Chromium run or proof that a rebuilt browser fixes every observed
failure. The candidate still needs full receiver integration, clock drift,
outage/discontinuity, decoder and A/V synchronization tests. Firefox's failure
must not be assigned the identical internal cause based on these tests.

## GPU-backed detailed 1080p60

Marked `testsrc2` footage was generated for 180 seconds at 1920×1080/60, including
the counter/complement marker and a 997 Hz audio tone. Preview remained enabled.

| Control | Result |
| --- | --- |
| Direct OBS VAAPI recording, CQP 18, 30 s | 1,799 consecutive decoded markers; zero render/output skips |
| Plugin VAAPI publishing, CBR 12 Mbps, detailed footage, 30 s | 1,801 consecutive markers; presentation passes |
| Same, OBS-clocked fixture, 30 s | 1,800 consecutive markers; presentation passes |
| Detailed footage repeated, 30 s | 1,800 consecutive markers; presentation passes |
| Detailed footage, 120 s | 7,199 consecutive markers, zero OBS skips; presentation fails with 11 omissions |
| Tracing-OFF release build, detailed footage, 120 s | 7,200 consecutive markers, zero OBS skips; no presentation omissions, but cadence and audio gates fail |
| Tracing-OFF build, clock source, three viewers with reloads | Main: 7,200 consecutive markers; each additional viewer: 1,200 before and 1,200 after reload; all presentation gates pass |
| Tracing-OFF build, detailed footage, synchronized probe stop, 120 s | All strict gates pass: 7,200 consecutive markers, no presentation omissions, no OBS skips, raw PCM and Web Audio continuity pass |

The 120-second run averaged 1.58 ms OBS render time and had no reported decoded
frame drops or freezes. Chromium's maximum presentation cadence deviation was
16.73 ms against the unchanged 12.5 ms threshold. Short passes do not establish
sustained frame-perfect presentation. Missed JavaScript callbacks are reported
separately from compositor omissions.

The repeated 30-second detailed run captured audio but did not require its audio
gate. Its overall video pass therefore does not establish audio continuity:
raw decoded PCM contained one discontinuity near the end of capture. The
120-second native packet trace overflowed its bounded diagnostic storage and
must not be used for complete native timing analysis. Its separately bounded
pixel/presentation reports remain available.

The separately built tracing-OFF module was verified in the process map and by
matching its SHA-256 to the build output. Its detailed run again exceeded the
12.5 ms cadence limit (16.73 ms maximum), despite zero actual presentation
omissions. Browser stats did not provide decoder implementation or power-efficiency
information; these tests prove OBS hardware rendering/encoding, not Chromium
hardware decoding. The clean reload control used the simpler clock fixture and
does not replace detailed-footage stress validation or Firefox reload coverage.

After the capture-boundary fix, the final detailed 120-second run passed with
both audio and video gates explicitly required. It captured 5,760,480 raw audio
samples at 48 kHz, with zero detected dropouts, clicks or non-forward timestamps;
the independent Web Audio analysis also passed. One 12.325 ms raw timestamp step
was reported without a PCM discontinuity. This is a successful sustained control,
not proof that the earlier intermittent presentation failures are universally
eliminated or that the default impaired cold-start path is repaired.

## Measurement fixes and reproduction

- Recorded-frame extraction now uses FFmpeg `-fps_mode passthrough`. The original
  720p recording contained 900 distinct frames, but default CFR extraction added
  a duplicate because video started after audio. A real FFmpeg regression covers
  that offset and verifies that actual duplicate/skipped markers still fail.
- Direct recording controls fit the source to the requested canvas. The fixed
  1080p clock fixture was otherwise cropped at 720p, hiding its marker strip.
- Publisher reports create their output directory before setup can fail. Early
  setup errors no longer disappear behind an `ENOENT` while writing the report.
- The live-progress check has a fresh timeout after deliberate stabilization and
  screenshots. Stream cleanup checks actual output shutdown before restoring
  video settings. None of these changes relax frame or timing thresholds.
- Both continuity probes now freeze before large video/timing records are
  exported. Previously, audio kept recording during that export; the tracing-OFF
  120-second run included PCM discontinuities at 120.01 and 120.46 seconds, while
  the harness was already collecting artifacts. A regression verifies that both
  producers stop and pending reads finish before export can proceed. This fixes
  the measurement boundary without discarding samples inside the requested soak
  or changing audio thresholds.
- `VDONINJA_EXPECT_VIDEO_CODEC=video/H264` makes a mismatched negotiation fail.
  Reports also retain decoder implementation/power-efficiency fields when the
  browser provides them, rather than assuming browser hardware decoding.

Use the [previous reproduction commands](linux-receiver-timing-isolation.md)
with this machine's isolated runtime and fixture paths. For additional measured
viewers, run `scripts/browser-extra-viewers.cjs VIEW_URL OUTPUT_DIRECTORY` while
the publisher remains active. Supply `VDONINJA_VIEWER_ICE_SERVERS_JSON` for the
same private relay. Its two default additional Chromium viewers each collect
independent decoded-marker, presentation and WebRTC-stat samples. Keep the main
publisher running long enough for their connection, 20-second stabilization and
20-second measurement. `EXTRA_VIEWER_COUNT`, `EXTRA_VIEWER_FPS` and
`EXTRA_VIEWER_DURATION_MS` adjust the explicit test configuration.
Set `EXTRA_VIEWER_RELOADS=1` to reload both viewers and repeat their readiness,
stabilization and measurement phases; keep the publisher active for both rounds.

## Local checks

The 728 main C++ tests passed; the locale-dependent test was rerun successfully
after installing its locale. The real-libdatachannel diagnostic executable passed
all three cases, including overflow leaving media delivery unchanged. The
JavaScript measurement suite passed 48 tests, packet/timing Python checks passed
nine tests, and both upstream timing executables passed their 24 scenarios.
Tracing-ON and tracing-OFF plugin builds succeeded. No installer or packaging
behavior was changed.

Generated recordings, raw reports and packet captures live under the ignored
`artifacts/gpu-linux/` directory; setup/runtime logs live under
`/home/steve/obs-testing/logs/`. No new release was published.

## Subsequent controller and browser validation

See [the receiver controller follow-up](linux-receiver-controller-validation.md)
for the one-time bootstrap candidate's rejected cached-keyframe/drift behavior,
repeated Chromium/Firefox impairment and reload trials, native Firefox timing
logs, and a reproduction in installed Chrome 151.
