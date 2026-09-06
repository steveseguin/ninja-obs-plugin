# Pinned upstream receiver timing experiment

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
