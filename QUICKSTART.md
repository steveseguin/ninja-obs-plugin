# OBS VDO.Ninja Plugin - Quick Start

This guide gets you from install to first working stream quickly.

Web quick start (recommended): `https://steveseguin.github.io/ninja-obs-plugin/#quick-start`

On Windows, use `obs-vdoninja-windows-x64-setup.exe` first.  
If you are using the ZIP package instead, run `install.cmd`.

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

## 3) Ingest a VDO.Ninja stream in OBS

1. Recommended today: use Browser Source or room-based auto-inbound.
2. `VDO.Ninja Source` exists, but native ingest is still experimental.
3. Confirm video/audio appears in preview/program.

## 4) Recommended first validation pass

- Open two viewer tabs to verify multi-viewer behavior.
- Refresh a viewer page and confirm playback resumes.
- Try one run with password and one run without.
- If using rooms, test `room + scene + view` links for room workflows.

## 5) Useful advanced options

- `Salt` (optional; blank uses default `vdo.ninja`) for compatibility/self-hosting needs
- Custom signaling WebSocket URL (optional; blank uses default `wss://wss.vdo.ninja:443`)
- Custom STUN/TURN servers (use `;` to separate multiple entries)
- Force TURN for difficult NAT/network paths (requires a TURN server entry)

Default ICE behavior:
- Empty custom ICE field uses built-in STUN (`stun:stun.l.google.com:19302`, `stun:stun.cloudflare.com:3478`)
- TURN is not auto-added; provide your own TURN server if needed

## 6) Troubleshooting basics

- Restart OBS after install/update.
- Verify stream ID/password exactly match viewer URL.
- Check OBS logs and VDO.Ninja stats for packet loss/RTT.
- Use `INSTALL.md` for reinstall/uninstall paths.
- If using portable OBS from terminal, launch `obs64.exe` from `bin\64bit` (wrong working directory can trigger `Failed to load theme`).

Additional docs:

- `README.md`
- `TESTING_OBS_MANUAL.md`
- `SECURITY_AND_TRUST.md`
