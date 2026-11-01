# WHEP publishing validation

## Scope

The OBS plugin accepts player offers through the existing `whep.vdo.ninja`
service and sends the program directly to each player. The receiver uses WHEP
HTTP requests and WebRTC; it does not need VDO.Ninja JavaScript or its client API.
This is separate from Matt's group-room Guest Return.

Only the plugin repository changed. No edits or deployment were made to
VDO.Ninja, `whip_private`, or the signaling service. The older prototype remains
on `work/parked-unfinished-features` and was not merged wholesale.

The local server check used the restored, unchanged `whip.js`, SHA-256
`b227165aa5a50d4fd243ef200b24978fa46c2c9f1cd7cd12fd8214fa575bd00e`.
That matches the repository's last-reported deployed snapshot. Separate tests
against the actual public endpoint verify its observed behavior; the local hash
does not prove the currently running server's binary identity.

## Results — Windows, 2026-10-31

Portable OBS 32.2.2, x264 H.264 / Opus, 1280×720 at 30 fps. The harness verifies
that the running OBS process loads exactly the intended plugin DLL. Independent
Chromium 145 and Firefox 146 players use `tests/tools/whep-player.html`.

| Check | Result |
| --- | --- |
| Unchanged local server and live HTTPS endpoint | Passed |
| Decoded video advances in both browsers | Passed; about 60 frames over each two-second sample |
| Orange video pixels and decoded 440 Hz audio | Passed; audio peak approximately 445 Hz at FFT resolution |
| Sequential joins with HTTP POST / PATCH | Passed; 201 / 204 responses |
| Two simultaneous joins with fully gathered offers | Passed against the live endpoint |
| Three players, fourth above Max Viewers | Three play; fourth is rejected |
| Close one player, keep others, admit replacement | Passed |
| Two output cycles and an immediate start/stop | Passed; players disconnect on output stop |
| OBS process shutdown | Clean exit |
| OBS UI: stop-first message, saved key, clipboard URL, copy while live | Passed |
| Unit tests / full OBS-linked native suite | 805 / 92 passed |
| H.264, HEVC, AV1 offer/answer and encrypted RTP | Native checks passed; this is not HEVC/AV1 browser decode qualification |

The native wire regression sends the server's existing POST `get` callback and
DELETE messages through a real WebSocket. It verifies two independent viewers,
no added `sessionId` field, acknowledgment of DELETE without disconnecting other
viewers, cleanup after an actual connection closes, and host shutdown. A custom
key containing a space and slash checks that joining matches the escaped URL.

The first browser run exposed missing OpenH264 in a fresh Firefox automation
profile. The harness now copies only that codec's DLL and metadata from an
installed Firefox profile. It does not copy browsing data or user preferences.
An early fixture reused input names before OBS completed asynchronous removal;
test inputs now have unique names. These were harness corrections, not server
or player-protocol changes.

The first Arch package run failed the new native close check's five-second
deadline. The regression now waits for both peers to connect and allows up to
45 seconds for ICE consent expiry if the close notification is not delivered.
It still requires the closed viewer to retire while the other stays connected.
The Windows repeat observed closure in 5 ms; the complete Arch native suite,
package install/reinstall and OBS initialization then passed in
https://github.com/steveseguin/ninja-obs-plugin/actions/runs/41257162367.
No production cleanup code or server behavior was changed to satisfy the test.
The v1.1.81 build was cancelled before publication. All v1.1.82 platform packages
subsequently passed, and all seven published package checksums were verified.
The packaged Windows DLL (`a77ba3adfa4e6ef52d0c9dd91fa360f88b4be21eeb266cc995c2046dd67c6ce1`)
passed the two-cycle live-endpoint browser test with clean OBS shutdown.

The subsequent packaged UI check exposed a real feedback problem: the one-second
stats refresh could erase a WHEP instruction before it was read. The v1.1.83
follow-up holds that message for eight seconds, while stream-state changes take
priority. Local UI validation passed delayed reading, return to LIVE, copying
while live, and keeping the same link after an OBS restart. Media and signaling
code are unchanged from v1.1.82. Final packaged UI evidence is recorded below.

## Existing server limits

- The service stores one ETag/ICE state per stream. An overlapping POST can
  invalidate another player's pending PATCH. Sequential trickle and simultaneous
  complete offers were tested; concurrent trickle and ICE restart are not
  supported claims.
- DELETE notifications contain a fresh HTTP callback and the stream key, but no
  reference to the originating viewer. The plugin acknowledges DELETE and retires
  viewers through DTLS/ICE closure. A player that sends DELETE while leaving its
  peer connection open is not guaranteed immediate cleanup. Even after a close,
  a lost close notification can leave its viewer slot occupied until ICE times out.
- A signaling connection loss currently disconnects that host's WHEP viewers
  before retrying the service. Seamless playback across that outage is not promised.
- The current implementation accompanies the main plugin publishing output; it
  is not a separate WHEP-only OBS output and is not a media redistribution server.
- Browser media checks ran on this Windows machine. The public endpoint was
  remote, but the media peers were local. Cross-network/mobile/TURN paths,
  macOS/Linux WHEP runtime, and third-party player products remain unqualified.

These boundaries are documented in the guide. None were addressed by changing
the server, adding protocol fields, or silently disconnecting every viewer.

## Reproduce

```powershell
cmake --build build-plugin-release-validation --config RelWithDebInfo --target obs-vdoninja vdoninja-native-media-linked-gate --parallel 6
cmake --install build-plugin-release-validation --config RelWithDebInfo --prefix artifacts/whep-install
$env:PATH = "$pwd/_obs-portable/bin/64bit;$env:PATH"
& ./build-plugin-release-validation/RelWithDebInfo/vdoninja-native-media-linked-gate.exe WHEP
python scripts/whep-host-native-e2e.py --install-prefix artifacts/whep-install --obs-review artifacts/whep-obs --output artifacts/whep-local --broker local --cycles 2
python scripts/whep-host-native-e2e.py --install-prefix artifacts/whep-install --obs-review artifacts/whep-obs --output artifacts/whep-hosted --broker hosted --cycles 2
```

Use a new output directory for each run. The default local server source is
`../whip_private`; its existing `tests/server-fixture.cjs` runs on loopback and
blocks unrelated external API calls. The hosted test uses a random disposable
key and performs no server deployment. The Python wrapper creates a separate
portable OBS copy, replaces only that copy's plugin/configuration, generates
synthetic video/audio, and checks the loaded module path and clean exit.

For UI review, `scripts/guest-return-room-ui.py` captures only the OBS process
listed in the review's `state.json`. Qt button invocation was used for the WHEP
walkthrough; raw mouse-message automation did not activate these floating-dock
buttons reliably. Clipboard contents were restored after the check.

## Screenshots and evidence

The [screenshot manifest](../media-assets/whep/manifest.json) records original
captures and exact guide crops. All UI stages and seven playback screenshots
were visually reviewed: labels are readable, expected controls are enabled, and
the decoded orange test image appears. Frame progression and audio correctness
are verified separately by browser statistics, pixel sampling and Web Audio.

[Compact machine-readable results](../media-assets/whep/validation-results.json)
include DLL hashes, browser versions, response codes, media samples and timings.
Full local logs are under `artifacts/whep-existing-protocol/`; they are not public
release assets. Development screenshots use a synthetic stream and exclude the
old version footer from guide crops.

The user guide is https://steveseguin.github.io/ninja-obs-plugin/whep.html.
