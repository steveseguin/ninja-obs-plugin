# WHEP publishing validation

## Scope

The OBS plugin accepts player offers through the existing `whep.vdo.ninja`
service and sends the program directly to each player. The receiver uses WHEP
HTTP requests and WebRTC; it does not need VDO.Ninja JavaScript or its client API.
This is separate from Matt's group-room Guest Return.

## UI and input audit shipped in v1.1.84

This audit fixes problems found in the actual Windows OBS interface. The
audited development builds display v1.1.83; their DLL hashes in the evidence
distinguish them from the published package. The same changes are shipped in
v1.1.84; downloaded-package results appear below.

| Finding | Change |
| --- | --- |
| Copy feedback hid LIVE and read like a warning paragraph | Short feedback beside the links; LIVE remains visible. One **Copy WHEP Link** button. |
| Guest Return opened with a large instruction block | Two short sentences retain the guest-microphone warning. Detailed routing advice remains in the audio-track tooltip. |
| Narrow forms clipped Generate New ID and the bitrate suffix | Full-width ID button; wrapping forms; a bitrate minimum that survives OBS theme overrides. |
| Field accessibility names could be unclear | Explicit translated names, keyboard-focusable advanced toggle, and verified Tab/Shift+Tab/Space/arrow-key behavior. |
| New WHEP text existed only in English | Ten reviewed strings supplied and checked in every shipped locale file. |
| The documented disable route was absent from OBS's normal Stream screen | Existing masked WHEP key added to **Studio → Publish → Advanced Settings**. Clearing it disables WHEP; copying again replaces the link. |
| Input queue limited messages but had no explicit byte budget | 2 MiB WebSocket frame limit, 8 MiB queued-byte budget, bounded candidates/MIDs, NUL rejection and stop checks between queued requests. |

Validation:

- Clang 18 / WSL, ASan + UBSan: **2,361,669** production JSON/SDP parser fuzz
  executions over 181 seconds, plus a fresh 100,000-execution CI smoke run.
  No sanitizer finding, crash or timeout. Separate boundary seeds cover a
  2 MiB input, deeply nested JSON, many SDP media sections and a huge number.
- Real Windows WebSocket worker: 1,000 malformed message mutations, four
  malformed/oversized offers, a 16 MiB burst, malformed and ambiguous trickle,
  then two connected viewers, independent close and cleanup. Passed.
- **805 unit tests and 92 native media tests passed**. The additional burst
  regression also passed all four WHEP native cases.
- Actual portable OBS controls: copy, start, stop, copy while live, restart,
  clear the key, start with WHEP disabled, replace the key and restart again.
  The disabled key remains disabled while live; the replacement persists.
- All 15 locale files were rendered and keyboard-tested during the audit.
  Final advanced-key layout checks include English, German, Russian, Japanese
  and Chinese. Existing untranslated legacy labels still use the English
  fallback; this does not claim a complete native-language review of the plugin.
- Chromium and Firefox use the standalone HTTP/WebRTC fixture, not a
  third-party player product. Playback, audio, capacity, replacement and output
  restart are rechecked against the live endpoint; fuzz traffic is loopback only.

The [audit evidence](../media-assets/whep-ui-audit/results.json) records each
run's DLL hash and exact scope. [Capture provenance](../media-assets/whep-ui-audit/manifest.json)
retains original images and crop coordinates for future guides. Raw logs and
all locale captures remain in `artifacts/whep-ui-audit/`.

Keyboard tests deliver Windows key events to the owned Qt window and inspect
actual focus and resulting state through UI Automation. They do not substitute
button invocation for keyboard activation. Run clipboard checks serially because
the Windows clipboard is shared. This is basic Windows keyboard/accessibility
validation, not a screen-reader or macOS/Linux accessibility certification.

Reproduce parser fuzzing on Linux/WSL:

```sh
cmake -S . -B build-whep-fuzz -DBUILD_PLUGIN=OFF -DBUILD_WHEP_FUZZER=ON \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build-whep-fuzz --target vdoninja-whep-fuzz
cp -r tests/fuzz-corpus/whep /tmp/whep-corpus
UBSAN_OPTIONS=halt_on_error=1 build-whep-fuzz/vdoninja-whep-fuzz \
  /tmp/whep-corpus -max_total_time=180 -max_len=65536 -timeout=5
python3 scripts/check-whep-locales.py
```

Windows UI checks use a disposable OBS copy prepared by
`scripts/whep-host-native-e2e.py`. Run `scripts/plugin-ui-audit.py --obs-review
artifacts/<copy> --output artifacts/<new-result> --locale de-DE` for layouts and
navigation, and `scripts/whep-host-ui-check.py --obs-review artifacts/<copy>
--output artifacts/<new-result> --expected-version 1.1.84` for the full live flow.

Release follow-up completed in v1.1.84: CI, downloaded Windows package,
third-party player checks, and updated guide captures. Remote/mobile paths and
macOS/Linux UI/runtime remain outside this Windows audit.

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
  remote, but the media peers were local. Cross-network/mobile/TURN paths
  and macOS/Linux WHEP runtime remain unqualified. Eyevinn qualification is
  recorded separately below.

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
python scripts/whep-host-ui-check.py --obs-review artifacts/whep-obs --output artifacts/whep-ui --expected-version 1.1.84
```

Use a new output directory for each run. The default local server source is
`../whip_private`; its existing `tests/server-fixture.cjs` runs on loopback and
blocks unrelated external API calls. The hosted test uses a random disposable
key and performs no server deployment. The Python wrapper creates a separate
portable OBS copy, replaces only that copy's plugin/configuration, generates
synthetic video/audio, and checks the loaded module path and clean exit.

For UI review, `scripts/whep-host-ui-check.py` operates a stopped disposable
review under `artifacts/`, clears that review's WHEP key, and starts its own OBS
process. It checks messages after multiple stats refreshes, normal status
restoration, link generation, copying while live, and persistence after restart.
`scripts/guest-return-room-ui.py` captures only the process listed in its
`state.json`. Focused Windows keyboard events activate the controls. Raw
mouse-message automation did not activate these floating-dock buttons reliably.
Existing text clipboard contents are restored after the check.

## Published v1.1.83 qualification

https://github.com/steveseguin/ninja-obs-plugin/releases/tag/v1.1.83

All platform release jobs passed:
https://github.com/steveseguin/ninja-obs-plugin/actions/runs/41259152687.
All seven downloaded package hashes match the published checksum file.

The Windows 32.2.x package's loaded DLL was verified as SHA-256
`a09108b69b9e3634c05ea740eb12c7cb3796bca1f19f0ba270a40958be268544`.
That exact DLL passed the two-cycle live-endpoint browser workflow and the full
UI walkthrough: stopped, stop-first instructions, enable/copy, live, copy while
live, stop, and reopen with the same saved link. Feedback was checked after
multiple status-refresh ticks and also checked to return to LIVE afterward.
OBS exited cleanly, with no WHEP host errors or start-thread failures.

The Windows installer uses the existing signing certificate. This machine reports
an untrusted root for that certificate, as it did for v1.1.80; signer continuity
and package hashes were checked. This is not a claim of public-CA trust.
Full macOS/Linux WHEP browser playback and remote-network paths remain outside
this qualification; package and native-suite success do not replace those tests.

## Archived v1.1.83 screenshots and evidence

The [screenshot manifest](../media-assets/whep/manifest.json) records original
captures and exact guide crops. All UI stages and seven playback screenshots
were visually reviewed: labels are readable, expected controls are enabled, and
the decoded orange test image appears. Frame progression and audio correctness
are verified separately by browser statistics, pixel sampling and Web Audio.

[Compact machine-readable results](../media-assets/whep/validation-results.json)
include DLL hashes, browser versions, response codes, media samples and timings.
Full local logs are under `artifacts/whep-existing-protocol/`; they are not public
release assets. The archived guide images and original/cropped UI captures use
the actual v1.1.83 package and a synthetic stream. Guide crops focus on actions
and status; the originals retain the installed version footer.

The user guide is https://steveseguin.github.io/ninja-obs-plugin/whep.html.

## Published v1.1.84 qualification

https://github.com/steveseguin/ninja-obs-plugin/releases/tag/v1.1.84

All platform release jobs passed:
https://github.com/steveseguin/ninja-obs-plugin/actions/runs/41270030989

All seven release packages matched the published SHA-256 checksums. The loaded
Windows 32.2.x DLL was verified as
`ce56856a412e7f79507a18f191ec4ef3375964ded848c289d7cfc0e42f2d2c29`.
That exact downloaded DLL passed two hosted-endpoint cycles, capacity rejection,
viewer replacement, immediate stop, clean OBS shutdown, and the complete eleven
stage keyboard-operated UI flow, including disable/replacement and persistence.
All 15 packaged locale files match the reviewed source text after normalizing
the release checkout's CRLF line endings.

The actual public Eyevinn WebRTC Player also passed in Chromium and Firefox:
https://webrtc.player.eyevinn.technology/

The test selected **WHEP**, pasted the endpoint, and used the application's own
Play/Stop controls. No fetch, SDP, ICE, or adapter implementation was replaced.
An observer records peer statistics; a silent Web Audio probe measures decoded
audio. H.264 decoded frames and playback time advanced, orange pixels matched,
and Opus decoded a 440 Hz test tone. One viewer stopped while the other continued;
the stopped viewer rejoined successfully. Vendor script hashes are retained.
Firefox's video-element frame counter returned zero despite healthy decoding;
the assertion therefore uses inbound decoded frames and advancing playback time.

Run the hosted wrapper with `--third-party` to include this check. The flag
requires `--broker hosted`; fuzz traffic remains confined to loopback.

[Package results](../media-assets/whep-1.1.84/results.json) retain hashes, media
samples and UI assertions. [Capture provenance](../media-assets/whep-1.1.84/manifest.json)
retains original screenshots and guide crop coordinates. The guide now shows the
short copy button and the **Publish → Advanced Settings** key-management route.

This qualifies Windows on one machine with the public signaling service. It does
not qualify remote/mobile media paths, every WHEP player, or macOS/Linux runtime.
The parked unfinished branch and the VDO.Ninja/server repositories were unchanged.
