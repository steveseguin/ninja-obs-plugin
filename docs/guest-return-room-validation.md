# Group room Guest Return validation

## Scope

The plugin provides an independent OBS program return alongside the normal public
stream. Room guests receive program video using `broadcast=RETURN_ID`, hear the
selected OBS return track, and retain direct guest-to-guest audio. No VDO.Ninja
code changes are required for this workflow.

Implemented controls: room guest invitation, viewer capacity (1–50, default 10),
connected/maximum viewer status, independent return size/FPS, group audio guidance,
and independent signaling,
salt, custom STUN/TURN, and Force TURN settings. Links preserve signaling/salt and
omit relay credentials. Settings remain locked during a session and saved per
OBS profile. Force TURN without a TURN entry is rejected before allocation.

## Validation record

Completed on Windows on 2026-10-31 with OBS 32.2.2, Chrome 143.0.7499.4,
one browser director and three camera/microphone guests. A fourth guest was used
to exceed the configured capacity. Browsers used synthetic camera devices and
distinct audio tones; room signaling, WebRTC, OBS ingest, encoding, and playback
were real. All clients ran on the same machine. The public output went to a local
RTMP recorder, not a public YouTube account.

The loaded module path was verified in the isolated portable OBS process. The
final DLL SHA-256 was
`6c3e7fe719e1e71c4096aa670437bd227138f0003f732bbd22594450654c1ecf`.
The media manifest records both the initial and final build hashes; a private
helper rename was rebuilt before the persistence/custom-network checks.
The user's regular OBS configuration was not changed.

| Check | Result |
| --- | --- |
| Plugin build and unit suite | Passed; 806 tests. Changed C++ files passed clang-format 14. |
| Copied room invitation | Actual clipboard URL used to join; matching room/password/broadcast ID; no fixed guest publishing ID or viewer-only flags. |
| Guest flow | Join Room with Camera, device selection, START, self-preview and composed program verified visually. |
| Full room video | All three guests decoded 1280x720 program video; decoded frames continued advancing. Other guest connections carried audio without video. |
| Return audio | Host 1500 Hz and clip 1200 Hz reached all guests. Guest 300/600/900 Hz tones were at least 74.8 dB below the intended return tones. |
| Direct conversation | Other guests' tones and received audio bytes were present on each guest's direct room connections. |
| Public audio | Local RTMP recording contained all five tones: three guests, host, clip. |
| Actual OBS routing control | A follow-up native mouse click on Alice's Track 2 checkbox added her tone; excluding her reduced it by 87.3 dB. OBS's backend track state was also checked. |
| Capacity | Three guests plus director reached 4/4. An extra viewer was rejected and logged; existing guests continued receiving. |
| Late join and reconnect | Third guest joined an active return. Alice reloaded, rejoined, retained her stream ID, and received the return again. |
| Independent outputs | Stopping return left the main RTMP output active; restarting return reconnected guests. Stopping the public output left the return active. |
| Saved settings | Capacity 6, custom signaling, salt, STUN and Force TURN state survived OBS restart. Return stayed stopped until started. |
| Custom network | Playback succeeded through `wss://proxywss.rtc.ninja:443` with explicitly configured STUN. This does not establish a TURN relay path. |
| Relay validation | Force TURN without a TURN entry showed an inline error before starting. Test relay credentials never appeared in either copied link. |
| UI | Missing room disables the room invite; normal/narrow layouts inspected; settings locked while active; readable error and capacity status. |
| Shutdown | Test OBS instances exited with code 0, including the follow-up with return active. Test browser/controller processes were closed. |

### Screenshots for guides

[Screenshot walkthrough](../media-assets/guest-return-room/README.md) contains
**23 original/cropped image pairs**, individual visual review notes, and a
[machine-readable manifest](../media-assets/guest-return-room/manifest.json)
with crop coordinates, dimensions, binary provenance and SHA-256 hashes. Captures
cover the main output, public track, individual guest source, audio routing,
return settings, invitation, device selection, all three guests, director room,
capacity, stop/rejoin, error handling, saved network settings, and narrow layout.

These are actual application captures with crops only, not reconstructed UI.
Some settings-only images were captured after the media session ended. The
manifest identifies that context. Synthetic media and disposable demo identities
are deliberate; do not reuse the shown links for production.

Visual inspection caught two automation problems: selecting an OBS settings
list item changed its highlight without opening the page, and toggling a Qt
checkbox value did not emit OBS's routing `clicked()` signal. Captures were
retaken after native mouse interactions. Audio assertions wait for playback to
settle after routing changes. Rejected diagnostic captures remain in local
artifacts and are not part of the guide set.

### Reproduction and evidence

The persistent Windows review helpers are:

- `scripts/guest-return-room-setup.py OUTPUT`: copy the portable fixture, install
  the built DLL, configure a local RTMP destination, launch OBS and verify the
  loaded module. OUTPUT must be new.
- `scripts/guest-return-room-ui.py OUTPUT ACTION ...`: operate the owned OBS
  process and capture application windows; `capture NAME OBJECT_ID` saves full
  and cropped PNGs plus crop metadata. `audio-track NAME TRACK` sends a mouse
  click to the actual OBS routing checkbox.
- `scripts/guest-return-room-browser.cjs OUTPUT`: run a loopback controller on
  port 19356 for real Chrome director/guest sessions and OBS WebSocket requests.
  It supports `init`, `join`, `click`, `ingest`, `snap`, `probe`, `reload`,
  `stream`, `request`, and `close`. The local RTMP receiver uses port 19355.
- `scripts/check-guest-return-room.py OUTPUT`: assert recorded media, audio,
  identity, capacity, persistence, link and public-recording results.

For example, send `{ "action": "init" }` as JSON to
`http://127.0.0.1:19356`, then `join`, click `text=Join Room with Camera`, click
`#gowebcam`, and `ingest` with a zero-based `guest` index. Copy the invitation
through the actual plugin button into `OUTPUT/invite.txt` before joining.
These are review tools, not shipping application dependencies. They need Python
Pillow/numpy/psutil/pywinauto, the repository's Playwright installation, installed
Chrome, the existing portable OBS fixture, and the local FFmpeg build.

Raw run evidence is in `artifacts/guest-return-room-20261031/`; the additional
mouse-routing run is in `artifacts/guest-return-audio-click-20261031/`. These
ignored directories contain session observations, recordings, logs and saved
test settings. The compact verified results are preserved in
[validation-results.json](../media-assets/guest-return-room/validation-results.json).

Commands run:

```powershell
cmake --build build-plugin-release-validation --config RelWithDebInfo --target obs-vdoninja
cmake --build build-installer-release-tests --config Release --target vdoninja-tests
ctest --test-dir build-installer-release-tests -C Release --output-on-failure
python scripts/check-guest-return-room.py artifacts/guest-return-room-20261031
```

## Extended qualification: 20 guests and remote devices

The final Windows build (`b7309f9222b25a254f4419c79e9bf5b6d323a51f3e3a2bd913894f816fa3a852`)
passed a director plus **20 synthetic camera/microphone guests**, each ingested into OBS.
The public stream ran at 1280x720/30 fps to a local RTMP recorder. The independent
return ran at 960x540/15 fps and 1500 kbps, with capacity 22 (21 connected).
All 20 returns advanced video and audio over the 30.305-second measurement;
measured video rates ranged from 14.95 to 15.01 fps. OBS reported zero skipped
output/render frames. Guest 20 rejoined, and stopping the return left the public
output active. The full public test ran for more than 11 minutes. This is a local
synthetic load test, not a rehearsal on 20 independent Internet connections.

A separate remote-device run passed Firefox 146, BrowserStack iPhone Safari and
Android Chrome using the actual copied invite, device selection and START flow.
Each decoded 1280x720 program video and advancing audio. Browser-side selected
candidate stats confirmed the publisher's **UDP relay**, corroborated by native
selected-route logs. Device-farm cameras may appear black. No cellular or
TCP/TLS-only network claim is made. Raw user agents are retained in the evidence.

Testing exposed a bundled libdatachannel/libjuice bug: relay-only policy filtered
announced candidates but still nominated direct pairs. The pinned dependency
patch prevents direct local pairs; the same remote tests passed after a clean
rebuild. An unreachable relay test did not fall back to a direct connection.
Source installers and release workflows apply the patch to both dependencies.
The bundled backend still does not support TCP/TLS-only TURN networks.

Resolution, FPS divisor and capacity survived OBS restart; return remained
stopped until manually started. Two Qt automation issues were caught by media
assertions and screenshots: changing combo values via keyboard/native messages
had no effect, and an off-screen dock defeated physical clicks. The harness moves its owned dock on screen and verifies the selected text.
A later clean-build smoke also caught a Qt popup belonging to its own native
window: the driver now opens the popup through accessibility and sends the mouse
click to that popup handle, rather than to the dock behind it.
The failed 720p assertions are retained locally, not counted as reduced-size passes.

Six-channel native reception passed distinct-tone 5.1 recording and reconnect.
Stereo downmix passed its five main channels; OBS correctly omits the LFE channel.
This used a packaged Game Capture publisher with synthetic capture, not physical
six-channel hardware. 807 unit cases and 91 OBS/FFmpeg-linked cases passed in the
combined working tree (three linked cases cover pending WHEP work).

[Extended screenshot manifest](../media-assets/guest-return-room/extended/manifest.json)
and [compact results](../media-assets/guest-return-room/extended/validation-results.json)
preserve the final screenshots and measurements. The original 23-image walkthrough
remains the complete base setup flow; the extended set adds final controls,
20-person playback, director view, and four stages for each remote browser.
All captures were visually reviewed; permission-transition captures do not show
native permission dialogs. Crops never reconstruct UI.

The three HTML guides passed 1280px desktop and 390px mobile layout, local links,
image loading, calculator and checklist checks. Full-page and section captures,
plus printable PDFs, are in `artifacts/guest-return-guides-published-1.1.78/`.

## Delivery checklist and remaining work

- [x] Windows full-room load test with 20 guests, rejoin and independent outputs.
- [x] Original three-guest audio isolation, capacity and native checkbox checks.
- [x] Separate Firefox, iPhone Safari and Android Chrome remote playback checks.
- [x] Proven UDP relay path and unreachable-relay negative check; fix direct fallback.
- [x] Independent return size/FPS, final UI captures, and restart persistence.
- [x] Six-channel 5.1 reception and stereo downmix with reconnect.
- [x] Host, guest and pre-show visual guides; desktop/mobile review.
- [x] Release packaging/CI, ZIP installation, checksum verification and final
  packaged-binary 20-guest room test.
- [ ] Actual rehearsal with Matt's participants and their Internet connections.
  Target up to 20 people. This needs participant/device access; automated local
  guests do not establish their network quality or acoustic echo behavior.
- [ ] macOS/Linux full-room runtime qualification. CI compile/module-load checks
  are useful gates but do not replace a room rehearsal on those hosts.
- [ ] TCP/TLS-only TURN support: requires a suitable ICE backend and separate
  restricted-network tests. Current UDP relay checks do not establish this.
- [ ] Hosted WHEP publication: production broker returned HTTP 504. A disposable WebSocket/HTTP protocol
  probe confirmed its POST contains no `sessionId`, which the pending native
  implementation requires. Preserve the
  pending implementation, fix/deploy the broker protocol and rerun hosted E2E
  before shipping its controls or guide. Local broker success is insufficient.
- [ ] Native Meshcast/server distribution: separate feature requiring native
  WHIP publishing, server-return identity in room invitations, auth/reconnect
  handling and multi-device tests. Direct WHEP hosting does not reduce OBS upload.

At 22 return viewers and 1500 kbps video, allow about **46.8 Mbps** for the return
including the guide's audio/headroom allowance, plus public streaming and other
uploads. Lower upload capacity needs a lower return bitrate or server distribution.
Automatic per-guest mix-minus is separate work; the documented group return
excludes all guest mics and keeps their direct room conversation.

## Release verification

v1.1.78 was cut with `scripts/release.ps1 -Action cut -Bump patch -Push` from
a clean main checkout; pending WHEP work remains uncommitted in the original
checkout. The clean release passed 807 unit and 88 native OBS/FFmpeg tests.
Windows (both OBS compatibility bands), macOS arm64 and Linux CI packages passed.
The Arch source package also passed its current OBS 33 build, linked tests,
install/reinstall, module initialization and removal after updating its SDK
dependency band and SIMD header dependency. Official archives retain their
OBS 32.2.0 / Windows legacy 32.0.4 SDK baselines.

A clean locally built v1.1.78 passed two-guest return/media/independent-output
smoke checks. A separate native receiving test passed decoded moving pixels and
audio with the native selected route reporting UDP relay. Chrome learned that
remote candidate as `prflx`; do not describe it as browser-side `relay`-type proof.
The earlier timeout and remote-type assertions were harness errors (publisher
peer map versus receiver peer map, and the learned candidate classification);
the raw runs are retained in ignored artifacts. Browser-side relay-type proof
for Guest Return publishing is from the separate remote-device run above.

The three visual guides are published at
[Host setup](https://steveseguin.github.io/ninja-obs-plugin/guest-return.html),
[Guest joining](https://steveseguin.github.io/ninja-obs-plugin/guest-return-join.html),
and [Pre-show check](https://steveseguin.github.io/ninja-obs-plugin/guest-return-rehearsal.html).
All three returned HTTP 200 and loaded their screenshot assets in a real browser.

The published Windows ZIP was installed with its own `install.ps1` into isolated
portable OBS. Its loaded DLL hash was
`2e8f55ff538885dc26201206bc035f4e41dd35e957042ddca035187b59a7929e`.
Both the downloaded ZIP and setup EXE matched published SHA-256 checksums.
The packaged DLL passed the full **20-guest** run again: moving 960x540/15 fps
return video, advancing audio, twentieth guest rejoin, unreachable-relay negative
check, and public output surviving return stop. Final packaged controls and
20-guest program captures now appear in the host guide. One clipped settings
capture was rejected and retaken with the complete window visible.

Release: https://github.com/steveseguin/ninja-obs-plugin/releases/tag/v1.1.78

All owned test OBS and browser/controller sessions were closed after validation.
