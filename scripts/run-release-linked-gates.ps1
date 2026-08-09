[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$BuildDirectory,
    [Parameter(Mandatory = $true)][string]$ObsRundir,
    [Parameter(Mandatory = $true)][string]$LibobsRuntimeDirectory,
    [Parameter(Mandatory = $true)][string]$PthreadsRuntimeDirectory,
    [Parameter(Mandatory = $true)][string]$ObsDepsBinDirectory,
    [Parameter(Mandatory = $true)][string]$ExpectedFfmpegImports
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$ExpectedReleaseLinkedTests = @(
    "vdoninja-native-media-linked-owner-lifetime-track",
    "vdoninja-native-media-linked-owner-lifetime-data-channel",
    "vdoninja-native-media-linked-owner-order-peer-connection",
    "vdoninja-native-media-linked-owner-order-same-handle-data-channel",
    "vdoninja-native-media-linked-owner-hook-pre-permit",
    "vdoninja-native-media-linked-owner-hook-registration-failure",
    "vdoninja-native-media-linked-owner-hook-track-kinds",
    "vdoninja-native-media-linked-owner-hook-feedback",
    "vdoninja-native-media-linked-full",
    "vdoninja-module-lifecycle-linked"
)
$ExpectedReleaseLinkedMetadata = @{
    "vdoninja-native-media-linked-owner-lifetime-track" = @{
        Labels = @("native-media-linked", "owner-lifetime", "release-linked")
        Timeout = 30
    }
    "vdoninja-native-media-linked-owner-lifetime-data-channel" = @{
        Labels = @("native-media-linked", "owner-lifetime", "release-linked")
        Timeout = 30
    }
    "vdoninja-native-media-linked-owner-order-peer-connection" = @{
        Labels = @("native-media-linked", "owner-lifetime", "release-linked")
        Timeout = 60
    }
    "vdoninja-native-media-linked-owner-order-same-handle-data-channel" = @{
        Labels = @("native-media-linked", "owner-lifetime", "release-linked")
        Timeout = 60
    }
    "vdoninja-native-media-linked-owner-hook-pre-permit" = @{
        Labels = @("native-media-linked", "owner-lifetime", "release-linked")
        Timeout = 60
    }
    "vdoninja-native-media-linked-owner-hook-registration-failure" = @{
        Labels = @("native-media-linked", "owner-lifetime", "release-linked")
        Timeout = 60
    }
    "vdoninja-native-media-linked-owner-hook-track-kinds" = @{
        Labels = @("native-media-linked", "owner-lifetime", "release-linked")
        Timeout = 60
    }
    "vdoninja-native-media-linked-owner-hook-feedback" = @{
        Labels = @("native-media-linked", "owner-lifetime", "release-linked")
        Timeout = 60
    }
    "vdoninja-native-media-linked-full" = @{
        Labels = @("native-media-linked", "release-linked")
        Timeout = 600
    }
    "vdoninja-module-lifecycle-linked" = @{
        Labels = @("module-lifecycle-linked", "release-linked")
        Timeout = 60
    }
}
$ExpectedCMakeCacheEntries = @(
    "BUILD_PLUGIN:BOOL=ON",
    "BUILD_NATIVE_MEDIA_LINKED_GATE:BOOL=ON",
    "BUILD_MODULE_LIFECYCLE_LINKED_GATE:BOOL=ON"
)
$ExpectedReleaseLinkedExecutables = @(
    "vdoninja-module-lifecycle-linked-gate.exe",
    "vdoninja-native-media-linked-gate.exe"
)
$ExpectedReleasePeArtifacts = @(
    "obs-vdoninja.dll",
    "vdoninja-module-lifecycle-linked-gate.exe",
    "vdoninja-native-media-linked-gate.exe"
)
$BlockedDynamicImports = @("datachannel.dll", "libcrypto-3-x64.dll", "libssl-3-x64.dll")
$ExpectedObsImportArtifacts = @("obs-vdoninja.dll", "vdoninja-native-media-linked-gate.exe")
$ExpectedFfmpegImportArtifacts = @("obs-vdoninja.dll", "vdoninja-native-media-linked-gate.exe")
$NoObsOrFfmpegImportArtifacts = @("vdoninja-module-lifecycle-linked-gate.exe")
$KnownFfmpegImports = @(
    "avcodec-61.dll", "avutil-59.dll", "swscale-8.dll", "swresample-5.dll",
    "avcodec-62.dll", "avutil-60.dll", "swscale-9.dll", "swresample-6.dll"
)
$RequiredRuntimeDirectories = @($ObsRundir, $LibobsRuntimeDirectory, $PthreadsRuntimeDirectory, $ObsDepsBinDirectory)

function Invoke-CheckedCommand {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [Parameter(Mandatory = $true)][string[]]$ArgumentList
    )

    $commandOutput = & $FilePath @ArgumentList
    $commandExitCode = $LASTEXITCODE
    if ($commandExitCode -ne 0) {
        $commandOutput | ForEach-Object { Write-Host $_ }
        throw "External tool failed with exit code ${commandExitCode}: $FilePath"
    }
    return $commandOutput
}

function Assert-ExactSet {
    param(
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][string[]]$Expected,
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][string[]]$Actual,
        [string]$Description = "set"
    )

    $difference = @(Compare-Object -ReferenceObject $Expected -DifferenceObject $Actual)
    $expectedDuplicates = @($Expected | Group-Object | Where-Object Count -gt 1)
    $actualDuplicates = @($Actual | Group-Object | Where-Object Count -gt 1)
    if ($difference.Count -gt 0 -or $expectedDuplicates.Count -gt 0 -or $actualDuplicates.Count -gt 0) {
        throw "Exact $Description mismatch. Expected=[$($Expected -join ', ')]; actual=[$($Actual -join ', ')]."
    }
}

function Assert-CMakeCacheContract {
    param([Parameter(Mandatory = $true)][string]$Directory)

    $cachePath = Join-Path $Directory "CMakeCache.txt"
    if (-not (Test-Path -LiteralPath $cachePath -PathType Leaf)) {
        throw "Missing CMake cache: $cachePath"
    }
    $cacheLines = @([IO.File]::ReadAllText($cachePath) -split "`r?`n")
    foreach ($entry in $ExpectedCMakeCacheEntries) {
        if (-not $cacheLines.Contains($entry)) {
            throw "Missing exact CMake cache entry: $entry"
        }
    }

    $plogPrefix = "LIBDATACHANNEL_PLOG_INCLUDE_DIR:PATH="
    $plogLines = @($cacheLines | Where-Object { $_.StartsWith($plogPrefix, [StringComparison]::Ordinal) })
    if ($plogLines.Count -ne 1) {
        throw "Expected one exact LIBDATACHANNEL_PLOG_INCLUDE_DIR:PATH entry; found $($plogLines.Count)."
    }
    $plogPath = $plogLines[0].Substring($plogPrefix.Length)
    if (-not (Test-Path -LiteralPath $plogPath -PathType Container)) {
        throw "Configured plog include directory does not exist: $plogPath"
    }
    $resolvedPlogPath = (Resolve-Path -LiteralPath $plogPath).Path
    $plogHeader = Join-Path $resolvedPlogPath "plog\Log.h"
    if (-not (Test-Path -LiteralPath $plogHeader -PathType Leaf)) {
        throw "Configured plog include directory does not contain plog/Log.h: $resolvedPlogPath"
    }
    Write-Host "[release-linked] CMake cache and plog header validated: $resolvedPlogPath"
}

function Assert-PeAmd64 {
    param([Parameter(Mandatory = $true)][string]$Path)

    $bytes = [IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -lt 64 -or $bytes[0] -ne 0x4d -or $bytes[1] -ne 0x5a) {
        throw "Not a complete DOS/PE image: $Path"
    }
    $peOffset = [BitConverter]::ToInt32($bytes, 0x3c)
    if ($peOffset -lt 0 -or $peOffset + 6 -gt $bytes.Length) {
        throw "Invalid PE header offset: $Path"
    }
    if ($bytes[$peOffset] -ne 0x50 -or $bytes[$peOffset + 1] -ne 0x45 -or
        $bytes[$peOffset + 2] -ne 0 -or $bytes[$peOffset + 3] -ne 0) {
        throw "Missing PE signature: $Path"
    }
    $machine = [BitConverter]::ToUInt16($bytes, $peOffset + 4)
    if ($machine -ne 0x8664) {
        throw ("PE machine is not IMAGE_FILE_MACHINE_AMD64 (0x8664): {0} has 0x{1:x4}" -f $Path, $machine)
    }
}

function Get-PeImports {
    param([Parameter(Mandatory = $true)][string]$Path)

    $dumpOutput = Invoke-CheckedCommand -FilePath "dumpbin.exe" -ArgumentList @("/DEPENDENTS", $Path)
    return @(
        foreach ($line in $dumpOutput) {
            if ([string]$line -match '(?i)^\s*(?<dll>[^\s:]+\.dll)\s*$') {
                $Matches['dll'].ToLowerInvariant()
            }
        }
    )
}

foreach ($runtimeDirectory in $RequiredRuntimeDirectories) {
    if (-not (Test-Path -LiteralPath $runtimeDirectory -PathType Container)) {
        throw "Missing required runtime directory: $runtimeDirectory"
    }
    [void](Resolve-Path -LiteralPath $runtimeDirectory)
}
$env:PATH = (($RequiredRuntimeDirectories -join [IO.Path]::PathSeparator) + [IO.Path]::PathSeparator + $env:PATH)

Assert-CMakeCacheContract -Directory $BuildDirectory
foreach ($executable in $ExpectedReleaseLinkedExecutables) {
    $executablePath = Join-Path $BuildDirectory $executable
    if (-not (Test-Path -LiteralPath $executablePath -PathType Leaf)) {
        throw "Missing release-linked executable: $executablePath"
    }
}

$importsByArtifact = @{}
foreach ($artifact in $ExpectedReleasePeArtifacts) {
    $artifactPath = Join-Path $BuildDirectory $artifact
    if (-not (Test-Path -LiteralPath $artifactPath -PathType Leaf)) {
        throw "Missing release PE artifact: $artifactPath"
    }
    Assert-PeAmd64 -Path $artifactPath
    $importsByArtifact[$artifact] = @(Get-PeImports -Path $artifactPath)
    Write-Host "[release-linked] $artifact is AMD64; imports=[$($importsByArtifact[$artifact] -join ', ')]"
}

foreach ($blockedImport in $BlockedDynamicImports) {
    $blockedArtifacts = @(
        $importsByArtifact.GetEnumerator() |
            Where-Object { @($_.Value) -contains $blockedImport } |
            ForEach-Object Key
    )
    if ($blockedArtifacts.Count -gt 0) {
        throw "Forbidden dynamic import $blockedImport found in: $($blockedArtifacts -join ', ')"
    }
}

foreach ($artifact in $ExpectedObsImportArtifacts) {
    $actualImports = @($importsByArtifact[$artifact])
    if ($actualImports -notcontains "obs.dll") {
        throw "$artifact does not import required OBS runtime obs.dll."
    }
}

$expectedFfmpegSet = @($ExpectedFfmpegImports -split ',')
$expectedFfmpegSet = @(
    $expectedFfmpegSet |
        ForEach-Object { $_.Trim().ToLowerInvariant() } |
        Where-Object { $_ }
)
if ($expectedFfmpegSet.Count -ne 4 -or @($expectedFfmpegSet | Where-Object { $KnownFfmpegImports -notcontains $_ }).Count -gt 0) {
    throw "ExpectedFfmpegImports must name exactly one known four-DLL release ABI set."
}
foreach ($artifact in $ExpectedFfmpegImportArtifacts) {
    $actualImports = @($importsByArtifact[$artifact])
    $actualFfmpeg = @($actualImports | Where-Object { $KnownFfmpegImports -contains $_ })
    Assert-ExactSet -Expected $expectedFfmpegSet -Actual $actualFfmpeg -Description "$artifact FFmpeg imports"
}

foreach ($artifact in $NoObsOrFfmpegImportArtifacts) {
    $actualImports = @($importsByArtifact[$artifact])
    $badImports = @($actualImports | Where-Object { $_ -eq "obs.dll" -or $KnownFfmpegImports -contains $_ })
    if ($badImports.Count -gt 0) {
        throw "$artifact unexpectedly imports OBS or FFmpeg: $($badImports -join ', ')"
    }
}

$discoveryJson = Invoke-CheckedCommand -FilePath "ctest.exe" -ArgumentList @(
    "--test-dir", $BuildDirectory, "-C", "Release", "-L", "^release-linked$", "--show-only=json-v1"
)
$discoveryModel = ($discoveryJson -join "`n") | ConvertFrom-Json
$discoveredTests = @($discoveryModel.tests | ForEach-Object name)
Assert-ExactSet -Expected $ExpectedReleaseLinkedTests -Actual $discoveredTests -Description "release-linked CTest inventory"

foreach ($discoveredTest in @($discoveryModel.tests)) {
    $expectedMetadata = $ExpectedReleaseLinkedMetadata[$discoveredTest.name]
    if ($null -eq $expectedMetadata) {
        throw "No expected metadata exists for discovered CTest: $($discoveredTest.name)"
    }
    $labelProperties = @($discoveredTest.properties | Where-Object name -CEQ "LABELS")
    $timeoutProperties = @($discoveredTest.properties | Where-Object name -CEQ "TIMEOUT")
    if ($labelProperties.Count -ne 1 -or $timeoutProperties.Count -ne 1) {
        throw "CTest $($discoveredTest.name) must expose exactly one LABELS and one TIMEOUT JSON property."
    }
    $actualLabels = @($labelProperties[0].value | ForEach-Object { [string]$_ })
    Assert-ExactSet -Expected @($expectedMetadata.Labels) -Actual $actualLabels -Description "$($discoveredTest.name) labels"
    if ([double]$timeoutProperties[0].value -ne [double]$expectedMetadata.Timeout) {
        throw "CTest $($discoveredTest.name) timeout mismatch. Expected $($expectedMetadata.Timeout); actual $($timeoutProperties[0].value)."
    }
    Write-Host "[release-linked] CTest metadata validated: $($discoveredTest.name) labels=[$($actualLabels -join ', ')] timeout=$($timeoutProperties[0].value)s"
}

$executionOutput = Invoke-CheckedCommand -FilePath "ctest.exe" -ArgumentList @(
    "--test-dir", $BuildDirectory, "-C", "Release", "-L", "^release-linked$", "--no-tests=error",
    "--parallel", "1", "--output-on-failure", "--stop-on-failure"
)
$executionOutput | ForEach-Object { Write-Host $_ }
