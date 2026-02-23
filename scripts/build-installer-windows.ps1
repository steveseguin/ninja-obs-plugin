param(
    [string]$SourceDir = "install",
    [string]$OutputDir = ".",
    [string]$Version = "",
    [string]$OutputBaseFilename = "obs-vdoninja-windows-x64-setup",
    [string]$InnoScript = "packaging/windows/installer-Windows.iss"
)

$ErrorActionPreference = "Stop"

function Resolve-IsccPath {
    $candidate = Get-Command iscc.exe -ErrorAction SilentlyContinue
    if ($candidate) {
        return $candidate.Source
    }

    $defaultPaths = @(
        "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
        "${env:ProgramFiles}\Inno Setup 6\ISCC.exe"
    )

    foreach ($path in $defaultPaths) {
        if (Test-Path $path) {
            return $path
        }
    }

    throw "ISCC.exe not found. Install Inno Setup 6 or add it to PATH."
}

function Resolve-Version {
    param([string]$RequestedVersion)

    if ($RequestedVersion -and $RequestedVersion.Trim().Length -gt 0) {
        return $RequestedVersion.Trim().TrimStart("v")
    }

    $cmake = "CMakeLists.txt"
    if (-not (Test-Path $cmake)) {
        throw "CMakeLists.txt not found and -Version was not provided."
    }

    $line = Select-String -Path $cmake -Pattern "project\(obs-vdoninja VERSION ([0-9]+\.[0-9]+\.[0-9]+)"
    if (-not $line) {
        throw "Unable to derive plugin version from CMakeLists.txt. Pass -Version explicitly."
    }

    return $line.Matches[0].Groups[1].Value
}

$resolvedVersion = Resolve-Version -RequestedVersion $Version
if (-not (Test-Path $SourceDir)) {
    throw "Source directory not found: $SourceDir"
}
if (-not (Test-Path $OutputDir)) {
    New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
}
if (-not (Test-Path $InnoScript)) {
    throw "Inno Setup script not found: $InnoScript"
}

$resolvedSource = Resolve-Path $SourceDir
$resolvedOutput = Resolve-Path $OutputDir
$resolvedScript = Resolve-Path $InnoScript

if (-not (Test-Path (Join-Path $resolvedSource "obs-plugins\64bit\obs-vdoninja.dll"))) {
    throw "Missing plugin payload in '$resolvedSource\obs-plugins\64bit\obs-vdoninja.dll'."
}

$iscc = Resolve-IsccPath

Write-Host "Building installer with ISCC: $iscc"
Write-Host "Version:      $resolvedVersion"
Write-Host "SourceDir:    $resolvedSource"
Write-Host "OutputDir:    $resolvedOutput"
Write-Host "OutputName:   $OutputBaseFilename"

& $iscc `
    "/DMyAppVersion=$resolvedVersion" `
    "/DMySourceDir=$resolvedSource" `
    "/DMyOutputDir=$resolvedOutput" `
    "/DMyOutputBaseFilename=$OutputBaseFilename" `
    $resolvedScript

if ($LASTEXITCODE -ne 0) {
    throw "ISCC failed with exit code $LASTEXITCODE."
}

$installerPath = Join-Path $resolvedOutput "$OutputBaseFilename.exe"
if (-not (Test-Path $installerPath)) {
    throw "Installer build succeeded but expected output was not found: $installerPath"
}

Write-Host "Installer created: $installerPath"
