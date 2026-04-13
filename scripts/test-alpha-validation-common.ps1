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

    $portablePluginRoot = Join-Path $RepoRoot "_obs-portable\\config\\obs-studio\\plugins\\obs-vdoninja"
    $portableDll = Join-Path $portablePluginRoot "bin\\64bit\\obs-vdoninja.dll"
    New-Item -ItemType Directory -Force -Path (Split-Path $portableDll -Parent) | Out-Null
    Copy-Item $sourceDll $portableDll -Force
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
