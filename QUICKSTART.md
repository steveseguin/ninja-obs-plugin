# OBS Plugin for VDO.Ninja - Quick Start

This guide is for first use after plugin installation.
For install/update steps, see `INSTALL.md`.

Web quick start (recommended): `https://steveseguin.github.io/ninja-obs-plugin/#quick-start`

## 1) Confirm plugin loaded in OBS

After installation, restart OBS and check:

- `Add Source` includes: `VDO.Ninja Source`
- `Settings -> Stream` includes service: `VDO.Ninja`
- `Tools` menu includes: `VDO.Ninja Control Center`

If either is missing, reinstall and confirm plugin/data paths from `INSTALL.md`.

## 2) Publish your first stream

1. In `Settings -> Stream`, set service to `VDO.Ninja`.
2. Keep `Server` at default unless you use a custom signaling host.
3. Use `Tools -> VDO.Ninja Control Center` for normal setup.
   - `Signaling Server` and `Salt` are optional; leave blank for defaults.
4. The `Stream Key` box remains in OBS for compatibility; you can still use it directly with:
   - URL form: `https://vdo.ninja/?push=mytest123&password=secret&room=myroom&salt=vdo.ninja&wss=wss://wss.vdo.ninja:443`
   - Compact form: `mytest123|secret|myroom|vdo.ninja|wss://wss.vdo.ninja:443`
5. If `VDO.Ninja` is not listed yet, open `Tools -> VDO.Ninja Control Center` and click `Apply As Stream Service`.
6. Click `Start Streaming`.

`Tools -> VDO.Ninja Control Center` `Start Publishing`/`Stop Publishing` map to the same OBS `Start Streaming`/`Stop Streaming` pipeline; they do not run as a second parallel destination.

The Tools action also applies Opus audio defaults for compatibility.

Optional: open `Tools -> VDO.Ninja Control Center` for one-place publish config, start/stop controls, generated links, and runtime peer stats.

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
A: No. In this plugin, `Go Live` in `Tools -> VDO.Ninja Control Center` triggers the same OBS stream start/stop pipeline as `Start Streaming`.

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
