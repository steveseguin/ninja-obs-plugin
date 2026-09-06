# Linux receiver buffer and reload follow-up

This continues [the receiver refinement](linux-receiver-refinement.md) on the same
Intel ARL Linux host. OBS 32.2.2 loads the fixed v1.1.67 plugin from
`/home/steve/obs-testing/config/obs-studio/plugins/obs-vdoninja/bin/64bit/obs-vdoninja.so`;
its SHA256 remains
`03ff5c1e593e8081caf4b01a9a9d53f487549db26567a6c9766744232b284b24`.
No encoder quality, plugin pacing limit, or measurement threshold is relaxed.

## Firefox reload reproduction

Four independent OBS publishing sessions used H.264 x264 CBR 3 Mbps, marked
720p30 footage, private TURN/UDP, a 300 ms viewer buffer and reordering from
startup (`netem delay 25ms 8ms distribution normal`, confined to the relay).
Each session had a primary Chromium viewer and two native Firefox viewers.
The first session had one reload per Firefox viewer; the next three had two.
Each Firefox round measured 30 seconds after 20 seconds of stabilization.

These are diagnostic Firefox 146.0.1 runs with the previously documented OpenH264
installation and continuous retransmitted-frame timing samples enabled. They do
not validate the default receiver sampling policy. Full decoded-pixel and raw
track audio checks remain unavailable in this native Firefox driver.

| Artifact directory | Viewing rounds | Reloads | All gates pass | Presentation / Web Audio pass |
| --- | ---: | ---: | ---: | --- |
| `ff-reload-app-errors-0` | 4 | 2 | 3 | 4 / 4 |
| `ff-reload-app-errors-1` | 6 | 4 | 4 | 6 / 6 |
| `ff-reload-app-errors-2` | 6 | 4 | 4 | 6 / 6 |
| `ff-reload-app-errors-3` | 6 | 4 | 3 | 6 / 6 |
| Total | 22 | 14 | 14 | 22 / 22 |

All four primary Chromium sessions passed. Eight Firefox rounds failed the
unchanged signed lost-packet counter delta gate. They remain failures. None of
the 14 reloads reproduced the earlier accepted-offer/no-`createAnswer` stall;
there is no justified negotiation fix from this sweep. The earlier failure is
retained, not reclassified as fixed.

The native driver now retains bounded console errors, uncaught errors, rejected
promises, application peer identities and negotiation traces in successful and
failed rounds. Error capture preserves console call arguments, receiver and
return value, including diagnostic objects whose string conversion throws.
Observed console errors in this sweep concerned the separate debug WebSocket.

The live alpha `webrtc.js?ver=960` fetched for inspection had SHA256
`17c5707b56182e9b36480833db2a7fd627a08d5af6c603c3fafa1c56d04d161c` and HTTP
Last-Modified `2026-09-06 00:22:50 GMT`; the inspected adapter SHA256 was
`f295969838a63e2a656d566aa92a7b87d00dbc1e35754fd7a888a9c74e2beb97`.
These identify the inspected files, not a network-content hash verified inside
every browser session. The separate application checkout is unchanged.

## Buffer and native synchronization controls

`VDONINJA_TRACE_BUFFER_WRITES=1` records requested and applied receiver buffer
values, track kind and browser timestamp. With the existing
`VDONINJA_FIXED_VIEW_BUFFER=1` isolation flag, attempts to change the initial
300 ms target are recorded but do not change that target. Both audio and video
receivers are observed. This is a diagnostic control at the same configured
buffer size, not a larger-buffer mitigation or a shipping application change.
Repeated track events preserve the existing observer rather than attempting to
redefine its nonconfigurable property.

The dedicated sink harness supports the same control, records its applied
capabilities, and retains one-second receiver statistics alongside raw-track,
Web Audio and virtual-sink audio. Missing or invalid fixed-buffer URL values are
rejected. Physical speaker/monitor latency is outside this capture's coverage.

`tests/webrtc-timing-probe/browser-sync-patch.py` adds the opt-in
`WebRTC-AvSyncTrace/Enabled/` trial to the pinned Chromium WebRTC source. It logs
native synchronization input delays, relative audio/video delay, proposed output
delays and whether an adjustment occurred. It does not change the calculation,
minimum-delay setters, synchronization thresholds, or their one-second schedule.
Targets logged with `adjusted=0` are unused output arguments, not applied delays.

Apply once to the already prepared diagnostic source tree, then rebuild before
starting runtime tests:

```sh
python3 tests/webrtc-timing-probe/browser-sync-patch.py "$CHROMIUM_SRC/third_party/webrtc"
autoninja -C "$CHROMIUM_SRC/out/ReceiverTiming" -j4 headless_shell
```

The browser patch refuses an unexpected WebRTC revision or an already patched
site. The trace-only rebuild completed successfully on the pinned source.
Runtime comparisons must keep normal application feedback and fixed targets
separate; native A/V synchronization remains enabled in both.

## Audio probe clock transition

The four original follow-up captures (`audio-feedback-cold`, `audio-fixed-clean`,
`audio-fixed-cold`, `audio-fixed-loss`) each recorded 120 seconds at the dedicated
sink while a primary viewer measured 180 seconds. Raw decoded audio and the
virtual sink passed in all four. The legacy Web Audio probe failed in all four
with a 10 ms dropout; all four combined audio checks therefore failed. Capture
timestamp coverage and default-sink restoration passed. The fixed controls
verified both native audio and video targets at 300 ms. Keeping that target
constant did not fix the legacy probe's dropout.

In `audio-feedback-cold`, the Web Audio discontinuity begins around 34.58 seconds.
Raw audio has a 12.05 ms timestamp step around 34.56 seconds, rather than its usual
10 ms. Concealment counters did not change across that interval. These results
are distinct from the prior capture where raw decoded audio and the actual
virtual sink both dropped out. That earlier output failure is not dismissed.

The new `browser-audio-fifo-repro.cjs` removes OBS, encoding, RTP and TURN entirely.
It feeds continuous 997 Hz PCM through an audio track generator on a worker,
records both the raw track and Web Audio, and retains actual source scheduling
jitter. An optional 2 ms source phase change happens at source time 30 seconds.
Even the zero-added-delay legacy controls reproduced a Web Audio dropout with
continuous raw samples. A zero-added-delay run is not a claim of perfect worker
timer scheduling; measured source jitter remains in its report.

The native silent-sink trace directly identifies a second timing mechanism.
In `audio-capture-script-processor-0-1`, the silent output switches to the fake
callback worker at capture time 30.024 seconds. A Web Audio FIFO underrun follows
at 30.089 seconds; the measured PCM dropout starts at 30.09 seconds. Chromium's
`RealtimeAudioDestinationHandler::SetDetectSilenceIfNecessary()` enables that
30-second transition for an interactive context without automatic pull nodes.
The old probe connected a ScriptProcessor through a zero-gain destination, so
its own output was silent even while it recorded an incoming tone.

The preferred probe now uses an AudioWorklet with one input and no outputs.
Chromium treats it as an automatic pull node, preserving the context's callback
clock without playing the probe or sending silence through a gain node. It
changes no receiver buffer or plugin setting. PCM thresholds are unchanged;
partial final chunks are flushed, and missing input remains recorded as silence.
The old ScriptProcessor path is available explicitly for comparisons and as a
reported fallback when AudioWorklet is unavailable. Module/processor failures are
not silently treated as passing audio.

The first four traced local comparisons were:

| Run | Raw / Web Audio | Silent-sink transitions | Interpretation |
| --- | --- | ---: | --- |
| `audio-capture-worklet-0-0` | Pass / fail | 0 | A separate source-delivery gap reached 14.6 ms; native FIFO underrun at capture 28.982 s is retained |
| `audio-capture-script-processor-0-1` | Pass / fail | 1 | Clock transition at 30.024 s, FIFO underrun at 30.089 s |
| `audio-capture-worklet-2-2` | Pass / pass | 0 | Deliberate source phase change; measured maximum delivery gap 12.5 ms |
| `audio-capture-worklet-0-3` | Pass / pass | 0 | Repeated zero-added-delay control |

This corrects the probe-induced silent-clock transition; it is not a general fix
for source scheduling stalls or network audio loss. Full application pages can
have other AudioContexts and silent-sink transitions, so their native events must
not all be attributed to this probe. The standalone reproduction isolates that
ambiguity.

To reproduce using the prepared diagnostic Chromium checkout:

```sh
python3 tests/webrtc-timing-probe/browser-silent-sink-patch.py "$CHROMIUM_SRC"
autoninja -C "$CHROMIUM_SRC/out/ReceiverTiming" -j4 headless_shell
export VDONINJA_BROWSER_EXECUTABLE="$CHROMIUM_SRC/out/ReceiverTiming/headless_shell"
VDONINJA_AUDIO_CAPTURE_MODE=script-processor node scripts/browser-audio-fifo-repro.cjs artifacts/audio-legacy 0
VDONINJA_AUDIO_CAPTURE_MODE=worklet node scripts/browser-audio-fifo-repro.cjs artifacts/audio-worklet 0
```

The reproduction's exit status reports setup/capture errors; its `report.json`
contains the strict audio pass/fail result. Retain failing reports. Apply each
source patch only once, never while an active runtime comparison is measuring.

## Raw audio capture queue isolation

The subsequent live Worklet sweep (`audio-worklet-clean`, `audio-worklet-cold`,
`audio-worklet-loss`) passed all three audio taps in the clean control. The cold
run passed Web Audio and the virtual sink but failed raw capture near 112 seconds,
with raw timestamp gaps up to 128.2 ms. The loss run passed raw and Web Audio but
retained two sink click events near 8.575 seconds. Another application context's
silent-sink transition occurred about 37 ms earlier; correlation does not establish
that it caused the output discontinuity. No application AudioContext lifecycle
change is made from that evidence.

Chromium's audio MediaStreamTrackProcessor uses a default queue of ten blocks.
A blocked page thread can overflow that capture queue independently of playout.
The raw probe now transfers its readable stream to a worker, preserving the
browser's default queue capacity. It drains and stores samples there, then returns
PCM and timestamp diagnostics when frozen. It does not enlarge the receiver or
capture queue. Capture freezes before exporting large video record arrays; final
Worklet chunks are flushed and the worker reader is cancelled at that boundary.

The network-free controlled comparison injected the same 250 ms page-thread stall
three seconds into a ten-second capture. `raw-stall-main-thread` failed raw PCM
while passing Worklet PCM; `raw-stall-worker` passed both. The source worker and
buffer settings were identical. This isolates a measurement failure and does not
reclassify previous real output failures.

```sh
VDONINJA_AUDIO_CAPTURE_MODE=worklet \
VDONINJA_AUDIO_REPRO_DURATION_MS=10000 \
VDONINJA_AUDIO_MAIN_THREAD_STALL_MS=250 \
VDONINJA_RAW_AUDIO_CAPTURE_MODE=main-thread \
node scripts/browser-audio-fifo-repro.cjs artifacts/raw-main-thread 0
# Repeat with VDONINJA_RAW_AUDIO_CAPTURE_MODE=worker (the new default).
```

## GPU-backed simultaneous Chromium and Firefox

OBS used Intel ARL OpenGL with Mesa 25.2.8 and the loaded `ffmpeg_vaapi_tex` H.264
encoder at CBR 12 Mbps, 1920×1080, 60 fps, with detailed marked footage. Runtime
logs verified VAAPI encoding. Chromium required GPU rendering; windowed Firefox
146.0.1's `about:support` verified hardware WebRender, active Intel ARL 0x7d67 and
Mesa/iris. This verifies Firefox compositing, not hardware H.264 decoding.

Each session had both viewers: Chromium measured 150 seconds and Firefox 120
seconds, each after 20 seconds of stabilization. Chromium used continuous incoming
samples and the RTP compositor cadence experiment; Firefox used continuous incoming
samples. These diagnostic browser builds do not establish stock-browser behavior.
The raw audio worker and sink-only Worklet were enabled. Network impairment was
confined to the private relay; loss means the same reordering plus 0.5% loss.

| Artifact | Chromium | Firefox |
| --- | --- | --- |
| `gpu-firefox-clean` | All gates pass | Video statistics and audio pass; cadence deviation 16.53 ms exceeds 12.5 ms |
| `gpu-firefox-cold` | All gates pass | All available gates pass |
| `gpu-firefox-loss` | Fail: 19 decoded marker skips, 41 presentation omissions, video lost counter +6 | Fail: 13 presentation omissions, maximum cadence deviation 60.07 ms |

OBS performance gates passed all three sessions. Firefox Web Audio passed all
three; decoded pixels and raw-track audio remain unavailable in its driver.
Chromium raw PCM passed all three; the loss run retained a separate 10 ms Worklet
PCM dropout. The primary harness's audio gate uses raw decoded PCM when available,
so its raw-audio pass must not be described as proof of flawless output playout.

The clean Firefox outlier advanced RTP by 1500 ticks (one 60 fps frame). Native
assigned render time advanced 17 ms, with the minimum delay unchanged at 300 ms,
while the observed presentation interval reached 33.2 ms. This differs from the
previous Chromium minimum-delay jump; it does not justify assigning Firefox the
same cause. Further compositor tracing is required.

## Buffer-feedback and instrumentation matrix

The follow-up single-viewer matrix uses the same VAAPI 1080p60 CBR 12 Mbps source,
120 seconds of measurement after 20 seconds of stabilization, continuous incoming
timing samples and the RTP compositor cadence experiment. Fixed controls hold
both receiver targets at the existing 300 ms value while retaining native A/V
synchronization. They do not increase the buffer. Traced runs enable native
receiver, compositor, A/V synchronization and audio FIFO diagnostics.

`analyze-receiver-buffer-sync.py` joins native release `ReceivedTime` to the same
encoded frame's browser `receiveTime`, checks clock spread and matching video
SSRC, and attaches nearby native decisions and application buffer writes to
measured cadence failures. The first clean control had 8,402 matching anchors
with zero offset spread. It had 17 synchronization adjustments and no cadence
failure; the reordered feedback control had 21 adjustments and no cadence
failure. The presence of a synchronization adjustment alone does not establish
the cause of a presentation anomaly.

Final controls use `VDONINJA_CAPTURE_ENCODED_FRAMES=0` to leave encoded media out of
the JavaScript transform while retaining relay and receiver-buffer controls.
Native verbose traces are also off in these controls. They cannot provide the
encoded-frame clock anchors required by the correlation analyzer. Raw audio
worker and Worklet PCM capture remain enabled, so these are reduced-instrumentation
controls, not completely uninstrumented playback.

| Artifact | Primary gates | Separate Web Audio |
| --- | --- | --- |
| `buffer-feedback-clean` | Pass | Pass |
| `buffer-feedback-cold` | Fail: lost counter +8; pixels and cadence pass | Pass |
| `buffer-legacy-cold` | Pass (worker raw audio is the primary audio basis) | Fail: three click events |
| `buffer-fixed-clean` | Pass | Pass |
| `buffer-fixed-cold-0` | Pass | Pass |
| `buffer-fixed-cold-1` | Fail: lost counter +9, cadence deviation 16.63 ms; pixels pass | Pass |
| `buffer-fixed-loss-0` | Fail: lost counter +12, 16 decoded skips, 40 presentation omissions | Pass |
| `buffer-fixed-loss-1` | Fail: lost counter +11, 10 decoded skips, 27 presentation omissions | Pass |
| `buffer-feedback-native-cold` | Fail: lost counter +11; pixels and cadence pass | Pass |

`buffer-fixed-cold-1` and `buffer-fixed-loss-1` had the encoded transform and verbose
native traces disabled. Their remaining failures show those probes are not a
necessary condition. The traced fixed cold start had 47 native synchronization
adjustments and no cadence failure. None of the five traced cases reproduced the
earlier cadence jump, so this sweep does not prove which native delay decision
caused that earlier failure. Fixed buffering is not a complete mitigation.

The last normal-feedback cold start also disabled encoded capture and verbose
native traces. All nine sessions passed OBS performance and worker raw PCM.
These are repeated two-minute controls, not an hours-long stability claim.

## Validation and remaining work

All 71 JavaScript measurement tests and 21 Python packet/timing/audio tests passed.
Changed Python scripts parse successfully and `git diff --check` passed. The two
trace-only Chromium rebuilds completed before runtime measurement. Shipping C++
and packaging were unchanged; the prior plugin validation and compatibility
baseline remain as documented in the refinement report. No release is cut.

The new tests cover Worklet full/partial PCM and missing-input silence, worker
PCM/timestamp preservation and cancellation, capture freeze ordering, passive
application errors, buffer writes and repeated track events, encoded-probe opt-out,
and matching versus mismatched native/browser clock and SSRC correlation.

The remaining receiver fix is still open. Next useful work is to capture a
failing cadence run with native synchronization/compositor tracing, then test a
justified timing change against repeated cold starts and loss without lowering
configured quality or removing pacing. Firefox needs compositor evidence for its
separate cadence outlier. The earlier intermittent reload negotiation stall did
not reproduce in 14 reloads and is not declared fixed. Actual sink audio failures
also remain distinct from corrected capture artifacts. Longer GPU soaks and
physical output/audio latency are not established by this sweep.

Ignored runtime artifacts remain local under `artifacts/gpu-linux/`; they include
passing and failing reports and PCM, rather than only successful recordings.
