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

# macOS OBS requires a .plugin bundle structure
BUNDLE_DIR="${HOME}/Library/Application Support/obs-studio/plugins/obs-vdoninja.plugin"
DST_PLUGIN_DIR="$BUNDLE_DIR/Contents/MacOS"
DST_DATA_DIR="$BUNDLE_DIR/Contents/Resources/data"

echo "Installing OBS VDO.Ninja plugin from package..."
echo "Source:      $PKG_ROOT"
echo "Bundle dst:  $BUNDLE_DIR"

# Remove legacy flat layout if present (from older installer versions)
LEGACY_DIR="${HOME}/Library/Application Support/obs-studio/plugins/obs-vdoninja"
if [[ -d "$LEGACY_DIR" && ! -d "$LEGACY_DIR/Contents" ]]; then
  echo "Removing legacy flat layout at $LEGACY_DIR ..."
  rm -rf "$LEGACY_DIR"
fi

mkdir -p "$DST_PLUGIN_DIR" "$DST_DATA_DIR"

# Copy plugin binary — rename .so to plain executable name for the bundle
PLUGIN_BIN=""
for candidate in obs-vdoninja.so libobs-vdoninja.so obs-vdoninja.dylib obs-vdoninja; do
  if [[ -f "$SRC_PLUGIN_DIR/$candidate" ]]; then
    PLUGIN_BIN="$SRC_PLUGIN_DIR/$candidate"
    break
  fi
done
if [[ -z "$PLUGIN_BIN" ]]; then
  echo "Could not find plugin binary in $SRC_PLUGIN_DIR"
  exit 1
fi
cp -a "$PLUGIN_BIN" "$DST_PLUGIN_DIR/obs-vdoninja"

# Copy data files
cp -a "$SRC_DATA_DIR"/. "$DST_DATA_DIR"/

# Create Info.plist (required for macOS bundle loading)
cat > "$BUNDLE_DIR/Contents/Info.plist" << 'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleName</key>
    <string>obs-vdoninja</string>
    <key>CFBundleIdentifier</key>
    <string>com.vdoninja.obs-plugin</string>
    <key>CFBundleVersion</key>
    <string>1</string>
    <key>CFBundleShortVersionString</key>
    <string>1.0</string>
    <key>CFBundleExecutable</key>
    <string>obs-vdoninja</string>
    <key>CFBundlePackageType</key>
    <string>BNDL</string>
</dict>
</plist>
PLIST

QUICKSTART_PATH="$PKG_ROOT/QUICKSTART.md"
echo
echo "Install complete."
echo
echo "Next steps:"
echo "1. Restart OBS Studio"
echo "2. Open Settings -> Stream and select VDO.Ninja"
echo "3. Set Stream ID (and optional password/room)"
echo "4. Start streaming and open your view URL"
if [[ -f "$QUICKSTART_PATH" ]]; then
  echo
  echo "Quick guide: $QUICKSTART_PATH"
  if [[ -t 0 && -t 1 ]]; then
    read -r -p "Open QUICKSTART.md now? [Y/n] " RESP
    if [[ ! "$RESP" =~ ^[Nn]$ ]]; then
      open "$QUICKSTART_PATH" >/dev/null 2>&1 || true
    fi
  fi
fi
