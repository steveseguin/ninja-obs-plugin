# OBS Plugin for VDO.Ninja - Quick Start

This guide is for first use after plugin installation.
For install/update steps, see `INSTALL.md`.

Web quick start (recommended): `https://steveseguin.github.io/ninja-obs-plugin/#quick-start`

## 1) Confirm plugin loaded in OBS

After installation, restart OBS and check:

- `Add Source` includes: `VDO.Ninja Source`
- `Settings -> Stream` includes service: `VDO.Ninja`
- `Tools` menu includes: `VDO.Ninja Studio`

If either is missing, reinstall and confirm plugin/data paths from `INSTALL.md`.

## 2) Publish your first stream

1. In `Settings -> Stream`, set service to `VDO.Ninja`.
2. Keep `Server` at default unless you use a custom signaling host.
3. Use `Tools -> VDO.Ninja Studio` for basic setup: stream ID, password, optional room, viewer limit, links, and live controls.
   - Configure advanced options such as `Signaling Server`, `Salt`, custom ICE/TURN, and packet protection under `Settings -> Stream`; leave optional values blank for defaults.
4. The `Stream Key` box remains in OBS for compatibility; you can still use it directly with:
   - URL form: `https://vdo.ninja/?push=mytest123&password=secret&room=myroom&salt=vdo.ninja&wss=wss://wss.vdo.ninja:443`
   - Compact form: `mytest123|secret|myroom|vdo.ninja|wss://wss.vdo.ninja:443`
5. Click OBS `Start Streaming` or `Go Live` in `VDO.Ninja Studio`. Both use the saved session settings; `Go Live` also preserves advanced VDO.Ninja service options.
6. Click `Copy Viewer Link` in the Studio dock and share it. While publishing, this copies the actual running session, including any custom password, room, salt, and signaling server.

Your stream ID is saved with the OBS profile and reused. Editing the dock while stopped updates the selected VDO.Ninja service; changing to another streaming destination does not activate VDO.Ninja until you click `Go Live`.

For ICQ/CQP/CRF or custom bitrates, use `Settings -> Output -> Advanced -> Streaming`. Simple Output uses CBR. The plugin preserves the encoder's rate control and does not impose a 6000 or 12000 kbps encoder limit. It disables B-frames, including conflicting NVENC custom options, and bounds the keyframe interval to two seconds for browser playback. Quality modes vary their bitrate with the content, and REMB adaptation is disabled for those modes. Choose quality settings that fit the viewers' connections.

If recording uses the streaming encoder, start VDO.Ninja streaming before recording. An already-running shared encoder without compatible settings must finish recording before streaming can start; OBS displays instructions and leaves that recording running.

A viewer URL's `&bitrate=12000` does not reconfigure the OBS encoder. The fallback signaling server also does not change encoder settings.

To choose local publishing ports, expand `Advanced Settings` in the Studio dock and set `Local UDP ports`:

- `Auto` (or blank): keep normal port allocation, the default.
- `50000`: use that one local UDP port.
- `50000-50100`: choose available ports within that inclusive range.

Use a range for multiple viewers: each peer connection needs an available local UDP port. A single port supports one simultaneous peer; another connection must wait until that socket is released. The setting is saved per OBS profile and applies on the next stream start through either start button. It is also available in the native VDO.Ninja service's advanced properties and the legacy Control Center. Invalid values prevent publishing instead of falling back to other ports.

This controls the plugin publisher's local UDP sockets. It does not set browser-source ports, the viewer's ports, the router's external mapping, or the relay server's ports. Port forwarding is normally unnecessary, and choosing a range does not guarantee a direct connection. If you intentionally maintain router/firewall rules, match them to the chosen local range. If the port/range is occupied or exhausted, that viewer connection fails instead of allocating outside it.

`VDO.Ninja Studio` `Go Live`/`Stop` map to the same OBS `Start Streaming`/`Stop Streaming` pipeline; they do not run as a second parallel destination.

Starting VDO.Ninja publishing also applies Opus audio defaults for compatibility.

The Studio dock also provides generated links and runtime peer stats.

Viewer link pattern:

- No password: `https://vdo.ninja/?view=mytest123`
- With password: `https://vdo.ninja/?view=mytest123&password=yourpass`
- Room view: `https://vdo.ninja/?view=mytest123&room=myroom&solo`
- Room view with password: `https://vdo.ninja/?view=mytest123&room=myroom&solo&password=yourpass`

## 3) Ingest a VDO.Ninja stream in OBS

1. Recommended today: use Browser Source or room-based auto-inbound.
2. `VDO.Ninja Source` defaults to a browser-backed viewer. Enable `Use Native Receiver (Experimental)` only for native VP9/H.264/Opus testing or compatible VP9 alpha transparency.
3. Confirm video/audio appears in preview/program.

## 4) Transparent avatars and alpha video

For transparent avatars or graphics, use Game Capture as the VDO.Ninja publisher and this OBS plugin as the native receiver:

```text
Spout2 avatar/graphics app -> Game Capture -> VDO.Ninja -> OBS VDO.Ninja Source
```

1. Enable Spout2 output in the avatar or graphics app. VTube Studio has been tested.
2. In Game Capture, choose `Video Source -> Spout2 (avatar apps)`.
3. Select the Spout2 sender, choose the audio source you want, select `VP9`, and enable the OBS alpha workflow.
4. Start publishing from Game Capture.
5. In OBS, add `VDO.Ninja Source`, enter the matching stream ID/password, and enable `Use Native Receiver (Experimental)`.

Browser Sources and normal browser viewers do not composite the alpha track; they receive standard color video. VP9 alpha is CPU-heavy, so lower Game Capture resolution/FPS if CPU usage or dropped frames become a problem.

## 5) Recommended first validation pass

- Open two viewer tabs to verify multi-viewer behavior.
- Refresh a viewer page and confirm playback resumes.
- Try one run with password and one run without.
- If using rooms, test `room + scene + view` links for room workflows.
- If using transparency, confirm OBS receives the stream through `VDO.Ninja Source` with `Use Native Receiver (Experimental)` enabled.

## 6) Useful advanced options

Configure these under `Settings -> Stream`; the Studio dock does not expose them:

- `Salt` (optional; blank uses default `vdo.ninja`) for compatibility/self-hosting needs
- Custom signaling WebSocket URL (optional; blank uses default `wss://wss.vdo.ninja:443`)
- Custom STUN/TURN servers (use `;` to separate multiple entries)
- Force TURN for difficult NAT/network paths (requires a TURN server entry)

Default ICE behavior:
- Empty custom ICE field uses built-in STUN (`stun:stun.l.google.com:19302`, `stun:stun.cloudflare.com:3478`)
- TURN is not auto-added; provide your own TURN server if needed

## 7) Troubleshooting basics

- Restart OBS after install/update.
- Verify stream ID/password exactly match viewer URL.
- For transparency, verify Game Capture can see the Spout2 sender and is publishing with VP9 plus OBS alpha workflow enabled.
- Check OBS logs and VDO.Ninja stats for packet loss/RTT.
- Use `INSTALL.md` for reinstall/uninstall paths.
- If using portable OBS from terminal, launch `obs64.exe` from `bin\64bit` (wrong working directory can trigger `Failed to load theme`).

## FAQ

Q: `Go Live` vs `Start Streaming` - are they different?  
A: No. In this plugin, `Go Live` in `Tools -> VDO.Ninja Studio` triggers the same OBS stream start/stop pipeline as `Start Streaming`.

Q: Can I stream to VDO.Ninja and another destination at the same time?  
A: Not with this plugin/service path by itself. It uses OBS's active stream output slot, so VDO.Ninja is the single active destination in that slot.

Q: Where are Linux/macOS install steps?  
A: See `INSTALL.md`:
- Linux: [INSTALL.md#install-linux](INSTALL.md#install-linux)
- macOS: [INSTALL.md#install-macos](INSTALL.md#install-macos)

Additional docs:

- `README.md`
- `TESTING_OBS_MANUAL.md`
- `SECURITY_AND_TRUST.md`
