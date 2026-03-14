#!/usr/bin/env bash
set -euo pipefail

REMOVE_DATA=0
if [[ "${1:-}" == "--remove-data" ]]; then
  REMOVE_DATA=1
fi

# Current .plugin bundle layout
BUNDLE_DIR="${HOME}/Library/Application Support/obs-studio/plugins/obs-vdoninja.plugin"
# Legacy flat layout (older installer versions)
LEGACY_DIR="${HOME}/Library/Application Support/obs-studio/plugins/obs-vdoninja"

echo "Uninstalling OBS VDO.Ninja plugin..."

# Remove .plugin bundle
if [[ -d "$BUNDLE_DIR" ]]; then
  if [[ "$REMOVE_DATA" -eq 1 ]]; then
    rm -rf "$BUNDLE_DIR"
    echo "Removed bundle: $BUNDLE_DIR"
  else
    rm -f "$BUNDLE_DIR/Contents/MacOS/obs-vdoninja"
    rm -f "$BUNDLE_DIR/Contents/Info.plist"
    echo "Removed plugin binary from: $BUNDLE_DIR"
    echo "Data preserved at: $BUNDLE_DIR/Contents/Resources/data"
  fi
fi

# Clean up legacy flat layout if present
if [[ -d "$LEGACY_DIR" && ! -d "$LEGACY_DIR/Contents" ]]; then
  if [[ "$REMOVE_DATA" -eq 1 ]]; then
    rm -rf "$LEGACY_DIR"
    echo "Removed legacy layout: $LEGACY_DIR"
  else
    rm -f "$LEGACY_DIR/bin/64bit/obs-vdoninja.so"
    rm -f "$LEGACY_DIR/bin/64bit/libobs-vdoninja.so"
    echo "Removed legacy plugin binaries from: $LEGACY_DIR/bin/64bit"
  fi
fi

echo
echo "Uninstall complete. Restart OBS Studio."
