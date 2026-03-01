param(
	[string]$ObsSdkPath = $env:OBS_SDK_PATH,
	[string]$LibDataChannelPrefix = $env:LIBDATACHANNEL_PREFIX,
	[string]$Qt6Prefix = $env:QT6_PREFIX
)

$ErrorActionPreference = "Stop"

function Resolve-PathSafe {
	param([string]$PathValue)
	if ([string]::IsNullOrWhiteSpace($PathValue)) {
		return ""
	}
	try {
		return (Resolve-Path $PathValue).Path
	} catch {
		return $PathValue
	}
}

function Add-Result {
	param(
		[string]$Name,
		[bool]$Ok,
		[string]$Detail
	)
	$script:results += [PSCustomObject]@{
		Name = $Name
		Ok = $Ok
		Detail = $Detail
	}
}

function First-ExistingPath {
	param([string[]]$Candidates)
	foreach ($candidate in $Candidates) {
		if ($candidate -and (Test-Path $candidate)) {
			return $candidate
		}
	}
	return ""
}

$results = @()

# Tools
$cmake = Get-Command cmake -ErrorAction SilentlyContinue
$cmakeDetail = "Install CMake and add it to PATH."
if ($cmake) {
	$cmakeDetail = $cmake.Path
}
Add-Result -Name "CMake" -Ok ([bool]$cmake) -Detail $cmakeDetail

$git = Get-Command git -ErrorAction SilentlyContinue
$gitDetail = "Install Git and add it to PATH."
if ($git) {
	$gitDetail = $git.Path
}
Add-Result -Name "Git" -Ok ([bool]$git) -Detail $gitDetail

$vswhere = Join-Path "${env:ProgramFiles(x86)}" "Microsoft Visual Studio\Installer\vswhere.exe"
$hasVs = $false
$vsInstallPath = ""
if (Test-Path $vswhere) {
	try {
		$vsInstallPath = & $vswhere -latest -property installationPath
		if (-not [string]::IsNullOrWhiteSpace($vsInstallPath)) {
			$hasVs = $true
		}
	} catch {
		$hasVs = $false
	}
}
$vsDetail = "Install VS 2022 Build Tools with C++ workload."
if ($hasVs) {
	$vsDetail = $vsInstallPath
}
Add-Result -Name "Visual Studio Build Tools" -Ok $hasVs -Detail $vsDetail

# OBS SDK
$obsSdkResolved = Resolve-PathSafe -PathValue $ObsSdkPath
if ([string]::IsNullOrWhiteSpace($obsSdkResolved)) {
	Add-Result -Name "OBS_SDK_PATH" -Ok $false -Detail "Set -ObsSdkPath or OBS_SDK_PATH."
} else {
	$obsHeader = Join-Path $obsSdkResolved "include\obs\obs-module.h"
	$obsLib = Join-Path $obsSdkResolved "lib\obs.lib"
	$obsFrontendLib = Join-Path $obsSdkResolved "lib\obs-frontend-api.lib"
	$ok = (Test-Path $obsHeader) -and (Test-Path $obsLib) -and (Test-Path $obsFrontendLib)
	$detail = if ($ok) {
		$obsSdkResolved
	} else {
		"Missing required files under '$obsSdkResolved'. Need include/obs/obs-module.h, lib/obs.lib, lib/obs-frontend-api.lib"
	}
	Add-Result -Name "OBS SDK" -Ok $ok -Detail $detail
}

# libdatachannel
$ldcResolved = Resolve-PathSafe -PathValue $LibDataChannelPrefix
$ldcConfigPath = ""
$ldcLibPath = ""
if ([string]::IsNullOrWhiteSpace($ldcResolved)) {
	Add-Result -Name "libdatachannel prefix" -Ok $false -Detail "Set -LibDataChannelPrefix or LIBDATACHANNEL_PREFIX."
} else {
	$ldcConfigPath = First-ExistingPath -Candidates @(
		(Join-Path $ldcResolved "LibDataChannelConfig.cmake"),
		(Join-Path $ldcResolved "lib\cmake\LibDataChannel\LibDataChannelConfig.cmake"),
		(Join-Path $ldcResolved "share\cmake\LibDataChannel\LibDataChannelConfig.cmake")
	)
	$ldcLibPath = First-ExistingPath -Candidates @(
		(Join-Path $ldcResolved "lib\datachannel.lib"),
		(Join-Path $ldcResolved "lib64\datachannel.lib")
	)
	$ok = (-not [string]::IsNullOrWhiteSpace($ldcConfigPath)) -or (-not [string]::IsNullOrWhiteSpace($ldcLibPath))
	$detail = if ($ok) {
		$ldcResolved
	} else {
		"Could not find LibDataChannelConfig.cmake or datachannel.lib under '$ldcResolved'."
	}
	Add-Result -Name "libdatachannel" -Ok $ok -Detail $detail
}

# Qt6
$qtPrefixResolved = Resolve-PathSafe -PathValue $Qt6Prefix
$qt6Dir = ""
if ([string]::IsNullOrWhiteSpace($qtPrefixResolved)) {
	Add-Result -Name "Qt6 prefix" -Ok $false -Detail "Set -Qt6Prefix or QT6_PREFIX."
} else {
	$qt6Dir = First-ExistingPath -Candidates @(
		(Join-Path $qtPrefixResolved "lib\cmake\Qt6"),
		(Join-Path $qtPrefixResolved "cmake\Qt6"),
		$qtPrefixResolved
	)
	$qtConfig = First-ExistingPath -Candidates @(
		(Join-Path $qt6Dir "Qt6Config.cmake")
	)
	$ok = -not [string]::IsNullOrWhiteSpace($qtConfig)
	$detail = if ($ok) {
		$qt6Dir
	} else {
		"Could not find Qt6Config.cmake under '$qtPrefixResolved'."
	}
	Add-Result -Name "Qt6" -Ok $ok -Detail $detail
}

Write-Host ""
Write-Host "Windows Build Requirements Check"
Write-Host "================================"
foreach ($entry in $results) {
	$status = if ($entry.Ok) { "PASS" } else { "FAIL" }
	Write-Host "[$status] $($entry.Name): $($entry.Detail)"
}

$failed = @($results | Where-Object { -not $_.Ok })
if ($failed.Count -gt 0) {
	Write-Host ""
	Write-Host "One or more checks failed."
	Write-Host "Fix the failing items, then run this script again."
	exit 1
}

$cmakePrefix = "$ldcResolved;$qtPrefixResolved"
Write-Host ""
Write-Host "All required checks passed."
Write-Host ""
Write-Host "Suggested configure command:"
Write-Host "cmake -S . -B build-plugin -G `"Visual Studio 17 2022`" -A x64 -DBUILD_PLUGIN=ON -DBUILD_TESTS=OFF -DCMAKE_BUILD_TYPE=Release -DOBS_SDK_PATH=`"$obsSdkResolved`" -DCMAKE_PREFIX_PATH=`"$cmakePrefix`" -DQt6_DIR=`"$qt6Dir`""
