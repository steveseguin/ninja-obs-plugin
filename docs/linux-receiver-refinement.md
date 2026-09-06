# Linux receiver refinement

Further reload, buffer-feedback and audio comparisons are in
[the buffer follow-up](linux-receiver-buffer-followup.md).

This continues [the receiver controller validation](linux-receiver-controller-validation.md).
Configured quality, bitrate and pacing limits are unchanged. A separately isolated
plugin ICE-restart deadlock is fixed below.
Testing is complete for the comparisons below. The plugin restart fix passes its
regression; the browser experiments remain incomplete fixes.

## Diagnostic changes

The Chromium 145.0.7632.6 source build retains the earlier explicitly documented
nonfatal data-channel assertion control. `browser-patch.py` provides the original
timing trace and continuous completion-time sampling experiment.
`browser-refinement-patch.py` adds:

- `WebRTC-ReceiverTimingTrace/Enabled/`: an additional `release` event, distinct
  from timing queries, with RTP timestamp, release time and assigned render time.
- `WebRTC-CompositorTimingTrace/Enabled/`: decoded-frame enqueue and compositor
  selection events, including reference time, display deadlines, repeat and drop
  counters. It does not change frame selection.
- `WebRTC-FirstPacketTimingSamples/Enabled/`: use the earliest finite received
  packet time for the timestamp extrapolator, falling back to frame completion
  time when packet metadata is unavailable. Enable this together with
  `WebRTC-RetransmittedTimingSamples/Enabled/`. The jitter estimator still excludes
  retransmitted frames. This is an experiment, not a validated shipping fix.

`analyze-compositor-timing.py` joins these events to measured rVFC observations
using RTP timestamps. Full native-run startup omissions and measured-window
omissions remain separate. Missing logging coverage is not inferred as success.

`browser-audio-patch.py` adds opt-in `WebRTC-AudioFifoTrace/Enabled/` logging of
Web Audio input FIFO underruns and overruns. FIFO capacity and output behavior
are unchanged.

## Sustained detailed 1080p60

Each run uses 120 measured seconds following 20 seconds of stabilization, marked
detailed footage, OBS VAAPI H.264 CBR 12 Mbps on `/dev/dri/renderD128`, and a verified
Intel ARL Chromium renderer using `--enable-gpu`. The private relay alone receives
`netem delay 25ms 8ms distribution normal` for cold reordering. No deliberate loss
is configured in these comparisons. Strict loss and cadence thresholds remain.

| Run | Sampling / condition | Measured presentation | Raw / Web Audio | Overall |
| --- | --- | --- | --- | --- |
| `refine-clean-trace` | Completion time, clean, tracing | Pass, zero omissions, 1.27 ms maximum deviation | Pass / pass | Pass |
| `refine-cold-trace` | Completion time, cold, tracing | Fail, six omissions, 16.93 ms maximum deviation | Pass / pass | Fail; also one reported lost packet |
| `refine-first-cold-trace` | First packet, cold, tracing | Fail, three omissions, 16.53 ms maximum deviation | Pass / fail | Fail |
| `refine-first-cold-off-0` | First packet, cold, no timing logging | Pass, zero omissions, 0.97 ms maximum deviation | Pass / fail | Fail; five reported lost packets |
| `refine-first-cold-off-1` | First packet, cold, no timing logging | Fail, zero omissions, 16.53 ms maximum deviation | Pass / pass | Fail |
| `refine-clean-off` | Completion time, clean, no timing logging | Pass, zero omissions, 1.47 ms maximum deviation | Pass / pass | Pass |

The six completion-time omissions and three first-packet omissions in the traced
runs were directly observed as compositor skips. Each omitted frame had already
been released for decoding and enqueued in the compositor. These are not missing
JavaScript callbacks or failed source/encoder delivery. In an example from the
completion-time run, RTP 6435000 was enqueued at native time 27953268428 us with
reference time 27953293000 us. The compositor advanced from RTP 6433500 to 6436500,
reporting one dropped frame at deadline 27953315156 us. Both adjacent source
intervals had ordinary 17 ms assigned render-time steps.

The first-packet experiment therefore does not establish a complete 1080p60 fix.
Logging-disabled controls are retained because tracing changes load. A fresh clean
pass also does not erase the prior clean-control cadence failures.

## Firefox source and automation

The official Firefox 146.0.1 source archive was verified against Mozilla's release
SHA512SUMS:

```
ae95b86e483febf8dfec8347748dd9048ed7d7f845250e07aa8048e2b351da61f6f3c5f83bb0d0c72e1a75ec61b60e59bbe69639f0f33532910ff8bf5ca07394
```

Its `sourcestamp.txt` identifies Mozilla release revision
`86bb7f6af6312ba3c0161085f854bcdff68f1a91`.
`firefox-patch.py` checks that version/revision and applies the same timing probe
sites as the Chromium experiment. Firefox's diagnostic field-trial adapter exposes
only `VDONINJA_FIREFOX_TIMING_TRACE=1` and
`VDONINJA_FIREFOX_TIMING_SAMPLES=all`; normal trial behavior remains unchanged
when these are absent. Chromium command-line field-trial flags are not used.

`browser-native-firefox-timing.py` uses Selenium 4.35.0, geckodriver 0.36.0 and
WebDriver BiDi preload scripts, so this source build needs no Playwright/Juggler
patch. `receiver-browser-scripts.cjs` supplies the repository's existing browser
measurement functions and strict analyzers. Full decoded-pixel and raw-track-audio
coverage remains explicitly unavailable in Firefox.

## Reported loss-counter interpretation

The strict zero-delta loss gate remains unchanged. Its wording now identifies an
increase in the **reported lost-packet counter**, rather than claiming proof of
permanent packet loss. The analysis additionally records the counter range,
negative samples and decreasing intervals.

In `refine-cold-trace`, video loss counters ranged from −17 to +18 and decreased
in 55 sampled intervals; the measured start/end were −8/−7. The strict failure
was therefore a +1 counter delta. In `refine-first-cold-off-0`, the range was
−15 to +14, with 56 decreases and start/end −1/+4. The pinned upstream
`StreamStatisticianImpl::UpdateCounters()` decrements cumulative loss for received
packets and adds sequence advancement for in-order packets. A signed cumulative
counter under reordering/retransmission must not be equated with irrecoverably
missing decoded frames. This does not prove the absence of network loss, change
any threshold, or turn either failed run into a pass. A regression explicitly
keeps a −8 to −7 counter change failing the zero-delta gate.


## Controller regression coverage

The six upstream timing/controller targets pass 96 scenarios: 48 existing timing
component scenarios and 12 scenarios for each of four controller variants. The
60 FPS completion-jitter case supplies exact first-packet times and alternating
12 ms frame-completion delay. Continuous completion-time samples produce a maximum
28 ms render-time step; first-packet samples reduce it to 17 ms. Missing packet
metadata and delayed first packets retain the 28 ms behavior, explicitly showing
the limit of that policy. Cache jumps, a two-second outage, RTP wrap, clean sample
resumption, and ten simulated minutes at ±1000 ppm remain covered. The unsafe
one-time bootstrap is still characterized as failing those adversarial cases;
a passing characterization target does not validate that rejected implementation.

## Audio coverage controls

The dedicated-sink analyzer retains raw-track, Web Audio and actual sink results
separately. It clips the measured interval using the NUT packets' wall-clock PTS,
verifies that decoded PCM bytes match packet coverage, and records timestamp gaps.
The combined audio result also requires contiguous packet timestamps within 1 ms
and duration coverage within 100 ms; it cannot silently concatenate a missing
recording interval into apparently continuous audio. Boundary clipping, missing
packet coverage and timestamp-gap rejection have focused regressions. These new
capture-integrity checks do not weaken any existing audio-continuity threshold.

## Direct Firefox receiver results

The optimized Firefox source build completed successfully using its bootstrapped
Clang toolchain and Rust 1.83.0, matching the source tree's Linux Rust toolchain
configuration. The loaded OBS plugin was reverified through the process module
map: `/home/steve/obs-testing/config/obs-studio/plugins/obs-vdoninja/bin/64bit/obs-vdoninja.so`,
SHA256 `3c8d359b2bfaa88455a41986ab61d6e2feaa00e619543bfc37150d2a5587ac8e`.
It remains the previously validated v1.1.66 runtime, not a newly built v1.1.67
plugin. The sender uses fixed H.264 3 Mbps at 720p30 for these comparisons.

A primary Chromium viewer remains active while the separate native Firefox viewer
measures 30 seconds after 20 seconds of stabilization. H.264 and selected relay
transport are verified in each Firefox round.

| Firefox condition | Valid native events | Measured presentation / Web Audio |
| --- | --- | --- |
| Default, clean | 1,559 insertions, zero NACK-marked; 1,559 incoming samples; zero uninitialized queries | Pass, 3.89 ms maximum cadence deviation / pass |
| Default, cold reordering | 1,541 insertions, all NACK-marked; zero incoming samples; 1,541 uninitialized queries with a 300 ms requested minimum | Fail, 62.27 ms maximum cadence deviation / pass |
| Continuous samples, cold reordering | 1,554 insertions, all NACK-marked; 1,554 incoming samples; zero uninitialized queries | Pass, zero established omissions, 5.21 ms maximum deviation / pass |

The default clean and cold captures each ended with one truncated shutdown log
record. The counts above describe valid records only; that last record is not
inferred. Capture cleanup now closes peer connections and drains the receiver/log
queues before WebDriver teardown, and requests synchronous native logging. The
continuous-sample trace contained no malformed records.

This directly confirms Firefox initialization starvation, independently of its
previously measured A/V symptom. The continuous-sample run still failed its strict
video-statistics gate; its presentation and Web Audio passes are not an overall
matrix pass. Requested 300 ms buffering remained unchanged; Firefox's internal
A/V synchronization varied the native minimum between 279 and 300 ms in that run.


Two further cold runs with continuous sampling and native timing logging disabled
passed Firefox presentation, video-statistics and Web Audio checks. The second
run's accompanying Chromium viewer failed, so these remain Firefox results, not
whole-session passes. A clean startup followed by the same reordering also passed
Firefox; its native trace had 1,549 insertions, 907 NACK-marked frames, 642 incoming
samples and no uninitialized queries.

With 0.5% deliberate loss added to the same reordering, the three continuous-sample
Firefox runs produced one omission, zero omissions and two omissions respectively.
Their maximum cadence deviations were 41.83, 6.95 and 39.39 ms; only the middle
Firefox run passed all checks. All three passed Web Audio continuity. Continuous
timing initialization therefore does not establish loss resilience.

The first two-viewer reload run (`ff-native-multi-reload`) passed both Firefox
viewers' first measured rounds, but neither resumed video after navigation. The
primary Chromium viewer passed its 150-second window. Both Firefox logs contained
an adapter `getStats()` InvalidStateError; the publisher created new peers but did
not reach a connected state for them. That error is a diagnostic clue, not an
established cause. The failure is retained separately from cadence measurements.


The clean single-viewer reload control passed both Firefox rounds. Repeating the
impaired two-viewer reload recovered one viewer and left the other in
`have-remote-offer`, with no local description, selected ICE pair or media bytes.
This places that reload failure before media timing initialization. The failed
round's startup snapshot, negotiation history and screenshot are retained in
`ff-reload-diagnostic-cold/firefox-0`; the adapter error alone does not explain it.

The 120-second dedicated-sink clean audio control (`audio-fifo-clean`) passed raw
track and actual virtual-sink continuity, but the Web Audio measurement tap failed
with one 10 ms dropout and two detected click events near 30.03 seconds. Native
FIFO tracing independently recorded an underrun at 30.011 seconds relative to
raw capture. Packet timestamps covered the sink recording continuously (maximum
absolute gap 0.021 ms), and the original desktop sink remained selected. This
localizes that clean-run dropout to the Web Audio tap; it was absent from the
captured browser output. The combined all-probe result remains a failure. Native
FIFO counts also include startup and post-capture events, which are not measured
window failures. This is virtual-sink validation, not physical speaker testing.


| Dedicated-sink condition | Raw track | Web Audio tap | Actual virtual output | Timestamp coverage |
| --- | --- | --- | --- | --- |
| Clean | Pass | Fail: 10 ms dropout, two clicks | Pass | Pass |
| Cold reordering | Fail: 30 ms dropout | Fail: dropout and five clicks | Fail: 30 ms dropout | Pass |
| Reordering plus 0.5% loss | Pass | Fail: 10 ms dropout, two clicks | Pass | Pass |

Each audible viewer measured 120 seconds while a primary Chromium viewer remained
active for a 180-second measurement. The primary passed only the clean run. All
three audible captures completed with H.264 and a selected relay verified, and
restored the original default sink. Strict combined audio checks failed all three;
the passing loss run's actual output does not negate the reordered run's dropout.


## Plugin ICE-restart deadlock

The first impaired TCP control (`tcp-relay-cold-1`) froze after initial video and
failed to stop OBS within ten seconds. Native stacks from the isolated OBS process
showed `requestIceRestart -> retirePeerForDeferredCleanup -> retirePeerDataChannel`
waiting on the old peer's media mutex. GDB confirmed that the same RTC worker
(LWP 683195) already owned that mutex and the registry mutex. This is a directly
confirmed self-deadlock, separate from browser timing initialization.

`requestIceRestart` now takes registry and media locks in registry-before-media
order for the state transfer and peer swap, releases them before sending the
offer, and releases outgoing-send locks before retiring either peer. The original
state transfer, session identity, rollback and pacing behavior are retained.
The real libdatachannel/OBS-linked regression reaches stable SDP signaling,
requests an ICE restart, checks generation replacement/session preservation and
then stops publishing. The original implementation timed out at 30 seconds;
the fixed implementation completed in 0.05 seconds. All nine linked CTest targets
passed, including the full native gate. The regression is included in that
existing full target; release gate inventory is unchanged.

The fixed live runtime is v1.1.67, built against the unchanged OBS 32.2.0 SDK
baseline and loaded from the same isolated plugin path. Its SHA256 is
`03ff5c1e593e8081caf4b01a9a9d53f487549db26567a6c9766744232b284b24`.
Results preceding `tcp-fixed-*` use the old runtime and are kept separately.


## Viewer-to-relay TCP comparison

`VDONINJA_VIEW_TURN` uses the existing alpha viewer TURN URL parameter;
`VDONINJA_PRESERVE_VIEWER_ICE_CONFIGURATION=1` lets the application apply that
configuration, and `VDONINJA_EXPECT_VIEWER_RELAY_PROTOCOL=tcp` requires a selected
local relay candidate whose statistics report TCP. The native Firefox driver
checks the same property with `--relay-protocol tcp`. Both clean controls (old and
fixed plugin runtimes) passed Chromium and Firefox with that transport verified.
The requested 300 ms buffer, 720p30 H.264 3 Mbps encoder and plugin pacing are
unchanged. This configures the viewer's TURN connection only; the publisher's
libjuice connection remains UDP. It is not a plugin-enforced TCP path.

Both fixed-plugin cold-reordering attempts failed to progress into sustained
video. Firefox's second captured negotiation completed answer creation and local
SDP successfully, including a subsequent restart, before its connection closed.
The publisher logged completed peer rebuilds and stopped normally after the
harness failed. Its repair cache missed tens of thousands of requested packets,
repair expiration rose, and reported repair queue delay reached 499 ms. Neither quality nor pacing limits were relaxed to rescue these runs.
TCP is therefore rejected as a validated mitigation in this topology.

The failed TCP-configured runs never reached the final selected-transport gate;
the retained failure reports do not establish the selected transport throughout
those failed sessions. Only the clean passes carry completed TCP verification.
Future native-driver startup failures also retain candidate pairs observed before
a peer closes, rather than retaining only the final closed-peer snapshot.


The final two TCP-configured loss runs also failed before complete media
measurement and both stopped OBS normally. After removing TCP configuration,
`ff-native-cold-flushed` completed a fresh UDP control on the fixed plugin:
primary Chromium passed, Firefox presentation failed while video statistics and
Web Audio passed. Its fully drained native trace has **zero malformed records**:
1,554 insertions, all NACK-marked, zero incoming timing samples and 1,554
uninitialized queries (1,508 after a positive minimum buffer was requested).
This independently reconfirms receiver initialization starvation after the plugin
restart fix; the two defects are separate.


## Source-cadence renderer experiment

The new probe links Chromium's actual `VideoRendererAlgorithm`. It runs 36
combinations of 30/60 FPS, zero/2 ms reference-time corrections, and nine display
phases over 60 simulated seconds, analyzing the interior 50 seconds. With ordinary
reference-time duration samples, the 60 FPS, 2 ms correction, 10 ms display-phase
case omits 12 frames and deviates by 16.67 ms. All clean-reference cases pass.
The baseline executable's zero exit status means it successfully characterized
that failure, not that every baseline case passed.

`WebRTC-RtpCadenceSamples/Enabled/` derives duration samples from forward RTP
progression when available. It leaves render reference times, drift limits,
cadence-switch thresholds and the fallback for unavailable/invalid RTP samples
unchanged. All 36 source-cadence cases pass with zero omissions, as do the same
36 cases spanning RTP wrap. This reproduces a compositor failure mechanism
without a network, decoder, GPU, or reimplemented frame selector. Real 1080p60
integration results must be assessed separately.

In the final UDP two-viewer reload run (`ff-reload-answer-trace`), both first
rounds passed presentation and Web Audio but failed the signed loss-counter gate.
One viewer then reloaded and passed its entire second round. The other accepted a
remote offer but never entered `createAnswer`, selected an ICE pair or received
media. Answer creation was explicitly instrumented before/after invocation, so
this is not a pending native `createAnswer` promise. It remains an unresolved
application negotiation path, distinct from the repaired plugin deadlock.


## Final detailed GPU comparison

The five final runs use the fixed plugin, Intel ARL VAAPI H.264 CBR 12 Mbps,
1920×1080 at 60 FPS, and verified hardware Chromium rendering. Each measures
120 seconds after 20 seconds of stabilization with the same detailed marked
fixture. The host is Ubuntu 24.04, OBS 32.2.2 and Mesa 25.2.8. Encoder use and
the loaded module path were verified from the actual OBS process/log.

| Run | RTP cadence experiment | Network / native timing logs | Decoded markers | Omissions | Maximum cadence deviation | Overall |
| --- | --- | --- | --- | --- | --- | --- |
| `cadence-baseline-clean` | Off | Clean / off | 7,201 consecutive | 0 | 2.57 ms | Pass |
| `cadence-clean` | On | Clean / off | 7,200 consecutive | 0 | 17.13 ms | Fail |
| `cadence-cold-trace` | On | Cold reordering / on | 7,200 consecutive | 0 | 16.83 ms | Fail |
| `cadence-cold-off-0` | On | Cold reordering / off | 7,201 consecutive | 0 | 1.03 ms | Pass |
| `cadence-cold-off-1` | On | Cold reordering / off | 7,201 consecutive | 0 | 1.03 ms | Pass |

All five pass raw-track and Web Audio continuity and report zero OBS render/output
skips. The two failures remain strict presentation-cadence failures. Native logs
confirm no measured compositor omissions in `cadence-cold-trace`. At its failing
interval, RTP 13053000 and 13054500 are consecutive source frames, but assigned
render times move from 35476440 to 35476472 ms. The native minimum delay increases
from 305 to 320 ms, and the existing `VideoReceiveStream2` warning explicitly
identifies `sync min delay=320 ms` with `base min delay=280 ms`. The compositor
repeats the previous frame once, then presents the next without omitting it.

Thus this observed 33.5 ms presentation interval comes from an A/V synchronization
buffer increase, not timestamp-extrapolator starvation or a compositor omission.
The browser's base delay also varies: the alpha application computes delay hints
from its configured buffer and measured jitter-buffer delay. A requested
`buffer=300` is not evidence that every internal minimum remains fixed at 300 ms.
The RTP cadence experiment addresses the separately reproduced frame-selection
mechanism; these results do not justify claiming a complete 1080p60 fix or disabling
A/V synchronization to pass a cadence threshold.

## Validation and remaining limits

- 728 C++ unit tests passed normally and under ASan/UBSan. One locale-dependent
  test initially skipped because the host lacked a suitable locale; it then passed
  in both builds using an isolated generated ISO-8859-1 locale via `LOCPATH`.
- Nine real OBS/FFmpeg/libdatachannel linked targets passed, including the new
  ICE-restart case in the existing full gate. The opt-in RTP trace target passed.
- 60 JavaScript and 18 Python measurement tests passed.
- Six pinned upstream timing/controller targets characterize 96 scenarios.
  The actual renderer probe covers 36 baseline cases, 36 source-cadence cases,
  and 36 source-cadence cases across RTP wrap; baseline failure characterization
  is distinguished above from candidate passes.
- OBS exited normally after the final runs. The disposable relay had no remaining
  qdisc impairment and was removed. All dedicated audio sinks were unloaded and
  the original desktop sink was restored. Recordings, traces, source checkouts,
  browser builds and the isolated locale remain local, outside committed artifacts.

The shipping change fixes the directly reproduced ICE-restart deadlock. Receiver
initialization starvation is confirmed independently in Chromium and Firefox;
continuous timing samples initialize buffering but do not establish loss recovery.
RTP cadence sampling removes the isolated renderer fault and produced two complete
impaired 1080p60 passes, but the full matrix still fails strict cadence because of
remaining delay adjustments. Firefox reload negotiation and intermittent real
audio dropouts also remain unresolved. Firefox lacks full decoded-pixel and
raw-track-audio instrumentation; virtual-sink results are not physical speaker
latency validation. No new release or upstream browser submission was made.
