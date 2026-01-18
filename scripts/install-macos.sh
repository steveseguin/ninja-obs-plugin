#!/bin/bash
# OBS VDO.Ninja Plugin - macOS Installation Script
# SPDX-License-Identifier: AGPL-3.0-only

set -e

echo "========================================"
echo "OBS VDO.Ninja Plugin Installer (macOS)"
echo "========================================"
echo ""

# Installation directory
INSTALL_DIR="$HOME/Library/Application Support/obs-studio/plugins/obs-vdoninja.plugin"
DATA_DIR="$INSTALL_DIR/Contents/Resources/data"

# Check for Homebrew
if ! command -v brew &> /dev/null; then
    echo "Error: Homebrew is required but not installed."
    echo "Install it from: https://brew.sh"
    exit 1
fi

echo "Checking dependencies..."

# Check for Xcode command line tools
if ! xcode-select -p &> /dev/null; then
    echo "Error: Xcode command line tools not installed."
    echo "Install with: xcode-select --install"
    exit 1
fi

# Install dependencies via Homebrew
echo "Installing build dependencies..."
brew install cmake openssl@3 || true

echo "All dependencies found."
echo ""

# Check for OBS
OBS_APP="/Applications/OBS.app"
if [ ! -d "$OBS_APP" ]; then
    echo "Warning: OBS Studio not found at $OBS_APP"
    echo "Please install OBS Studio first."
fi

# Get the script directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
DEPS_DIR="$PROJECT_DIR/deps"

mkdir -p "$DEPS_DIR"

# Build libdatachannel if not present
LIBDC_DIR="$DEPS_DIR/libdatachannel"
LIBDC_INSTALL_DIR="$DEPS_DIR/libdatachannel-install"

if [ ! -f "$LIBDC_INSTALL_DIR/lib/libdatachannel.dylib" ]; then
    echo "Building libdatachannel..."
    echo ""

    rm -rf "$LIBDC_DIR"

    git clone --depth 1 --branch v0.20.2 https://github.com/paullouisageneau/libdatachannel.git "$LIBDC_DIR"
    cd "$LIBDC_DIR"
    git submodule update --init --recursive --depth 1

    cmake -B build \
        -DCMAKE_BUILD_TYPE=Release \
        -DNO_EXAMPLES=ON \
        -DNO_TESTS=ON \
        -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" \
        -DOPENSSL_ROOT_DIR=$(brew --prefix openssl@3)

    cmake --build build -j$(sysctl -n hw.ncpu)
    cmake --install build --prefix "$LIBDC_INSTALL_DIR"

    echo "libdatachannel built successfully."
else
    echo "libdatachannel already built."
fi

echo ""
echo "Building OBS VDO.Ninja plugin..."
echo ""

cd "$PROJECT_DIR"

# Build the plugin
cmake -B build \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$LIBDC_INSTALL_DIR" \
    -DOPENSSL_ROOT_DIR=$(brew --prefix openssl@3) \
    -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"

cmake --build build -j$(sysctl -n hw.ncpu)

echo ""
echo "Installing plugin..."

# Create plugin bundle structure
mkdir -p "$INSTALL_DIR/Contents/MacOS"
mkdir -p "$DATA_DIR/locale"

# Copy plugin binary
if [ -f "build/obs-vdoninja.so" ]; then
    cp build/obs-vdoninja.so "$INSTALL_DIR/Contents/MacOS/obs-vdoninja"
fi

# Copy libdatachannel
if [ -f "$LIBDC_INSTALL_DIR/lib/libdatachannel.dylib" ]; then
    cp "$LIBDC_INSTALL_DIR/lib/libdatachannel.dylib" "$INSTALL_DIR/Contents/MacOS/"
    # Fix rpath
    install_name_tool -change "@rpath/libdatachannel.dylib" "@loader_path/libdatachannel.dylib" "$INSTALL_DIR/Contents/MacOS/obs-vdoninja" 2>/dev/null || true
fi

# Copy locale data if exists
if [ -d "data/locale" ]; then
    cp -r data/locale/* "$DATA_DIR/locale/"
fi

# Create Info.plist
cat > "$INSTALL_DIR/Contents/Info.plist" << EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleName</key>
    <string>obs-vdoninja</string>
    <key>CFBundleIdentifier</key>
    <string>com.vdoninja.obs-plugin</string>
    <key>CFBundleVersion</key>
    <string>1.0.0</string>
    <key>CFBundleShortVersionString</key>
    <string>1.0.0</string>
    <key>CFBundleExecutable</key>
    <string>obs-vdoninja</string>
    <key>CFBundlePackageType</key>
    <string>BNDL</string>
</dict>
</plist>
EOF

echo ""
echo "========================================"
echo "Installation complete!"
echo "========================================"
echo ""
echo "Plugin installed to: $INSTALL_DIR"
echo ""
echo "Restart OBS Studio to load the plugin."
echo ""
