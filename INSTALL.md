# OBS VDO.Ninja Plugin - Install Guide

This package does not use a GUI installer yet. Install is file-copy based.

## Windows (`obs-vdoninja-windows-x64.zip`)

1. Extract the ZIP.
2. Run PowerShell as Administrator in the extracted folder.
3. Execute:

```powershell
.\install.ps1
```

Manual install (if you prefer):

- Copy `obs-plugins\64bit\*` to `C:\Program Files\obs-studio\obs-plugins\64bit\`
- Copy `data\obs-plugins\obs-vdoninja\*` to `C:\Program Files\obs-studio\data\obs-plugins\obs-vdoninja\`

## Linux (`obs-vdoninja-linux-x86_64.tar.gz`)

1. Extract the archive.
2. Run:

```bash
sudo ./install.sh
```

Manual install:

- Copy `lib/obs-plugins/*` to `/usr/lib/obs-plugins/`
- Copy `share/obs/obs-plugins/obs-vdoninja/*` to `/usr/share/obs/obs-plugins/obs-vdoninja/`

## macOS (`obs-vdoninja-macos-arm64.zip`)

1. Extract the ZIP.
2. Run:

```bash
chmod +x install.sh
./install.sh
```

Manual install:

- Copy plugin binaries from `lib/obs-plugins/` into your OBS plugin path.
- Copy `share/obs/obs-plugins/obs-vdoninja/` into OBS plugin data path.

## Verify in OBS

After install, restart OBS and confirm:

- Service `VDO.Ninja` appears in `Settings -> Stream`
- Source `VDO.Ninja Source` appears in `Add Source`
