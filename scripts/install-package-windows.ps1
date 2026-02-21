param(
    [switch]$CurrentUser,
    [string]$ObsRoot = "$env:ProgramFiles\obs-studio"
)

$ErrorActionPreference = "Stop"

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$packageRoot = $scriptDir
if (-not (Test-Path (Join-Path $packageRoot "obs-plugins\64bit"))) {
    $parent = Resolve-Path (Join-Path $scriptDir "..")
    if (Test-Path (Join-Path $parent "obs-plugins\64bit")) {
        $packageRoot = $parent
    }
}

$srcPluginDir = Join-Path $packageRoot "obs-plugins\64bit"
$srcDataDir = Join-Path $packageRoot "data\obs-plugins\obs-vdoninja"

if (-not (Test-Path $srcPluginDir)) {
    Write-Error "Package plugin directory not found: $srcPluginDir"
}
if (-not (Test-Path $srcDataDir)) {
    Write-Error "Package data directory not found: $srcDataDir"
}

if ($CurrentUser) {
    $dstPluginDir = Join-Path $env:APPDATA "obs-studio\plugins\obs-vdoninja\bin\64bit"
    $dstDataDir = Join-Path $env:APPDATA "obs-studio\plugins\obs-vdoninja\data"
} else {
    $dstPluginDir = Join-Path $ObsRoot "obs-plugins\64bit"
    $dstDataDir = Join-Path $ObsRoot "data\obs-plugins\obs-vdoninja"
}

Write-Host "Installing OBS VDO.Ninja plugin from package..."
Write-Host "Source:      $packageRoot"
Write-Host "Plugin dst:  $dstPluginDir"
Write-Host "Data dst:    $dstDataDir"

New-Item -ItemType Directory -Force -Path $dstPluginDir | Out-Null
New-Item -ItemType Directory -Force -Path $dstDataDir | Out-Null

Copy-Item (Join-Path $srcPluginDir "*") $dstPluginDir -Recurse -Force
Copy-Item (Join-Path $srcDataDir "*") $dstDataDir -Recurse -Force

Write-Host ""
Write-Host "Install complete. Restart OBS Studio."
