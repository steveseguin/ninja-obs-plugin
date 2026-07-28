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
test. The following is a representative 1080p60 check:

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
under `artifacts/`.

Useful encoder checks:

- `VDONINJA_EXPECT_STREAM_ENCODER=apple_h264` verifies the Apple
  VideoToolbox hardware H.264 simple-mode encoder.
- `VDONINJA_EXPECT_STREAM_ENCODER=x264` verifies x264 in simple mode.
- `VDONINJA_EXPECT_ADVANCED_STREAM_ENCODER=com.apple.videotoolbox.videoencoder.h264`
  verifies Apple VideoToolbox software H.264 in advanced mode. Use ABR for this
  encoder; OBS does not expose CBR for the software VideoToolbox implementation.

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

## Reading the result

- `videoContinuityAnalysis` summarizes `getStats()` rates, loss, freezes,
  stalls, and inter-frame variance.
- `presentationContinuityAnalysis` uses
  `requestVideoFrameCallback()`. It detects viewer composition hitches, but a
  headless browser can occasionally miss a display callback even when every
  decoded picture is intact.
- `visualSequenceAnalysis` reads the embedded frame counter directly from each
  decoded `VideoFrame`. Zero marker errors is the strongest proof that the
  encoder-to-decoder picture sequence had no repeat, skip, reversal, or marker
  corruption.
- Audio `concealedSamples`, `concealmentEvents`, `packetsLost`, and
  `packetsDiscarded` expose repairs or gaps that may sound like clicks even
  when video remains intact.
- The OBS plugin publish summary records pacer queue depth and delay, NACK
  repairs, PLI/FIR requests, RTT, receiver loss, and audio timestamp steps.
  A growing pacer delay is a latency failure even if OBS reports zero skipped
  output frames.

For a clean local path, require stable WebRTC video and a perfect visual
sequence. Treat presentation-callback misses separately unless the target
browser and display environment are controlled. For relay and loss tests,
record the impairment rate and candidate pair; those tests intentionally
measure the failure boundary rather than assuming every run should pass.
