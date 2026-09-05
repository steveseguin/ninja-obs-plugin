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
