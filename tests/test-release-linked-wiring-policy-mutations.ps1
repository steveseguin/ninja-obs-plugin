[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repositoryRoot = Split-Path -Parent $PSScriptRoot
$policyPath = Join-Path $PSScriptRoot "test-release-linked-wiring.ps1"
$hostExecutable = (Get-Process -Id $PID).Path
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)

function New-GreenCMakeFixture {
    $filters = [ordered]@{
        "vdoninja-native-media-linked-owner-lifetime-track" = "Track in-flight callback is safe during manager destruction"
        "vdoninja-native-media-linked-owner-lifetime-data-channel" = "DataChannel in-flight callback is safe during manager destruction"
        "vdoninja-native-media-linked-owner-order-peer-connection" = "PeerConnection description and feedback functions share one owner session"
        "vdoninja-native-media-linked-owner-order-same-handle-data-channel" = "same-handle DataChannel replacement drains before detaching functions"
        "vdoninja-native-media-linked-owner-hook-pre-permit" = "completion delayed until after owner shutdown is rejected"
        "vdoninja-native-media-linked-owner-hook-registration-failure" = "installed function registration failure detaches setter"
        "vdoninja-native-media-linked-owner-hook-track-kinds" = "video alpha audio Track completions share one owner session"
        "vdoninja-native-media-linked-owner-hook-feedback" = "admitted feedback completion drains before owner state"
        "vdoninja-native-media-linked-full" = ""
    }
    $lines = New-Object System.Collections.Generic.List[string]
    foreach ($line in @(
            'cmake_minimum_required(VERSION 3.16)',
            'set(LIBDATACHANNEL_PLOG_INCLUDE_DIR "" CACHE PATH "Explicit plog include")',
            'if(BUILD_MODULE_LIFECYCLE_LINKED_GATE)',
            '    if(NOT IS_DIRECTORY "${LIBDATACHANNEL_PLOG_INCLUDE_DIR}")',
            '        message(FATAL_ERROR "LIBDATACHANNEL_PLOG_INCLUDE_DIR must exist")',
            '    endif()',
            '    target_include_directories(vdoninja-module-lifecycle-linked-gate PRIVATE',
            '        ${LIBDATACHANNEL_PLOG_INCLUDE_DIR})',
            '    add_test(NAME vdoninja-module-lifecycle-linked',
            '        COMMAND $<TARGET_FILE:vdoninja-module-lifecycle-linked-gate>)',
            '    set_tests_properties(vdoninja-module-lifecycle-linked PROPERTIES',
            '        LABELS "module-lifecycle-linked;release-linked"',
            '        TIMEOUT 60)',
            'endif()',
            'if(BUILD_NATIVE_MEDIA_LINKED_GATE)'
        )) {
        $lines.Add($line) | Out-Null
    }
    foreach ($entry in $filters.GetEnumerator()) {
        $lines.Add("    add_test(NAME $($entry.Key)") | Out-Null
        $command = '        COMMAND $<TARGET_FILE:vdoninja-native-media-linked-gate>'
        if ($entry.Value) {
            $command += " `"$($entry.Value)`""
        }
        $lines.Add("${command})") | Out-Null
    }
    foreach ($entry in $filters.GetEnumerator()) {
        $timeout = if ($entry.Key -like '*owner-lifetime-*') { 30 } elseif ($entry.Key -like '*-full') { 600 } else { 60 }
        $labels = if ($entry.Key -like '*-full') {
            'native-media-linked;release-linked'
        } else {
            'native-media-linked;owner-lifetime;release-linked'
        }
        $lines.Add("    set_tests_properties($($entry.Key) PROPERTIES") | Out-Null
        $lines.Add("        LABELS `"$labels`"") | Out-Null
        $lines.Add("        TIMEOUT $timeout)") | Out-Null
    }
    $lines.Add('endif()') | Out-Null
    return ($lines -join "`n")
}

$greenBuildWorkflow = @'
name: Build
jobs:
  build-linux:
    name: Linux
    runs-on: ubuntu-latest
    steps:
      - name: Configure Plugin
        run: |
          export OBS_LIB_DIR="${{ github.workspace }}/obs-sdk/lib"
          cmake -B build -G Ninja \
            -DCMAKE_PREFIX_PATH="${OBS_LIB_DIR}/cmake/libobs"
  build-windows:
    name: Windows (${{ matrix.label }})
    runs-on: windows-2022
    strategy:
      matrix:
        include:
          - label: OBS 32.2.x
            obs_version: 32.2.0
            obs_source_ref: 32.2.0
            obs_source_tag: 32.2.0
            asset_suffix: ''
            ffmpeg_imports: avcodec-62.dll,avutil-60.dll,swscale-9.dll,swresample-6.dll
          - label: OBS 32.0-32.1 legacy
            obs_version: 32.0.4
            obs_source_ref: 32.0.4
            obs_source_tag: 32.0.4
            asset_suffix: -obs32.0-32.1
            ffmpeg_imports: avcodec-61.dll,avutil-59.dll,swscale-8.dll,swresample-5.dll
    steps:
      - name: Configure Plugin
        shell: pwsh
        run: |
          $plogIncludeDir = (Resolve-Path "${{ github.workspace }}\libdatachannel-src\deps\plog\include").Path
          cmake -B build `
            -DBUILD_PLUGIN=ON `
            -DBUILD_NATIVE_MEDIA_LINKED_GATE=ON `
            -DBUILD_MODULE_LIFECYCLE_LINKED_GATE=ON `
            -DLIBDATACHANNEL_PLOG_INCLUDE_DIR="$plogIncludeDir"
      - name: Build Plugin
        shell: pwsh
        run: cmake --build build
      - name: Run release linked gates
        shell: pwsh
        timeout-minutes: 20
        run: |
          $ffmpegDepsRoot = Get-Item "${{ github.workspace }}\obs-studio\.deps\obs-deps-fixture"
          pwsh -NoLogo -NoProfile -ExecutionPolicy Bypass -File scripts\run-release-linked-gates.ps1 `
            -BuildDirectory build `
            -ObsRundir "${{ github.workspace }}\obs-studio\build_x64\rundir\RelWithDebInfo\bin\64bit" `
            -LibobsRuntimeDirectory "${{ github.workspace }}\obs-studio\build_x64\libobs\RelWithDebInfo" `
            -PthreadsRuntimeDirectory "${{ github.workspace }}\obs-studio\build_x64\deps\w32-pthreads\RelWithDebInfo" `
            -ObsDepsBinDirectory "$($ffmpegDepsRoot.FullName)\bin" `
            -ExpectedFfmpegImports "${{ matrix.ffmpeg_imports }}"
      - name: Package
        shell: pwsh
        run: cmake --install build
  release:
    needs: [build-windows]
    runs-on: ubuntu-latest
    steps:
      - name: Publish
        run: echo publish
'@

$greenCiWorkflow = @'
name: CI
jobs:
  test:
    name: Dependency-light checks
    runs-on: ubuntu-latest
    steps:
      - name: Checkout
        uses: actions/checkout@v4
      - name: Release wiring policy
        shell: pwsh
        run: pwsh -NoLogo -NoProfile -File tests/test-release-linked-wiring.ps1
      - name: Release wiring policy mutations
        shell: pwsh
        run: pwsh -NoLogo -NoProfile -File tests/test-release-linked-wiring-policy-mutations.ps1
'@

$greenWrapper = @'
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
    param([string]$FilePath, [string[]]$ArgumentList)
    $commandOutput = & $FilePath @ArgumentList
    $commandExitCode = $LASTEXITCODE
    if ($commandExitCode -ne 0) {
        throw "External tool failed: $FilePath"
    }
    return $commandOutput
}

function Assert-ExactSet {
    param([string[]]$Expected, [string[]]$Actual)
    $difference = @(Compare-Object -ReferenceObject $Expected -DifferenceObject $Actual)
    if ($difference.Count -gt 0) {
        throw "Exact set mismatch"
    }
}

function Assert-CMakeCacheContract {
    param([string]$Directory)
    $cachePath = Join-Path $Directory "CMakeCache.txt"
    $cacheText = [IO.File]::ReadAllText($cachePath)
    foreach ($entry in $ExpectedCMakeCacheEntries) {
        if (-not $cacheText.Contains($entry)) {
            throw "Missing cache entry: $entry"
        }
    }
    $plogPrefix = "LIBDATACHANNEL_PLOG_INCLUDE_DIR:PATH="
    $plogLine = @($cacheText -split "`n" | Where-Object { $_.StartsWith($plogPrefix) })
    if ($plogLine.Count -ne 1) {
        throw "Missing exact plog cache path"
    }
    $plogPath = $plogLine[0].Substring($plogPrefix.Length)
    if (-not (Test-Path -LiteralPath $plogPath -PathType Container)) {
        throw "Plog cache path is not a directory"
    }
    [void](Resolve-Path -LiteralPath $plogPath)
}

function Assert-PeAmd64 {
    param([string]$Path)
    $bytes = [IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -lt 64) {
        throw "Truncated PE image"
    }
    $peOffset = [BitConverter]::ToInt32($bytes, 0x3c)
    $machine = [BitConverter]::ToUInt16($bytes, $peOffset + 4)
    if ($machine -ne 0x8664) {
        throw "PE machine is not IMAGE_FILE_MACHINE_AMD64"
    }
}

function Get-PeImports {
    param([string]$Path)
    $dumpOutput = Invoke-CheckedCommand -FilePath "dumpbin.exe" -ArgumentList @("/DEPENDENTS", $Path)
    return @($dumpOutput | Where-Object { $_ -match '\.dll' })
}

foreach ($runtimeDirectory in $RequiredRuntimeDirectories) {
    if (-not (Test-Path -LiteralPath $runtimeDirectory -PathType Container)) {
        throw "Missing runtime directory"
    }
    [void](Resolve-Path -LiteralPath $runtimeDirectory)
}
$env:PATH = (($RequiredRuntimeDirectories -join [IO.Path]::PathSeparator) + [IO.Path]::PathSeparator + $env:PATH)

Assert-CMakeCacheContract -Directory $BuildDirectory
foreach ($executable in $ExpectedReleaseLinkedExecutables) {
    if (-not (Test-Path -LiteralPath (Join-Path $BuildDirectory $executable) -PathType Leaf)) {
        throw "Missing linked executable"
    }
}

$importsByArtifact = @{}
foreach ($artifact in $ExpectedReleasePeArtifacts) {
    $artifactPath = Join-Path $BuildDirectory $artifact
    Assert-PeAmd64 -Path $artifactPath
    $importsByArtifact[$artifact] = @(Get-PeImports -Path $artifactPath)
}
foreach ($blockedImport in $BlockedDynamicImports) {
    if (@($importsByArtifact.Values | Where-Object { $_ -contains $blockedImport }).Count -gt 0) {
        throw "Forbidden dynamic import: $blockedImport"
    }
}
foreach ($artifact in $ExpectedObsImportArtifacts) {
    $actualImports = @($importsByArtifact[$artifact])
    if ($actualImports -notcontains "obs.dll") {
        throw "Missing obs.dll import"
    }
}
$expectedFfmpegSet = @($ExpectedFfmpegImports -split ',')
foreach ($artifact in $ExpectedFfmpegImportArtifacts) {
    $actualImports = @($importsByArtifact[$artifact])
    $actualFfmpeg = @($actualImports | Where-Object { $KnownFfmpegImports -contains $_ })
    Assert-ExactSet -Expected $expectedFfmpegSet -Actual $actualFfmpeg
}
foreach ($artifact in $NoObsOrFfmpegImportArtifacts) {
    $actualImports = @($importsByArtifact[$artifact])
    $badImports = @($actualImports | Where-Object { $_ -eq "obs.dll" -or $KnownFfmpegImports -contains $_ })
    if ($badImports.Count -gt 0) {
        throw "Module gate imports OBS or FFmpeg"
    }
}

$discoveryJson = Invoke-CheckedCommand -FilePath "ctest.exe" -ArgumentList @(
    "--test-dir", $BuildDirectory, "-L", "^release-linked$", "--show-only=json-v1"
)
$discoveryModel = $discoveryJson | ConvertFrom-Json
$discoveredTests = @($discoveryModel.tests | ForEach-Object name)
Assert-ExactSet -Expected $ExpectedReleaseLinkedTests -Actual $discoveredTests
[void](Invoke-CheckedCommand -FilePath "ctest.exe" -ArgumentList @(
        "--test-dir", $BuildDirectory, "-L", "^release-linked$", "--no-tests=error",
        "--parallel", "1", "--output-on-failure", "--stop-on-failure"
    ))
'@

function Write-Fixture {
    param(
        [string]$Root,
        [string]$CMake,
        [string]$BuildWorkflow,
        [string]$CiWorkflow,
        [string]$Wrapper
    )
    foreach ($directory in @(".github/workflows", "scripts", "tests")) {
        [void](New-Item -ItemType Directory -Force -Path (Join-Path $Root $directory))
    }
    [IO.File]::WriteAllText((Join-Path $Root "CMakeLists.txt"), $CMake, $utf8NoBom)
    [IO.File]::WriteAllText((Join-Path $Root ".github/workflows/build.yml"), $BuildWorkflow, $utf8NoBom)
    [IO.File]::WriteAllText((Join-Path $Root ".github/workflows/ci.yml"), $CiWorkflow, $utf8NoBom)
    [IO.File]::WriteAllText((Join-Path $Root "scripts/run-release-linked-gates.ps1"), $Wrapper, $utf8NoBom)
}

function Invoke-PolicyFixture {
    param([string]$Root)
    $output = @(& $hostExecutable -NoLogo -NoProfile -ExecutionPolicy Bypass -File $policyPath -RepositoryRoot $Root 2>&1 | ForEach-Object ToString)
    $exitCode = $LASTEXITCODE
    $failedNames = @(
        foreach ($line in $output) {
            if ($line -match '^\[FAIL\] (?<name>.*?) :: ') {
                $Matches['name']
            }
        }
    )
    return [pscustomobject]@{
        ExitCode = $exitCode
        FailedNames = $failedNames
        Output = $output
    }
}

$missingReleaseInvocationFailures = @(
    "Windows workflow executes the checked release-linked wrapper exactly once",
    "Release-linked wrapper runs after build and before Package",
    "Release-linked workflow step is common and unconditional",
    "Release-linked workflow step has an exact 20-minute limit",
    "Release-linked invocation supplies exact BuildDirectory=build",
    "Release-linked invocation supplies ObsRundir",
    "Release-linked invocation supplies LibobsRuntimeDirectory",
    "Release-linked invocation supplies PthreadsRuntimeDirectory",
    "Release-linked invocation supplies ObsDepsBinDirectory",
    "Release-linked invocation supplies ExpectedFfmpegImports"
)
$greenReleaseInvocation = @'
          pwsh -NoLogo -NoProfile -ExecutionPolicy Bypass -File scripts\run-release-linked-gates.ps1 `
            -BuildDirectory build `
            -ObsRundir "${{ github.workspace }}\obs-studio\build_x64\rundir\RelWithDebInfo\bin\64bit" `
            -LibobsRuntimeDirectory "${{ github.workspace }}\obs-studio\build_x64\libobs\RelWithDebInfo" `
            -PthreadsRuntimeDirectory "${{ github.workspace }}\obs-studio\build_x64\deps\w32-pthreads\RelWithDebInfo" `
            -ObsDepsBinDirectory "$($ffmpegDepsRoot.FullName)\bin" `
            -ExpectedFfmpegImports "${{ matrix.ffmpeg_imports }}"
'@
$hiddenReleaseInvocation = "          function Invoke-HiddenReleaseGate {`n" +
    ((@($greenReleaseInvocation -split "`r?`n") | ForEach-Object { "    $_" }) -join "`n") +
    "`n          }"
$wrapperExecutionMarker = 'foreach ($runtimeDirectory in $RequiredRuntimeDirectories) {'
$wrapperExecutionIndex = $greenWrapper.IndexOf($wrapperExecutionMarker, [StringComparison]::Ordinal)
if ($wrapperExecutionIndex -lt 0) {
    throw "Known-green wrapper execution marker is missing."
}
$greenWrapperExecution = $greenWrapper.Substring($wrapperExecutionIndex)
$hiddenWrapperExecution = "function Invoke-HiddenWrapperGate {`n" +
    ((@($greenWrapperExecution -split "`r?`n") | ForEach-Object { "    $_" }) -join "`n") +
    "`n}"

$mutations = @(
    [pscustomobject]@{
        Name = "Linux Configure Plugin forced to pwsh"
        File = "build"
        Old = "      - name: Configure Plugin`n        run: |`n          export OBS_LIB_DIR="
        New = "      - name: Configure Plugin`n        shell: pwsh`n        run: |`n          export OBS_LIB_DIR="
        Expected = @("Linux Configure Plugin remains an unconditional Bash step")
    },
    [pscustomobject]@{
        Name = "dead release-linked step"
        File = "build"
        Old = "      - name: Run release linked gates`n        shell: pwsh"
        New = "      - name: Run release linked gates`n        if: false`n        shell: pwsh"
        Expected = @("Release-linked workflow step is common and unconditional")
    },
    [pscustomobject]@{
        Name = "dead normal-CI policy step"
        File = "ci"
        Old = "      - name: Release wiring policy`n        shell: pwsh"
        New = "      - name: Release wiring policy`n        if: false`n        shell: pwsh"
        Expected = @("Dependency-light normal CI unconditionally executes the static policy once")
    },
    [pscustomobject]@{
        Name = "removed normal-CI mutation-policy invocation"
        File = "ci"
        Old = "        run: pwsh -NoLogo -NoProfile -File tests/test-release-linked-wiring-policy-mutations.ps1"
        New = "        run: Write-Host 'mutation policy intentionally omitted'"
        Expected = @("Dependency-light normal CI unconditionally executes the policy mutation harness once")
    },
    [pscustomobject]@{
        Name = "dead normal-CI mutation-policy step"
        File = "ci"
        Old = "      - name: Release wiring policy mutations`n        shell: pwsh"
        New = "      - name: Release wiring policy mutations`n        if: false`n        shell: pwsh"
        Expected = @("Dependency-light normal CI unconditionally executes the policy mutation harness once")
    },
    [pscustomobject]@{
        Name = "comment-only normal-CI mutation-policy invocation"
        File = "ci"
        Old = "        run: pwsh -NoLogo -NoProfile -File tests/test-release-linked-wiring-policy-mutations.ps1"
        New = "        run: # pwsh -NoLogo -NoProfile -File tests/test-release-linked-wiring-policy-mutations.ps1"
        Expected = @("Dependency-light normal CI unconditionally executes the policy mutation harness once")
    },
    [pscustomobject]@{
        Name = "wrong-path normal-CI mutation-policy invocation"
        File = "ci"
        Old = "tests/test-release-linked-wiring-policy-mutations.ps1"
        New = "tests/test-release-linked-wiring-policy-mutations-disabled.ps1"
        Expected = @("Dependency-light normal CI unconditionally executes the policy mutation harness once")
    },
    [pscustomobject]@{
        Name = "comment-only release invocation"
        File = "build"
        Old = '          pwsh -NoLogo -NoProfile -ExecutionPolicy Bypass -File scripts\run-release-linked-gates.ps1 `'
        New = '          # pwsh -NoLogo -NoProfile -ExecutionPolicy Bypass -File scripts\run-release-linked-gates.ps1 `'
        Expected = $missingReleaseInvocationFailures
    },
    [pscustomobject]@{
        Name = "release invocation hidden in uncalled function"
        File = "build"
        Old = $greenReleaseInvocation
        New = $hiddenReleaseInvocation
        Expected = $missingReleaseInvocationFailures
    },
    [pscustomobject]@{
        Name = "stale build directory"
        File = "build"
        Old = '-BuildDirectory build `'
        New = '-BuildDirectory build-stale `'
        Expected = @("Release-linked invocation supplies exact BuildDirectory=build")
    },
    [pscustomobject]@{
        Name = "wrong current-row FFmpeg ABI"
        File = "build"
        Old = "ffmpeg_imports: avcodec-62.dll,avutil-60.dll,swscale-9.dll,swresample-6.dll"
        New = "ffmpeg_imports: avcodec-61.dll,avutil-60.dll,swscale-9.dll,swresample-6.dll"
        Expected = @("Windows matrix exact OBS/FFmpeg mapping: OBS 32.2.x")
    },
    [pscustomobject]@{
        Name = "unused PE enforcement array"
        File = "wrapper"
        Old = 'foreach ($artifact in $ExpectedReleasePeArtifacts) {'
        New = 'foreach ($artifact in @("obs-vdoninja.dll")) {'
        Expected = @(
            "Wrapper enforces PE AMD64 for the plugin and both linked executables",
            "Wrapper inspects imports for the plugin and both linked executables"
        )
    },
    [pscustomobject]@{
        Name = "reset external-tool exit before capture"
        File = "wrapper"
        Old = '    $commandExitCode = $LASTEXITCODE'
        New = '    $LASTEXITCODE = 0' + "`n" + '    $commandExitCode = $LASTEXITCODE'
        Expected = @("Wrapper centralizes external tools with fatal exit propagation")
    },
    [pscustomobject]@{
        Name = "whole wrapper execution hidden in uncalled function"
        File = "wrapper"
        Old = $greenWrapperExecution
        New = $hiddenWrapperExecution
        Expected = @(
            "Wrapper centralizes external tools with fatal exit propagation",
            "CTest and import tooling only run through Invoke-CheckedCommand",
            "Wrapper discovers CTest inventory as JSON",
            "Wrapper compares discovered tests with the exact inventory",
            "CTest discovery JSON feeds the compared inventory",
            "Wrapper makes an empty CTest selection fatal",
            "Wrapper serializes linked CTest execution",
            "Wrapper emits linked CTest failures",
            "Both CTest calls select only the exact release-linked label",
            "Wrapper stops on the first linked failure",
            "Exact-set mismatches are fatal",
            "Wrapper validates the configured CMake cache",
            "Wrapper resolves the explicit plog cache path",
            "Wrapper fatally validates both linked executable paths",
            "Wrapper enforces PE AMD64 for the plugin and both linked executables",
            "Wrapper inspects imports for the plugin and both linked executables",
            "Forbidden imports are consumed by a fatal validation loop",
            "Wrapper compares actual and matrix-expected FFmpeg imports",
            "Wrapper requires the OBS runtime import",
            "Wrapper applies the exact FFmpeg comparison to both artifacts",
            "Wrapper rejects OBS or any matrix FFmpeg ABI on the module gate",
            "Wrapper validates every runtime directory",
            "Wrapper prepends all runtime directories to PATH"
        )
    },
    [pscustomobject]@{
        Name = "artifact import alias skips native and module inspection"
        File = "wrapper"
        Old = '    $artifactPath = Join-Path $BuildDirectory $artifact' + "`n" +
            '    Assert-PeAmd64 -Path $artifactPath' + "`n" +
            '    $importsByArtifact[$artifact] = @(Get-PeImports -Path $artifactPath)'
        New = '    $artifactPath = Join-Path $BuildDirectory "obs-vdoninja.dll"' + "`n" +
            '    Assert-PeAmd64 -Path $artifactPath' + "`n" +
            '    $importsByArtifact[$artifact] = @($importsByArtifact["obs-vdoninja.dll"])'
        Expected = @(
            "CTest and import tooling only run through Invoke-CheckedCommand",
            "Wrapper enforces PE AMD64 for the plugin and both linked executables",
            "Wrapper inspects imports for the plugin and both linked executables"
        )
    },
    [pscustomobject]@{
        Name = "different runtime list prepended to PATH"
        File = "wrapper"
        Old = '$env:PATH = (($RequiredRuntimeDirectories -join [IO.Path]::PathSeparator) + [IO.Path]::PathSeparator + $env:PATH)'
        New = '$PathRuntimeDirectories = @($ObsRundir)' + "`n" +
            '$env:PATH = (($PathRuntimeDirectories -join [IO.Path]::PathSeparator) + [IO.Path]::PathSeparator + $env:PATH)'
        Expected = @("Wrapper prepends all runtime directories to PATH")
    },
    [pscustomobject]@{
        Name = "raw comma-delimited FFmpeg string compared as one item"
        File = "wrapper"
        Old = 'Assert-ExactSet -Expected $expectedFfmpegSet -Actual $actualFfmpeg'
        New = 'Assert-ExactSet -Expected $ExpectedFfmpegImports -Actual $actualFfmpeg'
        Expected = @(
            "Wrapper compares actual and matrix-expected FFmpeg imports",
            "Wrapper applies the exact FFmpeg comparison to both artifacts"
        )
    },
    [pscustomobject]@{
        Name = "vendored plog fallback"
        File = "cmake"
        Old = 'set(LIBDATACHANNEL_PLOG_INCLUDE_DIR "" CACHE PATH "Explicit plog include")'
        New = 'set(LIBDATACHANNEL_PLOG_INCLUDE_DIR "" CACHE PATH "Explicit plog include")' + "`n" + 'set(PLOG_FALLBACK "${CMAKE_CURRENT_SOURCE_DIR}/deps/libdatachannel/deps/plog/include")'
        Expected = @("CMake has no vendored plog fallback")
    },
    [pscustomobject]@{
        Name = "wrong native add_test target"
        File = "cmake"
        Old = 'COMMAND $<TARGET_FILE:vdoninja-native-media-linked-gate> "Track in-flight callback is safe during manager destruction"'
        New = 'COMMAND $<TARGET_FILE:vdoninja-module-lifecycle-linked-gate> "Track in-flight callback is safe during manager destruction"'
        Expected = @("CTest command mapping: vdoninja-native-media-linked-owner-lifetime-track")
    },
    [pscustomobject]@{
        Name = "wrong native add_test filter"
        File = "cmake"
        Old = '"Track in-flight callback is safe during manager destruction"'
        New = '"Track callback wrong filter"'
        Expected = @("CTest command mapping: vdoninja-native-media-linked-owner-lifetime-track")
    },
    [pscustomobject]@{
        Name = "extra native CTest label"
        File = "cmake"
        Old = 'LABELS "native-media-linked;owner-lifetime;release-linked"'
        New = 'LABELS "native-media-linked;owner-lifetime;release-linked;unexpected"'
        Expected = @("Native CTest exact labels: vdoninja-native-media-linked-owner-lifetime-track")
        ReplaceFirst = $true
    },
    [pscustomobject]@{
        Name = "missing native CTest label"
        File = "cmake"
        Old = 'LABELS "native-media-linked;owner-lifetime;release-linked"'
        New = 'LABELS "native-media-linked;owner-lifetime"'
        Expected = @("Native CTest exact labels: vdoninja-native-media-linked-owner-lifetime-track")
        ReplaceFirst = $true
    },
    [pscustomobject]@{
        Name = "native labels and timeout hidden under if FALSE"
        File = "cmake"
        Old = '    set_tests_properties(vdoninja-native-media-linked-owner-lifetime-track PROPERTIES' + "`n" +
            '        LABELS "native-media-linked;owner-lifetime;release-linked"' + "`n" +
            '        TIMEOUT 30)'
        New = '    if(FALSE)' + "`n" +
            '        set_tests_properties(vdoninja-native-media-linked-owner-lifetime-track PROPERTIES' + "`n" +
            '            LABELS "native-media-linked;owner-lifetime;release-linked"' + "`n" +
            '            TIMEOUT 30)' + "`n" +
            '    endif()'
        Expected = @(
            "CTest timeout: vdoninja-native-media-linked-owner-lifetime-track",
            "Native CTest exact labels: vdoninja-native-media-linked-owner-lifetime-track"
        )
    }
)

$tempBase = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$runRoot = Join-Path $tempBase ("ninja-plugin-release-policy-mutations-" + [guid]::NewGuid().ToString("N"))
[void](New-Item -ItemType Directory -Path $runRoot)
try {
    $greenCMake = New-GreenCMakeFixture
    $baselineRoot = Join-Path $runRoot "baseline"
    Write-Fixture $baselineRoot $greenCMake $greenBuildWorkflow $greenCiWorkflow $greenWrapper
    $baseline = Invoke-PolicyFixture $baselineRoot
    if ($baseline.ExitCode -ne 0 -or $baseline.FailedNames.Count -ne 0) {
        throw "Known-green policy fixture did not pass:`n$($baseline.Output -join "`n")"
    }

    $killed = 0
    foreach ($mutation in $mutations) {
        $cmake = $greenCMake
        $build = $greenBuildWorkflow
        $ci = $greenCiWorkflow
        $wrapper = $greenWrapper
        $source = Get-Variable -Name $mutation.File -ValueOnly
        $occurrenceCount = [regex]::Matches($source, [regex]::Escape($mutation.Old)).Count
        if ($occurrenceCount -lt 1) {
            throw "Mutation '$($mutation.Name)' seed was not found."
        }
        if ($mutation.PSObject.Properties.Name -contains 'ReplaceFirst' -and $mutation.ReplaceFirst) {
            $match = [regex]::Match($source, [regex]::Escape($mutation.Old))
            $source = $source.Substring(0, $match.Index) + $mutation.New + $source.Substring($match.Index + $match.Length)
        } else {
            if ($occurrenceCount -ne 1) {
                throw "Mutation '$($mutation.Name)' seed was not unique (count=$occurrenceCount)."
            }
            $source = $source.Replace($mutation.Old, $mutation.New)
        }
        Set-Variable -Name $mutation.File -Value $source

        $fixtureRoot = Join-Path $runRoot ([regex]::Replace($mutation.Name, '[^A-Za-z0-9]+', '-').Trim('-'))
        Write-Fixture $fixtureRoot $cmake $build $ci $wrapper
        $result = Invoke-PolicyFixture $fixtureRoot
        $missingFailures = @($mutation.Expected | Where-Object { $result.FailedNames -cnotcontains $_ })
        $unexpectedFailures = @($result.FailedNames | Where-Object { $mutation.Expected -cnotcontains $_ })
        if ($result.ExitCode -ne 1 -or $missingFailures.Count -gt 0 -or $unexpectedFailures.Count -gt 0) {
            throw "Mutation '$($mutation.Name)' was not isolated. exit=$($result.ExitCode); missing=[$($missingFailures -join ', ')]; unexpected=[$($unexpectedFailures -join ', ')]; output:`n$($result.Output -join "`n")"
        }
        $killed++
        Write-Host "[KILLED] $($mutation.Name) -> $($result.FailedNames -join ', ')"
    }
    Write-Host "Release-linked policy mutation check: $killed/$($mutations.Count) mutations killed; baseline 100% green; no unrelated assertion flips."
} finally {
    $resolvedRunRoot = [IO.Path]::GetFullPath($runRoot)
    if ($resolvedRunRoot.StartsWith($tempBase, [StringComparison]::OrdinalIgnoreCase) -and
        (Split-Path -Leaf $resolvedRunRoot) -like 'ninja-plugin-release-policy-mutations-*') {
        Remove-Item -LiteralPath $resolvedRunRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}
