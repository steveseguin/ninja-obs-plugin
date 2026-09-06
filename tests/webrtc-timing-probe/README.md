# Pinned upstream receiver timing experiment

The latest [synchronization investigation](../../docs/linux-receiver-sync-baseline.md)
includes direct native A/V decisions, configured-floor and decode-scheduling
experiments, and Firefox compositor tracing. These remain diagnostic comparisons;
the configured-floor experiment regressed playback under application feedback.

This opt-in standalone target compiles **actual WebRTC timing and controller sources** from the
revision used by Playwright Chromium 145.0.7632.6. It does not build Chromium,
modify an installed browser, or ship code in the OBS plugin.

The original component driver explicitly models the controller's exclusion of retransmission-delayed
frames from `IncomingTimestamp`. Internal hooks count calls to that method and
the uninitialized extrapolator branch in `VCMTiming::RenderTimeInternal`. The
controller, network, decoder, and compositor are outside this component test.
Pair it with the real OBS all-NACK trace and clean/warm/cold browser controls.

```bash
git clone --depth 1 https://github.com/webrtc-mirror/webrtc.git /path/to/webrtc
git -C /path/to/webrtc fetch --depth 1 origin b47e68e6966d5a5a0e4bc861ff364221600f31c3
git -C /path/to/webrtc checkout b47e68e6966d5a5a0e4bc861ff364221600f31c3
git clone --depth 1 --branch 20250814.1 \
  https://github.com/abseil/abseil-cpp.git /path/to/abseil
cmake -S tests/webrtc-timing-probe -B build-webrtc-timing -G Ninja \
  -DWEBRTC_SOURCE=/path/to/webrtc -DABSEIL_SOURCE=/path/to/abseil
cmake --build build-webrtc-timing -j4
ctest --test-dir build-webrtc-timing --output-on-failure
build-webrtc-timing/timing-baseline > baseline.csv
build-webrtc-timing/timing-bootstrap > bootstrap.csv
```

Requires a C++20 compiler, CMake 3.20+, Git and Python 3. No full WebRTC dependency
sync or GN build is needed for these standalone targets. Abseil is compiled from
source; Ubuntu 24.04's system Abseil lacks headers needed by this WebRTC revision.

`instrument.py` writes instrumented copies into the build directory and checks
the expected source sites before modifying them. The upstream checkout stays
unchanged. The baseline asserts the existing failure, so its test passing means
**starvation reproduced**, not starvation fixed.

The original experimental variant anchors the timestamp extrapolator once when render
time is requested without any timing samples, then applies the existing bounded
playout delay. Later retransmission-delayed frames remain excluded from incoming
timing samples. The test checks 1,800 frames in each of 24 scenarios per variant:
cold/warm startup, 300/1000 ms minimum buffers, ordinary/wrapping RTP timestamps,
and three resets of each timing object. Arrival jitter alternates by 16 ms;
initialized rendering must retain 32–34 ms cadence and the expected buffer lead.

The full controller tests below **reject this one-time anchor**: it causes long
stalls after cached-to-live RTP jumps and fails to track clock drift. It remains
here to reproduce those regressions and must not be used as a production fix.
The original narrow component probe does not establish decoder scheduling or
A/V synchronization.
A first render query may precede decode; its time is an experimental anchor, not an
unbiased network timing observation. Multiple simulated timing objects do not
establish real multiple-viewer performance.

## Full controller characterization

The same build also produces `controller-baseline`, `controller-bootstrap` and
`controller-all-arrivals`. These run the actual upstream controller, frame buffer,
decode scheduler and timing code using synthetic frame metadata and a deterministic
task queue. The sink does not decode payloads. See
[controller validation](../../docs/linux-receiver-controller-validation.md) for
observed cached-keyframe stalls and clock drift that reject the bootstrap candidate.
The all-arrivals target is an experimental comparison, not a shipping fix.

For a separately checked-out Chromium 145 browser, `browser-patch.py` instruments
the pinned `third_party/webrtc` directory and adds opt-in diagnostic field trials.
It deliberately excludes the unsafe one-time bootstrap. Build the browser before
using `VDONINJA_BROWSER_EXECUTABLE` with either browser harness. The minimal harness
has no network NACK generator; its forced exclusion trial isolates sample selection
inside the actual browser receiver and must be paired with real network tests.

After checking out Chromium `145.0.7632.6` and completing its normal dependency
sync/build-dependency setup, the browser experiment can be built as follows:

```sh
python3 /path/to/ninja-obs-plugin/tests/webrtc-timing-probe/browser-patch.py \
  /path/to/chromium/src/third_party/webrtc
cd /path/to/chromium/src
buildtools/linux64/gn gen out/ReceiverTiming --args='is_debug=false is_component_build=true symbol_level=0 blink_symbol_level=0 v8_symbol_level=0 use_remoteexec=false use_siso=false proprietary_codecs=true ffmpeg_branding="Chrome" chrome_pgo_phase=0'
/path/to/depot_tools/vpython3 /path/to/depot_tools/autoninja.py \
  -C out/ReceiverTiming -j4 headless_shell
cd /path/to/ninja-obs-plugin
VDONINJA_BROWSER_EXECUTABLE=/path/to/chromium/src/out/ReceiverTiming/headless_shell \
VDONINJA_CHROMIUM_FIELD_TRIALS=WebRTC-ReceiverTimingTrace/Enabled/WebRTC-ForceTimingSampleExclusion/Enabled/ \
  node scripts/browser-minimal-receiver-timing.cjs artifacts/minimal-excluded
python3 scripts/analyze-receiver-timing-log.py artifacts/minimal-excluded/chromium.log
```

Repeat with `WebRTC-RetransmittedTimingSamples/Enabled/` appended, and retain a
normal control without forced exclusion. The patch is deliberately revision/site
checked and refuses to apply a second time. Do not run media benchmarks while the
browser compiler is active. These local experimental field trials have no effect
in ordinary installed browsers.

The non-official browser build enables fatal DCHECKs by default. In this run, a
separate data-channel scheduler assertion interrupted viewer startup/reloads.
`browser-dcheck-control.py /path/to/chromium/src` optionally makes **only that
assertion** log-only, preserving the otherwise unchanged official-release code
path. Rebuild afterward with all test browsers stopped. See the validation report
for the original failures and the separately identified control runs; this is not
part of the receiver fix and must not be silently applied to an installed browser.

## Receiver/compositor refinement

See [the refinement report](../../docs/linux-receiver-refinement.md) for measured
results and remaining failures. These patches apply only to the pinned diagnostic
source trees. They do not modify the installed plugin or system browsers.

After the original Chromium timing and explicit data-channel assertion patches:

```sh
python3 tests/webrtc-timing-probe/browser-refinement-patch.py /path/to/chromium/src
python3 tests/webrtc-timing-probe/browser-audio-patch.py /path/to/chromium/src
```

`WebRTC-ReceiverTimingTrace/Enabled/` now includes decode-release events.
`WebRTC-CompositorTimingTrace/Enabled/` records decoded enqueue and actual frame
selection. `WebRTC-AudioFifoTrace/Enabled/` records Web Audio FIFO underruns and
overruns without changing its buffer or output. First-packet timing samples are
an optional experiment, enabled with both
`WebRTC-RetransmittedTimingSamples/Enabled/` and
`WebRTC-FirstPacketTimingSamples/Enabled/`.

Analyze a single browser's trace and measured presentation capture:

```sh
python3 scripts/analyze-compositor-timing.py chromium.log presentation-records.json --fps 60
python3 scripts/analyze-receiver-timing-log.py chromium.log
```

The compositor analyzer reports full-run and measured-window omissions separately.
It requires one timing object and compositor in the input log rather than joining
unrelated processes. The timing analyzer also recognizes native Firefox process
prefixes; malformed/truncated records remain failures.

The source-clock cadence experiment and actual Chromium renderer probe are opt-in:

```sh
python3 tests/webrtc-timing-probe/browser-cadence-patch.py /path/to/chromium/src
python3 tests/webrtc-timing-probe/prepare-renderer-probe.py /path/to/chromium/src
# In the Chromium source root, using the existing diagnostic GN arguments:
buildtools/linux64/gn gen out/ReceiverTiming
autoninja -C out/ReceiverTiming -j4 headless_shell renderer_probe
out/ReceiverTiming/renderer_probe reference
out/ReceiverTiming/renderer_probe rtp
out/ReceiverTiming/renderer_probe rtp wrap
```

`WebRTC-RtpCadenceSamples/Enabled/` changes the duration estimator's samples for
frames carrying forward RTP timestamps; renderer drift limits and cadence-switch
thresholds are unchanged. The probe links Chromium's real renderer algorithm and
simulates precise source clocks with small reference-time corrections. Its result
must be distinguished from codec, network and GPU integration validation.

## Native Firefox build

Use the official Firefox **146.0.1** source archive and verify its SHA512 against
Mozilla's release checksums before extracting. `firefox-patch.py` additionally
checks the source version and `sourcestamp.txt` revision before applying the shared
timing instrumentation and Firefox-specific diagnostic trial adapter.

```sh
python3 tests/webrtc-timing-probe/firefox-patch.py /path/to/firefox-146.0.1
# In the Firefox source root:
MOZBUILD_STATE_PATH=/path/to/isolated-mozbuild ./mach --no-interactive bootstrap --application-choice browser
```

The tested `.mozconfig` uses `--enable-application=browser`, `--disable-debug`,
`--enable-optimize`, `--disable-debug-symbols`, `--disable-tests`,
`--disable-updater`, and `MOZ_MAKE_FLAGS=-j4`. Rust 1.83.0 matches the source tree's
Linux toolchain configuration. Do not compile browsers during performance runs.

Native automation uses Selenium 4.35.0 and geckodriver 0.36.0, without a Juggler
patch. Install its optional Python dependencies in an isolated virtual environment:

```sh
python3 -m venv /path/to/firefox-driver-venv
/path/to/firefox-driver-venv/bin/pip install -r tests/webrtc-timing-probe/firefox-requirements.txt
export VDONINJA_VIEWER_ICE_SERVERS_JSON='[{"urls":"turn:PRIVATE_RELAY:3478","username":"TEST_USER","credential":"TEST_PASSWORD"}]'
export MOZ_GMP_PATH=/path/to/gmp-gmpopenh264/2.6.0
export VDONINJA_FIREFOX_TIMING_TRACE=1
/path/to/firefox-driver-venv/bin/python scripts/browser-native-firefox-timing.py \
  'VIEWER_URL_FOR_RUNNING_TEST_PUBLISHER' artifacts/native-firefox \
  --browser /path/to/obj-receiver-timing/dist/bin/firefox --driver /path/to/geckodriver
```

Set `VDONINJA_FIREFOX_TIMING_SAMPLES=all` for continuous-sample comparisons;
leave it unset for the default receiver. `--reloads 1` records both rounds in
separate files. The driver requires H.264 and relay evidence, preserves strict
presentation/video/audio analyzers, and reports missing decoded-pixel/raw-audio
coverage. Native logs close after explicit peer shutdown so trailing events can
flush before WebDriver terminates the content processes.


## Native A/V sync decisions

After the existing diagnostic patches, apply `browser-sync-patch.py` to the pinned
Chromium `third_party/webrtc` checkout and rebuild `headless_shell`. Enable
`WebRTC-AvSyncTrace/Enabled/` to log input and proposed delays once per native sync
calculation. Only `adjusted=1` decisions proceed to the minimum-delay setters.
This trace changes no synchronization policy. For corresponding JavaScript buffer
requests use `VDONINJA_TRACE_BUFFER_WRITES=1` in the OBS publish harness; retain
`VDONINJA_FIXED_VIEW_BUFFER=1` runs separately from ordinary application feedback.
See [the follow-up results](../../docs/linux-receiver-buffer-followup.md).

`browser-silent-sink-patch.py "$CHROMIUM_SRC"` adds clock-transition logging under
`WebRTC-AudioFifoTrace/Enabled/`. It traces the actual silent-sink transition without
changing its policy. Apply once to the pinned source, then rebuild. The local
`node scripts/browser-audio-fifo-repro.cjs OUTPUT SOURCE_PHASE_DELAY_MS` reproduction
removes OBS and networking; its report retains strict PCM results and actual
source timer jitter. See [the follow-up](../../docs/linux-receiver-buffer-followup.md)
for Worklet/legacy and worker/main-thread controlled comparisons.

The measurement harness defaults to a sink-only AudioWorklet and worker raw PCM
capture, preserving native queue capacity. Explicit controls are
`VDONINJA_AUDIO_CAPTURE_MODE=script-processor` and
`VDONINJA_RAW_AUDIO_CAPTURE_MODE=main-thread`. Unsupported Worklet runtimes report
the legacy fallback; raw-track capture remains unavailable in native Firefox.

For buffer correlation, enable `VDONINJA_TRACE_BUFFER_WRITES=1` and the native
receiver/compositor/sync traces, then run:

```sh
python3 scripts/analyze-receiver-buffer-sync.py CHROMIUM_LOG ENCODED_JSON PRESENTATION_JSON --fps 60
```

The analyzer verifies matching frame clock anchors and video SSRC before joining
nearby synchronization decisions and buffer writes. It reports correlation, not
causation. `VDONINJA_CAPTURE_ENCODED_FRAMES=0` disables the JavaScript encoded-frame
transform while retaining negotiation and receiver-buffer controls for independent
performance checks; those runs cannot supply this analyzer's clock anchors.


## Synchronization, decode lead and native Firefox composition

See [runtime outcomes and rejected candidates](../../docs/linux-receiver-sync-baseline.md)
before using these controls. The standalone build now includes:

```sh
build-webrtc-timing/sync-base-probe baseline
build-webrtc-timing/sync-base-probe configured
build-webrtc-timing/sync-base-probe replay tests/webrtc-timing-probe/sync-floor-feedback.csv
```

The replay fixture contains 146 numeric native synchronization decisions from
`sync-base-feedback-cold-0`. It must reproduce all targets, including the abrupt
video-delay reduction that rejects the candidate. A passing characterization is
not a passing media test.

In the existing pinned Chromium build, rebuild `renderer_probe` after copying the
updated source. Additional modes are:

```sh
out/ReceiverTiming/renderer_probe rtp step 15
out/ReceiverTiming/renderer_probe rtp step 31
out/ReceiverTiming/renderer_probe rtp decode-step 0
out/ReceiverTiming/renderer_probe rtp decode-step 16
```

`step` changes render references after 30 seconds; its argument is enqueue lead
in ms. `decode-step` leaves references steady, changes lead from 28 to 8 ms, and
adds the specified headroom. Baseline modes explicitly characterize failures.

After the existing Chromium receiver, cadence and synchronization trace patches:

```sh
python3 tests/webrtc-timing-probe/browser-sync-base-patch.py "$CHROMIUM_SRC/third_party/webrtc"
python3 tests/webrtc-timing-probe/browser-decode-headroom-patch.py "$CHROMIUM_SRC/third_party/webrtc"
```

Rebuild before running. `WebRTC-SyncConfiguredBase/Enabled/` enables the rejected
common-floor candidate; `WebRTC-DecodeSchedulingHeadroom/Enabled/` enables the
incomplete 16 ms decode experiment. Both default off. Keep the earlier timing
sampling trials explicit when comparing results.

For pinned Firefox 146.0.1, after the existing timing patch:

```sh
python3 tests/webrtc-timing-probe/firefox-compositor-patch.py "$FIREFOX_SRC"
```

Rebuild Firefox, then set `VDONINJA_FIREFOX_COMPOSITOR_TRACE=1` when running
`scripts/browser-native-firefox-timing.py`. Use `--headed --require-gpu` and
optionally `--require-window-protocol x11` (or `wayland`). The driver checks
about:support, analyzes flushed native logs, and adds the native verdict to each
round. Trace coverage, native omissions and cadence can fail a round whose
callback/stats checks pass. Existing thresholds are retained. This instrumentation
covers Linux first-composition notifications, not physical scanout.

Standalone analysis accepts one child-process log and one driver round JSON:

```sh
python3 scripts/analyze-firefox-compositor.py /path/to/child.moz_log /path/to/round-0.json --fps 60
python3 tests/test-firefox-compositor.py
```

The minimal loopback harness accepts `VDONINJA_MINIMAL_AUDIO=1`,
`VDONINJA_MINIMAL_FPS=60` and `VDONINJA_MINIMAL_AUDIO_DELAY_MS=70`. Audio mode
requires a working `pactl` server and uses its own null sink. With
`WebRTC-AvSyncTrace/Enabled/`, absent native sync decisions fail the report even
when media arrives. The tested generated-track controls still hit this coverage
failure; they do not yet reproduce native A/V synchronization.
