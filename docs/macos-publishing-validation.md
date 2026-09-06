# macOS Publishing Validation

The OBS publishing check can validate the complete path from an OBS encoder to
a browser viewer. It measures WebRTC counters, every browser presentation
callback, and a deterministic frame number embedded in the decoded pixels.
That last measurement catches repeated, skipped, backwards, or corrupted
pictures even when OBS and WebRTC both report zero dropped frames.

## Generate a deterministic source

```bash
node tests/tools/generate-publish-sequence-video.cjs \
  --duration 75 \
  --fps 60 \
  --output artifacts/publish-sequence-75s.mp4

node scripts/analyze-publish-sequence-video.cjs \
  artifacts/publish-sequence-75s.mp4
```

The offline analysis must report zero invalid, duplicate, backwards, and
skipped markers before the file is used to judge a publishing run.

## Run a publishing check

Enable and authenticate OBS WebSocket, then launch OBS with the plugin under
test. On macOS, create the first Browser Source through the OBS interface, or
load a saved scene containing one, before creating browser sources through
WebSocket. OBS 32.2.2 can initialize CEF on the WebSocket worker thread when
the first browser source is created remotely; subsequent browser sources then
stay black. Restart OBS with a saved Browser Source to initialize it on the UI
thread. This also applies to the browser receiver fallback.

For browser receive audio checks with `obs-websocket-vdoninja-source-check.cjs`,
set `VDONINJA_SKIP_CAPTURE=0` as well as `VDONINJA_CHECK_AUDIO_METER=1`. The default
browser smoke check skips rendering capture and its startup wait.

The following is a representative 1080p60 check:

```bash
OBS_WEBSOCKET_PASSWORD='your-local-password' \
VDONINJA_STREAM_ID="macPublish$(date +%s)" \
VDONINJA_PASSWORD=false \
VDONINJA_SOURCE_MODE=media-sequence \
VDONINJA_MEDIA_SEQUENCE_PATH="$PWD/artifacts/publish-sequence-75s.mp4" \
VDONINJA_VIDEO_WIDTH=1920 \
VDONINJA_VIDEO_HEIGHT=1080 \
VDONINJA_VIDEO_FPS_NUMERATOR=60 \
VDONINJA_VIDEO_FPS_DENOMINATOR=1 \
VDONINJA_VIDEO_BITRATE_KBPS=8000 \
VDONINJA_EXPECT_STREAM_ENCODER=apple_h264 \
VDONINJA_SOAK_MS=60000 \
VDONINJA_VIEWER_SAMPLE_MS=250 \
VDONINJA_REQUIRE_STABLE_VIDEO=1 \
VDONINJA_REQUIRE_ZERO_FREEZES=1 \
VDONINJA_REQUIRE_VISUAL_SEQUENCE=1 \
node scripts/obs-websocket-vdoninja-publish-check.cjs
```

The script temporarily changes the requested OBS canvas, output, FPS, and
simple-mode bitrate, then restores them. Reports and screenshots are written
under `artifacts/`. The measured window starts after live video has decoded
another second of frames; a cached initial keyframe alone is not steady-state
playback. PCM capture starts after this warmup too.

Useful encoder checks:

- `VDONINJA_EXPECT_STREAM_ENCODER=apple_h264` verifies the Apple
  VideoToolbox hardware H.264 simple-mode setting.
- `VDONINJA_EXPECT_STREAM_ENCODER=x264` verifies x264 in simple mode.
- `VDONINJA_EXPECT_ADVANCED_STREAM_ENCODER=com.apple.videotoolbox.videoencoder.h264`
  verifies Apple VideoToolbox software H.264 in advanced mode. Use ABR for this
  encoder; OBS does not expose CBR for the software VideoToolbox implementation.

Changing a saved encoder setting through WebSocket does not necessarily replace
OBS's existing encoder. Restart OBS after changing encoder type and verify the
encoder actually used in the OBS log, including the VideoToolbox hardware-session
message. The expected-encoder checks alone verify configuration, not runtime use.

VDO.Ninja plugin publishing currently advertises H.264 only. HEVC, AV1, VP9,
and ProRes encoder availability in OBS does not mean those codecs are valid for
this browser publishing output.

## Multi-viewer pressure

Add real OBS Browser Source viewers while retaining the instrumented Chromium
viewer:

```bash
VDONINJA_OBS_BROWSER_VIEWER=1 \
VDONINJA_OBS_BROWSER_VIEWER_COUNT=3 \
VDONINJA_OBS_BROWSER_VIEWER_BITRATES_KBPS=8000,4000,1000
```

This produces four concurrent viewers in total. The different receive targets
help expose per-peer pacing or feedback interactions.

## TURN and packet-loss pressure

`VDONINJA_FORCE_TURN=1` adds `&relay` to the browser URL and configures the
plugin for relay-only ICE. Pair it with
`VDONINJA_REQUIRE_RELAY_CANDIDATE=1`; a passing run then requires the selected
candidate pair to contain a relay candidate.

Custom servers use one entry per line:

```text
turn:turn.example.test:3478|username|credential
```

TURN credentials should be supplied at runtime from a protected local secret,
never committed. VDO.Ninja-hosted credentials can be ephemeral and must be
refreshed when expired.

The UDP proxy can introduce deterministic loss between the plugin and a UDP
TURN server:

```bash
node scripts/udp-loss-proxy.cjs \
  --remote-host turn.example.test \
  --remote-port 3478 \
  --local-port 34790 \
  --bind-address 0.0.0.0 \
  --drop-every 200 \
  --warmup-ms 7000
```

Point the plugin's custom TURN URL at the Mac's LAN address and port `34790`.
`--drop-every 200` drops 0.5% of datagrams in each active direction after
warmup. Compare protection-off against
`VDONINJA_VIDEO_PROTECTION_MODE=3 VDONINJA_AUDIO_RED=1`.

## BrowserStack remote viewers

The publishing check can start a BrowserStack viewer while the local
frame-accurate viewer runs. Keep credentials in a mode-600 environment file:

```bash
VDONINJA_BROWSERSTACK_PROFILE=android-s25-chrome \
VDONINJA_BROWSERSTACK_SECRET_FILE=/path/to/browserstack.env \
VDONINJA_BROWSERSTACK_REQUIRE_CANDIDATE_TYPE=relay \
VDONINJA_BROWSERSTACK_REQUIRE_PLAYBACK_CLOCK=1 \
VDONINJA_BROWSERSTACK_REQUIRE_ZERO_AUDIO_CONCEALMENT=1 \
VDONINJA_BROWSERSTACK_EXPECTED_MEDIA_KBPS=8000 \
node scripts/obs-websocket-vdoninja-publish-check.cjs
```

Supported profiles are `win-chrome`, `win-edge`, `win-firefox`,
`mac-chrome`, `mac-webkit`, `android-pixel9-chrome`,
`android-s25-chrome`, and `ios-iphone15-safari`.

`VDONINJA_BROWSERSTACK_REQUIRE_PLAYBACK_CLOCK=1` requires the remote video
element's playback clock to advance, in addition to transport decode. This
prevents an autoplay-blocked or paused iOS element from being reported as
visible playback merely because WebRTC continues decoding frames.
`VDONINJA_BROWSERSTACK_REQUIRE_ZERO_AUDIO_CONCEALMENT=1` also rejects any
newly lost audio packet, concealed sample, or concealment event in a measured
remote phase.

Pixel/Samsung per-frame canvas reads can consume enough device or cloud-screen
capacity to lower the measured presentation rate. Use the lightweight remote
probe for an unperturbed playback-clock/getStats control:

```bash
VDONINJA_BROWSERSTACK_REQUIRE_PRESENTATION=0 \
VDONINJA_BROWSERSTACK_REQUIRE_VISUAL_SEQUENCE=0 \
VDONINJA_BROWSERSTACK_CAPTURE_PRESENTATION=0
```

Keep the local `VDONINJA_REQUIRE_VISUAL_SEQUENCE=1` gate enabled in the same
run. It proves the exact encoder-to-decoder frame sequence while the remote
device proves codec negotiation, relay selection, playback-clock movement,
loss, NACK, freeze, and audio-concealment behavior.

BrowserStack's network-update API returning success is not proof that its
limits affected WebRTC media. The remote report labels the effect `observed`
only when receiver statistics show the requested cap or loss. Use the UDP TURN
proxy for deterministic packet-loss conclusions.

## Reading the result

- `videoContinuityAnalysis` summarizes `getStats()` rates, loss, freezes,
  stalls, and inter-frame variance.
- `presentationContinuityAnalysis` uses
  `requestVideoFrameCallback()`. It detects viewer composition hitches, but a
  headless browser can occasionally miss a display callback even when every
  decoded picture is intact.
- `visualSequenceAnalysis` reads the embedded frame counter directly from each
  decoded `VideoFrame`, using a bounded eight-frame inspection queue so short
  delivery bursts do not overflow the default single-frame queue.
  Zero marker errors is the strongest proof that the
  encoder-to-decoder picture sequence had no repeat, skip, reversal, or marker
  corruption.
- Audio `concealedSamples` and `concealmentEvents` expose repairs that may
  sound like clicks even when video remains intact. Packet loss and discards
  remain diagnostics: RED can recover a missing primary packet without
  concealment, and Chromium counts unused RED copies as discarded even on a
  lossless path. Neither packet counter alone fails the local PCM/zero-concealment
  gate; raw decoded PCM is checked separately. The additional
  WebAudio capture crosses another audio-clock/processing boundary and can
  contain discontinuities even in a browser-only tone control; its result is
  reported separately when raw track capture is available.
- The OBS plugin publish summary records pacer queue depth and delay, NACK
  repairs, PLI/FIR requests, RTT, receiver loss, and audio timestamp steps.
  A growing pacer delay is a latency failure even if OBS reports zero skipped
  output frames.

For a clean local path, require stable WebRTC video and a perfect visual
sequence. Treat presentation-callback misses separately unless the target
browser and display environment are controlled. For relay and loss tests,
record the impairment rate and candidate pair; those tests intentionally
measure the failure boundary rather than assuming every run should pass.

Confirm that the UDP proxy reports substantial media traffic and nonzero drop
counts during an impaired run. A selected relay candidate alone does not prove
that media passed through the configured proxy. The proxy keeps separate
upstream sockets for separate clients, including concurrent ICE gathering sockets.

Very large encoder keyframes can still cause periodic stalls on a lossless
path. In a 720p30, 4 Mbps textured-scene test, roughly 550 KB Apple CBR keyframes
took about 560 ms to traverse the 8 Mbps pacer. For comparison, x264 with a
400 kbit custom buffer at 4,000 kbps (100 ms) kept keyframes below 50 KB and
passed the same warmed-up video/audio test. A smaller encoder buffer trades
picture quality on difficult frames for lower latency; it is an explicit encoder
tuning option, not a plugin default. The plugin's bounded pacing is unchanged.

OBS 32.2.2's VideoToolbox ABR limit has a separate upstream issue: it creates
`CFNumber` values for two `double` variables using integer/float types. A
50,000-byte / 0.1-second request becomes zero bytes and a negative interval.
A standalone hardware control using the correct double types applied the limit
and reduced later keyframes without dropping input frames, but retained a large
initial keyframe. Do not assume the OBS limit works merely because its property
setter returns success, or work around it by silently overriding encoder quality.

Adaptive bitrate is driven by fresh receiver REMB estimates. It cannot be
assumed to remedy reordering or loss when the receiver still advertises enough
bandwidth. Similarly, inspect the actual video receiver buffer target when
trying `buffer=`; additional buffering did not eliminate the tested combined
loss/reordering failure. Keep these as explicit workload choices.

NACK repairs keep their existing 500 ms deadline while waiting for the shared
viewer pacing budget. An expired wait is cancelled and reported through the
normal repair-expiry path; it must not transmit obsolete repair traffic afterward.

Repeated NACK feedback now prioritizes the packet's existing queued repair.
Packet traces showed one-off requests from reordering otherwise held missing
packets behind nearly 500 ms of repair backlog. Prioritization retains the
original deadline, queue bound, media fairness and bandwidth budgets. In a
matched 45-second 0.5% datagram-loss plus 15–40 ms reordering test, decoded rate
improved from 39.60 to 58.66 fps and freeze duration fell from 31.489 to 5.291
seconds. Both still failed the zero-freeze gate. Clean and loss-only controls
passed before and after; this is a mitigation, not a guarantee against jitter.

## September 5, 2026 screen-capture stress follow-up

On an M1 MacBook Air with 8 GB RAM and OBS 32.2.2, a real ScreenCaptureKit
window source sustained 1080p60 at 8 Mbps for 180 seconds with three OBS Browser
Source viewers, a fourth instrumented Chromium viewer, and local recording.
Six verified CPU workers, continuous Metal work, and 768 MiB of actively touched
memory ran concurrently. The receiver decoded 10,800 frames, with no reported
video loss, dropped frames, or freezes. A 90-second 30 fps control also passed
transport continuity. These are synthetic workload results, not certification
of a particular game or long-term thermal behavior.

During the central sampled portion of the 60 fps stress run, OBS used a median
13.55% of the eight-core CPU capacity, 237 MiB of reported memory, and 8.42 ms
render time (11.87 ms p95). The OBS process tree's summed RSS was about 959 MiB;
this includes shared pages more than once and excludes the separate load tools
and instrumented Chromium. A system snapshot reached zero CPU idle. Sampled
thermal states remained nominal, without a physical temperature measurement.

When testing screen capture, compare receiver frame markers with a simultaneous
local recording. Source/display/capture clocks can repeat or skip source images
even when every encoded image reaches the receiver. A 60 fps fixture sampled at
30 fps also necessarily skips counter values; that alone is not transport loss.
Keep capture cadence, OBS render/encode skips, and WebRTC drops/freezes separate.

An isolated OBS H.264 encoder A/B also confirmed that correcting the VideoToolbox
ABR `CFNumber` types reduced freeze duration on the difficult 720p30 noise test
from 12.727 to 1.318 seconds over 60 seconds. Both variants still failed the
zero-freeze gate. The installed OBS application and plugin encoder defaults were
not changed; the result does not justify bundling a replacement OBS encoder.

Windows GameCapture hardware texture sharing and device-specific behavior still
require a Windows host. Mac native alpha tests do not certify those features.

## July 28, 2026 validation campaign

The release validation host was an Apple M1 MacBook Air with 8 GB RAM, which is
less capable than the reported M1 Pro/16 GB system. The plugin was built
against the OBS 32 compatibility baseline and loaded by OBS 32.1.0-rc1.

- Apple hardware H.264 sustained 1080p60 at 20 Mbps for 60 seconds with
  3,598 decoded frames, no loss/drop/freeze, and 3,597 perfect marker
  comparisons.
- Apple software VideoToolbox H.264 and x264 each sustained 1080p60 at 8 Mbps
  for 60 seconds with a perfect decoded marker sequence.
- Apple hardware H.264 at 500 kbps still delivered 1,798 frames in 30 seconds
  with no loss/drop/freeze or marker error.
- A deterministic 0.5% UDP loss run through forced TURN broke the unprotected
  stream: three source frames were skipped, one 172 ms freeze occurred, and
  audio concealment was recorded.
- With High packet duplication and Opus RED, the corrected aggregate pacer
  sent every scheduled copy without expiration. The same loss rate delivered
  all 1,800 expected video markers in order, zero video loss/freeze, and raw
  decoded audio with no dropout, click, or concealment.
- BrowserStack capability/recovery coverage passed Windows Chrome/Edge/Firefox,
  macOS Chrome/WebKit, Pixel 9 Chrome, Galaxy S25 Ultra Chrome, and iPhone
  Safari. The protected S25 plug-in viewer advanced 15.196 seconds while
  decoding 909 1080p60 frames over relay-to-relay UDP, with zero video
  loss/freeze and zero new audio concealment.
- The dedicated real-iPhone Safari WebDriver test proved bidirectional
  audio/video playback clocks. The plug-in-specific WebDriver run proved
  1080p60 playback and zero video loss/freeze, while also detecting a
  241-sample audio concealment event in one run. A second strict run decoded
  900 measured frames in 15.025 seconds, advanced the playback clock by
  15.027 seconds, and passed with zero video loss/freeze, audio packet loss,
  concealed samples, or concealment events over relay-to-relay UDP. Generic
  Playwright-over-CDP iPhone automation can decode while autoplay remains
  paused, so that path is transport evidence unless the playback-clock gate
  also passes.

## System CPU meter validation

The dock's system CPU meter samples aggregate operating-system busy/idle counters,
not OBS process CPU. On macOS, each `mach_host_self()` reference must be released
after reading processor information. A regression reproduced 100 leaked port
references in 100 samples; the corrected sampler retains a constant reference
count, including over a separate 1,000-sample probe.

In a controlled six-worker load test, mean sampler/system-`top` readings were
28.14%/29.49% before load, 89.62%/87.24% under load and 14.10%/14.47% after load.
Their independent one-second windows are not synchronized, so compare steady
intervals rather than demanding identical individual samples. OBS's separate
process CPU statistic has a different meaning and should not be used as the
reference for this meter.

## Expanded encoder and audio matrix

A subsequent 1080p60 / 8 Mbps matrix used native plugin reception, one OBS Browser
Source viewer, and an instrumented Chromium viewer concurrently. Hardware VT CBR
and x264 CBR each passed Off/Low/Medium/High protection with continuous decoded
video and audio. Hardware VT ABR/CRF and x264 ABR/VBR/CRF also passed their tested
30-second controls. x264 used the veryfast preset. These are representative
rate-control tests, not every possible preset, tune or custom encoder string.

Software VT ABR did not meet the same 1080p60 criteria: source-image repeats/skips
and audio concealment appeared despite no reported receiver video drops/freezes.
Its simultaneous local recording also contained 1,221 repeated markers and 1,216
skipped markers over 2,446 comparisons. That places a cadence problem before
network delivery. A 720p60 single-viewer control passed, with all 2,198 local
recording comparisons in order; 720p30 also passed transport/audio checks. Keep
hardware H.264 or the tested x264 settings for the heavier workload rather than
silently changing the software encoder's quality or buffering.

All four video-protection modes passed separate 30-second loss-only controls
with every 200th proxy datagram dropped after warmup and Opus RED enabled. This
does not remove the documented combined loss/reordering limitation. Higher
protection consumes additional network bandwidth and CPU.

A native macOS app playing a known tone was captured using application-only
ScreenCaptureKit audio, with the OBS media-file audio muted. Publishing passed
raw decoded-audio continuity and zero-concealment checks. Native H.264 and VP9
reception passed source/audio activity checks. Native VP8 and AV1 offers were
rejected as unsupported; browser-mode H.264, VP8, VP9 and AV1 passed their audio
checks (and screenshot checks where requested). H.264 recordings from native
and browser reception each passed an 11.25-second analyzed PCM window without
dropouts, click candidates or clipping. Physical microphone quality was not
certified by these synthetic tests.

When requesting mixer-audio checks without screenshots, the source-check tool
now waits for the requested observation period. Previously browser mode could
fail with zero audio-meter samples immediately after creating the source.


## Cached startup frames and native audio timing

A longer four-viewer Mac test exposed a native receiver timing error missed by
short checks. A cached keyframe with RTP timestamp 0 was followed about 17 ms
later by the live GOP at timestamp 180000. Anchoring both to the cached frame's
arrival scheduled live video almost two seconds into the future. OBS subsequently
increased global audio buffering from 64 ms to 960 ms, with a corresponding
roughly 0.9-second stream freeze and audio concealment.

The receive timestamp mapper now rebases timestamps more than 250 ms ahead of
arrival, retaining monotonic output. Ordinary burst spacing, real elapsed gaps,
RTP wrapping, and exact primary/alpha pairing remain covered by tests. The
correction changes the local output clock, not RTP values or paired-frame identity.

Matched instrumented five-minute runs reduced p95 video timestamp lead from
1982.30 ms to 2.49 ms. The corrected OBS session stayed at its 85 ms startup audio
buffer, and all four complete minute PCM windows passed. Its strict overall media
gate still failed during concurrent builds in the final minute; drops/freezes
first appeared after 251 seconds. This establishes the timing correction, not a
claim of frame-perfect playback under unlimited local load. Private traces and before/after results are in
`artifacts/obs-soak-20260905/`.

## Two-hour Mac screen-capture soak

The September 5 follow-up completed 7,200 seconds of uninterrupted publishing
using the corrected release-mode plugin, official OBS 32.2.2, and an 8 GB M1 MacBook
Air. ScreenCaptureKit captured a moving, marked native-app window; a separate
native tone app supplied application-only ScreenCaptureKit audio. Hardware VT
H.264 CBR used 1080p60, 8 Mbps, two-second keyframes, High video protection, and
Opus RED. Adaptive bitrate remained off. Four viewers ran on the same Mac: one
native plugin source at 640×360 output, two OBS Browser Sources, and an instrumented Chromium viewer.
Their local decode/render overhead is part of these measurements; this is not a
measurement of four viewers running on separate computers or of an actual game.

Each phase lasted 15 minutes. CPU is whole-system utilization; FPS and freezes
are from the external viewer. Audio failures count approximately minute-long
raw decoded-track PCM windows, with a separate final window described below.

| Phase | System CPU mean | Decoded FPS | OBS render skips | Frozen seconds | Failed PCM windows |
| --- | ---: | ---: | ---: | ---: | ---: |
| Baseline | 92.8% | 59.95 | 0.95% | 0.770* | 1* |
| Three CPU workers, 384 MiB | 99.4% | 58.93 | 0.54% | 7.281 | 0 |
| GPU workload | 97.3% | 59.24 | 6.99% | 7.161 | 1 |
| Combined CPU/GPU | 99.5% | 58.13 | 1.80% | 12.941 | 0 |
| Six CPU workers, 768 MiB, continuous GPU | 99.8% | 33.29 | 0.33% | 216.757 | 0 |
| Reduced combined load | 99.4% | 58.43 | 1.32% | 11.940 | 0 |
| Synthetic load removed | 97.4% | 59.92 | 2.90% | 0.179 | 0 |
| Continued recovery | 97.0% | 60.02 | 3.38% | 0.000 | 0 |

*A five-second macOS stack-sampling operation coincided with the baseline freezes
and a 310 ms PCM dropout. Raw totals retain this measurement interference;
excluding elapsed 430–445 seconds removes those freezes, but does not remove
other baseline render skips, receiver drops, or minor concealment. No further
stack sampling or builds ran during the soak.

The strict overall media gate failed: 8,237 receiver frame drops, 110 freezes
totaling 257.029 seconds, 241 ms maximum observed RTP jitter, 9,835 OBS render
skips, and 88 encoder skips. Publisher logs show receiver bandwidth estimates
falling below the fixed encoder rate, followed by bounded video queues reaching
roughly four seconds. Both OBS browser viewers also froze. The stream remained
connected, and video recovered when load decreased. The last 15 minutes had no
external-viewer drops or freezes, although OBS render skips continued.

Of 114 rolling raw PCM windows, two failed: the profiling interval and the initial
GPU transition (220 ms dropout and one click candidate). The final 69.39-second
raw window passed. Together these analyzed about 6,904.87 seconds of audio;
restart/trim gaps were not PCM-certified. Continuous RTP statistics recorded
89,863 concealed samples and 26 concealment events, so zero-concealment failed.
The final Web Audio playout capture also detected five click candidates despite
clean raw decoded samples. No claim of click-free audible output or measured
acoustic lip-sync follows from the raw-track checks. Browser estimated audio/video
playout offsets are retained as telemetry, not acoustic synchronization proof.

Final flushed OBS logs show audio buffering rose from 64 to 597 ms during stack
sampling and reached OBS's 960 ms maximum during the GPU transition, with source
audio restarts. The cached-startup timestamp fix does not prevent scheduling-load
buffer growth. Changing encoder quality, source timing, or audio buffering
defaults to conceal these failures would need separate validation.

OBS-reported memory stayed between approximately 348 and 489 MiB, without sustained
growth. The thermal monitor reported `fair` throughout its observed interval;
no absolute temperature was measured. Native Mac reception used software H.264
decode; VideoToolbox hardware publishing does not imply hardware native decoding.
The screen fixture loops and source-image uniqueness was not certified for this
soak. Windows GameCapture hardware behavior and actual macOS 13 execution remain
unverified on this Mac.

## Balanced repair-scheduler comparison

After the soak, eight 60-second runs compared the previous `c4c5fb7` scheduler
with the integrated repair changes from `61c80c8`, using identical private
dependency libraries. The order was old/new/new/old/new/old/old/new. A verified
TURN relay carried real media through a loopback proxy that dropped every 200th
datagram in each direction after its seven-second warmup and independently
delayed packets by 15–40 ms, creating reordering. The synthetic CPU/GPU loads
and screen/audio fixture apps were stopped. Hardware H.264 remained 1080p60 at
8 Mbps with High protection and Opus RED; this comparison used marked file input
and one external viewer.

| Four runs per scheduler | Previous | Updated |
| --- | ---: | ---: |
| Mean decoded FPS | 58.99 | 59.75 |
| Mean decoded-frame drops per run | 11.25 | 1.50 |
| Frozen seconds per run, range | 6.972–9.041 | 0 |
| Mean browser presentation FPS | 29.27 | 43.90 |
| Missing decoded source markers per run, range | 41–77 | 7–21 |

All eight strict media gates still failed. Every updated run avoided detected
freezes, but source-marker omissions and presentation failures remained. OBS
reported no render or encoder skips in these comparisons, and decoded raw PCM
passed throughout. Some runs still had audio concealment or Web Audio click
candidates. Proxy logs confirm substantial media traffic and actual injected
drops. These results support retaining the repair scheduling mitigation, not
increasing its budgets or claiming that combined loss/reordering is resolved.

Separate 30-second clean and loss-only controls passed the configured decoded
video, source-marker, and raw-audio gates with both schedulers: no decoded drops,
freezes, missing markers, or OBS render/encoder skips. The optional stricter
compositor analysis still found small presentation omissions in these controls,
and some Web Audio captures contained click candidates. These control passes
must not be presented as certification of every playback metric.

## Recording-only software encoder isolation

With plugin publishing and all viewers disabled, the detailed marked file at
1080p60 still failed Apple software VT ABR: the analyzed recording contained
108 repeated and 108 skipped source markers, and OBS reported 382 output skips
with zero render skips during the sampled interval. This reproduces a problem
before plugin network delivery. Hardware VT CBR at 1080p60, software VT ABR at
720p60, and software VT ABR with a simple OBS-clocked 1080p60 counter each passed
1,078 recorded-marker comparisons and had no OBS render/output skips in repeated
controls. The clock fixture uses simple graphics; it does not certify complex
game content at the same resolution.

The initial recording extractor added one boundary duplicate after input seeking
because FFmpeg applied output frame pacing. `-fps_mode passthrough` removed that
artifact from the preserved software recording, and all three controls were
recorded and checked again with corrected extraction. The substantial software
1080p failure remained; the disputed recording and corrected control recordings
are retained privately.

Recorded hardware-1080p and software-720p audio passed their 17.25-second PCM
windows. The first simple-clock software recording also passed audio, while its
repeat had six short dropout windows (20 ms maximum), with no click candidates
or clipping. Publishing was inactive in every sample. These observations do not
justify a plugin-side encoder workaround or silently changing software-mode
quality settings; software VT's usable workload remains content-dependent.

## Adaptive bitrate and measurement teardown

Four primary 120-second runs used adaptive off/on/on/off with three CPU workers,
384 MiB of added memory pressure, and the moderate GPU workload. The file-based
publisher fed the external viewer, two OBS browser viewers requesting 2/8 Mbps,
and the 640×360 native viewer requesting 800 kbps. This shorter test did not
reproduce the long screen-capture soak's video failure: all four runs had zero
decoded drops, freezes, missing source markers, and OBS render/encoder skips.

Adaptive mode correctly selected the minimum fresh receiver estimate. Logs show
the hardware encoder target stepping 8000→4000→2000→1000→720 kbps and returning
to 8000 kbps after each adaptive session. The small native viewer was the limiting
request. This is a real quality tradeoff affecting all viewers, not a validated
reason to enable adaptive mode by default. One on-run had a 10 ms raw PCM dropout;
the other three raw PCM checks passed. All four Web Audio captures still contained
click candidates, and strict compositor checks found small presentation omissions.
These results verify adaptation/restoration, not universal smooth playback.

An exploratory batch exposed a harness error: video capture was serialized before
audio collection stopped. Three runs reported a single raw PCM click around the
120-second endpoint. Freezing both collections before either export removed those
endpoint clicks in the four primary runs while still detecting the in-window
10 ms dropout. The shared regression tests verify that collection stops before either reader
cancellation, both readers drain before export, disabled probes are supported,
and the freeze function works when serialized into the browser. This correction changes
measurement teardown only; earlier raw observations remain retained.
