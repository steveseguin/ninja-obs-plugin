# Linux receiver timing isolation

Follow-up to [the sender and rendering investigation](linux-streaming-isolation.md). These are synthetic, real OBS/WebRTC runs, not simulated unit-test results. The remaining cold-start network failure is **not fixed** by this change.

## Conditions and results

OBS 32.2.2 loaded the repository module from an isolated plugin directory. The publisher used a marked 1280×720/30 fps MP4, x264 at 3 Mbps, a private coturn container, and the current VDO.Ninja alpha viewer. Only the container's network interface was impaired: `netem delay 25ms 8ms distribution normal`, with no configured loss except the explicitly lossy trial. Each measured interval lasted 30 seconds after 20 seconds of stabilization. Packet captures, sender reports, encoded-frame arrivals, decoded pixel markers and compositor callbacks were recorded separately.

Chromium was 145.0.7632.6; Firefox was 146.0.1. Firefox required Cisco OpenH264 2.6.0 before a codec-matched comparison was possible. The browser-control publisher was Chromium with the same marked source converted to Y4M, H.264, and a 3 Mbps maximum. Its congestion-controlled startup bitrate was lower than OBS's fixed bitrate; this is a material difference.

| Publisher → viewer / condition | Decoded pixels | Presentation result |
| --- | --- | --- |
| OBS → Chromium, clean | 901 valid, no missing/repeated markers | Pass, 30.001 fps |
| OBS → Chromium, reordering from connection start | 900 valid, no missing/repeated markers | Fail, 11 omitted source frames, 29.634 fps |
| Same, fixed browser buffer 300 ms | 900 valid | Fail, 23 omitted frames |
| Same, fixed browser buffer 1000 ms | 900 valid | Fail, 22 omitted frames |
| OBS → Chromium, clean startup then reordering | 901 valid | Pass, zero omissions, 30.018 fps |
| OBS → Chromium, reordering, fixed 500 kbps | 900 valid | Pass, zero omissions, 30.001 fps |
| OBS → Chromium, reordering, existing adaptive mode enabled | 900 valid | Fail, 16 omissions |
| Browser → Chromium, reordering, 3 Mbps maximum | 901 valid | Pass, 30.001 fps |
| OBS → Firefox H.264, clean | Probe unavailable | Pass, 29.999 fps |
| OBS → Firefox H.264, reordering | Probe unavailable | Fail, maximum cadence deviation 57.29 ms |
| Browser → Firefox H.264, reordering | Probe unavailable | Pass, 29.999 fps |

Additional browser-publisher runs passed with fixed 300/1000 ms targets and with 0.5% loss. The corresponding native-publisher lossy run failed both decoded and presentation continuity. Successful controls do not waive that failure. Firefox lacks the encoded/decoded stream probes used here: its presentation pass does not establish pixel-perfect delivery.

The native reordering repeat captured NACKs for **all 1,545 received video frames**, including startup. The adaptive-mode follow-up independently captured NACKs for all **1,599 received frames**. Its OBS render and output skip counters were both zero. Adaptive mode remained at the configured encoder rate in this trial; merely enabling it did not solve the problem. At fixed 500 kbps, 277 of 1,543 received frames had no NACK and presentation passed. Lower bitrate is a tested diagnostic control, not an automatic quality-preserving fix.

## What the timing evidence establishes

In the clean run, median encoded-frame-ready to composition time was about 293 ms. With cold-start reordering it was about 10 ms, despite a requested 300 ms buffer. Starting clean before applying reordering retained about 255 ms and passed. The fixed-500-kbps run retained about 289 ms. Sender-report NTP/RTP mapping drift remained approximately 5 ms; the observations do not indicate a seconds-long sender clock error.

The [Chromium 145 DEPS file](https://chromium.googlesource.com/chromium/src/+/145.0.7632.6/DEPS) pins WebRTC revision `b47e68e6966d5a5a0e4bc861ff364221600f31c3`. At that revision:

- [VideoStreamBufferController](https://webrtc.googlesource.com/src/+/b47e68e6966d5a5a0e4bc861ff364221600f31c3/video/video_stream_buffer_controller.cc) supplies incoming timing samples only for frames not marked delayed by retransmission.
- [TimestampExtrapolator](https://webrtc.googlesource.com/src/+/b47e68e6966d5a5a0e4bc861ff364221600f31c3/modules/video_coding/timing/timestamp_extrapolator.cc) cannot extrapolate before its first timing sample.
- [VCMTiming](https://webrtc.googlesource.com/src/+/b47e68e6966d5a5a0e4bc861ff364221600f31c3/modules/video_coding/timing/timing.cc) returns immediate render time when extrapolation is unavailable, before applying the requested minimum playout delay.

The source behavior, all-NACK traces, and cold/clean-start comparison strongly support receiver timing initialization starvation. We did not instrument Chromium's internal timing object, so this is a causal inference from external traces and matching source, not a direct observation of that internal state. Firefox's cadence failure is not proven to have the identical cause.

OBS spreads packets across each frame to respect its pacer. The browser sender's packets were more tightly grouped, and some early frames avoided retransmission. A direct browser control with SDP startup-bitrate hints still reported a target of only about 286 kbps; it did not reproduce fixed-3-Mbps startup and must not be represented as such.

No default bitrate, pacing budget, protection mode or quality threshold was changed. Increasing the viewer buffer and enabling existing adaptive mode were unsuccessful mitigations. The next production fix needs to preserve the configured quality and congestion limits and pass this cold-start test; removing pacing or silently reducing every stream's bitrate is not validated by these results.

## Reproduce and inspect

Use an idle, isolated OBS profile with authenticated WebSocket and a private TURN relay. The existing [network runner](../tests/run-linux-network-regression.sh) documents container setup. Never apply `tc` to the host interface. The following commands assume that disposable relay already exists as `ninja-turn-e2e` at `172.17.0.2`, with synthetic credentials `e2e` / `syntheticTest42`.

Build an opt-in trace module (the normal release option is OFF):

```bash
cmake -S . -B build-timing-trace -DBUILD_PLUGIN=ON -DBUILD_TESTS=ON \
  -DOBS_SDK_PATH=/path/to/obs-sdk -DBUILD_RTP_TIMING_TRACE=ON
cmake --build build-timing-trace --target obs-vdoninja vdoninja-rtp-timing-trace-tests
ctest --test-dir build-timing-trace -L diagnostics --output-on-failure
mkdir -p /tmp/ninja-native-timing
```

Launch the isolated OBS with `VDONINJA_RTP_TRACE_DIR=/tmp/ninja-native-timing` and verify its loaded module path. Headers are collected in bounded memory and written when the stream/handler closes. The trace records original outgoing RTP, plaintext sender-report mappings before SRTP encryption, and incoming NACK sequence numbers. It is not a complete retransmission packet log; use the packet capture for wire traffic. Overflow makes the analysis fail. Versioned traces distinguish unavailable NACK instrumentation from zero NACKs.

For the cold-start native trial, start packet capture and impairment **before** the viewer connects:

```bash
docker exec ninja-turn-e2e tc qdisc replace dev eth0 root netem \
  delay 25ms 8ms distribution normal
mkdir -p artifacts/timing-cold
sudo tcpdump -i docker0 -n -s 0 -U -w artifacts/timing-cold/network.pcap \
  'udp and host 172.17.0.2'
```

In another terminal, with the WebSocket URL/password exported:

```bash
VDONINJA_SOURCE_MODE=media-sequence \
VDONINJA_MEDIA_SEQUENCE_PATH=/tmp/ninja-counter720p30.mp4 \
VDONINJA_MEDIA_SEQUENCE_LOOP=0 \
VDONINJA_VIDEO_WIDTH=1280 VDONINJA_VIDEO_HEIGHT=720 \
VDONINJA_VIDEO_FPS_NUMERATOR=30 VDONINJA_VIDEO_FPS_DENOMINATOR=1 \
VDONINJA_VIDEO_BITRATE_KBPS=3000 \
VDONINJA_SOAK_MS=30000 VDONINJA_VIEWER_STABILIZE_MS=20000 \
VDONINJA_VIEW_BUFFER_MS=300 VDONINJA_CAPTURE_RTP_TIMING=1 \
VDONINJA_BASE_URL=https://vdo.ninja/alpha/ VDONINJA_FORCE_TURN=1 \
VDONINJA_ICE_SERVERS='turn:172.17.0.2:3478|e2e|syntheticTest42' \
VDONINJA_VIEWER_ICE_SERVERS_JSON='[{"urls":"turn:172.17.0.2:3478","username":"e2e","credential":"syntheticTest42"}]' \
VDONINJA_REQUIRE_VISUAL_SEQUENCE=1 VDONINJA_REQUIRE_PRESENTATION_CONTINUITY=1 \
VDONINJA_REQUIRE_STABLE_VIDEO=1 VDONINJA_REQUIRE_OBS_PERFORMANCE=1 \
VDONINJA_MAX_LOST_VIDEO_PACKETS=100 VDONINJA_OUTPUT_DIR=artifacts/timing-cold \
node scripts/obs-websocket-vdoninja-publish-check.cjs TimingCold false
```

Use marked synthetic input in the `counter-complement` format, at least 75 seconds long. The 100-packet allowance accommodates temporarily missing packets during deliberate reordering; all frame-quality gates remain strict. Stop tcpdump with Ctrl-C after the harness has stopped streaming, move that stream's CSV files into the output directory, then run:

```bash
python3 scripts/analyze-rtp-timing.py artifacts/timing-cold
```

For the warm-start control, remove the relay qdisc before connection and apply it only when the harness logs `capturing every browser-presented video frame`. Change only `VDONINJA_VIDEO_BITRATE_KBPS=500` for the low-bitrate control. Set `VDONINJA_FIXED_VIEW_BUFFER=1` to freeze browser buffer writes. Set `VDONINJA_ADAPTIVE_BITRATE=1` to test existing adaptive mode.

For the browser-publisher control, convert the marked MP4 using `ffmpeg -i INPUT.mp4 -pix_fmt yuv420p OUTPUT.y4m`, leave the same relay impairment in place, and run:

```bash
ISOLATION_Y4M=/path/to/OUTPUT.y4m ISOLATION_WARMUP_MS=20000 \
ISOLATION_OUTPUT=artifacts/browser-control \
node scripts/browser-network-isolation.cjs
```

`ISOLATION_ICE_JSON` overrides the synthetic relay above. `ISOLATION_BROWSER=firefox` selects the viewer (publisher remains Chromium); the script rejects missing or incorrectly negotiated H.264. An externally supplied OpenH264 installation can be selected with `MOZ_GMP_PATH`, following [Mozilla's GMP layout](https://wiki.mozilla.org/GeckoMediaPlugins). `ISOLATION_MODE=direct` bypasses VDO signaling/application code for a minimal WebRTC control. Raw reports identify unsupported probes explicitly. Stop/remove the disposable TURN container afterward.

## Rendering and validation limits

The hardware-backed high-detail 1080p60 test could not run: this host exposes neither `/dev/dri` nor NVIDIA devices, and an explicit NVENC encode failed with `CUDA_ERROR_NO_DEVICE`. The prior OBS-clocked software 1080p60 pixel pass remains valid; it does not establish high-detail hardware performance or resolve the measured software workload limit.

This change adds Linux CI coverage for the opt-in real-libdatachannel trace wrapper, packet/feedback analysis, and browser-probe regressions. The release module is rebuilt separately with tracing OFF. The upstream ARM fixture include fix and native cached-keyframe timestamp rebase were reviewed and integrated; the latter affects native viewing, not the Chromium presentation path investigated here.

Final combined-tree validation passed 728 C++ tests normally and under ASan/UBSan, 44 JavaScript checks, nine Python packet/timing checks, three real-libdatachannel diagnostic cases, nine release-linked cases, and 13 installer/package regressions. `scripts/release.ps1 -Action verify` passed. A final 30-second clean live run loaded the tracing-OFF release module into OBS 32.2.2, connected browser and native viewers, and passed decoded-marker, presentation, and OBS performance gates. Native audio/video decoding was observed in OBS logs; this run does not independently certify native-viewer pixel continuity. The impaired cold-start failure above remains open.

## GPU-host continuation

The [Intel GPU follow-up](linux-gpu-receiver-followup.md) reproduces the cold-start
failure, instruments the pinned upstream timing component, tests a bootstrap
candidate and a receiver NACK-delay control, and adds real hardware-backed
1080p60, multiple-viewer and reload measurements. It also documents measurement
fixes and the remaining sustained presentation/audio limits. The production
cold-start failure remains open.
