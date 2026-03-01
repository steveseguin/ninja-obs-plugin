param(
    [switch]$CurrentUser,
    [string]$ObsRoot = "$env:ProgramFiles\obs-studio",
    [switch]$NoQuickStartPopup,
    [switch]$OpenQuickStart
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

Write-Host "Installing OBS Plugin for VDO.Ninja from package..."
Write-Host "Source:      $packageRoot"
Write-Host "Plugin dst:  $dstPluginDir"
Write-Host "Data dst:    $dstDataDir"

New-Item -ItemType Directory -Force -Path $dstPluginDir | Out-Null
New-Item -ItemType Directory -Force -Path $dstDataDir | Out-Null

Copy-Item (Join-Path $srcPluginDir "*") $dstPluginDir -Recurse -Force
Copy-Item (Join-Path $srcDataDir "*") $dstDataDir -Recurse -Force

$quickStartPath = Join-Path $packageRoot "QUICKSTART.md"
$quickStartUrl = "https://steveseguin.github.io/ninja-obs-plugin/#quick-start"
$nextSteps = @"

Install complete.

Next steps:
1. Restart OBS Studio
2. Open Settings -> Stream and select VDO.Ninja
3. Open Tools -> VDO.Ninja Control Center and set Stream ID (optional password/room/salt/signaling)
4. Start Streaming and open your view URL

"@

$nextSteps += "`nQuick guide (web): $quickStartUrl`n"
if (Test-Path $quickStartPath) {
    $nextSteps += "Offline guide copy: $quickStartPath`n"
}

Write-Host ""
Write-Host $nextSteps

if ($OpenQuickStart) {
    try {
        Start-Process $quickStartUrl
    } catch {
        if (Test-Path $quickStartPath) {
            Start-Process $quickStartPath
        }
    }
}

if (-not $NoQuickStartPopup) {
    try {
        Add-Type -AssemblyName System.Windows.Forms -ErrorAction Stop
        $message = "OBS Plugin for VDO.Ninja installed.`n`nOpen web Quick Start now?"
        $result = [System.Windows.Forms.MessageBox]::Show(
            $message,
            "OBS Plugin for VDO.Ninja",
            [System.Windows.Forms.MessageBoxButtons]::YesNo,
            [System.Windows.Forms.MessageBoxIcon]::Information
        )
        if ($result -eq [System.Windows.Forms.DialogResult]::Yes) {
            try {
                Start-Process $quickStartUrl
            } catch {
                if (Test-Path $quickStartPath) {
                    Start-Process $quickStartPath
                }
            }
        }
    } catch {
        # Non-interactive/headless shells may not support popup dialogs.
    }
}
