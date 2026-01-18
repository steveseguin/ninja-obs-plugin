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

echo "All dependencies found."
echo ""

# Build libdatachannel if not present
LIBDC_INSTALLED=false
if pkg-config --exists libdatachannel 2>/dev/null || [ -f "/usr/lib/libdatachannel.so" ]; then
    echo "libdatachannel found."
    LIBDC_INSTALLED=true
else
    echo "libdatachannel not found. Building from source..."
    echo ""

    TEMP_DIR=$(mktemp -d)
    cd "$TEMP_DIR"

    git clone --depth 1 --branch v0.20.2 https://github.com/paullouisageneau/libdatachannel.git
    cd libdatachannel
    git submodule update --init --recursive --depth 1

    cmake -B build \
        -DCMAKE_BUILD_TYPE=Release \
        -DNO_EXAMPLES=ON \
        -DNO_TESTS=ON

    cmake --build build -j$(nproc)

    echo ""
    echo "Installing libdatachannel (requires sudo)..."
    sudo cmake --install build

    cd /
    rm -rf "$TEMP_DIR"

    LIBDC_INSTALLED=true
    echo "libdatachannel installed successfully."
fi

echo ""
echo "Building OBS VDO.Ninja plugin..."
echo ""

# Get the script directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

cd "$PROJECT_DIR"

# Build the plugin
cmake -B build -DCMAKE_BUILD_TYPE=Release
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
