# OBS Manual Test Matrix

Use this checklist on a real OBS install to validate plugin behavior beyond browser-only automation.

## Preconditions

- OBS restarted after plugin install
- `Settings -> Stream` shows `VDO.Ninja` service
- `Add Source` shows `VDO.Ninja Source`

## Test 1: Direct View, Password

1. Set stream settings:
   - Stream ID: `manual-pass-01`
   - Password: `somepassword`
   - Room ID: empty
2. Start streaming from OBS.
3. Open viewer URL:
   - `https://vdo.ninja/?view=manual-pass-01&password=somepassword`

Pass criteria:
- Video and audio play
- Reloading viewer reconnects quickly
- OBS stream remains stable

## Test 2: Direct View, No Password

1. Stream ID: `manual-open-01`
2. Password: empty
3. Start streaming
4. Open:
   - `https://vdo.ninja/?view=manual-open-01`

Pass criteria:
- Video/audio play
- Reload survives without session break

## Test 3: Room + Password

1. Stream ID: `manual-room-01`
2. Room ID: `manual-room-a`
3. Password: `somepassword`
4. Start streaming
5. Open:
   - `https://vdo.ninja/?view=manual-room-01&room=manual-room-a&scene&password=somepassword`

Pass criteria:
- Viewer joins and receives media
- Scene/room refresh reconnects correctly

## Test 4: Multi-Viewer Stability

1. Keep one stream active from tests above.
2. Open 2-3 viewer tabs/devices on same view URL.
3. Let run for 10+ minutes.

Pass criteria:
- All viewers continue playing
- No repeated reconnect loops
- OBS CPU/network remains acceptable

## Test 5: High Bitrate + Audio Quality

1. Set higher OBS output bitrate (for example 10-15 Mbps).
2. Publish to VDO.Ninja with active audio content.
3. Monitor stats and subjective quality in viewer(s).

Pass criteria:
- Audio remains continuous (no frequent dropouts)
- Video remains smooth enough for target FPS
- Refresh reconnect still works

## Test 6: Firefox Viewer

1. Run any active stream from tests 1-3.
2. Open view URL in Firefox.

Pass criteria:
- Audio/video present
- Refresh reconnects

## Test 7: VDO.Ninja Source, Browser-Backed Mode

1. Add `VDO.Ninja Source`.
2. Leave `Use Native Receiver (Experimental)` unchecked.
3. Set Stream ID/password to match any active publish from tests 1-3.

Pass criteria:
- Video and audio render inside OBS through the source
- Hiding/showing or activating/deactivating the source does not break playback
- OBS logs show the source created in browser-backed mode

## Test 8: VDO.Ninja Source, Native Receiver (Experimental)

1. Add `VDO.Ninja Source`.
2. Enable `Use Native Receiver (Experimental)`.
3. Use a publisher that sends H.264 video and Opus audio.
4. Set Stream ID/password to match that publisher.

Pass criteria:
- Video and audio render inside OBS through the source without Browser Source fallback
- Initial playback starts after signaling/view negotiation completes
- If video stalls, keyframe recovery resumes playback
- OBS logs show native receiver mode plus received audio/video tracks

## Capture for Regression Tracking

- OBS log file
- VDO.Ninja stats snapshot (codec, bitrate, packet loss, RTT)
- Stream URL pattern used (room/password/no-password)
- Approximate test duration and failure timestamp

## Test 9: Signaling Fallback Regression

Use portable OBS for this test so you know exactly which DLL is under test.

1. Back up `C:\Windows\System32\drivers\etc\hosts`.
2. Add:
   - `127.0.0.1 wss.vdo.ninja # ninja-plugin fallback test`
3. Launch portable OBS and start streaming with:
   - Stream ID: `steve12345`
   - Room ID: empty
4. Open a viewer URL:
   - `https://vdo.ninja/?view=steve12345`
5. Restore the original `hosts` file after the test.

Pass criteria:
- OBS log shows a primary signaling failure before open
- OBS log shows `Trying fallback signaling server: wss://proxywss.rtc.ninja:443`
- OBS log shows successful connection to the proxy host
- Streaming still starts
- A viewer still connects

See [`docs/windows-obs32-build-and-validation.md`](docs/windows-obs32-build-and-validation.md) for the exact
validated log sequence from `2026-03-21`.

## Test 10: Cellular Viewer / Candidate Proof

1. Start a publish from OBS with no room ID.
2. Put the phone on cellular, not Wi-Fi.
3. Open the viewer link on the phone.
4. Capture phone-side `RTCPeerConnection.getStats()` if you want hard candidate proof.

Pass criteria:
- Phone receives stable audio/video
- If candidate proof is required for release, phone-side stats show the selected candidate pair and transport

Important:
- OBS/plugin logs alone do not prove the phone's exact selected ICE candidate type.
- If the release note says "`srflx` works on cellular", capture phone-side stats or add temporary candidate-pair
  logging before release.

## Test 11: Default-Off 1080p60 Publishing

1. Set the encoder to H.264, 1920x1080, 59.94/60 fps, and 8000 kbps.
2. Leave packet duplication, audio RED, and adaptive bitrate off.
3. Connect an OBS Browser Source viewer and at least one additional viewer.
4. Run high-motion content for 10+ minutes.

Pass criteria:
- Every viewer advances continuously with no periodic GOP-boundary freeze
- The `Publish:` summary reports a keyframe interval no greater than about two seconds
- Maximum pacer batch is materially smaller than the largest keyframe
- No media-queue drops, pacer frame drops, or transport send errors
- Audio RTP has no large or non-forward steps

## Test 12: Opt-In Protection and Fallback

Run these independently so failures are attributable:

1. Enable `Audio RED`; connect one compatible current VDO.Ninja browser viewer.
2. Repeat with a viewer constrained to ordinary Opus.
3. Return audio RED to off, then test `Low`, `Medium`, and `High` packet duplication separately.

Pass criteria:
- Compatible audio negotiates RED and the summary records redundant packets
- The constrained viewer receives plain Opus and the RED counters remain zero for that peer/session
- Each video mode sends paced copies without transport failures
- Copies yield or expire instead of increasing live-media delay without bound
- Returning every protection setting to off reproduces the default-off baseline

Packet duplication adds upload and is not video RED/ULPFEC. Do not enable it as a default.

## Test 13: Adaptive Bitrate Stability

1. Start at H.264 1920x1080, 59.94/60 fps, and 8000 kbps.
2. Enable adaptive bitrate with a 500 kbps floor.
3. Connect multiple viewers, including one that reports a substantially lower REMB estimate.
4. Hold the lower estimate long enough to reach the target, then raise available bandwidth.
5. Stop streaming and inspect the encoder setting.

Pass criteria:
- One transient estimate does not change bitrate
- Decreases occur in stages and do not freeze the Browser Source viewer
- Pacer delay remains bounded with no frame drops or send errors during the transition
- Increases are slower and do not oscillate
- The lowest fresh estimate across all connected viewers controls the session
- Stale/missing feedback stops adaptation instead of guessing
- Stopping restores the original configured encoder bitrate

## Test 14: Packaged Game Capture VP9 Alpha

Use the packaged Game Capture application, not a browser-only substitute.

1. Publish a real Spout2 source from Game Capture with VP9 alpha enabled.
2. Receive it using `VDO.Ninja Source` with the native receiver enabled.
3. Place the source over a contrasting OBS background and exercise transparent, semi-transparent, and opaque areas.
4. Restart/hide/show both ends and repeat.

Pass criteria:
- Color and alpha tracks negotiate independently and remain timestamp-aligned
- Transparent and semi-transparent pixels composite correctly
- Ordinary VDO.Ninja browser viewers continue to receive the color stream
- Hiding/showing, reconnect, and source restart do not leave stale alpha frames

## Test 15: BrowserStack Browser, Device, and Route Matrix

Use BrowserStack to validate the real VDO.Ninja viewer in remote browser engines and real mobile devices while portable OBS remains the publisher. Credentials are read only from environment variables or an explicitly supplied secret file.

Example PowerShell setup for a real Pixel viewer through TURN:

```powershell
$env:VDONINJA_OBS_BROWSER_VIEWER = "1"
$env:VDONINJA_SKIP_CHROMIUM_VIEWER = "1"
$env:VDONINJA_VIDEO_WIDTH = "1920"
$env:VDONINJA_VIDEO_HEIGHT = "1080"
$env:VDONINJA_VIDEO_FPS_NUMERATOR = "60"
$env:VDONINJA_VIDEO_FPS_DENOMINATOR = "1"
$env:VDONINJA_VIDEO_BITRATE_KBPS = "8000"
$env:VDONINJA_BROWSERSTACK_PROFILE = "android-pixel9-chrome"
$env:VDONINJA_BROWSERSTACK_SECRET_FILE = "C:\path\to\browserstack.env"
$env:VDONINJA_BROWSERSTACK_VIEW_PARAMS = "privacy&buffer=1000"
$env:VDONINJA_BROWSERSTACK_REQUIRE_CANDIDATE_TYPE = "relay"
.\scripts\run-vdoninja-publish-smoke.ps1 -CheckTimeoutSeconds 240
```

The generated `artifacts/browserstack-viewer-*.json` report includes receiver codecs, the selected candidate pair, received bitrate, decoded frames, NACK/PLI/FIR/FEC counters, audio concealment, and freezes. Repeat with the supported profiles in `scripts/browserstack-vdoninja-viewer-check.cjs` and with VDO.Ninja codec steering parameters where the publisher can supply that codec.

BrowserStack `customNetwork` is not automatically proof of controlled WebRTC impairment. During the 2026-07-26 validation, BrowserStack accepted bandwidth/loss updates but both direct UDP and TURN/UDP media continued above the requested cap. Treat those phases as engine/route continuity tests unless the report marks `networkEffect.status` as `observed`. To make a controlled-loss gate fail closed, set:

```powershell
$env:VDONINJA_BROWSERSTACK_PHASES = '[{"name":"cap","customNetwork":"4000,4000,80,0","durationMs":15000,"expectedMediaKbps":8000,"requireNetworkEffect":true}]'
$env:VDONINJA_BROWSERSTACK_REQUIRE_NETWORK_EFFECT = "1"
$env:VDONINJA_BROWSERSTACK_EXPECTED_MEDIA_KBPS = "8000"
```

If that gate reports `not-applied-to-media` or `unproven`, use an OS/router impairment tool that actually controls the selected UDP candidate path for random and burst-loss testing. BrowserStack remains useful for H.264 playback, SDP/capability comparisons, direct-versus-relay candidate proof, and remote device continuity.

Validated recovery note: on the tested Pixel TURN/UDP route, `buffer=1000` gave NACK repairs enough time to arrive and removed the PLI/freezes seen with the shorter buffer. This is route-dependent; retain the counters in the report rather than assuming one buffer value fits every viewer.

Video RED/ULPFEC note: Chromium, WebKit, and Firefox can advertise RED/ULPFEC payloads, and the current libwebrtc [receiver processes RED/ULPFEC](https://chromium.googlesource.com/external/webrtc/+/master/video/rtp_video_stream_receiver.cc). Its [sender disables H.264 ULPFEC when NACK is enabled](https://chromium.googlesource.com/external/webrtc/+/master/call/rtp_video_sender.cc) because the combination is inefficient. That does not prove a browser receiver will reject correctly generated native FEC, but it means SDP presence is not recovery proof. Audio RED remains a valid negotiated option. Validate H.264 RED/ULPFEC only with a native generator and induced loss; FlexFEC is the preferred H.264 FEC candidate.

VP9 alpha note: BrowserStack can compare VP9 capability and ordinary color playback, but a normal browser does not composite the plugin's separate alpha track. The packaged Game Capture to native `VDO.Ninja Source` workflow in Test 14 is the release gate for transparency.
