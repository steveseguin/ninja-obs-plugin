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
