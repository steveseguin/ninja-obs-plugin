# OBS VDO.Ninja Plugin - Windows Installation Script
# SPDX-License-Identifier: AGPL-3.0-only

param(
    [switch]$SystemWide,
    [switch]$BuildOnly
)

$ErrorActionPreference = "Stop"

Write-Host "========================================"
Write-Host "OBS VDO.Ninja Plugin Installer (Windows)"
Write-Host "========================================"
Write-Host ""

# Determine installation directory
if ($SystemWide) {
    $OBS_PLUGIN_DIR = "${env:ProgramFiles}\obs-studio\obs-plugins\64bit"
    $OBS_DATA_DIR = "${env:ProgramFiles}\obs-studio\data\obs-plugins\obs-vdoninja"
} else {
    $OBS_PLUGIN_DIR = "${env:APPDATA}\obs-studio\plugins\obs-vdoninja\bin\64bit"
    $OBS_DATA_DIR = "${env:APPDATA}\obs-studio\plugins\obs-vdoninja\data"
}

# Check for Visual Studio
$vsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vsWhere)) {
    Write-Host "Error: Visual Studio not found."
    Write-Host "Please install Visual Studio 2019 or later with C++ support."
    exit 1
}

$vsPath = & $vsWhere -latest -property installationPath
if (-not $vsPath) {
    Write-Host "Error: Could not find Visual Studio installation."
    exit 1
}

Write-Host "Found Visual Studio at: $vsPath"

# Check for CMake
if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    Write-Host "Error: CMake not found."
    Write-Host "Please install CMake and add it to PATH."
    exit 1
}

Write-Host "CMake found."

# Check for Git
if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
    Write-Host "Error: Git not found."
    Write-Host "Please install Git and add it to PATH."
    exit 1
}

Write-Host "Git found."
Write-Host ""

# Get script directory
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$projectDir = Split-Path -Parent $scriptDir

# Create deps directory
$depsDir = Join-Path $projectDir "deps"
New-Item -ItemType Directory -Force -Path $depsDir | Out-Null

# Build libdatachannel
$libdcDir = Join-Path $depsDir "libdatachannel"
$libdcInstallDir = Join-Path $depsDir "libdatachannel-install"

if (-not (Test-Path (Join-Path $libdcInstallDir "lib\datachannel.lib"))) {
    Write-Host "Building libdatachannel..."

    if (Test-Path $libdcDir) {
        Remove-Item -Recurse -Force $libdcDir
    }

    git clone --depth 1 --branch v0.20.2 https://github.com/paullouisageneau/libdatachannel.git $libdcDir
    Push-Location $libdcDir
    git submodule update --init --recursive --depth 1

    cmake -B build -G "Visual Studio 17 2022" -A x64 `
        -DCMAKE_BUILD_TYPE=Release `
        -DNO_EXAMPLES=ON `
        -DNO_TESTS=ON

    cmake --build build --config Release
    cmake --install build --prefix $libdcInstallDir

    Pop-Location
    Write-Host "libdatachannel built successfully."
} else {
    Write-Host "libdatachannel already built."
}

Write-Host ""
Write-Host "Building OBS VDO.Ninja plugin..."

Push-Location $projectDir

# Configure
cmake -B build -G "Visual Studio 17 2022" -A x64 `
    -DCMAKE_BUILD_TYPE=Release `
    -DCMAKE_PREFIX_PATH="$libdcInstallDir"

# Build
cmake --build build --config Release

if ($BuildOnly) {
    Write-Host ""
    Write-Host "Build complete. Plugin located at: build\Release\obs-vdoninja.dll"
    Pop-Location
    exit 0
}

Write-Host ""
Write-Host "Installing plugin..."

# Create directories
New-Item -ItemType Directory -Force -Path $OBS_PLUGIN_DIR | Out-Null
New-Item -ItemType Directory -Force -Path "$OBS_DATA_DIR\locale" | Out-Null

# Copy plugin
$pluginDll = Join-Path $projectDir "build\Release\obs-vdoninja.dll"
if (Test-Path $pluginDll) {
    Copy-Item $pluginDll $OBS_PLUGIN_DIR
}

# Copy dependencies
$datachanelDll = Join-Path $libdcInstallDir "bin\datachannel.dll"
if (Test-Path $datachanelDll) {
    Copy-Item $datachanelDll $OBS_PLUGIN_DIR
}

# Copy locale data
$localeDir = Join-Path $projectDir "data\locale"
if (Test-Path $localeDir) {
    Copy-Item "$localeDir\*" "$OBS_DATA_DIR\locale\" -Recurse
}

Pop-Location

Write-Host ""
Write-Host "========================================"
Write-Host "Installation complete!"
Write-Host "========================================"
Write-Host ""
Write-Host "Plugin installed to: $OBS_PLUGIN_DIR"
Write-Host ""
Write-Host "Restart OBS Studio to load the plugin."
Write-Host ""
