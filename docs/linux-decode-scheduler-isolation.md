# Decode scheduling and repaired minimal A/V reproduction

This continues [the synchronization investigation](linux-receiver-sync-baseline.md).
The shipping plugin, configured quality, sender pacing and release baseline remain
unchanged. Browser patches here are opt-in experiments in the pinned source builds.

## Minimal A/V control repaired

The earlier generated-audio control received samples but never produced native
synchronization decisions. New `VDONINJA_SYNC_STAGE` traces show that a synchronizer
was configured and reached `ComputeRelativeDelay`; the relative calculation was
rejected. `VDONINJA_SYNC_CLOCK` then exposed an apparent relative delay near
23,220,000 ms (about 6.4 hours), far outside the accepted range.

Pinned Chromium's `PushableMediaStreamAudioSource::DeliverData` passes
`base::TimeTicks() + data->timestamp()` to its tracks. The old generator used
timestamps starting at zero. `WebRtcAudioSink` uses those as capture timestamps
because `WebRtcAudioSinkUseTimestampAligner` is disabled by default. The video
sink separately recognizes timestamps far from the native clock and estimates
capture time from the current native clock. Receiving both tracks therefore did
not establish a valid common capture clock. The precise offset depends on the
source clocks, RTP wrapping and sender-report extrapolation; it is not a real
six-hour audio buffer.

The minimal harness now defaults to a 48 kHz Web Audio oscillator at 997 Hz when
`VDONINJA_MINIMAL_AUDIO=1`. The browser supplies its capture timestamps. It uses
the same private null sink and restores the default sink. The original generator
is retained as `VDONINJA_MINIMAL_AUDIO_SOURCE=generator` for negative controls.
This changes the test source, not receiver delay policy or production audio.

Two 45-second measurements after 20 seconds settle established the repaired
control at 640×360/60 with H.264, both receiver targets 300 ms, continuous incoming
timing and RTP cadence:

| Capture | Audio transport delay | Native sync decisions | Result |
| --- | ---: | ---: | --- |
| `scheduler-minimal-native-audio` | 0 ms | 58 | Pixels and presentation pass; relative delay about −7 to −8 ms |
| `scheduler-minimal-native-delay` | 70 ms | 59, including 18 adjustments | Pixels pass; presentation deviation 16.73 ms exceeds the unchanged 12.5 ms limit |

Audio transport delay uses independent encoded-frame deadlines, without serially
reducing throughput. This loopback does not simulate real NACKs or packet loss.
The existing all-startup-frame retransmission tests remain separate.

## Direct scheduler evidence

`browser-scheduler-trace-patch.py` adds scheduler selection, intended decode
and render deadlines, metronome decisions/ticks and actual releases. Enable
`VDONINJA_DECODE_SCHEDULER_TRACE=1`; the existing receiver/A/V trace trials enable
selection and synchronization-stage logs. No behavior changes occur in this patch.

The build actually selects `DecodeSynchronizer`, with a 15.625 ms tick period.
In the delayed minimal run, one of six measured cadence events follows a video
minimum change from 303 to 327 ms and a 41 ms assigned render interval. Five
other events retain the same minimum and approximately 16–17 ms render intervals.
Those must not be attributed to an A/V target change.

For example, RTP 2754700106 → 2754701636 has minimum 344 ms throughout:

| Timing | First frame | Second frame |
| --- | ---: | ---: |
| Decode deadline (native µs) | 66421030670 | 66421047670 |
| Actual release (native µs) | 66421015782 | 66421047187 |
| Release minus deadline | −14.888 ms | −0.483 ms |
| Compositor enqueue (native µs) | 66421016137 | 66421047536 |
| Render reference (native µs) | 66421041000 | 66421058000 |

The second frame misses the tick whose next-tick boundary is 66421046987 µs,
just short of its deadline. It is released on the following tick. Releases are
31.405 ms apart despite a 17 ms render-reference interval. The compositor repeats
the first frame once; measured presentation is 33.2 ms apart. The initial trace
contains compositor deadlines, not selection-call times, so it cannot by itself
prove whether an enqueue preceded the actual selection call. The additional
`browser-compositor-clock-patch.py` logs that call's native clock for the candidate
comparisons.

`scripts/analyze-decode-scheduler.py` joins schedules/releases by scheduler and
RTP, rejects missing or ambiguous release identities, and adds enqueue/render/
selection evidence to measured cadence events. It does not replace or waive the
original media gates.

## Native characterization and experiment

`scheduler-probe` runs the actual upstream `DecodeSynchronizer` and
`TaskQueueFrameDecodeScheduler` with a deterministic task queue and 15.625 ms
metronome. It covers 30/60/120 fps, 16 phases, two initializations and 1,800 frames
per scenario. Optional Perfetto calls are removed from a generated source copy;
scheduling logic is unchanged. In 64/96 metronome scenarios, release gaps exceed
the source interval by more than 8 ms. All 96 precise-scheduler scenarios preserve
exact deadlines and source cadence. These tests cover scheduled releases, not
payload decoding, actual CPU load or presentation. Passing the metronome target
means its coalescing behavior was characterized, not that media passed.

`browser-precise-decode-patch.py` adds `WebRTC-PreciseDecodeScheduling/Enabled/`.
It selects the already-existing high-precision task-queue scheduler instead of
the metronome and logs that scheduler's deadlines/releases using the same schema.
Render targets, min/max receiver buffers, sender bitrate and sender pacing are
unchanged. This may increase task wakeups; no power-use result is claimed.
It is compared alone and with the previous 16 ms decode-headroom experiment.
Neither experiment changes the A/V synchronization algorithm.

## GPU controls and environment

The pre-candidate controls `scheduler-gpu-cold` and
`scheduler-gpu-headroom-cold` each measured detailed 1080p60 for 120 seconds after
20 seconds settle. Both used Intel ARL, Mesa 25.2.8, OBS 32.2.2, actual VAAPI
H.264 CBR 12 Mbps, private TURN and cold `netem delay 25ms 8ms distribution normal`.
The first has no measured cadence outlier but fails the reported lost-counter
gate (+1). The second has one cadence outlier despite fixed 300 ms minimum and
a 17 ms render interval, also fails the lost counter (+1), and lacks final relay
verification. Pixels, decoded audio and OBS performance pass in both. These
controls preserve their failed overall verdicts; the cadence failure is intermittent.

The actual loaded module was verified through the running OBS process:
`/home/steve/obs-testing/config/obs-studio/plugins/obs-vdoninja/bin/64bit/obs-vdoninja.so`,
SHA256 `03ff5c1e593e8081caf4b01a9a9d53f487549db26567a6c9766744232b284b24`.
Chromium source remains `47e20adcc15fc15f01825aa17e570c8f5492ac0f`, with WebRTC
`b47e68e6966d5a5a0e4bc861ff364221600f31c3` (145.0.7632.6).

## Firefox startup isolation

A further verbose native-Wayland startup control reproduced the block:
`WebDriver BiDi ... Waiting for initial application window`. Source inspection
shows this awaits the browser-startup-finished promise, resolved by
`browser-idle-startup-tasks-finished`. The WebDriver constructor never completed;
no video measurement started. Its initial 45-second signal did not provide a
reliable wall-time bound through Selenium's HTTP/cleanup path; the HTTP operation
reported a 120-second timeout and the isolated browser was explicitly stopped.
This is startup evidence, not Wayland performance coverage. The existing user's
Firefox process was not touched. No startup-completion event was faked to bypass
this failure.


## Precise-scheduler minimal comparisons

The precise-only delayed control (`precise-minimal-delay`) produced 56 native
sync decisions. Its measured releases were 0.013–0.591 ms after the deadline
(median 0.0945 ms). All five cadence events follow A/V minimum increases:
300→316, 316→338, 338→353, 353→370 and 370→385 ms. There are no stable-minimum
cadence events in this run. Presentation still fails at 16.83 ms maximum
per-frame cadence deviation; pixels pass.

The precise-plus-headroom delayed control (`precise-headroom-minimal-delay`)
produced 58 native sync decisions. Releases were 0.016–1.076 ms after deadline
(median 0.09 ms). Its five cadence events also coincide with minimum changes;
maximum per-frame deviation is 16.73 ms. The matching zero-audio-delay control
(`precise-headroom-minimal-clean`) passes pixels and presentation and records
58 native sync decisions. All three use 45-second measurements after 20 seconds
settle and restore the private audio sink.

This is evidence that precise scheduling removes one cause of cadence variation
in these controls. It does not fix A/V target jumps, prove every stable-minimum
failure is eliminated, or establish production browser performance. The broader
GPU matrix and its unchanged gates remain necessary.


The clean 150-second GPU candidate run still has one stable-minimum cadence
failure (RTP 7177500→7179000), despite precise releases less than 1 ms late.
The actual compositor-call trace makes the remaining boundary concrete:
selection repeated the first frame at native 67285792128 µs; the next decoded
frame was enqueued at 67285792775 µs, **647 µs after that call**, although its
render reference was 67285815000 µs. Both minimum delays were 300 ms; both decode
estimates were 4 ms, with render delay 10 ms and extra headroom 16 ms. The renderer
was selecting ahead of the eventual reference time. An on-time decode release
therefore does not guarantee enqueue before compositor selection. This rejects
the precise-plus-16-ms combination as a complete fix, including on a clean link.
The remaining pipeline budget must be measured against actual selection calls,
not inferred solely from render-reference timestamps.


## Repeated GPU and multiple-viewer validation

The precise-plus-headroom GPU matrix retained the same 12 Mbps VAAPI detailed
1080p60 source and the normal application buffer feedback. Every measurement
followed 20 seconds of stabilization. The loss case added 0.5% deliberate loss
to the relay's 25 ± 8 ms delay; warm reordering was installed after clean startup.

| Capture | Measurement | Chromium result |
| --- | ---: | --- |
| `precise-gpu-clean` | 150 s | Pixels/audio/video stats/OBS/relay pass; one cadence outlier fails |
| `precise-gpu-cold` | 120 s | Pixels/cadence/audio/OBS/relay pass; reported lost counter +8 fails |
| `precise-gpu-warm` | 120 s | Pixels/audio/OBS/relay pass; three stable-minimum cadence outliers and reported lost counter +29 fail |
| `precise-gpu-loss` | 150 s | Audio/video stats/OBS/relay pass; 32 decoded-marker skips and 32 presentation omissions fail, with a 116.5 ms presentation interval |

In the warm run, precise releases are no more than 1.071 ms late, yet all three
cadence events retain minimum 300 ms and assigned render intervals 16–17 ms.
This again rejects scheduler precision alone as proof of timely compositor enqueue.
In the deliberate-loss run the reported lost counter actually decreases by one;
that stats gate passes, but pixel/presentation failures remain. A counter's final
delta is not equivalent to distinct-frame delivery.

Clean and loss sessions each also ran a simultaneous headed Firefox X11/WebRender
viewer through two 30-second measurements, each after 20 seconds settle, with one
reload/reconnection between rounds. Both clean Firefox callback/stats verdicts
pass, but native first composition shows 47 and 39 omissions. Both loss rounds
fail callback/stats and native composition, with 60 and 10 native omissions and
maximum composition gaps 116.526 and 33.29 ms. This is four Firefox rounds, two
reloads, and two simultaneous-viewer sessions. Firefox used the existing incoming
sample experiment, not Chromium's precise-scheduler patch; its cause remains
separate. Codec and GPU/window protocol were checked by the driver.

A further minimal metronome repeat with actual selection-call clocks
(`metronome-selection-clock-control`, 45 seconds after 20 seconds settle) records
58 native sync decisions and five cadence events. Four retain the same video
minimum. For those four events the next decoded image arrives **10.201–13.439 ms
after the compositor call that repeats the previous frame**. This directly closes
the missing selection-clock evidence in the earlier trace. The fifth event also
has an A/V minimum increase. Pixels pass; presentation still fails at 16.93 ms
maximum per-frame deviation. This control preserves the metronome behavior and
uses the repaired browser-clocked audio source.


The final `precise-gpu-cold-trace-off` control measured 120 seconds after
20 seconds settle with detailed native traces, buffer-write tracing and encoded
frame probes disabled. Continuous incoming timing, RTP cadence, precise scheduling
and 16 ms decode headroom remained enabled. Pixels, presentation, decoded audio,
OBS performance, video-stat and relay gates all pass. The reported lost-counter
delta was −12; this is not a claim of negative physical packet loss. One passing
control does not erase the clean/warm failures or establish that tracing caused
them. It does establish a passing run without the detailed instrumentation.

## Validation and remaining work

Local checks passed 70 JavaScript measurement tests, 29 Python packet/timing and
compositor/scheduler tests, and all 11 pinned upstream native CTest targets.
Chromium's diagnostic build succeeded, including the precise scheduler and actual
compositor-call clock. Format and syntax checks passed. The revised minimal source
was exercised through passing controls and retained failing delayed cases.

The minimal A/V harness clock is fixed. The metronome's release variation and
resulting late enqueue are directly confirmed, and the precise-scheduler experiment
is implemented and tested. The combination is **not ready as a general fix**:
abrupt A/V minimum changes still fail, and GPU decode/enqueue can miss a compositor
selection despite precise release and unchanged render targets. A justified next
receiver change needs to account for the actual selection lead and decode/enqueue
variability, and handle synchronization adjustments without abrupt presentation
jumps. Increasing application buffers or reducing sender quality is not supported
by this evidence. No shipping plugin mitigation was introduced.

Firefox's X11 native omission failure remains separate. Wayland is still blocked
before measurement while waiting for browser startup completion; it must not be
reported as a tested Wayland media path. Physical display scanout and sustained
physical audio-output continuity are outside this pass's measurements.

Full recordings and logs remain ignored local artifacts under `artifacts/gpu-linux/`
and `/home/steve/obs-testing/logs/`. Source patches, analyzers, native regression
controls and reproduction instructions are committed. No release was published.
