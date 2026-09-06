# Native synchronization baseline investigation

Continued in [decode scheduling and the repaired minimal A/V control](linux-decode-scheduler-isolation.md).

This continues [the buffer follow-up](linux-receiver-buffer-followup.md). The
shipping plugin and its encoder/pacing settings are unchanged. Browser changes
below are opt-in experiments in the pinned Chromium/WebRTC checkout, not fixes
installed in stock Chromium or Firefox.

## Direct reproduction

`cadence-cause-cold-0` used the existing Intel ARL GPU, Mesa 25.2.8, OBS 32.2.2,
VAAPI H.264 CBR 12 Mbps and detailed marked 1080p60 footage. The isolated loaded
plugin is unchanged at SHA256
`03ff5c1e593e8081caf4b01a9a9d53f487549db26567a6c9766744232b284b24`.
Both receiver targets were fixed at 300 ms. Reordering was confined to the private
TURN container: `netem delay 25ms 8ms distribution normal`, with no deliberate
loss. Measurement lasted 120 seconds after 20 seconds of stabilization.

The first repeat captured the native A/V decisions immediately preceding two
cadence jumps. The analyzer joined 8,523 matching frame clocks with zero offset
spread and verified the native/encoded video SSRC.

| Consecutive RTP timestamps | Native decision time (ms) | Video minimum | Assigned render interval | Presentation interval |
| --- | ---: | --- | ---: | ---: |
| 11617500 → 11619000 | 61300203 | 300 → 319 ms | 36 ms | 33.4 ms |
| 11976000 → 11977500 | 61304203 | 319 → 335 ms | 32 ms | 33.3 ms |

The corresponding `VDONINJA_SYNC` decisions have `adjusted=1` and target video
delays 319 and 335 ms. Application writes were intercepted and their applied
values remained 300 ms. The second frame in each pair had arrived hundreds of
milliseconds earlier. Decode release was postponed with its new render deadline;
the compositor repeated the previous frame before the next decoded frame was
enqueued. Neither pair omitted a source frame.

This run also had an early scheduling disturbance and two presentation omissions
with the minimum unchanged at 300 ms. Those are separate failures. Decoded pixel
continuity and OBS performance passed; the unchanged lost-counter gate failed
with +7. Explaining the two synchronization steps does not explain every failure
in that run.

## Minimal upstream controls

The actual Chromium renderer probe now accepts `rtp step 15` and `rtp step 31`.
It applies a single 15 or 19 ms render-reference increase at source time 30 seconds
and sweeps source rates 30/60 fps and nine display phases, with zero-step controls.
The last argument is decoded-frame enqueue lead in milliseconds. It preserves
renderer drift limits and the existing strict cadence thresholds.

Earlier enqueue reduces some failures but does not eliminate them. Its successful
exit means the characterization reproduced failures while zero-step controls
passed, not that playback passed. This rejects simply adding decode headroom as a
complete timing fix. The later browser headroom experiment below remains incomplete.

The new `sync-base-probe` links the actual upstream `StreamSynchronization` class.
A deterministic feedback fixture models configured receiver floors and a 70 ms
pipeline difference; it does not implement NetEQ, codecs or physical audio output.
With no relative jitter, the baseline's first video correction occurs at 1, 41
and 133 seconds for common floors 0, 300 and 1000 ms respectively. Calling the
existing `SetTargetBufferingDelay` with the configured floor restores the same
1-second start in all three. The baseline fails translation equivalence in 24 of
36 scenarios; the configured comparison matches in all 36. Cases cover audio or
video later, ±8 ms relative jitter, repeated initialization, and floor changes.
The standalone suite also retains the baseline characterizations and previous
controller experiments. A trace replay now checks the failed unequal-floor case.

## Browser experiment

`browser-sync-base-patch.py` exposes each receiver's configured base minimum to
the synchronizer. With `WebRTC-SyncConfiguredBase/Enabled/`, it supplies the smaller
common floor to the existing `SetTargetBufferingDelay` method before computing
adjustments. Unset floors normalize to zero. Native sync traces additionally log
both receiver floors and their common value.

This changes neither the requested receiver targets nor synchronization thresholds.
It does not disable A/V synchronization, change retransmission sampling policy,
increase sender bitrate, or remove pacing. Runtime testing below rejects this as a general fix: unequal receiver floors
and application feedback produce a new regression. The equal-floor model does
not establish their behavior.

Apply after the existing receiver and sync trace patches, then rebuild with all
performance captures stopped:

```sh
python3 tests/webrtc-timing-probe/browser-sync-base-patch.py "$CHROMIUM_SRC/third_party/webrtc"
autoninja -C "$CHROMIUM_SRC/out/ReceiverTiming" -j4 headless_shell
cmake -S tests/webrtc-timing-probe -B build-webrtc-timing \
  -DWEBRTC_SOURCE="$WEBRTC_SOURCE" -DABSEIL_SOURCE="$ABSEIL_SOURCE"
cmake --build build-webrtc-timing -j4
ctest --test-dir build-webrtc-timing --output-on-failure
build-webrtc-timing/sync-base-probe baseline
build-webrtc-timing/sync-base-probe configured
```

`VDONINJA_MINIMAL_AUDIO=1` adds an independent 48 kHz generated audio track to
`browser-minimal-receiver-timing.cjs`, in the same stream as the video. Both
receivers retain the existing 300 ms target. The harness verifies received audio
samples and retains receiver statistics; it does not claim physical audio-output
continuity. Without the flag, the original video-only control is unchanged.

## Candidate results and rejection

These runs used the same GPU-backed 12 Mbps detailed 1080p60 publication. Cold
means impairment was installed before connecting; warm means it was applied only
after clean startup. Cold reordering was 25 ± 8 ms with no deliberate loss. Loss
cases additionally used 0.5% loss on the relay container. Measurements retained
strict pixel, presentation, audio, OBS and lost-counter gates.

| Capture | Receiver experiment | Duration after 20 s settle | Result |
| --- | --- | ---: | --- |
| `sync-base-control-cold` | Common-floor candidate off, fixed 300 ms | 120 s | Pixels, cadence, audio and OBS pass; lost counter +9 fails |
| `sync-base-fixed-cold-0` | Common-floor candidate on, fixed 300 ms | 120 s | One cadence deviation of 16.83 ms fails |
| `sync-base-feedback-cold-0` | Common-floor candidate on, application feedback | 120 s | Eight presentation omissions, 66.63 ms cadence deviation and a 20 ms raw-audio dropout fail |
| `decode-headroom-fixed-cold-0` | 16 ms earlier decode scheduling, fixed 300 ms | 120 s | Media/OBS pass; lost counter +13 fails |
| `decode-headroom-feedback-cold-0` | Earlier decode, application feedback | 120 s | Media/OBS pass; final selected-relay snapshot is empty, so coverage fails |
| `decode-headroom-combined-fixed-0` | Both candidates, fixed 300 ms | 120 s | Four cadence outliers remain, without source-frame omissions |
| `headroom-warm-reordering` | Earlier decode, application feedback | 120 s | Pixels/audio/OBS/relay pass; cadence and lost counter +3 fail |

The common-floor regression is directly traced. Between RTP timestamps 8995500
and 9000000, video minimum falls from 304 to 184 ms and assigned render time moves
backward by 70 ms. The native decision at 62189750 ms has audio floor 300, video
floor 154, common floor 154, current audio 325, current video 304, and targets
155/184 ms. `sync-floor-feedback.csv` retains all 146 numeric decisions from this
run. `sync-base-probe replay` feeds them into the real upstream synchronizer,
checks every output, and requires that the abrupt reduction is reproduced. This
is a **rejected candidate**, not a recommended receiver or plugin change.

`browser-decode-headroom-patch.py` optionally advances normal decode scheduling
by 16 ms using `WebRTC-DecodeSchedulingHeadroom/Enabled/`. It preserves render
targets, min/max buffers, queue caps and the zero-delay pacing branch. This is a
60 Hz diagnostic constant, not a display-aware implementation. Native traces
include decode estimate, render delay, current delay and added headroom.

The real Chromium renderer's `rtp decode-step 0` fixture holds render references
steady but reduces decoded-frame enqueue lead from 28 to 8 ms after 30 seconds.
Nine of 36 baseline scenarios fail strict cadence; `rtp decode-step 16` passes all
36. However, render-reference steps still fail in 18/54 scenarios with 15 ms lead
and 12/54 with 31 ms lead. Live combined-candidate traces also show failures with
unchanged minimum delay, a 5 ms decode estimate and 10 ms render delay. Earlier
decode is therefore **not a complete fix**.

One next investigation is the actual decode scheduler: Chromium supplies a decode
metronome, and `DecodeSynchronizer::OnTick` can release a frame before its latest
decode time. The ordinary task-queue scheduler already uses a high-precision
posted task. Source inspection alone does not establish which tick/release caused
the captured failure; instrument scheduler selection and release deadlines before
proposing a scheduler change. No scheduler modification was made in this pass.

## Firefox native composition and repeated sessions

Firefox 146.0.1 (source `86bb7f6af6312ba3c0161085f854bcdff68f1a91`, instrumented
build ID `20260906114735`) used OpenH264, headed X11 and verified GPU WebRender.
Chromium was 145.0.7632.6, source `47e20adcc15fc15f01825aa17e570c8f5492ac0f`,
WebRTC `b47e68e6966d5a5a0e4bc861ff364221600f31c3`. The primary Chromium receiver
used continuous incoming timing, RTP cadence and decode headroom; Firefox used
its separate incoming-timing experiment. These are instrumented browser results,
not stock-browser certification.

`firefox-compositor-patch.py` adds Linux-only optional logging to ImageContainer:
queued image identity/RTP/reference time, and actual first-composition time plus
notification-delivery time. Enable `VDONINJA_FIREFOX_COMPOSITOR_TRACE=1` in the
native driver (or `MOZ_LOG=VdoninjaCompositor:3`). The analyzer joins RTP to
container/producer/frame identities and uses first composition, not delivery, as
the native timing clock. It does not measure physical display scanout.

Three sessions (`compositor-firefox-clean`, `-cold`, `-loss`) each had one primary
Chromium viewer for 150 seconds and two 45-second Firefox rounds, each after
20 seconds settle. This covers six Firefox rounds, three reloads/reconnections,
and simultaneous Chromium/Firefox viewers. Clean primary Chromium passed all
gates. Cold primary media/OBS passed but final relay coverage failed. Loss primary
had 29 decoded-marker skips and 51 presentation omissions; raw audio and OBS
passed. Both Firefox loss rounds failed cadence.

A clean Firefox reload's callback interval of 33.2 ms corresponded to only
17.095 ms between native first compositions. This particular callback outlier is
not a native composition stall. Conversely, full native traces reveal queued
frames that never received composition notifications: clean rounds have 80 and
71 native omissions; cold rounds have 73 and 39. Clean native median cadence is
about 17.06 ms, with gaps near 34.3 ms. Callback-only checks missed these real
coverage/presentation failures. The driver now retains its original callback and
stats verdict and additionally requires the native compositor gate when tracing
is requested. Missing or ambiguous native evidence fails coverage.

A native-Wayland clean attempt timed out in WebDriver startup before measurement.
The isolated browser was stopped and startup logs/stacks retained. There is no
Wayland validation result, and Firefox's failure is not assigned Chromium's cause.

## Audio output and minimal-loopback coverage

Two more sessions (`headroom-audio-clean`, `headroom-audio-loss`) used a 150-second
primary viewer and a 120-second secondary viewer routed to a dedicated virtual
sink. Secondary decoded raw PCM, Web Audio and recorded sink output all passed,
including sample coverage and restoration of the original default sink. Prior
actual-output glitches were not reproduced; they are not declared fixed. Native
FIFO events still appear in the overall browser logs, including startup/other
contexts. The primary clean case still failed video cadence; the loss case had
31 decoded skips and 31 presentation omissions.

Minimal loopback supports `VDONINJA_MINIMAL_FPS=60`, `VDONINJA_MINIMAL_AUDIO=1`
and optional `VDONINJA_MINIMAL_AUDIO_DELAY_MS=70`. Audio frames are delayed using
independent deadlines, preserving throughput. Audio mode removes browser muting,
uses a private PulseAudio-compatible null sink and verifies default-sink
restoration. It never intentionally routes to the physical speaker.

Four 60-second unmuted controls compared zero audio delay, 70 ms delay, common-floor
candidate, and combined candidates. All received audio and restored the sink,
but none produced a native synchronization decision. They are explicitly failed
coverage checks. An earlier three-case muted loopback series is also excluded
from A/V-sync evidence. A generated audio track and received samples alone do not
prove the native synchronizer was exercised. The current harness requires native
sync events when the A/V trace trial is requested. Its synthetic encoded delay is
not a real network NACK test.

No shipping plugin, configured quality, sender pacing, SDK baseline or release
version changed. No packaging change is proposed. The remaining work is to isolate
metronome/decode release versus renderer selection, make the minimal A/V control
exercise native synchronization, and reproduce Firefox's native omission path on
Wayland or another display control. Retain all failed gates when repeating the
matrix; passing pixels or decoded audio alone does not establish presentation or
output continuity.


## Final validation

The integrated native Firefox gate was exercised in `compositor-gate-clean`:
an 80-second primary GPU publication passed all gates; the simultaneous 30-second
Firefox X11/WebRender round passed callback, video-stat and audio checks but failed
native composition with 36 omissions and 17.706 ms maximum cadence deviation.
The driver saved both verdicts and returned failure as required.

Local validation passed 70 JavaScript measurement tests, 25 Python packet/timing
and compositor tests, and all nine pinned upstream native CTest targets. Chromium
and Firefox diagnostic builds completed successfully before runtime captures.
The renderer reference/RTP/wrap controls and step characterizations were also
run against the actual built Chromium renderer. Generated recordings and full
browser logs remain ignored local artifacts; the numeric regression fixture and
reproduction tools are committed.
