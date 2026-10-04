param(
    [switch]$CurrentUser,
    [string]$ObsRoot = "",
    [switch]$Yes,
    [switch]$NoQuickStartPopup,
    [switch]$OpenQuickStart,
    [ValidateSet('None', 'Private', 'Public', 'Private,Public')][string]$FirewallProfiles = 'None'
)

$ErrorActionPreference = "Stop"

function Test-ObsRoot {
    param([string]$Path)

    return (-not [string]::IsNullOrWhiteSpace($Path)) -and
        (Test-Path -LiteralPath (Join-Path $Path "bin\64bit\obs64.exe") -PathType Leaf) -and
        (Test-Path -LiteralPath (Join-Path $Path "bin\64bit\obs.dll") -PathType Leaf)
}

function Get-DefaultObsRoot {
    foreach ($hive in @([Microsoft.Win32.RegistryHive]::LocalMachine, [Microsoft.Win32.RegistryHive]::CurrentUser)) {
        $baseKey = [Microsoft.Win32.RegistryKey]::OpenBaseKey($hive, [Microsoft.Win32.RegistryView]::Registry64)
        try {
            foreach ($entry in @(
                @{ Key = "SOFTWARE\OBS Studio"; Value = "" },
                @{ Key = "SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\OBS Studio_is1"; Value = "InstallLocation" }
            )) {
                $key = $baseKey.OpenSubKey($entry.Key)
                if ($null -eq $key) { continue }
                try {
                    $candidate = [string]$key.GetValue($entry.Value)
                    if (Test-ObsRoot $candidate) { return $candidate }
                } finally {
                    $key.Dispose()
                }
            }
        } finally {
            $baseKey.Dispose()
        }
    }

    $programFiles = $env:ProgramW6432
    if (-not $programFiles) { $programFiles = $env:ProgramFiles }
    return Join-Path $programFiles "obs-studio"
}

if ($CurrentUser -and $ObsRoot) {
    throw "Use either -CurrentUser or -ObsRoot. For custom or portable OBS, use -ObsRoot."
}
if ($Yes -and -not $CurrentUser -and [string]::IsNullOrWhiteSpace($ObsRoot)) {
    throw "For unattended installation, specify -ObsRoot with -Yes (or use -CurrentUser -Yes)."
}

if ($CurrentUser -and $FirewallProfiles -ne 'None') {
    throw 'Firewall setup requires -ObsRoot so it targets the correct obs64.exe.'
}

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$packageRoot = $scriptDir
if (-not (Test-Path -LiteralPath (Join-Path $packageRoot "obs-plugins\64bit"))) {
    $parent = Resolve-Path -LiteralPath (Join-Path $scriptDir "..")
    if (Test-Path -LiteralPath (Join-Path $parent "obs-plugins\64bit")) {
        $packageRoot = $parent
    }
}

$srcPluginDir = Join-Path $packageRoot "obs-plugins\64bit"
$srcDataDir = Join-Path $packageRoot "data\obs-plugins\obs-vdoninja"

if (-not (Test-Path -LiteralPath (Join-Path $srcPluginDir "obs-vdoninja.dll") -PathType Leaf)) {
    Write-Error "Package plugin DLL not found: $srcPluginDir\obs-vdoninja.dll"
}
if (-not (Test-Path -LiteralPath $srcDataDir -PathType Container)) {
    Write-Error "Package data directory not found: $srcDataDir"
}

if ($CurrentUser) {
    $dstPluginDir = Join-Path $env:APPDATA "obs-studio\plugins\obs-vdoninja\bin\64bit"
    $dstDataDir = Join-Path $env:APPDATA "obs-studio\plugins\obs-vdoninja\data"
} else {
    if ([string]::IsNullOrWhiteSpace($ObsRoot)) { $ObsRoot = Get-DefaultObsRoot }
    if (-not $Yes) {
        Write-Host "Select the OBS Studio installation to receive the plugin."
        Write-Host "For custom or portable OBS, enter its root folder containing bin\64bit\obs64.exe."
        Write-Host "Suggested OBS folder: $ObsRoot"
        $selectedRoot = Read-Host "OBS folder (Enter to keep the suggestion)"
        if (-not [string]::IsNullOrWhiteSpace($selectedRoot)) { $ObsRoot = $selectedRoot }
    }
    $ObsRoot = $ObsRoot.Trim().Trim('"')
    if (-not (Test-ObsRoot $ObsRoot)) {
        throw "OBS Studio was not found in '$ObsRoot'. Choose the root containing bin\64bit\obs64.exe and bin\64bit\obs.dll, not bin or obs-plugins."
    }
    $ObsRoot = (Resolve-Path -LiteralPath $ObsRoot).ProviderPath
    $dstPluginDir = Join-Path $ObsRoot "obs-plugins\64bit"
    $dstDataDir = Join-Path $ObsRoot "data\obs-plugins\obs-vdoninja"
}

Write-Host "Installing OBS Plugin for VDO.Ninja from package..."
Write-Host "Source:      $packageRoot"
Write-Host "Plugin dst:  $dstPluginDir"
Write-Host "Data dst:    $dstDataDir"

if (-not $Yes) {
    $answer = Read-Host "Install the plugin to these folders? [y/N]"
    if ($answer -notmatch '^(?i:y|yes)$') {
        Write-Host "Installation cancelled. No files were copied."
        return
    }
}

New-Item -ItemType Directory -Force -Path $dstPluginDir | Out-Null
New-Item -ItemType Directory -Force -Path $dstDataDir | Out-Null

Get-ChildItem -LiteralPath $srcPluginDir | Copy-Item -Destination $dstPluginDir -Recurse -Force
Get-ChildItem -LiteralPath $srcDataDir | Copy-Item -Destination $dstDataDir -Recurse -Force

# Firewall permission is separate from permission to install files. Silent
# installs change no rules unless -FirewallProfiles was explicitly supplied.
if (-not $Yes -and -not $CurrentUser -and -not $PSBoundParameters.ContainsKey('FirewallProfiles')) {
    $choice = Read-Host 'Allow OBS peer-to-peer UDP through Windows Firewall? [private/public/both/N]'
    switch ($choice.Trim().ToLowerInvariant()) {
        'private' { $FirewallProfiles = 'Private' }
        'public' { $FirewallProfiles = 'Public' }
        'both' { $FirewallProfiles = 'Private,Public' }
    }
}
if ($FirewallProfiles -ne 'None') {
    try {
        & (Join-Path $packageRoot 'configure-windows-firewall.ps1') -Action Add -Profiles $FirewallProfiles -ObsExe (Join-Path $ObsRoot 'bin\64bit\obs64.exe')
    } catch {
        Write-Warning "Plugin installed, but firewall access was not configured: $($_.Exception.Message). Run the installer as administrator or allow this obs64.exe manually."
    }
}

$quickStartPath = Join-Path $packageRoot "QUICKSTART.md"
$quickStartUrl = "https://steveseguin.github.io/ninja-obs-plugin/#quick-start"
$nextSteps = @"

Install complete.

Next steps:
1. Restart OBS Studio
2. Open Settings -> Stream and select VDO.Ninja
3. Open Tools -> VDO.Ninja Studio and set Stream ID (optional password/room/salt/signaling)
4. Start with Go Live or Start Streaming, then use Copy Viewer Link

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
