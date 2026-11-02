# Flatpak validation — 2026-11-01

Status: working test bundle, not a published plugin release or Flathub listing.

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

## Why the dependency patch is included

The existing WHEP burst test exposed concurrent TCP queue draining in
libdatachannel 0.20.2: the writable-event handler called `trySendQueue()` without
its required send mutex. Payload bytes could then be interpreted as WebSocket
frames. The Flatpak build backports the one-line lock used upstream and retains
the stress test, with five additional repetitions. Tests were not weakened.

Upstream source:
https://github.com/paullouisageneau/libdatachannel/blob/master/src/impl/tcptransport.cpp

## Remaining work

1. Test Guest Return rooms and real guest devices on a Linux desktop, including
   microphone/camera access, hardware encoding, cross-NAT/TURN connectivity and a
   long session. Resolve or characterize the default-STUN gathering delay on the
   intended network. Server publishing/native reception and host-candidate WHEP
   lifecycle checks above are complete.
2. Keep the GitHub Flatpak workflow green for the eventual release commit; the
   first run passed. Its installed-OBS check is a release dependency and the
   bundle is included in release checksums.
3. Publish through `scripts/release.ps1` after qualification; replace the test-build
   notices in the install guides when the download exists.
4. Evaluate the TCP send-lock backport in native Windows/macOS/Linux dependency
   builds and run their network regression tests.
5. Optional: submit a Flathub extension for automatic plugin updates. Direct
   bundle installation works without a Flathub listing.

Reproduction and installation:
https://github.com/steveseguin/ninja-obs-plugin/tree/main/packaging/flatpak
