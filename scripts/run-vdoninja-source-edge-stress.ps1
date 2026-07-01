param(
    [string]$StreamId = "codexEdgeStress",
    [string]$Password = "false",
    [string]$RoomId = "",
    [string]$PushUrl = "",
    [string]$ViewUrl = "",
    [string]$ObsExe = ".\_obs-portable\bin\64bit\obs64.exe",
    [string]$ObsWorkingDirectory = ".\_obs-portable\bin\64bit",
    [string]$InstallPrefix = ".\install-obs32",
    [int]$ObsWebSocketPort = 4465,
    [int]$ObsStartupSeconds = 18,
    [int]$PublisherWarmupSeconds = 18,
    [int]$PublisherStartupWaitMs = 20000,
    [int]$PublisherViewProbeWaitMs = 25000,
    [int]$PublisherKeepAliveMs = 900000,
    [int]$RapidIterations = 30,
    [int]$RapidWaitMs = 150,
    [int]$ExtraSources = 0,
    [ValidateSet("off", "basic", "aggressive")]
    [string]$PublisherChurn = "off",
    [int]$PublisherChurnIterations = 0,
    [int]$PublisherChurnIntervalMs = 0,
    [switch]$PublisherExtraAudio,
    [int]$PublisherCount = 1,
    [int]$PublisherStaggerMs = 1500,
    [string]$PublisherUrlVariants = "",
    [int]$PublisherReloads = 0,
    [int]$PublisherReloadIntervalMs = 20000,
    [int]$PublisherReloadStartupWaitMs = 8000,
    [ValidateSet("off", "official", "aggressive", "terminal")]
    [string]$PublisherDataFuzz = "off",
    [switch]$VideoChurn,
    [ValidateSet("standard", "aggressive")]
    [string]$VideoChurnProfile = "standard",
    [switch]$SkipEdgeCases,
    [switch]$StableRapidSettings,
    [int]$RequestTimeoutMs = 30000,
    [int]$CheckTimeoutSeconds = 180,
    [int]$FinalWaitMs = 2500,
    [switch]$SkipPublisher
)

$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$obsExePath = (Resolve-Path $ObsExe).Path
$obsWorkingDirPath = (Resolve-Path $ObsWorkingDirectory).Path
$installPrefixPath = (Resolve-Path $InstallPrefix).Path
$pluginPath = (Resolve-Path (Join-Path $installPrefixPath "obs-plugins\64bit")).Path
$dataPath = (Resolve-Path (Join-Path $installPrefixPath "data\obs-plugins")).Path
$depsBin = "C:\Users\steve\Code\obs-build-dependencies\windows-deps-2023-06-01-x64\bin"
$obsWebSocketConfigPath = Join-Path $repoRoot "_obs-portable\config\obs-studio\plugin_config\obs-websocket\config.json"
$portableScenesDir = Join-Path $repoRoot "_obs-portable\config\obs-studio\basic\scenes"
$sentinelDir = Join-Path $repoRoot "_obs-portable\config\obs-studio\.sentinel"
$artifactRoot = Join-Path $repoRoot "artifacts"

if ([string]::IsNullOrWhiteSpace($PushUrl)) {
    $PushUrl = "https://vdo.ninja/?push=$StreamId&password=$Password&webcam=1&autostart=1"
}
if ([string]::IsNullOrWhiteSpace($ViewUrl)) {
    $ViewUrl = "https://vdo.ninja/?view=$StreamId&password=$Password"
}

$obsStdout = Join-Path $artifactRoot "obs-source-edge-obs.stdout.log"
$obsStderr = Join-Path $artifactRoot "obs-source-edge-obs.stderr.log"
$publisherOut = Join-Path $artifactRoot "obs-source-edge-publisher.out.log"
$publisherErr = Join-Path $artifactRoot "obs-source-edge-publisher.err.log"
$checkOut = Join-Path $artifactRoot "obs-source-edge-check.out.log"
$checkErr = Join-Path $artifactRoot "obs-source-edge-check.err.log"

foreach ($path in @($obsStdout, $obsStderr, $publisherOut, $publisherErr, $checkOut, $checkErr)) {
    if (Test-Path $path) {
        Remove-Item $path -Force
    }
}

function Get-RunningProcessByExecutablePath {
    param([string]$ExecutablePath)

    Get-CimInstance Win32_Process -Filter "Name = 'obs64.exe'" -ErrorAction SilentlyContinue |
        Where-Object { $_.ExecutablePath -and $_.ExecutablePath -ieq $ExecutablePath }
}

function Wait-TcpPort {
    param(
        [string]$HostName,
        [int]$Port,
        [int]$TimeoutSeconds
    )

    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ((Get-Date) -lt $deadline) {
        $client = [System.Net.Sockets.TcpClient]::new()
        try {
            $async = $client.BeginConnect($HostName, $Port, $null, $null)
            if ($async.AsyncWaitHandle.WaitOne(500)) {
                $client.EndConnect($async)
                return $true
            }
        } catch {
        } finally {
            $client.Close()
        }
        Start-Sleep -Milliseconds 250
    }

    return $false
}

function Quote-ProcessArgument {
    param([string]$Argument)

    if ($null -eq $Argument) {
        return '""'
    }

    return '"' + ($Argument -replace '"', '\"') + '"'
}

function Copy-IfExists {
    param(
        [string]$Path,
        [string]$DestinationDirectory
    )

    if (-not [string]::IsNullOrWhiteSpace($Path) -and (Test-Path $Path)) {
        Copy-Item -Path $Path -Destination $DestinationDirectory -Force -ErrorAction SilentlyContinue
    }
}

function Capture-ObsEdgeEvidence {
    param(
        [System.Diagnostics.Process]$ObsProcess,
        [string]$Reason
    )

    $stamp = Get-Date -Format "yyyyMMdd-HHmmss"
    $safeReason = ($Reason -replace '[^A-Za-z0-9_.-]', '_')
    $captureDir = Join-Path $artifactRoot "obs-edge-capture-$stamp-$safeReason"
    New-Item -ItemType Directory -Path $captureDir -Force | Out-Null

    $obsLogsDir = Join-Path $repoRoot "_obs-portable\config\obs-studio\logs"
    $metadataPath = Join-Path $captureDir "metadata.json"
    $processPath = Join-Path $captureDir "obs-process.txt"
    $dumpOut = Join-Path $captureDir "minidump.stdout.log"
    $dumpErr = Join-Path $captureDir "minidump.stderr.log"
    $dumpStatus = Join-Path $captureDir "minidump-status.txt"

    $metadata = [ordered]@{
        capturedAt = (Get-Date).ToString("o")
        reason = $Reason
        streamId = $StreamId
        obsPid = if ($ObsProcess) { $ObsProcess.Id } else { $null }
        obsHasExited = if ($ObsProcess) {
            try {
                $ObsProcess.Refresh()
                $ObsProcess.HasExited
            } catch {
                $true
            }
        } else {
            $null
        }
        obsStartedAt = if ($obsStartedAt) { $obsStartedAt.ToString("o") } else { $null }
        requestTimeoutMs = $RequestTimeoutMs
        checkTimeoutSeconds = $CheckTimeoutSeconds
        videoChurn = $VideoChurn.IsPresent
        videoChurnProfile = $VideoChurnProfile
        publisherChurn = $PublisherChurn
        publisherDataFuzz = $PublisherDataFuzz
        rapidIterations = $RapidIterations
        rapidWaitMs = $RapidWaitMs
        extraSources = $ExtraSources
    }
    $metadata | ConvertTo-Json -Depth 4 | Set-Content -Path $metadataPath -Encoding UTF8

    foreach ($path in @($obsStdout, $obsStderr, $publisherOut, $publisherErr, $checkOut, $checkErr)) {
        Copy-IfExists -Path $path -DestinationDirectory $captureDir
    }

    if (Test-Path $obsLogsDir) {
        Get-ChildItem $obsLogsDir -Filter "*.txt" -File -ErrorAction SilentlyContinue |
            Sort-Object LastWriteTime -Descending |
            Select-Object -First 5 |
            Copy-Item -Destination $captureDir -Force -ErrorAction SilentlyContinue
    }

    if ($ObsProcess) {
        try {
            $ObsProcess.Refresh()
            $ObsProcess | Format-List * | Out-File -FilePath $processPath -Encoding UTF8
            $cim = Get-CimInstance Win32_Process -Filter "ProcessId = $($ObsProcess.Id)" -ErrorAction SilentlyContinue
            if ($cim) {
                "`n--- Win32_Process ---`n" | Out-File -FilePath $processPath -Encoding UTF8 -Append
                $cim | Format-List * | Out-File -FilePath $processPath -Encoding UTF8 -Append
            }
        } catch {
            "Failed to write process metadata: $($_.Exception.Message)" | Out-File -FilePath $processPath -Encoding UTF8
        }

        if (-not $ObsProcess.HasExited) {
            $dumpPath = Join-Path $captureDir "obs64-$($ObsProcess.Id).dmp"
            $rundll32 = Join-Path $env:windir "System32\rundll32.exe"
            $comsvcs = Join-Path $env:windir "System32\comsvcs.dll"
            try {
                $dumpArgs = @("$comsvcs,MiniDump", $ObsProcess.Id, $dumpPath, "full")
                $dumpProc = Start-Process -FilePath $rundll32 `
                    -ArgumentList $dumpArgs `
                    -RedirectStandardOutput $dumpOut `
                    -RedirectStandardError $dumpErr `
                    -WindowStyle Hidden `
                    -Wait `
                    -PassThru
                if (Test-Path $dumpPath) {
                    $currentIdentity = [System.Security.Principal.WindowsIdentity]::GetCurrent().Name
                    & icacls $dumpPath /grant ($currentIdentity + ":(R)") | Out-Null
                }
                "MiniDump exit code: $($dumpProc.ExitCode)`nDump path: $dumpPath" |
                    Out-File -FilePath $dumpStatus -Encoding UTF8
            } catch {
                "MiniDump failed: $($_.Exception.Message)" | Out-File -FilePath $dumpStatus -Encoding UTF8
            }
        } else {
            "OBS already exited; no live process dump captured." | Out-File -FilePath $dumpStatus -Encoding UTF8
        }
    }

    Write-Output "OBS_EDGE_CAPTURE=$captureDir"
}

$existingObs = @(Get-RunningProcessByExecutablePath -ExecutablePath $obsExePath)
if ($existingObs.Count -gt 0) {
    $existingIds = ($existingObs | Select-Object -ExpandProperty ProcessId) -join ", "
    throw "Refusing to start OBS edge stress because obs64 is already running from $obsExePath (PID(s): $existingIds)"
}

if (Test-Path $sentinelDir) {
    Get-ChildItem $sentinelDir -File -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue
}

if (Test-Path $portableScenesDir) {
    Get-ChildItem $portableScenesDir -Filter "Untitled.json*" -File -ErrorAction SilentlyContinue |
        Remove-Item -Force -ErrorAction SilentlyContinue
}

if (-not (Test-Path $obsWebSocketConfigPath)) {
    throw "obs-websocket config not found at $obsWebSocketConfigPath"
}

$originalObsWebSocketConfig = Get-Content $obsWebSocketConfigPath -Raw
$obsWebSocketConfig = $originalObsWebSocketConfig | ConvertFrom-Json
$obsWebSocketConfig.server_enabled = $true
$obsWebSocketConfig.auth_required = $false
$obsWebSocketConfig.server_port = $ObsWebSocketPort
$obsWebSocketConfig | ConvertTo-Json | Set-Content -Path $obsWebSocketConfigPath -Encoding UTF8

$env:OBS_WEBSOCKET_URL = "ws://127.0.0.1:$ObsWebSocketPort"
$env:OBS_WEBSOCKET_REQUEST_TIMEOUT_MS = [string]$RequestTimeoutMs
$env:VDONINJA_STREAM_ID = $StreamId
$env:VDONINJA_PASSWORD = $Password
$env:VDONINJA_ROOM_ID = $RoomId
$env:VDONINJA_EDGE_RAPID_ITERATIONS = [string]$RapidIterations
$env:VDONINJA_EDGE_RAPID_WAIT_MS = [string]$RapidWaitMs
$env:VDONINJA_EDGE_EXTRA_SOURCES = [string]$ExtraSources
$env:VDONINJA_EDGE_VIDEO_CHURN = if ($VideoChurn) { "1" } else { "0" }
$env:VDONINJA_EDGE_VIDEO_CHURN_PROFILE = $VideoChurnProfile
$env:VDONINJA_EDGE_SKIP_CASES = if ($SkipEdgeCases) { "1" } else { "0" }
$env:VDONINJA_EDGE_STABLE_RAPID_SETTINGS = if ($StableRapidSettings) { "1" } else { "0" }
$env:VDONINJA_EDGE_FINAL_WAIT_MS = [string]([Math]::Max(0, $FinalWaitMs))
$env:PUBLISH_CHURN_PROFILE = $PublisherChurn
$env:PUBLISH_DATA_FUZZ_PROFILE = $PublisherDataFuzz
$env:PUBLISH_STARTUP_WAIT_MS = [string]([Math]::Max(0, $PublisherStartupWaitMs))
$env:VIEW_PROBE_WAIT_MS = [string]([Math]::Max(0, $PublisherViewProbeWaitMs))
$env:PUBLISH_DURATION_MS = [string]([Math]::Max(0, $PublisherKeepAliveMs))
if ($PublisherChurnIterations -gt 0) {
    $env:PUBLISH_CHURN_ITERATIONS = [string]$PublisherChurnIterations
}
if ($PublisherChurnIntervalMs -gt 0) {
    $env:PUBLISH_CHURN_INTERVAL_MS = [string]$PublisherChurnIntervalMs
}
if ($PublisherExtraAudio) {
    $env:PUBLISH_CHURN_EXTRA_AUDIO = "1"
}
$env:PUBLISHER_COUNT = [string]([Math]::Max(1, $PublisherCount))
$env:PUBLISHER_STAGGER_MS = [string]([Math]::Max(0, $PublisherStaggerMs))
if (-not [string]::IsNullOrWhiteSpace($PublisherUrlVariants)) {
    $env:PUBLISHER_URL_VARIANTS = $PublisherUrlVariants
}
if ($PublisherReloads -gt 0) {
    $env:PUBLISHER_RELOADS = [string]$PublisherReloads
}
if ($PublisherReloadIntervalMs -gt 0) {
    $env:PUBLISHER_RELOAD_INTERVAL_MS = [string]$PublisherReloadIntervalMs
}
if ($PublisherReloadStartupWaitMs -gt 0) {
    $env:PUBLISHER_RELOAD_STARTUP_WAIT_MS = [string]$PublisherReloadStartupWaitMs
}
$env:OBS_PLUGINS_PATH = $pluginPath
$env:OBS_PLUGINS_DATA_PATH = $dataPath
$env:PATH = "$depsBin;$env:PATH"

$obsProc = $null
$publisherProc = $null
$checkProc = $null

try {
    $obsStartedAt = Get-Date
    $obsProc = Start-Process -FilePath $obsExePath -ArgumentList "--portable" `
        -WorkingDirectory $obsWorkingDirPath `
        -RedirectStandardOutput $obsStdout `
        -RedirectStandardError $obsStderr `
        -PassThru

    Start-Sleep -Seconds $ObsStartupSeconds
    $obsProc.Refresh()
    if ($obsProc.HasExited) {
        throw "OBS exited during startup with code $($obsProc.ExitCode)"
    }
    if (-not (Wait-TcpPort -HostName "127.0.0.1" -Port $ObsWebSocketPort -TimeoutSeconds 30)) {
        throw "obs-websocket port $ObsWebSocketPort did not open for edge stress"
    }

    if (-not $SkipPublisher) {
        $publisherProc = Start-Process -FilePath "node" `
            -ArgumentList "scripts/playwright-vdo-publish-session.cjs", $PushUrl, $ViewUrl `
            -WorkingDirectory $repoRoot `
            -RedirectStandardOutput $publisherOut `
            -RedirectStandardError $publisherErr `
            -PassThru

        Start-Sleep -Seconds $PublisherWarmupSeconds
    }

    $checkArgs = @("scripts/obs-websocket-vdoninja-edge-stress.cjs", $StreamId, $Password)
    if ($RoomId -ne "") {
        $checkArgs += $RoomId
    }

    $checkProc = Start-Process -FilePath "node" `
        -ArgumentList $checkArgs `
        -WorkingDirectory $repoRoot `
        -RedirectStandardOutput $checkOut `
        -RedirectStandardError $checkErr `
        -PassThru

    try {
        Wait-Process -Id $checkProc.Id -Timeout $CheckTimeoutSeconds -ErrorAction Stop
    } catch {
        Capture-ObsEdgeEvidence -ObsProcess $obsProc -Reason "check-process-timeout"
        if ($checkProc -and -not $checkProc.HasExited) {
            Stop-Process -Id $checkProc.Id -Force -ErrorAction SilentlyContinue
        }
        throw "Timed out waiting for edge stress process PID $($checkProc.Id) after $CheckTimeoutSeconds seconds"
    }
    $checkProc.WaitForExit()
    $checkProc.Refresh()
    $checkExitCode = $checkProc.ExitCode
    $checkStderrText = if (Test-Path $checkErr) { Get-Content $checkErr -Raw } else { "" }
    $checkStdoutText = if (Test-Path $checkOut) { Get-Content $checkOut -Raw } else { "" }
    if ($null -eq $checkExitCode) {
        $checkExitCode = if ($checkStderrText -match "(?m)^Error:" -or $checkStdoutText -notmatch '"ok"\s*:\s*true') {
            1
        } else {
            0
        }
    }
    $checkExitCode = [int]$checkExitCode

    $obsProc.Refresh()
    if ($obsProc.HasExited) {
        Capture-ObsEdgeEvidence -ObsProcess $obsProc -Reason "obs-exited"
        throw "OBS exited during edge stress with code $($obsProc.ExitCode)"
    }

    Write-Output "OBS_PID=$($obsProc.Id)"
    Write-Output "PUBLISHER_PID=$($publisherProc.Id)"
    Write-Output "CHECK_EXIT=$checkExitCode"
    Write-Output "RAPID_ITERATIONS=$RapidIterations"
    Write-Output "RAPID_WAIT_MS=$RapidWaitMs"
    Write-Output "EXTRA_SOURCES=$ExtraSources"
    Write-Output "PUBLISHER_CHURN=$PublisherChurn"
    Write-Output "PUBLISHER_CHURN_ITERATIONS=$PublisherChurnIterations"
    Write-Output "PUBLISHER_CHURN_INTERVAL_MS=$PublisherChurnIntervalMs"
    Write-Output "PUBLISHER_EXTRA_AUDIO=$($PublisherExtraAudio.IsPresent)"
    Write-Output "PUBLISHER_STARTUP_WAIT_MS=$PublisherStartupWaitMs"
    Write-Output "PUBLISHER_VIEW_PROBE_WAIT_MS=$PublisherViewProbeWaitMs"
    Write-Output "PUBLISHER_KEEP_ALIVE_MS=$PublisherKeepAliveMs"
    Write-Output "PUBLISHER_COUNT=$PublisherCount"
    Write-Output "PUBLISHER_STAGGER_MS=$PublisherStaggerMs"
    Write-Output "PUBLISHER_URL_VARIANTS=$PublisherUrlVariants"
    Write-Output "PUBLISHER_RELOADS=$PublisherReloads"
    Write-Output "PUBLISHER_RELOAD_INTERVAL_MS=$PublisherReloadIntervalMs"
    Write-Output "PUBLISHER_RELOAD_STARTUP_WAIT_MS=$PublisherReloadStartupWaitMs"
    Write-Output "PUBLISHER_DATA_FUZZ=$PublisherDataFuzz"
    Write-Output "VIDEO_CHURN=$($VideoChurn.IsPresent)"
    Write-Output "VIDEO_CHURN_PROFILE=$VideoChurnProfile"
    Write-Output "SKIP_EDGE_CASES=$($SkipEdgeCases.IsPresent)"
    Write-Output "STABLE_RAPID_SETTINGS=$($StableRapidSettings.IsPresent)"
    Write-Output "REQUEST_TIMEOUT_MS=$RequestTimeoutMs"
    Write-Output "FINAL_WAIT_MS=$FinalWaitMs"
    Write-Output "OBS_STARTED_AT=$($obsStartedAt.ToString("o"))"
    if (Test-Path $checkOut) {
        Get-Content $checkOut
    }
    if (Test-Path $checkErr) {
        Get-Content $checkErr
    }
    if ($checkExitCode -ne 0) {
        $timeoutLikeFailure = $checkStderrText -match "Timed out after|obs-websocket connection closed|not connected" -or
            $checkStdoutText -match "Timed out after|obs-websocket connection closed|not connected"
        if ($timeoutLikeFailure) {
            Capture-ObsEdgeEvidence -ObsProcess $obsProc -Reason "obs-websocket-timeout"
        }
        throw "Edge stress failed with exit code $checkExitCode"
    }
} finally {
    if ($originalObsWebSocketConfig -ne $null) {
        Set-Content -Path $obsWebSocketConfigPath -Value $originalObsWebSocketConfig -Encoding UTF8
    }
    foreach ($proc in @($checkProc, $publisherProc, $obsProc)) {
        if ($proc -and -not $proc.HasExited) {
            Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
        }
    }
    Remove-Item Env:OBS_WEBSOCKET_URL -ErrorAction SilentlyContinue
    Remove-Item Env:OBS_WEBSOCKET_REQUEST_TIMEOUT_MS -ErrorAction SilentlyContinue
    Remove-Item Env:VDONINJA_STREAM_ID -ErrorAction SilentlyContinue
    Remove-Item Env:VDONINJA_PASSWORD -ErrorAction SilentlyContinue
    Remove-Item Env:VDONINJA_ROOM_ID -ErrorAction SilentlyContinue
    Remove-Item Env:VDONINJA_EDGE_RAPID_ITERATIONS -ErrorAction SilentlyContinue
    Remove-Item Env:VDONINJA_EDGE_RAPID_WAIT_MS -ErrorAction SilentlyContinue
    Remove-Item Env:VDONINJA_EDGE_EXTRA_SOURCES -ErrorAction SilentlyContinue
    Remove-Item Env:VDONINJA_EDGE_VIDEO_CHURN -ErrorAction SilentlyContinue
    Remove-Item Env:VDONINJA_EDGE_VIDEO_CHURN_PROFILE -ErrorAction SilentlyContinue
    Remove-Item Env:VDONINJA_EDGE_SKIP_CASES -ErrorAction SilentlyContinue
    Remove-Item Env:VDONINJA_EDGE_STABLE_RAPID_SETTINGS -ErrorAction SilentlyContinue
    Remove-Item Env:VDONINJA_EDGE_FINAL_WAIT_MS -ErrorAction SilentlyContinue
    Remove-Item Env:PUBLISH_CHURN_PROFILE -ErrorAction SilentlyContinue
    Remove-Item Env:PUBLISH_DATA_FUZZ_PROFILE -ErrorAction SilentlyContinue
    Remove-Item Env:PUBLISH_STARTUP_WAIT_MS -ErrorAction SilentlyContinue
    Remove-Item Env:VIEW_PROBE_WAIT_MS -ErrorAction SilentlyContinue
    Remove-Item Env:PUBLISH_DURATION_MS -ErrorAction SilentlyContinue
    Remove-Item Env:PUBLISH_CHURN_ITERATIONS -ErrorAction SilentlyContinue
    Remove-Item Env:PUBLISH_CHURN_INTERVAL_MS -ErrorAction SilentlyContinue
    Remove-Item Env:PUBLISH_CHURN_EXTRA_AUDIO -ErrorAction SilentlyContinue
    Remove-Item Env:PUBLISHER_COUNT -ErrorAction SilentlyContinue
    Remove-Item Env:PUBLISHER_STAGGER_MS -ErrorAction SilentlyContinue
    Remove-Item Env:PUBLISHER_URL_VARIANTS -ErrorAction SilentlyContinue
    Remove-Item Env:PUBLISHER_RELOADS -ErrorAction SilentlyContinue
    Remove-Item Env:PUBLISHER_RELOAD_INTERVAL_MS -ErrorAction SilentlyContinue
    Remove-Item Env:PUBLISHER_RELOAD_STARTUP_WAIT_MS -ErrorAction SilentlyContinue
}
