# OBS Flatpak extension

Keep OBS installed through Flatpak. This package is for x86-64 Linux and the current
Flathub stable OBS runtime (OBS 33.0.1, Freedesktop 26.08). It is separate from the
native Ubuntu archive and the Arch source package.

Status: test build; not yet published in a plugin release or listed on Flathub.

## Install

Close OBS. Use the supplied `obs-vdoninja-linux-x86_64.flatpak` test bundle (or build
it below), and run these commands in its folder:

```bash
flatpak update com.obsproject.Studio
flatpak install --user ./obs-vdoninja-linux-x86_64.flatpak
flatpak run com.obsproject.Studio
```

Open **Docks → VDO.Ninja Studio**, or add a **VDO.Ninja** source.
There is no need to replace Flatpak OBS with a pacman installation.

This is a downloadable extension bundle, not a Flathub listing. To update the
plugin, download and install the newer bundle with the same command. `flatpak
update` alone updates OBS but does not fetch new versions of this bundle.

Remove only the plugin:

```bash
flatpak uninstall --user com.obsproject.Studio.Plugin.VDONinja
```

## Build and validate on Linux

On Ubuntu, install `flatpak flatpak-builder xvfb xauth xdotool imagemagick
python3-websocket`. Add Flathub if needed, then install OBS and its SDK:

```bash
flatpak remote-add --user --if-not-exists flathub https://flathub.org/repo/flathub.flatpakrepo
flatpak install --user flathub com.obsproject.Studio org.freedesktop.Sdk//26.08
bash scripts/build-flatpak.sh
xvfb-run -a -s '-screen 0 1440x900x24' dbus-run-session -- \
  python3 scripts/validate-flatpak.py \
  "$HOME/.cache/obs-vdoninja-flatpak/obs-vdoninja-linux-x86_64.flatpak" \
  "$HOME/flatpak-validation-$(date +%s)"
```

The build runs the existing OBS/FFmpeg-linked media tests, including real local
WebRTC connections. The installed-package check launches OBS with a separate
configuration, exercises source creation/destruction, verifies the loaded module
path, captures a screenshot, and checks normal shutdown. It saves a JSON result
and OBS log beside the screenshot. It does not alter existing OBS scenes.

For an installed-package media check, install the repository's Node dependencies
and Playwright Chromium/Firefox browsers, and provide host `ffmpeg` with libx264.
Firefox needs an H.264 decoder; an external OpenH264 GMP can be supplied through
`MOZ_GMP_PATH` (the directory containing `gmpopenh264.info` and
`libgmpopenh264.so`). The media check uses synthetic video/audio, public VDO.Ninja
signaling and the hosted WHEP service:

```bash
MOZ_GMP_PATH=/path/to/gmp-gmpopenh264/2.6.0 \
xvfb-run -a -s '-screen 0 1440x900x24' dbus-run-session -- \
  python3 scripts/validate-flatpak.py \
  ./obs-vdoninja-linux-x86_64.flatpak ./flatpak-media-validation \
  --media-tests --soak-seconds 120
```

This checks 720p30 publishing with a simultaneous native receiver and browser
viewer, followed by two WHEP start/stop cycles in Chromium and Firefox. WHEP
checks decoded pixels and a 440 Hz audio tone, viewer limits, independent viewer
closure, replacement viewers and immediate output stop. Logs and screenshots
are retained in the output directory. This optional network test is separate
from the deterministic linked tests run during every build.

Use `--media-tests publishing` or `--media-tests whep` to rerun one workflow.
For peers on the same machine, `WHEP_PLAYER_ICE_SERVERS_JSON='[]'` restricts the
WHEP player to host candidates, avoiding dependence on public STUN replies. The
report records this setting and the selected candidate types; this mode does
not qualify connectivity across NAT or through TURN. The default keeps the
player's Google and Cloudflare STUN servers.

The manifest pins the SDK and libdatachannel source, including our existing relay
policy patch. Building fails if OBS switches SDKs, so a runtime change receives
review rather than silently producing a mismatched package. The native release
baseline remains unchanged. Live guest sessions, hardware capture/encoding and
long sessions still need testing on the user's Linux desktop.

The Flatpak build also backports upstream's TCP writable-event send lock to
libdatachannel 0.20.2. Without it, the poll thread and application thread can drain
the same send queue concurrently under backpressure. The existing 16 MiB WHEP
wire stress test reproduced WebSocket frame corruption; it now runs five extra
times per build. Upstream implementation:
https://github.com/paullouisageneau/libdatachannel/blob/master/src/impl/tcptransport.cpp

Follow-up: evaluate this same dependency backport in the native Windows, macOS
and Linux packages, with their network regression tests. This Flatpak task does
not change those dependency builds.

OBS extension specification:
https://github.com/obsproject/obs-studio/blob/master/build-aux/com.obsproject.Studio.json
