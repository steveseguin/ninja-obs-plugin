# Flatpak validation — 2026-11-01

[v1.1.85 is published](https://github.com/steveseguin/ninja-obs-plugin/releases/tag/v1.1.85).
Distribution is a direct Flatpak bundle; there is no Flathub listing. All nine
packages were downloaded and matched `checksums.txt` and GitHub asset digests.
[Release verification](assets/flatpak/release-v1.1.85-verification.json).

| Item | Result |
| --- | --- |
| Platform | x86-64, OBS Flatpak 33.0.1, Freedesktop 26.08 |
| Plugin core | 1.1.84; native package compatibility baseline unchanged |
| Build | Flatpak SDK build, pinned libdatachannel 0.20.2, relay patch and TCP send-lock backport |
| Linked tests | All 92 media cases; all nine CTest registrations passed |
| Repeated stress | Five extra passes: 1,000 wire mutations, malformed/oversized SDP, 16 MiB input burst, two independent viewers and independent viewer closure |
| Installation | Real OBS Flatpak on WSL Ubuntu 24.04 and a separate Ubuntu 24.04 server |
| Runtime | Extension-mounted module path verified; three native-source create/remove cycles; Studio opened by keyboard; clean OBS shutdown |
| Update | Installing a newer bundle over the previous bundle succeeded |
| Visual review | Tools menu and Studio screenshots reviewed on both machines; guide reviewed at desktop and mobile widths |

The runtime checks used Xvfb and software rendering. They are **not** a live
guest-session rehearsal or hardware encoder/capture qualification.

Evidence: [native Ubuntu result](assets/flatpak/validation.json),
[Studio screenshot](assets/flatpak/obs-studio.png),
[Tools menu](assets/flatpak/tools-menu.png).

## Installed-package media retest

The Flatpak workflow for `a475a88` [passed on GitHub Actions](https://github.com/steveseguin/ninja-obs-plugin/actions/runs/41377448639),
including all nine linked CTest registrations, five additional WHEP burst passes,
installation, module-path verification and clean OBS shutdown. Its actual bundle
(artifact `13111453716`, SHA-256
`736df900031e6a8c46f190ea25055295d155ba036abd45cf71ed06da0035a337`)
was then downloaded and tested on this Ubuntu server.

| Installed CI bundle check | Result |
| --- | --- |
| 720p30 H.264/Opus publishing, 120-second measured interval | Passed; 29.982 decoded fps; zero reported video drops, packet loss or freezes |
| OBS rendering/output during that interval | Zero render skips and output skips; 9.96 ms average render time |
| Browser audio during that interval | Zero new concealment samples/events |
| Simultaneous native receiver | Decoded audio/video; two screenshots showed moving test-pattern pixels |
| Hosted WHEP broker, Chromium 145 and Firefox 146 | Both decoded H.264/Opus, expected orange pixels and the 440 Hz tone |
| WHEP lifecycle with host candidates | Two cycles; three viewers, fourth-viewer rejection, independent closure and replacement all passed |
| Output stop / immediate start-stop | Normal stops around 250 ms; immediate start-stop 552 ms |
| Shutdown after WHEP | Clean exit |

These used synthetic media, Xvfb/software rendering and media peers on the same
host. Signaling and the WHEP broker were public services. They do not establish
WAN/TURN connectivity, physical device support or long-session reliability.

The default-STUN WHEP test twice timed out gathering a third viewer's complete
offer, before sending it to the plugin. A separate Chromium control without OBS
reproduced this: Google STUN, Cloudflare STUN and both together produced host and
server-reflexive candidates but remained in `gathering` after 25 seconds. With no
STUN servers, gathering completed in 130 ms. The full lifecycle retest explicitly
used `WHEP_PLAYER_ICE_SERVERS_JSON='[]'`; this is recorded in the evidence and is
not a passing result for the default-STUN workflow.

A later browser-only control allowed up to 90 seconds. All public-STUN cases
completed at approximately 40 seconds, with server-reflexive candidates available
within 120 ms and additional STUN binding requests ending with error 701. The
host-only control completed in 191 ms. The fixture's unchanged default is 12
seconds; `WHEP_ICE_GATHER_TIMEOUT_MS=60000` explicitly extends that deadline for
network qualification without changing media/lifecycle assertions.
[Control results](assets/flatpak/stun-gathering-control.json).

With that explicit 60-second deadline, the updated CI bundle passed both WHEP
cycles using the default STUN servers: Chromium/Firefox audio/video, three
viewers, fourth-viewer rejection, independent close/replacement, stop and clean
shutdown. Immediate stop now waits for OBS to report active first, because the
OBS WebSocket API rejects `StopStream` during asynchronous startup; startup and
stop durations are recorded separately (251 ms each in the successful retest).
An earlier attempt passed the first media cycle but hit that harness timing
error. The successful run is not a passing result for the original 12-second
gathering deadline and does not qualify WAN/TURN.
[Default-STUN results](assets/flatpak/default-stun-validation.json).

Firefox initially lacked an H.264 decoder. The retest supplied Cisco OpenH264
2.6.0 through `MOZ_GMP_PATH`, with its download SHA-512 checked against
[Mozilla's published metadata](https://bugzilla.mozilla.org/show_bug.cgi?id=1953427#c0).

The retest also found and fixed a harness issue: host `/tmp` output directories
were hidden by Flatpak's private `/tmp`. The launcher now grants access to the
specific test directory. It also bounds startup waiting by elapsed time, retains
failure reports, and supports optional `--media-tests` workflows. The plugin
binary did not need a change.

Evidence: [sanitized media results](assets/flatpak/media-validation.json).
Raw logs, screenshots and workflow reports are retained locally under
`artifacts/flatpak-ci-media/` and `artifacts/flatpak-ci-whep-host/`.
See [reproduction commands](../packaging/flatpak/README.md).

## Synthetic Guest Return room

The same installed CI bundle also passed a room session with a director and two
Chromium guests using synthetic cameras/microphones. Both guests received 1280×720
program video at approximately 30 fps during a 38-second measured interval.
The return audio contained the 1200 Hz shared clip and 1500 Hz host microphone;
the guests' 300/600 Hz microphone tones were over 90 dB below those program tones.
Guest audio was routed to OBS track 1 and excluded from return track 2.

One guest reloaded/rejoined successfully. Stopping and restarting Guest Return
restored program media to both guests, and OBS shut down cleanly afterward.
These checks used public room signaling with peers on the same server and do not
qualify physical cameras/microphones, WAN/TURN routes or extended sessions.

Evidence: [Guest Return results](assets/flatpak/guest-return-validation.json).

## NVIDIA encoding check

The updated pre-release CI bundle (`40a7ee4`, artifact `13114261277`) passed a
two-minute 720p30 H.264/Opus publishing check using the GTX 750 Ti and NVIDIA
580.178.04 driver. NVIDIA's process monitor confirmed OBS was using the encoder
engine. Under Xvfb software rendering, OBS selected NVENC's system-memory path.
This qualifies hardware encoding on this GPU, not GPU rendering/texture sharing.

The browser decoded 29.991 fps with zero reported drops, loss or freezes, and the
simultaneous native receiver decoded both audio and video. OBS reported zero
render/output skips; there were zero new audio concealment samples/events.
Shutdown was clean. [Sanitized results](assets/flatpak/nvenc-validation.json).

## Tests after publication

The released Flatpak bundle (SHA-256
`f71b4b92605b1f148662a05a8b97f25e9760410ba14231b2a904aeff7c7f6e28`)
was downloaded, installed, and confirmed to load plugin **1.1.85** in OBS 33.0.1.
A five-minute 720p30 NVENC H.264/Opus test passed with simultaneous browser and
native audio/video reception: 29.993 decoded fps, zero reported video drops,
packet loss, freezes, OBS render/output skips or new audio concealment. Shutdown
was clean. [Published-bundle results](assets/flatpak/published-v1.1.85-validation.json).

The first fresh-profile attempt timed out on `CreateScene` before streaming.
The same failure reproduced once in five plain OBS 33 controls with the plugin
absent; its screenshot showed that the main window had not appeared. The harness
now waits for a visible OBS main window before frontend edits, in addition to
WebSocket readiness. Five plain-OBS controls and three released-Flatpak startup
checks then passed, including normal shutdown. Failed attempts are retained,
not counted as passes. [Startup controls](assets/flatpak/startup-readiness-control.json).

Release downloads, logs, screenshots and scripts are retained on the validation
server under `/home/ubuntu/ninja-flatpak-validation/v1.1.85/`.

## Why the dependency patch is included

The existing WHEP burst test exposed concurrent TCP queue draining in
libdatachannel 0.20.2: the writable-event handler called `trySendQueue()` without
its required send mutex. Payload bytes could then be interpreted as WebSocket
frames. The Flatpak build backports the one-line lock used upstream and retains
the stress test, with five additional repetitions. Tests were not weakened.

Upstream source:
https://github.com/paullouisageneau/libdatachannel/blob/master/src/impl/tcptransport.cpp

## Remaining work

1. Test real guest devices on a Linux desktop, including
   microphone/camera access, other hardware encoders/GPU rendering, cross-NAT/TURN connectivity and a
   long session. Check full ICE gathering on the intended network; the server
   delay has been characterized above. Server publishing/native reception and host-candidate WHEP
   lifecycle checks and the synthetic Guest Return room above are complete.
2. Keep the GitHub Flatpak workflow green for each release. Its installed-OBS
   check is a release dependency and the bundle is included in release checksums.
3. Evaluate the TCP send-lock backport in native Windows/macOS/Linux dependency
   builds and run their network regression tests.
4. Optional: submit a Flathub extension for automatic plugin updates. Direct
   bundle installation works without a Flathub listing.

Reproduction and installation:
https://github.com/steveseguin/ninja-obs-plugin/tree/main/packaging/flatpak
