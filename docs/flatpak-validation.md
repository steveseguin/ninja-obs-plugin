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

## Why the dependency patch is included

The existing WHEP burst test exposed concurrent TCP queue draining in
libdatachannel 0.20.2: the writable-event handler called `trySendQueue()` without
its required send mutex. Payload bytes could then be interpreted as WebSocket
frames. The Flatpak build backports the one-line lock used upstream and retains
the stress test, with five additional repetitions. Tests were not weakened.

Upstream source:
https://github.com/paullouisageneau/libdatachannel/blob/master/src/impl/tcptransport.cpp

## Remaining work

1. Test real publishing, receiving, Guest Return and WHEP playback on a Linux
   desktop, including microphone/camera access, hardware encoding and a long session.
2. Run the new GitHub Flatpak workflow before a public release; its installed-OBS
   check is a release dependency and the bundle is included in release checksums.
3. Publish through `scripts/release.ps1` after qualification; replace the test-build
   notices in the install guides when the download exists.
4. Evaluate the TCP send-lock backport in native Windows/macOS/Linux dependency
   builds and run their network regression tests.
5. Optional: submit a Flathub extension for automatic plugin updates. Direct
   bundle installation works without a Flathub listing.

Reproduction and installation:
https://github.com/steveseguin/ninja-obs-plugin/tree/main/packaging/flatpak
