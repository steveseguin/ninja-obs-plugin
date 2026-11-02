#!/usr/bin/env bash
# Run on Linux. Keep generated sources/builds off a Windows-mounted checkout.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if [[ "$(uname -m)" != x86_64 ]]; then
  echo 'This package currently supports x86-64 only.' >&2
  exit 1
fi
work="${1:-${XDG_CACHE_HOME:-$HOME/.cache}/obs-vdoninja-flatpak}"
mkdir -p "$work/packaging/flatpak/source" "$work/scripts"
work="$(cd "$work" && pwd)"

# Copy only build inputs, never local artifacts, credentials, or other checkouts.
python3 - "$root" "$work" <<'PY'
from pathlib import Path
import shutil
import configparser
import json
import subprocess
import sys
root, work = map(Path, sys.argv[1:])
manifest = json.loads((root / 'packaging/flatpak/com.obsproject.Studio.Plugin.VDONinja.json').read_text())
metadata = configparser.ConfigParser(interpolation=None)
metadata.read_string(subprocess.check_output(
    ['flatpak', 'info', '--show-metadata', 'com.obsproject.Studio'], text=True))
sdk_name, sdk_branch = manifest['sdk'].split('//')
expected = f'{sdk_name}/x86_64/{sdk_branch}'
if metadata['Application']['sdk'] != expected:
    raise SystemExit(f'OBS Flatpak SDK changed; review the manifest before building (expected {expected}).')
source = work / 'packaging/flatpak/source'
# Replace only this script's generated source tree, avoiding stale removed files.
if source.exists():
    if source.is_symlink() or source.resolve() != work / 'packaging/flatpak/source':
        raise SystemExit('Unsafe generated source directory')
    shutil.rmtree(source)
source.mkdir(parents=True)
for name in ('src', 'data', 'tests', 'cmake', 'packaging/flatpak'):
    if (root / name).is_dir():
        shutil.copytree(root / name, source / name, dirs_exist_ok=True)
for name in ('CMakeLists.txt', 'LICENSE'):
    shutil.copy2(root / name, source / name)
shutil.copy2(root / 'scripts/patch-libdatachannel-relay.py', work / 'scripts')
shutil.copy2(root / 'packaging/flatpak/com.obsproject.Studio.Plugin.VDONinja.json',
             work / 'packaging/flatpak')
shutil.copy2(root / 'packaging/flatpak/libdatachannel-tcp-send-lock.patch', work / 'packaging/flatpak')
PY
flatpak-builder --user --arch=x86_64 --force-clean --keep-build-dirs --state-dir="$work/state" --repo="$work/repo" \
  "$work/build" "$work/packaging/flatpak/com.obsproject.Studio.Plugin.VDONinja.json"
flatpak build-bundle --runtime "$work/repo" "$work/obs-vdoninja-linux-x86_64.flatpak" \
  com.obsproject.Studio.Plugin.VDONinja stable
echo "Package: $work/obs-vdoninja-linux-x86_64.flatpak"
