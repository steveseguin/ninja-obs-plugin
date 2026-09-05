# Linux streaming isolation and regression validation

This follow-up separates the two failures in [the initial Linux report](linux-e2e-validation.md). Tests use the same Ubuntu 24.04 / Ryzen 5 5500 host, official OBS 32.2.2, x264, synthetic sources and `https://vdo.ninja/alpha/`. No production latency, bitrate, protection or graphics defaults are changed.

## Packet reordering combined with loss

The isolated relay has no published host ports. All `netem` changes apply to its Docker network interface. The original freeze-count failure was reproduced with reordering and real loss; later pixel/presentation probes also detected a separate presentation-only failure with reordering alone:

| Experiment | Duration | Video freezes | Interpretation |
| --- | ---: | ---: | --- |
| Delay every second outgoing datagram by 20 ms, drop none | 45 s | 0 | Reordering alone triggers NACKs without disrupting video |
| Original 25 ± 8 ms variable delay, remove packet loss | 45 s | 0 | The original variable delay alone also passes |
| Original variable delay + 0.5% loss | 45 s | 24 | Reproduces the failure, with zero OBS render/output skips |
| Same impairment, change only viewer buffer to 300 ms | 45 s | 19 | More buffering alone does not fix the sender backlog |
| Same impairment, prioritize keyframe delivery | 45 s | 15 | Keyframe delivery improves, but repair expiry remains |
| Diagnostic experiment: double the fixed repair sub-budget | 60 s | 1 | Zero repair expirations, but one 211 ms freeze remains |
| Doubled fixed repair sub-budget, 300 ms viewer buffer | 60 s | 0 | One run passes frame timing, but repetition still finds a 291 ms freeze |

These randomized trials are diagnostic observations, not a statistical guarantee. The separately repeatable UDP proxy can impair either direction, introduce periodic reordering, apply constant delay, and inject periodic packet loss. Its loopback tests verify packet order, association preservation, direction isolation and invalid-option rejection.

Two sender issues were reproduced with focused C++ tests before changing production code:

1. **Repair packets interrupted keyframe delivery.** A paused first keyframe packet followed by four queued repairs produced `keyframe[0], repair[0..3], keyframe[1..2]`. Completing the original keyframe first shortens decoder recovery; older repairs cannot repair the prediction chain faster than delivering its replacement keyframe. In the live test, maximum keyframe send time fell from roughly 90–96 ms to 61 ms.
2. **The repair sub-budget left usable capacity idle.** A 20 KB repair burst expired six packets with a 100 KB/s overall pacer, despite capacity being available before the 500 ms expiry. The old repair allowance was one quarter of the overall pacer rate. Because the overall rate normally has twice the encoder's rate, that allowed repairs only half the media rate. Reordering generates many first-time NACKs for delayed originals, so genuine missing packets waited behind this avoidable backlog.

The final scheduler keeps the original quarter-rate repair allowance **while live media is queued**. When no live media is waiting, repairs can use otherwise-idle capacity through the unchanged overall pacer. This removes a second unnecessary wait without increasing the overall or shared aggregate rate. Borrowing an idle slot does not create artificial debt against future repair slots. Keyframes take priority, while delta-frame repair interleaving, finite queues and expiry remain bounded.

The tests cover a repair burst alongside live media, a larger idle repair burst, keyframe send order, repeated-NACK priority, shared-budget waiting, and true overload expiry. Simply doubling the fixed repair allowance was insufficient in a repeated live trial, so that intermediate change was replaced by using idle capacity rather than raising the busy-path allowance.

Network loss is still visible in RTP counters. An original packet and its repair can both be lost; a finite test does not guarantee flawless playback under arbitrary loss. The stress profile uses an explicit 300 ms viewer buffer. The app's low-latency default remains unchanged.

### Final candidate: remaining failure is reproduced, not waived

The final idle-capacity scheduler passed the clean control and the first 60-second impaired repetition. The second impaired repetition failed with **five freezes totaling 1.008 seconds**. Repair expiry remained zero; maximum repair delay was approximately 138 ms, and OBS reported no render/output skips. The sender backlog defect is fixed, but this does **not** establish a fully repaired end-to-end network path.

A second matched test used a marked 720p30 MP4 through the real plugin and private TURN relay:

| Condition, 30 seconds | Decoded pixel markers | Browser presentation | OBS |
| --- | --- | --- | --- |
| No impairment | All valid, zero skips/duplicates | Pass, approximately 29.98 fps | Zero render/output skips |
| 25 ± 8 ms delay, no loss | All valid, zero skips/duplicates | Fails: approximately 29.50 fps, 15 source frames omitted from composition | Zero render/output skips |
| 25 ± 8 ms delay, 0.5% loss, protection off | Two skipped markers | Approximately 27.77 fps, stalls | Zero render/output skips |
| Same impairment, High video protection and audio RED | Five skipped markers | Approximately 27.55 fps; one 215 ms reported freeze | Zero render/output skips |

The no-loss reordering control delivered every decoded pixel marker and passed the RTP video-continuity gate, but failed actual presentation cadence. This isolates a receiver-presentation symptom that cannot be explained solely by unrecovered packets or OBS encoder skips. High protection is therefore not a validated workaround. The impaired viewer's jitter-buffer target stayed near 288 ms while actual residence was about 50–60 ms; that observation does not prove why presentation stalls persist. Further isolation must distinguish receiver playout, sender timing and lost recovery data. The strict network runner intentionally continues to fail on the remaining defect. No production defaults or frame-quality thresholds were relaxed to obtain a pass.

The browser probe also had a separate measurement defect. [The requestVideoFrameCallback specification](https://wicg.github.io/video-rvfc/) defines callbacks as best-effort and `presentedFrames` as frames submitted for composition. A counter jump can mean JavaScript missed callbacks. The analyzer now uses compositor submission timestamps, the actual compositor counter, and, when available, the 90 kHz RTP source timestamp to detect missing source frames. Receiver `mediaTime` adjustments alone are not classified as source-frame loss. Missed callbacks remain visible as diagnostics; long observation gaps still fail the coverage/stall limit. Averaging an interval across missed callbacks does not establish exact cadence inside that unobserved interval. Independent decoded-pixel validation remains strict. Regression tests cover callback lateness, missed callbacks, real missing frames, RTP rollover and playout-clock adjustment. Raw callback and decoded-frame records are now saved alongside the report.

### Repeat the network regression

Load the candidate plugin into an **idle, isolated OBS profile**, enable authenticated OBS WebSocket, and provide at least 120 seconds of synthetic 720p30 motion footage. For example:

```bash
ffmpeg -f lavfi -i testsrc2=size=1280x720:rate=30 \
  -f lavfi -i sine=frequency=997:sample_rate=48000 -t 120 \
  -c:v libx264 -preset ultrafast -crf 23 -c:a pcm_s16le synthetic.mkv

OBS_WEBSOCKET_URL=ws://127.0.0.1:4466 \
OBS_WEBSOCKET_PASSWORD=YOUR_TEST_PROFILE_PASSWORD \
VDONINJA_MEDIA_SEQUENCE_PATH="$PWD/synthetic.mkv" \
bash tests/run-linux-network-regression.sh
```

The runner creates and removes its own TURN container, runs a clean control and three impaired repetitions, and saves reports and qdisc counters. It rejects frame drops, freezes, render/encoder skips, and excessive frame-timing variation. The clean control permits zero reported missing video packets; the deliberately lossy cases explicitly permit up to 100. This counter allowance does not relax any frame or timing criterion. Browser RTP loss counters can include temporarily missing, subsequently recovered packets; a zero-packet-loss requirement is inappropriate for a test that deliberately drops packets. Audio concealment remains reported, and this video regression does not claim lossless audio without redundancy.

## Software-rendered 1080p60

The initial high-detail MP4 test mixed source decoding, source-clock scheduling, software graphics, x264 encoding, browser decoding, and browser pixel instrumentation on one CPU. Direct OBS recordings already showed skipped/repeated content, before VDO streaming was enabled.

| Direct OBS recording control | Mean render time | OBS render skips |
| --- | ---: | ---: |
| Original MP4, preview enabled, normal Mesa worker count | 15.37 ms | 2 / 30 s |
| Same MP4, preview disabled | 12.94 ms | 1 / 30 s |
| Same MP4, preview disabled, Mesa limited to two workers | 38.23 ms | 993 / 30 s |
| OBS-clocked source, same two-worker renderer, preview disabled | 7.82 ms | 0 / 15 s |

The two-worker MP4 control also exceeded the recording-stop timeout; reducing the software renderer's worker count is **not** a fix. Direct MP4 recordings contained three and one duplicate/skipped markers respectively in the first two controls. Their independent source clock and renderer scheduling must be distinguished from network delivery.

A new opt-in `vdoninja_clock_fixture` renders its marker from OBS's video clock. It removes the separately clocked MP4 decoder and the embedded browser from the source path. Its recorded pixels passed all 898 captured frames. The real plugin then published it at 1920×1080/60 fps to Chromium: **all 1800 decoded frames were valid, with zero duplicate, skipped or backward markers, and zero OBS render/output skips**. Mean OBS rendering was 10.37 ms; the same software renderer retained its two-worker limit.

This establishes that the plugin can carry frame-perfect 1080p60 in the tested pipeline. It does not establish that this host can also software-render, encode and decode every high-detail 1080p60 source simultaneously. The simple clock fixture has less visual complexity than `testsrc2`; it is an isolation control, not an equivalent encoder benchmark. There is no justified plugin rendering change that would remove the measured OBS/Mesa workload limit. GPU-backed high-detail performance remains to be measured on suitable hardware.

### Repeat the rendering control

```bash
cmake -S . -B build-linux -DBUILD_PLUGIN=ON \
  -DOBS_SDK_PATH=/path/to/obs-sdk -DBUILD_OBS_CLOCK_FIXTURE=ON
cmake --build build-linux --target obs-vdoninja vdoninja-clock-fixture
```

Load both modules through the isolated OBS test plugin directory. The fixture is never installed or shipped in release packages. Linux CI builds it and checks registration, source creation/dimensions and destruction against supported official OBS runtimes.

For direct recording, `scripts/obs-linux-e2e-record.cjs clock Counter` uses the clock source and automatically rejects invalid or repeated recorded markers. Set `E2E_WIDTH=1920 E2E_HEIGHT=1080 E2E_FPS=60`, plus the WebSocket settings above. For a marked MP4, use `file FILE` and `E2E_REQUIRE_VISUAL_SEQUENCE=1`. Unmarked footage is not suitable for the pixel-counter check.

For live streaming, run the publishing harness with `VDONINJA_SOURCE_MODE=obs-clock`, 1920×1080/60 video settings, and all three flags:

```bash
VDONINJA_REQUIRE_VISUAL_SEQUENCE=1
VDONINJA_REQUIRE_STABLE_VIDEO=1
VDONINJA_REQUIRE_OBS_PERFORMANCE=1
```

The decoded-pixel gate is essential: an early fixture implementation rendered black despite advancing OBS counters, and the marker validator correctly rejected it. Browser presentation callbacks are reported separately from decoded-frame correctness. These tests do not equate a nominal 60 fps counter with 60 distinct frames.

## Local validation of the final sender candidate

- 725 C++ tests passed normally and under AddressSanitizer/UndefinedBehaviorSanitizer.
- 38 Node measurement/proxy tests passed, including the new presentation regressions.
- Nine release-linked real-libdatachannel tests passed.
- The strict impaired-network E2E remains failing as documented above; unit/build success does not supersede that result.
- Real module initialization and clock-source creation passed with OBS 32.2.0, 32.2.1 and 32.2.2; 32.0.4 and 32.1.2 correctly rejected the binary.
