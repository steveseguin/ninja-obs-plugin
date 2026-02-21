#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PKG_ROOT="$SCRIPT_DIR"
if [[ ! -d "$PKG_ROOT/lib/obs-plugins" && ! -d "$PKG_ROOT/obs-plugins/64bit" ]]; then
  if [[ -d "$(dirname "$SCRIPT_DIR")/lib/obs-plugins" || -d "$(dirname "$SCRIPT_DIR")/obs-plugins/64bit" ]]; then
    PKG_ROOT="$(dirname "$SCRIPT_DIR")"
  fi
fi

SRC_PLUGIN_DIR=""
if [[ -d "$PKG_ROOT/lib/obs-plugins" ]]; then
  SRC_PLUGIN_DIR="$PKG_ROOT/lib/obs-plugins"
elif [[ -d "$PKG_ROOT/obs-plugins/64bit" ]]; then
  SRC_PLUGIN_DIR="$PKG_ROOT/obs-plugins/64bit"
else
  echo "Could not find plugin binaries in package."
  exit 1
fi

SRC_DATA_DIR=""
if [[ -d "$PKG_ROOT/share/obs/obs-plugins/obs-vdoninja" ]]; then
  SRC_DATA_DIR="$PKG_ROOT/share/obs/obs-plugins/obs-vdoninja"
elif [[ -d "$PKG_ROOT/data/obs-plugins/obs-vdoninja" ]]; then
  SRC_DATA_DIR="$PKG_ROOT/data/obs-plugins/obs-vdoninja"
else
  echo "Could not find plugin data directory in package."
  exit 1
fi

if [[ "${EUID}" -eq 0 ]]; then
  DST_PLUGIN_DIR="/usr/lib/obs-plugins"
  DST_DATA_DIR="/usr/share/obs/obs-plugins/obs-vdoninja"
else
  DST_PLUGIN_DIR="$HOME/.config/obs-studio/plugins/obs-vdoninja/bin/64bit"
  DST_DATA_DIR="$HOME/.config/obs-studio/plugins/obs-vdoninja/data"
fi

echo "Installing OBS VDO.Ninja plugin from package..."
echo "Source:      $PKG_ROOT"
echo "Plugin dst:  $DST_PLUGIN_DIR"
echo "Data dst:    $DST_DATA_DIR"

mkdir -p "$DST_PLUGIN_DIR" "$DST_DATA_DIR"
cp -a "$SRC_PLUGIN_DIR"/. "$DST_PLUGIN_DIR"/
cp -a "$SRC_DATA_DIR"/. "$DST_DATA_DIR"/

echo
echo "Install complete. Restart OBS Studio."
