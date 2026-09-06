# Pinned upstream receiver timing experiment

This opt-in standalone target compiles **actual WebRTC timing sources** from the
revision used by Playwright Chromium 145.0.7632.6. It does not build Chromium,
modify an installed browser, or ship code in the OBS plugin.

The driver explicitly models the controller's exclusion of retransmission-delayed
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

Requires a C++20 compiler, CMake 3.20+, Git and Python 3. No WebRTC dependency
checkout or GN build is needed for this narrow target. Abseil is compiled from
source; Ubuntu 24.04's system Abseil lacks headers needed by this WebRTC revision.

`instrument.py` writes instrumented copies into the build directory and checks
the expected source sites before modifying them. The upstream checkout stays
unchanged. The baseline asserts the existing failure, so its test passing means
**starvation reproduced**, not starvation fixed.

The experimental variant anchors the timestamp extrapolator once when render
time is requested without any timing samples, then applies the existing bounded
playout delay. Later retransmission-delayed frames remain excluded from incoming
timing samples. The test checks 1,800 frames in each of 24 scenarios per variant:
cold/warm startup, 300/1000 ms minimum buffers, ordinary/wrapping RTP timestamps,
and three resets of each timing object. Arrival jitter alternates by 16 ms;
initialized rendering must retain 32–34 ms cadence and the expected buffer lead.

This candidate needs upstream review and browser integration tests before use.
In particular, this probe does not establish behavior for clock drift, long
outages, RTP discontinuities, decoder scheduling, or A/V synchronization. A
cached IDR followed by a large jump to live RTP timestamps needs its own test.
A first render query may precede decode; its time is an experimental anchor, not an
unbiased network timing observation. Multiple simulated timing objects do not
establish real multiple-viewer performance.
