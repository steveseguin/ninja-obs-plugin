# OBS VDO.Ninja Plugin - Install / Update / Uninstall

Windows now ships with a GUI installer plus script-based fallback.
Preferred first-run guide: `https://steveseguin.github.io/ninja-obs-plugin/#quick-start`.
Every release archive still includes `QUICKSTART.md` as an offline copy.

## Windows

### Install or update

Recommended (new users):

1. Download `obs-vdoninja-windows-x64-setup.exe` from Releases.
2. Run the installer.
3. Keep the detected OBS folder (or browse to your portable OBS root).
4. On finish, optionally launch OBS and open Quick Start.

ZIP fallback (`obs-vdoninja-windows-x64.zip`):

1. Extract the ZIP.
2. Open PowerShell or Command Prompt in the extracted folder.
3. Run system-wide install (admin):

```powershell
.\install.cmd
```

New-user default: use `install.cmd` first. It bypasses strict PowerShell script policy for this run only.

Per-user install (no admin):

```powershell
.\install.cmd -CurrentUser
```

Skip post-install quick-start popup:

```powershell
.\install.cmd -NoQuickStartPopup
```

Portable OBS path:

```powershell
.\install.cmd -ObsRoot "D:\OBS\obs-studio"
```

If you need to run the PowerShell script directly:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\install.ps1
```

### Uninstall

If installed via setup `.exe`, uninstall via:

- Windows Settings -> Apps -> Installed apps -> `OBS VDO.Ninja Plugin`, or
- Control Panel -> Programs and Features

ZIP/script installs can be removed with:

```powershell
.\uninstall.cmd
```

Remove plugin + data:

```powershell
.\uninstall.cmd -RemoveData
```

## Linux (`obs-vdoninja-linux-x86_64.tar.gz`)

### Install or update

```bash
chmod +x install.sh
sudo ./install.sh
```

### Uninstall

```bash
chmod +x uninstall.sh
sudo ./uninstall.sh
```

Remove plugin + data:

```bash
sudo ./uninstall.sh --remove-data
```

## macOS (`obs-vdoninja-macos-arm64.zip`)

### Install or update

```bash
chmod +x install.sh
./install.sh
```

### Uninstall

```bash
chmod +x uninstall.sh
./uninstall.sh
```

Remove plugin + data:

```bash
./uninstall.sh --remove-data
```

## Manual install fallback

If scripts are not usable:

- Copy plugin binaries from `obs-plugins/64bit` or `lib/obs-plugins` into your OBS plugin binary path.
- Copy `data/obs-plugins/obs-vdoninja` or `share/obs/obs-plugins/obs-vdoninja` into your OBS data path.

## Verify in OBS

After install/update, restart OBS and confirm:

- `Settings -> Stream` includes `VDO.Ninja`
- `Add Source` includes `VDO.Ninja Source`

## Windows troubleshooting

If OBS log shows `obs-vdoninja.dll not loaded` and `Service 'vdoninja_service' not found`:

1. Fully close OBS.
2. Remove old plugin files from `C:\Program Files\obs-studio\obs-plugins\64bit\obs-vdoninja.dll`.
3. Reinstall from the newest setup `.exe` (preferred) or release ZIP with `install.cmd`.
4. Reopen OBS and check the newest log in `%APPDATA%\obs-studio\logs\`.

This plugin should not require replacing OBS core DLLs manually.

If portable OBS shows `Failed to load theme` when launched from a terminal:

1. Launch from the OBS executable directory (`bin\64bit`), not from the portable root.

```powershell
cd "D:\OBS\obs-studio\bin\64bit"
.\obs64.exe --portable
```

Or:

```powershell
Start-Process -FilePath "D:\OBS\obs-studio\bin\64bit\obs64.exe" -WorkingDirectory "D:\OBS\obs-studio\bin\64bit" -ArgumentList "--portable"
```
