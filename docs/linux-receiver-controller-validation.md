# Receiver controller and browser follow-up

Continuation of [the GPU receiver investigation](linux-gpu-receiver-followup.md),
starting from `d58afb6`. Production quality, the 3 Mbps 720p30 encoder setting,
the 6 Mbps pacer and the 4 KB aggregate burst budget remain unchanged. No receiver
experiment is shipped in the OBS plugin.

## Findings at a glance

- Direct Chromium traces confirm the receiver timing starvation: every startup
  frame can require retransmission while zero timing samples initialize the clock.
- The continuous-sample diagnostic restores 720p30 buffering and calibrated A/V
  alignment; the one-time bootstrap fails cached-keyframe and clock-drift tests.
- Repeated cold starts preserve media, but the complete packet-loss, multiple-viewer
  and strict 1080p60 matrices still contain failures. No production fix is claimed.
- Detailed VAAPI tests verify Intel GPU encoding and, in explicit Chromium controls,
  Intel GPU presentation. Firefox passes clean 1080p60 within its stated coverage.
- No shipping sender, quality, pacing or packaging change was justified. All failed
  controls and missing coverage are retained below.

## Controller integration rejects the one-time anchor

The opt-in [upstream test target](../tests/webrtc-timing-probe/README.md) now builds
WebRTC's actual `VideoStreamBufferController`, `FrameBuffer`,
`TaskQueueFrameDecodeScheduler`, decode timing, jitter estimator and timing
component. Synthetic encoded frames override the received-time/retransmission
metadata. A deterministic task runner drives the real scheduling code; its global
clock override also keeps repeating timeout tasks on the same clock. The sink
records **release for decoding**, not decoded pixels or compositor presentation.
No codec or network implementation is exercised by this controller test.

The default controller itself excludes every incoming timing sample in the cold
case. The earlier driver no longer has to stand in for that filter. With a
requested 300 ms buffer, its render cadence follows arrival variation (up to
42 ms between frames). The bootstrap candidate produces 33–34 ms render cadence,
but fails other requirements:

| Scenario | Default | One-time bootstrap | All-arrivals diagnostic |
| --- | --- | --- | --- |
| Cached keyframe, then 2-second RTP jump | Maximum decode-release gap 42 ms | 2,323 ms gap; final render lead 2,293 ms | Maximum gap 331 ms; final lead 283 ms |
| Cached keyframe, then 12-second RTP jump | Maximum gap 42 ms | 12,323 ms gap; final lead 12,293 ms | Maximum gap 331 ms; final lead 296 ms |
| +1,000 ppm clock offset, 10 simulated minutes | Immediate rendering | Final render lead 893 ms | Final lead 296 ms |
| −1,000 ppm clock offset, 10 simulated minutes | Immediate rendering | Final render lead −307 ms | Final lead 296 ms |

Cached-to-live transitions use independent keyframes. The all-arrivals comparison
feeds each received timestamp into the original estimator; jitter estimation
still excludes retransmission-delayed samples. It has startup render-cadence
variation up to 41 ms and a 140 ms render-time step in the 2-second jump case.
Its successful characterization is not proof of frame-perfect browser playback.
All variants also exercise a two-second outage with advancing RTP time, timestamp
wrap and clean timing samples becoming available after 60 retransmitted frames.

The bootstrap characterization explicitly asserts the observed regressions.
Passing that test means the unsafe behavior is reproducible, not repaired. The
one-time anchor must not be promoted to a production receiver patch.

## Browser experiment setup

The diagnostic Chromium source is pinned to 145.0.7632.6, Chromium commit
`47e20adcc15fc15f01825aa17e570c8f5492ac0f`, with WebRTC
`b47e68e6966d5a5a0e4bc861ff364221600f31c3`. The
[browser patch generator](../tests/webrtc-timing-probe/browser-patch.py) checks the
WebRTC revision and exact source sites before editing the isolated checkout.
The independent `WebRTC-ReceiverTimingTrace/Enabled/` trial logs controller insertions, accepted incoming timing samples, uninitialized
render queries and computed render times. It does not contain the rejected
bootstrap implementation.

`WebRTC-RetransmittedTimingSamples/Enabled/` opts into the all-arrivals comparison.
`WebRTC-ForceTimingSampleExclusion/Enabled/` forces sample exclusion in the minimal
loopback experiment; combining both enables samples again. This second trial
**does not create NACKs or prove network loss recovery**. Real impaired OBS tests
are separate from this deliberate isolation of the receiver timing path.

The publisher harness accepts `VDONINJA_BROWSER_EXECUTABLE` for the compiled
browser and `VDONINJA_CHROMIUM_LOG_FILE` for an absolute diagnostic log path. The
latter also enables native A/V synchronizer logging. Reports retain the path,
browser version and field-trial string. Logs affect workload; diagnostic passes
need controls without instrumentation before any production claim.

[The minimal browser harness](../scripts/browser-minimal-receiver-timing.cjs)
connects two local peer connections, uses H.264 marked canvas frames and applies
24 ms encoded-frame delay on alternating frames. The video track generator sets exact 30 fps timestamps independently of JavaScript timer jitter. It requests 300 ms receiver
buffering and independently measures decoded markers and presentation after
20 seconds of stabilization. It has no OBS, relay or VDO.Ninja dependency.

## Firefox investigation

The Firefox 146.0.1 release source also has the retransmission sample exclusion
and the uninitialized immediate-render fallback:
[controller](https://github.com/mozilla-firefox/firefox/blob/FIREFOX_146_0_1_RELEASE/third_party/libwebrtc/video/video_stream_buffer_controller.cc),
[timing](https://github.com/mozilla-firefox/firefox/blob/FIREFOX_146_0_1_RELEASE/third_party/libwebrtc/modules/video_coding/timing/timing.cc).
This source correspondence does not establish the running timing object's state.
Firefox's [field-trial adapter](https://github.com/mozilla-firefox/firefox/blob/FIREFOX_146_0_1_RELEASE/dom/media/webrtc/jsapi/PeerConnectionCtx.h)
only exposes its specific hard-coded overrides; Chromium's command-line NACK
trial is not a supported Firefox configuration.

Using Mozilla's [documented WebRTC logging](https://firefox-source-docs.mozilla.org/contributing/debugging/debugging_webrtc_calls.html):

```sh
MOZ_LOG=timestamp,sync,webrtc_trace:5,MediaPipeline:4
MOZ_LOG_FILE=/absolute/path/firefox-receiver.log
```

Matched 720p30 H.264 warm/cold runs retained the same sender and buffer settings.
Warm startup followed by reordering passed presentation (5.19 ms maximum cadence
deviation) and Web Audio continuity. Cold startup failed presentation (62.39 ms)
while audio continuity passed. Native synchronization logs reported an internal
startup difference up to 645 ms cold versus 38 ms warm. Cold audio delay climbed
to 540 ms in a periodic sync report while the video delay estimate stayed at
300 ms. These are internal synchronization estimates, not a physical lip-sync
measurement and not direct observation of the extrapolator initialization flag.
Firefox still lacks the decoded-pixel and raw-track-audio probes used by Chromium.

## Measurement coverage

Additional viewers now support Firefox, explicit browser executables and Chromium
field trials, with unavailable decoded-pixel coverage reported explicitly. Reload
rounds preserve the configured browser/trial. Each measured viewer must verify
its relay and, when requested, negotiated codec.

The selected relay check now also honors `RTCTransportStats.selectedCandidatePairId`
when the candidate pair lacks optional `selected`/`nominated` flags. It still
requires a succeeded pair and relay candidate. Regression tests include a failed
pair referenced by the transport, which must not establish relay coverage. A
previous trial with absent relay evidence remains failed; its stored data cannot
retroactively prove which transport was selected.

## Repeated stock-browser results

All runs below used the tracing-OFF OBS plugin. The single-viewer trials measured
30 seconds after 20 seconds of stabilization. Multi-viewer primary measurements
lasted 120 seconds; each additional viewer measured 20 seconds before and 20 seconds
after a reload. Ordinary and deliberately impaired trials remain separate.

| Condition | Independent media checks | Overall strict result |
| --- | --- | --- |
| Chromium 20 ms NACK delay, cold reordering, three repeats | Pixels, presentation and both audio analyses pass in all three | Two fail reported packet loss (3 and 2); one lacks selected-relay evidence |
| Chromium 20 ms NACK delay, reordering + 0.5% loss, repeat 1 | Pixels, presentation and audio pass | Fail: one reported lost packet |
| Same, repeat 2 | Two missing media frames; presentation fails; audio passes | Fail |
| Same, repeat 3 | All media checks pass | Pass |
| Chromium 20 ms NACK delay, three viewers and reloads under reordering | Main and every additional round pass pixels/presentation; main audio passes | Main and initial additional rounds pass; reload rounds fail 4 and 2 reported lost packets |
| Firefox H.264, clean | Presentation and Web Audio pass | Pass; pixel/raw-track probes unavailable |
| Firefox H.264, cold reordering | Presentation fails, 63.73 ms maximum cadence deviation; audio passes | Fail; one reported lost packet too |
| Firefox H.264, reordering + 0.5% loss | Presentation fails, 231.55 ms maximum cadence deviation and one 0.265 s freeze; audio passes | Fail |
| Firefox, three viewers and reloads, clean | Main and all additional rounds pass presentation; main Web Audio passes | Pass within stated Firefox coverage |
| Firefox, three viewers and reloads, cold reordering | Main fails at 72.11 ms; all additional rounds fail at 58.53–73.31 ms; main audio passes | Fail |

The 20 ms launch-time diagnostic is therefore repeatably helpful for presentation,
but has not passed the complete impaired matrix reliably. No packet-loss allowance
or media threshold was increased. Existing adaptive mode and larger buffers were
not promoted as mitigations. The sender cannot configure the remote browser's
internal NACK timer through the current plugin signaling API.

The first minimal-loopback control used a canvas capture timer as its RTP clock.
It preserved 909 decoded markers but failed presentation with 3 missing/2 excess
media intervals. This exposed a source-clock confound. The harness now generates
explicit frame timestamps; the earlier artifact remains a failed control and is
not evidence about forced receiver sample exclusion.

## Installed Chrome 151 control

The machine's installed Google Chrome **151.0.7922.71** was also tested through
`VDONINJA_BROWSER_EXECUTABLE=/usr/bin/google-chrome`. Compilation was paused and
active compiler jobs drained before these measurements. The corrected minimal
loopback control passed pixels and presentation in both Chromium 145 and Chrome
151, with 900 consecutive markers in each 30-second interval.

Real OBS publishing at 720p30/3 Mbps passed all strict checks in Chrome 151 on a
clean connection. With reordering present from startup, all 900 decoded markers
still arrived, but presentation omitted 42 media frames and maximum cadence
deviation reached 66.57 ms. The packet-loss gate also failed (three reported lost
packets); both audio analyses passed. OBS reported zero render/output skips and
about 1.65 ms average render time. Median encoded-ready-to-composition delay was
284.90 ms clean versus 12.10 ms cold. This confirms the symptom is not confined to
the older pinned browser. The compiled-source experiment remains pinned to 145
for an exact comparison with the original controller/source investigation.

## A/V playout measurement

`browser-av-sync-capture.cjs` routes only its test browser to a dedicated
`ninja-av-*` PulseAudio null sink, records that sink's monitor with timestamped
PCM, and reads source frame markers from compositor callbacks. It checks that the
browser actually created an audio stream on that sink, verifies H.264 and relay
selection for network viewers, unloads the sink afterward, and records the normal
default sink before/after. It does not capture the desktop's default audio output.

The separate fixture retains the marked 720p30 video and substitutes a 100 ms
pulse every two seconds. Pulse frequency is `997 + 100 * pulseIndex` Hz, so each
pulse identifies its source frame (`60 * pulseIndex`). This is an alignment test;
the ordinary constant-tone audio continuity gate is inapplicable to this pulse
fixture. Ordinary streaming tests keep that gate enabled and unchanged.

`analyze-av-playout.py` matches pulse identifiers to presented frame markers using
FFmpeg PulseAudio packet timestamps and browser compositor timestamps. Before
network measurements, it requires at least ten matches and no more than 80 ms
absolute error; a passing local-file run supplies the calibration offset. Late
callbacks over 25 ms are excluded because their canvas image may belong to a newer
frame. Unit tests cover pulse identification under output gain and reject silence.

The initial stock Chromium calibration captured audio but no markers because the
new presentation-marker option still hit the old decoded-marker guard. That
capture failed alignment and was retained. After correcting the guard, a fresh
local-file run matched 15 pulses: median video-minus-audio offset **0.146 ms**,
maximum absolute offset **0.792 ms**. Both default-sink names were identical before
and after capture. Compilation was paused and drained during both controls.
This measures software playout at the virtual sink, not physical speaker/monitor
latency. Separate browser-specific calibration is required for the actual matrix.

To recreate a pulse-coded fixture from an existing 240-second marked 720p30
sequence (do not use an unmarked counter overlay):

```sh
ffmpeg -y -i counter720p30-long.mp4 -f lavfi \
  -i "aevalsrc='if(lt(mod(t,2),0.1),0.1*sin(2*PI*(997+100*floor(t/2))*t),0.003*sin(2*PI*997*t))':s=48000:d=240" \
  -map 0:v:0 -map 1:a:0 -c:v copy -c:a aac -b:a 160k -shortest avsync720.mp4
node scripts/browser-av-sync-capture.cjs file avsync720.mp4 artifacts/av-calibration
python3 scripts/analyze-av-playout.py artifacts/av-calibration
# While the isolated synthetic publisher is active, with the private relay JSON set:
node scripts/browser-av-sync-capture.cjs url "$VIEWER_URL" artifacts/av-network
python3 scripts/analyze-av-playout.py artifacts/av-network \
  --calibration artifacts/av-calibration/av-alignment.json
```

Use the same browser executable and output setup for calibration and network
capture. `ok` in `av-capture.json` means successful capture only; the separate
`av-alignment.json` determines the alignment result.

## Repository and loaded binary provenance

During this investigation, `main` advanced to `e499cf6`, including the separately
published v1.1.67 release and active-output-mode validation. Those commits were
fast-forwarded into this checkout; the CI test-list conflict retained both sets
of tests. The isolated profile uses `Output/Mode=Advanced`, and the new encoder
mode checks pass. No release was cut by this investigation.

The runtime comparisons deliberately retain the same tracing-OFF Linux plugin
binary from the preceding GPU validation, built against OBS 32.2.0:

- OBS executable: `/usr/local/bin/obs` (32.2.2).
- Loaded module, verified in `/proc/<isolated-obs-pid>/maps`:
  `/home/steve/obs-testing/config/obs-studio/plugins/obs-vdoninja/bin/64bit/obs-vdoninja.so`.
- SHA-256: `3c8d359b2bfaa88455a41986ab61d6e2feaa00e619543bfc37150d2a5587ac8e`.

These are receiver experiments against that identified binary, not a claim that
an installed Linux binary automatically changes when the repository version is
updated. The new upstream commits change the version header but no sender runtime
logic relative to this binary's source.

The browser build used component libraries and zero debug symbols on this 16 GB
host. Twelve compiler jobs worked for most of the tree, but the final large
`content/browser` translation units caused heavy swapping. The interrupted build
was resumed with four jobs; runtime measurements wait until compilation and its
incremental verification have completely finished.

After installing Chromium's build dependencies, OBS still reports OpenGL on
`Mesa Intel(R) Graphics (ARL)`, OpenGL 4.6 / Mesa
`25.2.8-0ubuntu0.24.04.2`. The installed Intel media driver is
`24.1.0+ds1-1`; libva is `2.20.0-2ubuntu0.2`. The isolated OBS startup log explicitly
identifies the loaded plugin as **v1.1.66**. Installed encoder availability alone
is not the GPU proof: the live VAAPI run must also log the selected device/encoder
and pass the render/output checks.

## Compiled-browser minimal result

The full headless Chromium build completed successfully and its incremental
verification reported no remaining work. The headless delegate does not use
`--log-file` like installed Chrome: its `CHROME_LOG_FILE` environment variable is
now set explicitly by both harnesses. The first normal/excluded runs therefore
retain media results but have no native trace; fresh traced controls follow them.

The initial 30-second minimal comparisons all retained 900 consecutive decoded
markers. Normal timing and the all-arrivals experiment each delivered 900
presentation callbacks; forced exclusion delivered 853 callbacks with compositor
counter jumps. The existing presentation analyzer still passed that forced case:
it established no omitted compositor media intervals and its maximum cadence
deviation was 17.03 ms, below the unchanged 25 ms threshold. **This is not reported
as a presentation regression.**

Buffering nevertheless differed sharply. Median receive-to-composition time was
294.85 ms normal, 13.10 ms with forced sample exclusion, and 300.15 ms with forced
exclusion plus all-arrivals. These values use `requestVideoFrameCallback`'s
`receiveTime` and `presentationTime`; they are distinct from encoded-transform
ready timestamps. The all-arrivals native trace recorded 1,508 insertions and
1,508 timing samples, zero uninitialized queries, and a requested 300 ms minimum.
Its median logged render lead was 51 ms because render queries occur during decode
scheduling; that query lead must not be mistaken for the full receive-to-playout
buffer. The simple loopback separates initialization/buffering from presentation
failure, which still needs the independent impaired OBS reproduction.

The fresh **traced** minimal control passed with 900 decoded/presented frames and
295.35 ms median receive-to-composition time. Forced exclusion then reproduced the
presentation failure as well: all 900 frames decoded, but 450 presentation
callbacks and **449 omitted media intervals**, 15.00 presented FPS, 33.77 ms maximum
cadence deviation, and 4.90 ms median receive-to-composition time. Its native trace
recorded **1,508 insertions, zero incoming samples, 1,508 uninitialized queries
with a 300 ms minimum**, and no initialized render query. The earlier forced run's
media pass is retained: initialization starvation is consistent, while the
presentation symptom also depends on arrival/compositor phase.

Headless Chromium's log prefix omits the process ID. The parser reports `unknown`
in these logs; these traced tests use one receiver/timing object per browser
process. It does not invent a process identity or combine independently captured
browser logs.

## Real OBS receiver state confirmed

The compiled browser's clean 720p30/3 Mbps control passed every strict gate:
900 consecutive decoded markers, no omitted presentation frames, both audio
analyses, zero reported packet loss and zero OBS render/output skips. Median
receive-to-composition time was **297.30 ms**.

With reordering present from startup, the default receiver retained all 901
measured decoded markers but omitted **21 presentation media intervals** and had
66.67 ms maximum cadence deviation. Audio and OBS performance passed; median
receive-to-composition time fell to **18.70 ms**. The native trace directly recorded
**1,589/1,589 retransmission-marked insertions, zero incoming timing samples,
1,589 uninitialized render queries with a 300 ms minimum, and no initialized
render query**. This confirms the running receiver state rather than inferring it
from upstream source and encoded-frame metadata.

The first all-arrivals comparison passed every strict gate with 901 consecutive
markers and zero presentation omissions. All **1,588/1,588** insertions still had
the retransmission flag, but all 1,588 now supplied timing samples and there were
zero uninitialized queries. Native minimum-delay values ranged from 288–300 ms
as the receiver's A/V synchronization adjusted timing; the configured browser
buffer and sender settings were unchanged. Maximum presentation cadence deviation
was 16.87 ms, with zero OBS render/output skips and approximately 1.57 ms average
render time. Further repeats and A/V alignment measurements are separate checks.

An earlier logging-disabled cold attempt crashed its renderer before measurement.
Its failure artifact is retained and is not counted as a media result. No matching
kernel OOM/segfault entry or crash dump was available; a separate retry captures
Playwright's browser-process diagnostics without the native timing trace.

## Repeated diagnostic-browser matrix

All completed candidate cold-start measurements pass pixels, presentation, both
audio analyses, packet-loss accounting and OBS performance: 901 markers in both
the traced first run and the timing-trace-OFF second run. The third attempt crashed
before measurement, so this is **two complete passes and one startup failure**,
not a three-run reliability pass. Basic process-log capture is now retained for
custom-browser runs even when timing/A/V instrumentation is disabled.

With reordering plus 0.5% actual packet loss, the three candidate repeats were:

| Repeat | Pixels / presentation | Audio / OBS | Overall strict result |
| --- | --- | --- | --- |
| 1 | 901 markers, no omissions, both pass | Both audio analyses and OBS pass | Pass |
| 2 | 900 markers, no omissions, both pass | Both audio analyses and OBS pass | Fail: one reported lost packet |
| 3 | 899 markers with one missing source frame; one omitted presentation interval, 33.27 ms cadence deviation | Both audio analyses and OBS pass | Fail |

The receiver timing change is therefore not a complete loss-recovery fix. These
failures remain visible; no packet-loss allowance, marker allowance or cadence
threshold was increased.

## Separate diagnostic-build data-channel assertion

The first three-viewer run's primary measurement passed for 120 seconds, but an
additional viewer hit a fatal assertion at Chromium's
`RTCDataChannel::CreateFeatureHandleForScheduler()`:
`DCHECK(!feature_handle_for_scheduler_)`. The other page's pending `getStats()`
kept the extra-viewer harness waiting after that renderer crash. Its browser was
terminated, its failure report retained, and the harness now closes the browser
on a page crash so pending measurements reject rather than hang.

Chromium's `build/config/dcheck_always_on.gni` enables fatal DCHECKs by default in
non-official builds even when `is_debug=false`; official release builds omit this
assertion. This is separate from the instrumented video timing path. There is no
crash log proving that the two earlier startup crashes had this identical cause.

For the remaining diagnostic matrix,
[browser-dcheck-control.py](../tests/webrtc-timing-probe/browser-dcheck-control.py)
checks the exact Chromium revision and replaces **only this assertion** with a
nonfatal `VDONINJA_DIAGNOSTIC` log. The rest of the function executes unchanged,
matching its production release behavior, including scheduler registration and
handle assignment. It does not early-return, alter SCTP, disable other assertions,
change video timing, or relax a media gate. This is an explicit build control,
not a production data-channel fix. The incremental browser rebuild completed
successfully while all test browsers and the publisher were stopped.

Fresh clean/default-cold controls and three candidate cold starts use this control
before the multiple-viewer rerun, outage, GPU and A/V tests. Earlier results remain
separate, and basic process logs retain every occurrence of the assertion
condition. The tested browser is still a diagnostic non-official build with other
DCHECKs enabled, not a claim of bit-identical official Chrome performance.

With this explicit assertion control, the fresh clean run passed all gates. The
fresh default cold run decoded all 901 markers but omitted 31 presentation media
intervals (66.97 ms maximum cadence deviation), while audio and OBS passed.
The three fresh candidate cold starts all passed pixels, presentation, both audio
analyses and OBS performance, with 900/901/900 markers and no omitted intervals.
Their strict overall results were **pass / fail / pass**: the middle run reported
one lost packet. None crashed, and none logged the data-channel assertion condition.
Thus the assertion control does not account for these three media improvements:
its condition was never reached in those runs.

The controlled three-viewer/reload run preserved all **3,600** primary decoded
markers and presentation intervals over 120 seconds. Every additional viewer
passed pixels and presentation in both 20-second rounds and verified H.264 and
relay selection. Their overall results were fail/fail/fail/pass because the first
three rounds reported 5/3/3 lost packets.

The primary run still failed its final relay-evidence gate: its first playable
snapshot, measurement baseline and preceding measurement snapshots identified a
relay pair, but the final snapshot had no selected pair. That final requirement
was not waived. Raw-track audio passed; the separate Web Audio tap detected two
click events around 31.11 seconds, so **both audio probes did not pass** in this
run. OBS reported zero render/output skips and approximately 1.43 ms average render
time. No data-channel assertion condition was logged in the additional-viewer
process. These are useful presentation/reload passes, not an overall multi-viewer
reliability or audio-playout pass.

The matched default warm-start control also retained initialized timing: 656
incoming samples preceded/fell between 933 retransmission-marked insertions,
with zero uninitialized queries. Pixels, presentation, both audio analyses and OBS
passed; one reported lost packet kept its strict overall result failed.

## Outage and recovery

The relay alone was set to 100% loss for 2.05 seconds during an otherwise reordered
120-second candidate run. The full result failed as expected: a 2.988-second
reported freeze, frame-stall/jitter failures and a 1,920 ms decoded-audio dropout.
That result was not rewritten or excluded from the matrix.

`analyze-receiver-recovery.cjs` evaluates the predeclared final 30-second probe
windows, at least 20 seconds after relay restoration. In this run the window
started about 72.8 seconds after restoration. Decoded pixels (901 consecutive
markers), presentation, raw-track audio and Web Audio all recovered and passed;
there were no new freezes or dropped decoded frames. The sampled RTP-statistics
span was 29.452 seconds within that window. Its gate still failed one reported
lost packet, so `recoveryOk` is **false**, despite recovered media continuity.

## Diagnostic-browser detailed 1080p60

OBS's live VAAPI encoder log confirms `/dev/dri/renderD128`, CBR **12,000 kbps**,
H.264 profile 100, 1920×1080, and the active encoder bitrate used for RTP pacing.
The same marked detailed footage ran for 120 measured seconds in each condition.

| Candidate receiver | Decoded markers | Presentation | Audio | OBS |
| --- | --- | --- | --- | --- |
| Clean | All 7,201 consecutive | Fail: 34.46 FPS, 3,063 omitted intervals, 124.80 ms maximum interval | Raw-track and Web Audio pass | Zero render/output skips; 1.39 ms average render time |
| Cold reordering | All 7,200 consecutive | Fail: 34.17 FPS, 3,097 omitted intervals, 112.30 ms maximum interval | Raw-track passes; Web Audio fails | Zero render/output skips; 1.27 ms average render time |

The diagnostic receiver therefore **does not pass detailed 1080p60 presentation**
on this host. Its clean failure must not be attributed to network reordering.
Installed-browser controls with the same footage and VAAPI settings distinguish
this browser build from the OBS/encoder workload. The publisher harness now records
Chromium's CDP GPU devices, backend attributes and feature status when available;
these diagnostics are informational, and unavailable GPU metadata is explicit.

## Calibrated network A/V results

These 720p30/3 Mbps pulse-fixture runs used the same private relay and 300 ms buffer.
The ordinary publisher remained active while the separate audible viewer measured
30 seconds after 20 seconds of stabilization. This is a two-viewer workload.
The diagnostic Chromium used the explicit data-channel assertion control above.

Browser-specific local-file calibration passed: Chromium matched 14 pulses with
24.58 ms median offset and 41.74 ms maximum absolute offset; Firefox matched 14
with 34.10 ms median and 46.96 ms maximum. Network offsets below subtract those
calibration medians. Positive means video follows audio; negative means video leads.

| Browser / receiver | Matched pulses | Median calibrated offset | Maximum absolute calibrated offset | Alignment gate (80 ms) |
| --- | --- | --- | --- | --- |
| Chromium default, clean | 15 | +47.60 ms | 48.12 ms | Pass |
| Chromium default, cold reordering | 11 | −215.19 ms | 231.81 ms | Fail |
| Chromium all-arrivals, cold reordering | 15 | +57.35 ms | 61.85 ms | Pass |
| Firefox default, clean | 15 | +26.58 ms | 27.22 ms | Pass |
| Firefox default, cold reordering | 15 | −300.94 ms | 310.84 ms | Fail |

The Chromium candidate restores measured software A/V alignment in this test;
it is not merely improving a video-only counter. Firefox has an independently
measured cold-start A/V failure, but its internal initialization flag was still
not directly instrumented. Firefox's new presented-marker sampling supports this
pulse comparison; it does not provide the missing complete decoded-frame probe.

Pulse-detection rejections were retained and timestamped. In all five network
captures they occurred before the measured window, during startup; none was
silently dropped from reporting. Every virtual-sink capture recorded identical
normal default-sink names before/after, and all temporary sinks were unloaded.
These results do not measure physical speaker/monitor latency or establish
constant-tone audio continuity under every multiple-viewer/reload condition.

The accompanying Chromium pulse-publisher clean run passed its own media gates;
the cold default and candidate publisher runs remained strict failures with 3 and
7 reported lost packets respectively. Their separate A/V results do not turn the
whole network run into a pass.

## Hardware browser presentation control

The source confirms that headless Chromium forces SwiftShader unless GPU support
is requested. A blank-browser probe with `--enable-gpu` selected **ANGLE on Mesa
Intel(R) Graphics (ARL)**, vendor `0x8086`, with GPU compositing/rasterization enabled.
The publisher now accepts `VDONINJA_CHROMIUM_ARGS_JSON` and records the arguments.
`VDONINJA_REQUIRE_BROWSER_GPU=1` requires a reported hardware vendor and rejects
missing/SwiftShader/llvmpipe/software renderer metadata. Merely passing a GPU flag
cannot satisfy this gate. These runs also supplied the real display/Xauthority.

```sh
export VDONINJA_CHROMIUM_ARGS_JSON='["--enable-gpu"]'
export VDONINJA_REQUIRE_BROWSER_GPU=1
```

The same Intel renderer was verified in each 120-second 1080p60 run below, while
OBS continued using VAAPI CBR 12 Mbps with the same footage and pacing limits.

| Receiver / condition | Decoded markers | Presentation | Overall |
| --- | --- | --- | --- |
| Candidate, clean | All 7,202 consecutive | 59.997 FPS, no omitted intervals; maximum cadence deviation 16.63 ms exceeds 12.5 ms | Fail |
| Default, cold reordering | All 7,202 consecutive | 48.46 FPS, 1,385 omitted intervals; maximum deviation 33.93 ms | Fail |
| Candidate, cold reordering | All 7,200 consecutive | 59.894 FPS, 13 omitted intervals; maximum deviation 16.73 ms | Fail |

All three had zero OBS render/output skips; average render times were 1.49, 1.33
and 1.37 ms respectively. Raw-track audio passed in each; the separate Web Audio
tap failed in each. Hardware presentation removed the diagnostic browser's roughly
34 FPS software-rendering bottleneck, but **did not produce a strict 1080p60 pass**.
The candidate's improvement over the same hardware-backed default cold receiver
is measurable, and its remaining omissions are still failures.

The installed Playwright Chromium clean control, using its default SwiftShader
renderer, retained all 7,200 decoded markers and no omitted presentation intervals
at 59.994 FPS. It nevertheless failed the same maximum-cadence gate (16.63 ms).
OBS again had no skips, averaging 1.52 ms render time. This fresh strict failure
is retained alongside the earlier installed-browser pass in the preceding GPU
report; it does not erase or replace either result. Cadence outliers in the clean
installed-browser control prevent attributing every residual deviation to the
candidate or its diagnostic build alone.

Firefox's matching 120-second **clean** H.264 1080p60 control passed all available
gates: 59.997 presented FPS, no omitted media intervals, 3.17 ms maximum cadence
deviation, Web Audio continuity, relay verification and zero OBS skips (1.33 ms
average render time). Its **cold reordered** comparison failed cadence at 24.49 ms
against the unchanged 12.5 ms limit, despite approximately 60 FPS, no established
omitted media intervals and passing Web Audio/OBS checks (1.31 ms render time).
Firefox still lacks full decoded-marker/raw-track-audio coverage; its clean result
must not be presented as a complete pixel-validation pass.

The final logging-disabled Chromium 720p30 default cold retry did not crash. It
retained all 900 decoded markers but omitted 28 presentation media intervals and
reported one lost packet, so it failed as expected for the unchanged receiver.

## Conclusion and remaining work

Chromium's receiver initialization starvation is now confirmed directly in both
a minimal browser and a real all-retransmission OBS connection. Accepting timing
samples continuously restores the requested buffering and the measured 720p30
cold-start A/V alignment without changing sender quality or pacing. The one-time
anchor is rejected by controller tests. The continuous-sample experiment is still
**not a complete validated browser fix**: loss recovery, some multi-viewer audio
observations and strict 1080p60 presentation remain failing.

No justified plugin-side mitigation was found that preserves the configured
quality and pacing limits. The browser's launch-time NACK trial is not a plugin
signaling control; larger buffers and the existing adaptive mode were not promoted
as fixes. No shipping plugin runtime or packaging behavior was changed here, and
no release or upstream browser submission was made.

Further receiver work needs a reliable policy for timing samples under
retransmission, plus a strict clean compositor control before attributing every
1080p60 cadence outlier to that policy. Firefox's internal initialization state
still needs its own direct instrumentation. The compiled browser, source patches,
fixtures and local artifacts are retained for that continuation; generated media
and logs are not committed.

## Recreating the long 720p30 fixture

The 120-second multiple-viewer/outage tests use a 240-second source so startup and
reload time cannot run past the source's end. The local generator differed from
the committed marked generator only by scaling before encoding and limiting
fixture-generation threads. Recreate that exact preparation from the repository:

```sh
python3 - <<'PY'
from pathlib import Path
s = Path('tests/tools/generate-publish-sequence-video.cjs').read_text()
s = s.replace('const videoFilter = markerFilter(fps);',
              'const videoFilter = markerFilter(fps) + ",scale=1280:720";')
s = s.replace('    "-y",', '    "-y",\n    "-filter_threads",\n    "2",')
s = s.replace('    "libx264",', '    "libx264",\n    "-threads",\n    "2",')
Path('/tmp/vdoninja-generate720.cjs').write_text(s)
PY
node /tmp/vdoninja-generate720.cjs --fps 30 --duration 240 --output counter720p30-long.mp4
```

The normal fixture retains the 997 Hz constant tone. Use the separate pulse-audio
replacement command above only for calibrated A/V alignment measurements.

## Final local checks and cleanup

The final local checks passed: 54 JavaScript measurement tests, 14 Python tests,
and five upstream timing/controller targets (48 component scenarios and 27
controller scenarios). C++ formatting and diff checks passed. Passing the baseline
and bootstrap characterization targets means the documented failures are asserted,
not that those implementations are fixed. Browser integration remains an opt-in
local build/test, not part of the shipping plugin or ordinary CI binary.

After authenticated WebSocket checks confirmed that streaming and recording were
inactive, the identified isolated OBS process was stopped. The disposable relay
had no remaining impairment and was removed. The temporary 12 GB build swap file
was disabled and removed; the host's original 4 GB swap remains. All dedicated
A/V sinks were unloaded. OBS, drivers, the diagnostic browser checkout and ignored
test artifacts remain installed/available for continued investigation.

The first CI run exposed a unit-test dependency issue: importing the publisher's
measurement helpers also imported Playwright, which that dependency-free job does
not install. Playwright now loads inside the executable harness entrypoint. All
54 JavaScript tests passed again in a temporary source copy with no npm dependencies,
including the new relay-selection regression. This changes helper loading only,
not the already measured browser/receiver behavior.
