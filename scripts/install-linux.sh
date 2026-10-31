#!/bin/bash
# OBS VDO.Ninja Plugin - Linux Installation Script
# SPDX-License-Identifier: AGPL-3.0-only

set -e

echo "========================================"
echo "OBS VDO.Ninja Plugin Installer (Linux)"
echo "========================================"
echo ""

# Check if running as root for system-wide install
INSTALL_DIR=""
if [ "$EUID" -eq 0 ]; then
    INSTALL_DIR="/usr/lib/obs-plugins"
    DATA_DIR="/usr/share/obs/obs-plugins/obs-vdoninja"
else
    INSTALL_DIR="$HOME/.config/obs-studio/plugins/obs-vdoninja/bin/64bit"
    DATA_DIR="$HOME/.config/obs-studio/plugins/obs-vdoninja/data"
fi

# Check for required dependencies
echo "Checking dependencies..."

check_command() {
    if ! command -v "$1" &> /dev/null; then
        echo "Error: $1 is required but not installed."
        echo "Install it with: sudo apt-get install $2"
        exit 1
    fi
}

check_command cmake cmake
check_command g++ build-essential

# Check for OBS
if ! command -v obs &> /dev/null && [ ! -d "/usr/include/obs" ]; then
    echo "Warning: OBS Studio does not appear to be installed."
    echo "Please install OBS Studio first: sudo apt-get install obs-studio"
fi

# Check for libssl
if ! pkg-config --exists openssl 2>/dev/null; then
    echo "Error: OpenSSL development files not found."
    echo "Install with: sudo apt-get install libssl-dev"
    exit 1
fi

if ! pkg-config --exists libcurl 2>/dev/null; then
    echo "Error: libcurl development files not found."
    echo "Install with: sudo apt-get install libcurl4-openssl-dev"
    exit 1
fi

echo "All dependencies found."
echo ""

# Use our private pinned build: a system library may not enforce relay-only ICE.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
LIBDC_DIR="$PROJECT_DIR/deps/libdatachannel-relay-v1"
LIBDC_INSTALL_DIR="$PROJECT_DIR/deps/libdatachannel-relay-v1-install"
check_command python3 python3
if [ ! -f "$LIBDC_INSTALL_DIR/lib/libdatachannel.a" ]; then
    mkdir -p "$PROJECT_DIR/deps"
    if [ ! -d "$LIBDC_DIR/.git" ]; then
        git clone --depth 1 --branch v0.20.2 https://github.com/paullouisageneau/libdatachannel.git "$LIBDC_DIR"
    fi
    cd "$LIBDC_DIR"
    git submodule update --init --recursive --depth 1
    python3 "$SCRIPT_DIR/patch-libdatachannel-relay.py" "$LIBDC_DIR"

    cmake -B build \
        -DCMAKE_BUILD_TYPE=Release \
        -DNO_EXAMPLES=ON \
        -DNO_TESTS=ON \
        -DBUILD_SHARED_LIBS=OFF \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        -DCMAKE_INSTALL_LIBDIR=lib

    cmake --build build --clean-first -j"$(nproc)"
    cmake --install build --prefix "$LIBDC_INSTALL_DIR"
fi

echo ""
echo "Building OBS VDO.Ninja plugin..."
echo ""

cd "$PROJECT_DIR"

# Build the plugin
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$LIBDC_INSTALL_DIR"
cmake --build build -j$(nproc)

echo ""
echo "Installing plugin..."

# Create installation directories
mkdir -p "$INSTALL_DIR"
mkdir -p "$DATA_DIR/locale"

# Copy plugin
if [ -f "build/obs-vdoninja.so" ]; then
    cp build/obs-vdoninja.so "$INSTALL_DIR/"
elif [ -f "build/libobs-vdoninja.so" ]; then
    cp build/libobs-vdoninja.so "$INSTALL_DIR/obs-vdoninja.so"
fi

# Copy locale data if exists
if [ -d "data/locale" ]; then
    cp -r data/locale/* "$DATA_DIR/locale/"
fi

echo ""
echo "========================================"
echo "Installation complete!"
echo "========================================"
echo ""
echo "Plugin installed to: $INSTALL_DIR"
echo ""
echo "Restart OBS Studio to load the plugin."
echo ""
