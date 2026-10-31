function Sync-PortableObsPluginPayload {
    param(
        [Parameter(Mandatory = $true)]
        [string]$RepoRoot,
        [Parameter(Mandatory = $true)]
        [string]$InstallPrefixPath
    )

    $sourceDll = Join-Path $InstallPrefixPath "obs-plugins\\64bit\\obs-vdoninja.dll"
    if (-not (Test-Path $sourceDll)) {
        throw "Portable OBS sync requires plugin DLL at $sourceDll"
    }

    $sourceData = Join-Path $InstallPrefixPath "data\obs-plugins\obs-vdoninja"
    if (-not (Test-Path -LiteralPath (Join-Path $sourceData "locale\en-US.ini"))) {
        throw "Portable OBS sync requires module data at $sourceData"
    }
    $portableRoot = [IO.Path]::GetFullPath((Join-Path $RepoRoot '_obs-portable'))
    $runningObs = @(Get-Process obs64 -ErrorAction SilentlyContinue | Where-Object {
        $_.Path -and $_.Path.StartsWith($portableRoot + '\', [StringComparison]::OrdinalIgnoreCase)
    })
    if ($runningObs.Count) {
        throw "Stop the test portable OBS before replacing its plugin payload"
    }

    function Copy-PortableTestFile([string]$Source, [string]$Destination) {
        $target = [IO.Path]::GetFullPath($Destination)
        if (-not $target.StartsWith($portableRoot + '\', [StringComparison]::OrdinalIgnoreCase) -or
            $target -ieq [IO.Path]::GetFullPath($Source)) {
            throw "Invalid portable test payload destination: $target"
        }
        New-Item -ItemType Directory -Force -Path (Split-Path $target -Parent) | Out-Null
        # Test runtimes can share hardlinks with archived runs. Replace the link,
        # not the contents of the shared file.
        if (Test-Path -LiteralPath $target) {
            Remove-Item -LiteralPath $target -Force
        }
        Copy-Item -LiteralPath $Source -Destination $target
    }

    # Portable OBS discovers both its installation plugin directory and the
    # portable user-plugin directory. Keep both copies identical: otherwise the
    # installation copy loads first, registers the output, and silently makes a
    # newer user-plugin DLL look active even though its implementation is not.
    $portableDlls = @(
        (Join-Path $RepoRoot "_obs-portable\\obs-plugins\\64bit\\obs-vdoninja.dll"),
        (Join-Path $RepoRoot "_obs-portable\\config\\obs-studio\\plugins\\obs-vdoninja\\bin\\64bit\\obs-vdoninja.dll")
    )
    foreach ($portableDll in $portableDlls) {
        Copy-PortableTestFile $sourceDll $portableDll
    }

    # OBS resolves module resources relative to the installed DLL. The data-path
    # environment variable alone does not install the release's locale files.
    foreach ($portableData in @(
        (Join-Path $RepoRoot "_obs-portable\data\obs-plugins\obs-vdoninja"),
        (Join-Path $RepoRoot "_obs-portable\config\obs-studio\plugins\obs-vdoninja\data")
    )) {
        $sourceDataPath = (Resolve-Path -LiteralPath $sourceData).Path.TrimEnd('\')
        Get-ChildItem -LiteralPath $sourceDataPath -Recurse -File | ForEach-Object {
            $relativePath = $_.FullName.Substring($sourceDataPath.Length).TrimStart('\')
            Copy-PortableTestFile $_.FullName (Join-Path $portableData $relativePath)
        }
    }
}

function Wait-AlphaMotionDemoWindow {
    param(
        [Parameter(Mandatory = $true)][int]$ChromeProcessId,
        [int]$TimeoutSeconds = 20
    )

    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    do {
        $window = Get-Process -Id $ChromeProcessId -ErrorAction Stop |
            Where-Object { $_.MainWindowTitle -like '*GC Alpha Motion Demo*' } |
            Select-Object -First 1
        if ($window) {
            return
        }
        Start-Sleep -Milliseconds 250
    } while ((Get-Date) -lt $deadline)
    throw "Chrome did not expose the GC Alpha Motion Demo capture window within $TimeoutSeconds seconds"
}

function Stop-AlphaMotionDemoProcesses {
    param([Parameter(Mandatory = $true)][string]$ProfilePath)

    Get-CimInstance Win32_Process -Filter "Name = 'chrome.exe'" -ErrorAction SilentlyContinue |
        Where-Object {
            $_.CommandLine -and $_.CommandLine.IndexOf(
                $ProfilePath, [StringComparison]::OrdinalIgnoreCase) -ge 0
        } |
        ForEach-Object {
            Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue
        }
}

function Get-TestObsPluginEvidence {
    param(
        [Parameter(Mandatory = $true)][int]$ObsProcessId,
        [Parameter(Mandatory = $true)][string]$InstallPrefixPath
    )
    $expected = (Get-FileHash -LiteralPath (Join-Path $InstallPrefixPath 'obs-plugins/64bit/obs-vdoninja.dll')).Hash
    $modules = @((Get-Process -Id $ObsProcessId -ErrorAction Stop).Modules |
        Where-Object { $_.ModuleName -ieq 'obs-vdoninja.dll' })
    if ($modules.Count -ne 1) { throw "Test OBS must load exactly one plugin DLL; found $($modules.Count)" }
    $actual = (Get-FileHash -LiteralPath $modules[0].FileName).Hash
    if ($actual -ne $expected) { throw "Test OBS loaded a different plugin DLL: $($modules[0].FileName)" }
    [pscustomobject]@{ path = $modules[0].FileName; sha256 = $actual.ToLowerInvariant() }
}

function Get-LatestPortableObsLogPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$RepoRoot
    )

    $logsDir = Join-Path $RepoRoot "_obs-portable\\config\\obs-studio\\logs"
    $latest = Get-ChildItem $logsDir -File | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (-not $latest) {
        throw "No portable OBS log found in $logsDir"
    }

    return $latest.FullName
}
