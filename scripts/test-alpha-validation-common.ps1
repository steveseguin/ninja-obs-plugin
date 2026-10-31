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

    # Portable OBS discovers both its installation plugin directory and the
    # portable user-plugin directory. Keep both copies identical: otherwise the
    # installation copy loads first, registers the output, and silently makes a
    # newer user-plugin DLL look active even though its implementation is not.
    $portableDlls = @(
        (Join-Path $RepoRoot "_obs-portable\\obs-plugins\\64bit\\obs-vdoninja.dll"),
        (Join-Path $RepoRoot "_obs-portable\\config\\obs-studio\\plugins\\obs-vdoninja\\bin\\64bit\\obs-vdoninja.dll")
    )
    foreach ($portableDll in $portableDlls) {
        New-Item -ItemType Directory -Force -Path (Split-Path $portableDll -Parent) | Out-Null
        Copy-Item $sourceDll $portableDll -Force
    }

    # OBS resolves module resources relative to the installed DLL. The data-path
    # environment variable alone does not install the release's locale files.
    $sourceData = Join-Path $InstallPrefixPath "data\obs-plugins\obs-vdoninja"
    if (-not (Test-Path -LiteralPath (Join-Path $sourceData "locale\en-US.ini"))) {
        throw "Portable OBS sync requires module data at $sourceData"
    }
    foreach ($portableData in @(
        (Join-Path $RepoRoot "_obs-portable\data\obs-plugins\obs-vdoninja"),
        (Join-Path $RepoRoot "_obs-portable\config\obs-studio\plugins\obs-vdoninja\data")
    )) {
        New-Item -ItemType Directory -Force -Path $portableData | Out-Null
        Copy-Item -Path (Join-Path $sourceData '*') -Destination $portableData -Recurse -Force
    }
}

function Wait-AlphaMotionDemoWindow {
    param([int]$TimeoutSeconds = 20)

    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    do {
        $window = Get-Process chrome -ErrorAction SilentlyContinue |
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
    Get-CimInstance Win32_Process -Filter "Name = 'chrome.exe'" -ErrorAction SilentlyContinue |
        Where-Object {
            $_.CommandLine -like '*gc-motion-demo.html*' -or
            $_.CommandLine -like '*tests/tools/gc-motion-demo.html*' -or
            $_.CommandLine -like '*chrome-gc-alpha-*'
        } |
        ForEach-Object {
            Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue
        }
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
