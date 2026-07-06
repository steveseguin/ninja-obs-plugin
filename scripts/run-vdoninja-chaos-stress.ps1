param(
    [string]$StreamId = "",
    [string]$Password = "false",
    [string]$RoomId = "",
    [string]$PushUrl = "",
    [string]$ViewUrl = "",
    [string]$ObsExe = ".\_obs-portable\bin\64bit\obs64.exe",
    [string]$ObsWorkingDirectory = ".\_obs-portable\bin\64bit",
    [string]$InstallPrefix = ".\install",
    [int]$ObsWebSocketPort = 4471,
    [int]$ObsStartupSeconds = 22,
    [int]$PublisherWarmupSeconds = 25,
    [int]$PublisherRecoveryWarmupSeconds = 28,
    [int]$PublisherKeepAliveMs = 420000,
    [ValidateSet("off", "basic", "aggressive")]
    [string]$PublisherChurn = "aggressive",
    [ValidateSet("off", "official", "aggressive", "terminal")]
    [string]$PublisherDataFuzz = "aggressive",
    [int]$PublisherChurnIterations = 80,
    [int]$PublisherChurnIntervalMs = 700,
    [int]$PublisherReloads = 0,
    [int]$PublisherReloadIntervalMs = 25000,
    [int]$PublisherReloadStartupWaitMs = 8000,
    [int]$OfflineVerifySeconds = 10,
    [int]$ChaosPhaseTimeoutSeconds = 210,
    [switch]$SkipObsRestart,
    [switch]$SkipPublisherRecovery,
    [switch]$SkipWindowChurn,
    [switch]$KeepScene
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($StreamId)) {
    $StreamId = "codexChaos" + (Get-Date -Format "HHmmss")
}
if ([string]::IsNullOrWhiteSpace($PushUrl)) {
    $PushUrl = "https://vdo.ninja/?push=$StreamId&password=$Password&webcam=1&autostart=1"
}
if ([string]::IsNullOrWhiteSpace($ViewUrl)) {
    $ViewUrl = "https://vdo.ninja/?view=$StreamId&password=$Password"
}

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
$obsLogDir = Join-Path $repoRoot "_obs-portable\config\obs-studio\logs"
$runDir = Join-Path $repoRoot ("artifacts\vdoninja-chaos-" + (Get-Date -Format "yyyyMMdd-HHmmss"))
New-Item -ItemType Directory -Path $runDir -Force | Out-Null

$obsProc = $null
$publisherProc = $null
$originalObsWebSocketConfig = $null

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

function Stop-ProcessTree {
    param([int]$ProcessId)

    if ($ProcessId -le 0) {
        return
    }

    Get-CimInstance Win32_Process -Filter "ParentProcessId = $ProcessId" -ErrorAction SilentlyContinue |
        ForEach-Object { Stop-ProcessTree -ProcessId ([int]$_.ProcessId) }

    Stop-Process -Id $ProcessId -Force -ErrorAction SilentlyContinue
}

function Copy-LatestObsLog {
    param([string]$Label)

    if (-not (Test-Path $obsLogDir)) {
        return $null
    }
    $latest = Get-ChildItem $obsLogDir -Filter *.txt -File |
        Sort-Object LastWriteTimeUtc -Descending |
        Select-Object -First 1
    if (-not $latest) {
        return $null
    }
    $destination = Join-Path $runDir ("obs-$Label-" + $latest.Name)
    Copy-Item -Path $latest.FullName -Destination $destination -Force -ErrorAction SilentlyContinue
    return $destination
}

function Clear-ObsSentinel {
    if (Test-Path $sentinelDir) {
        Get-ChildItem $sentinelDir -File -ErrorAction SilentlyContinue |
            Remove-Item -Force -ErrorAction SilentlyContinue
    }
}

function Start-PortableObs {
    param([string]$Label)

    Clear-ObsSentinel
    $stdout = Join-Path $runDir "obs-$Label.stdout.log"
    $stderr = Join-Path $runDir "obs-$Label.stderr.log"
    $process = Start-Process -FilePath $obsExePath -ArgumentList "--portable" `
        -WorkingDirectory $obsWorkingDirPath `
        -RedirectStandardOutput $stdout `
        -RedirectStandardError $stderr `
        -WindowStyle Hidden `
        -PassThru
    $script:obsProc = $process

    Start-Sleep -Seconds $ObsStartupSeconds
    $process.Refresh()
    if ($process.HasExited) {
        throw "OBS exited during $Label startup with code $($process.ExitCode)"
    }
    if (-not (Wait-TcpPort -HostName "127.0.0.1" -Port $ObsWebSocketPort -TimeoutSeconds 30)) {
        throw "obs-websocket port $ObsWebSocketPort did not open during $Label"
    }
    return $process
}

function Stop-PortableObs {
    param([string]$Label)

    if ($script:obsProc -and -not $script:obsProc.HasExited) {
        try {
            [void]$script:obsProc.CloseMainWindow()
            [void]$script:obsProc.WaitForExit(12000)
            $script:obsProc.Refresh()
        } catch {
        }
        if (-not $script:obsProc.HasExited) {
            Stop-ProcessTree -ProcessId $script:obsProc.Id
            Start-Sleep -Seconds 3
        }
    }
    Copy-LatestObsLog -Label $Label | Out-Null
    Clear-ObsSentinel
    $script:obsProc = $null
}

function Start-Publisher {
    param([string]$Label)

    $env:PUBLISH_CHURN_PROFILE = $PublisherChurn
    $env:PUBLISH_DATA_FUZZ_PROFILE = $PublisherDataFuzz
    $env:PUBLISH_CHURN_ITERATIONS = [string]([Math]::Max(0, $PublisherChurnIterations))
    $env:PUBLISH_CHURN_INTERVAL_MS = [string]([Math]::Max(100, $PublisherChurnIntervalMs))
    $env:PUBLISH_CHURN_EXTRA_AUDIO = "1"
    $env:PUBLISH_DURATION_MS = [string]([Math]::Max(0, $PublisherKeepAliveMs))
    $env:PUBLISH_STARTUP_WAIT_MS = "18000"
    $env:VIEW_PROBE_WAIT_MS = "20000"
    $env:PUBLISHER_RELOADS = [string]([Math]::Max(0, $PublisherReloads))
    $env:PUBLISHER_RELOAD_INTERVAL_MS = [string]([Math]::Max(1000, $PublisherReloadIntervalMs))
    $env:PUBLISHER_RELOAD_STARTUP_WAIT_MS = [string]([Math]::Max(1000, $PublisherReloadStartupWaitMs))

    $stdout = Join-Path $runDir "publisher-$Label.stdout.log"
    $stderr = Join-Path $runDir "publisher-$Label.stderr.log"
    return Start-Process -FilePath "node" `
        -ArgumentList @("scripts/playwright-vdo-publish-session.cjs", $PushUrl, $ViewUrl) `
        -WorkingDirectory $repoRoot `
        -RedirectStandardOutput $stdout `
        -RedirectStandardError $stderr `
        -WindowStyle Hidden `
        -PassThru
}

function Stop-Publisher {
    param([string]$Label)

    if ($script:publisherProc -and -not $script:publisherProc.HasExited) {
        Stop-ProcessTree -ProcessId $script:publisherProc.Id
        Start-Sleep -Seconds 2
    }
    $script:publisherProc = $null
}

function Invoke-ObsWindowChurn {
    param([System.Diagnostics.Process]$Process)

    if ($SkipWindowChurn -or -not $Process) {
        return
    }

    try {
        Add-Type -Namespace Win32 -Name NativeMethods -MemberDefinition @"
[System.Runtime.InteropServices.DllImport("user32.dll")]
public static extern bool ShowWindowAsync(System.IntPtr hWnd, int nCmdShow);
[System.Runtime.InteropServices.DllImport("user32.dll")]
public static extern bool SetForegroundWindow(System.IntPtr hWnd);
"@ -ErrorAction SilentlyContinue
    } catch {
    }

    for ($i = 0; $i -lt 5; $i++) {
        $Process.Refresh()
        $handle = $Process.MainWindowHandle
        if ($handle -eq [IntPtr]::Zero) {
            Start-Sleep -Milliseconds 500
            continue
        }
        [void][Win32.NativeMethods]::ShowWindowAsync($handle, 6)
        Start-Sleep -Milliseconds 350
        [void][Win32.NativeMethods]::ShowWindowAsync($handle, 9)
        [void][Win32.NativeMethods]::SetForegroundWindow($handle)
        Start-Sleep -Milliseconds 500
    }
}

function Invoke-ChaosPhase {
    param(
        [string]$Phase,
        [string]$Label = "",
        [switch]$RequireMotion
    )

    $reportName = if ([string]::IsNullOrWhiteSpace($Label)) { $Phase } else { $Label }
    $out = Join-Path $runDir "chaos-$reportName.stdout.log"
    $err = Join-Path $runDir "chaos-$reportName.stderr.log"
    $report = Join-Path $runDir "chaos-$reportName.json"

    $env:OBS_WEBSOCKET_URL = "ws://127.0.0.1:$ObsWebSocketPort"
    $env:OBS_WEBSOCKET_REQUEST_TIMEOUT_MS = "60000"
    $env:VDONINJA_STREAM_ID = $StreamId
    $env:VDONINJA_PASSWORD = $Password
    $env:VDONINJA_ROOM_ID = $RoomId
    $env:VDONINJA_CHAOS_PHASE = $Phase
    $env:VDONINJA_CHAOS_REPORT_DIR = $runDir
    $env:VDONINJA_CHAOS_REPORT = $report
    $env:VDONINJA_CHAOS_REQUIRE_MOTION = if ($RequireMotion) { "1" } else { "0" }
    $env:VDONINJA_CHAOS_INITIAL_WAIT_MS = "16000"
    $env:VDONINJA_MIN_SCREENSHOT_BYTES = "8000"

    $phaseProc = Start-Process -FilePath "node" `
        -ArgumentList @("scripts/obs-websocket-vdoninja-chaos-check.cjs") `
        -WorkingDirectory $repoRoot `
        -RedirectStandardOutput $out `
        -RedirectStandardError $err `
        -WindowStyle Hidden `
        -PassThru

    try {
        Wait-Process -Id $phaseProc.Id -Timeout $ChaosPhaseTimeoutSeconds -ErrorAction Stop
        [void]$phaseProc.WaitForExit(5000)
    } catch {
        Copy-LatestObsLog -Label "phase-$Phase-timeout" | Out-Null
        Stop-ProcessTree -ProcessId $phaseProc.Id
        throw "Chaos phase '$Phase' timed out after $ChaosPhaseTimeoutSeconds seconds"
    }
    $phaseProc.Refresh()
    $exitCode = $phaseProc.ExitCode
    if ($null -eq $exitCode) {
        $exitCode = if (Test-Path $report) { 0 } else { 1 }
    }
    $exitCode = [int]$exitCode
    if ($exitCode -ne 0 -or -not (Test-Path $report)) {
        Copy-LatestObsLog -Label "phase-$Phase-fail" | Out-Null
        $stderrText = if (Test-Path $err) { Get-Content $err -Raw } else { "" }
        if ($exitCode -ne 0) {
            throw "Chaos phase '$Phase' failed with exit code $exitCode. $stderrText"
        }
        throw "Chaos phase '$Phase' did not write report $report. $stderrText"
    }
    return $report
}

$existingObs = @(Get-RunningProcessByExecutablePath -ExecutablePath $obsExePath)
if ($existingObs.Count -gt 0) {
    $existingIds = ($existingObs | Select-Object -ExpandProperty ProcessId) -join ", "
    throw "Refusing to start chaos stress because obs64 is already running from $obsExePath (PID(s): $existingIds)"
}

Clear-ObsSentinel
if (Test-Path $portableScenesDir) {
    Get-ChildItem $portableScenesDir -Filter "Untitled.json*" -File -ErrorAction SilentlyContinue |
        Remove-Item -Force -ErrorAction SilentlyContinue
}
if (-not (Test-Path $obsWebSocketConfigPath)) {
    throw "obs-websocket config not found at $obsWebSocketConfigPath"
}

try {
    $originalObsWebSocketConfig = Get-Content $obsWebSocketConfigPath -Raw
    $obsWebSocketConfig = $originalObsWebSocketConfig | ConvertFrom-Json
    $obsWebSocketConfig.server_enabled = $true
    $obsWebSocketConfig.auth_required = $false
    $obsWebSocketConfig.server_port = $ObsWebSocketPort
    $obsWebSocketConfig | ConvertTo-Json | Set-Content -Path $obsWebSocketConfigPath -Encoding UTF8

    $env:OBS_PLUGINS_PATH = $pluginPath
    $env:OBS_PLUGINS_DATA_PATH = $dataPath
    $env:PATH = "$depsBin;$env:PATH"

    $obsProc = Start-PortableObs -Label "initial"
    $publisherProc = Start-Publisher -Label "initial"
    Start-Sleep -Seconds $PublisherWarmupSeconds

    $setupReport = Invoke-ChaosPhase -Phase "setup" -RequireMotion
    Invoke-ObsWindowChurn -Process $obsProc
    $mutateReport = Invoke-ChaosPhase -Phase "mutate" -RequireMotion

    $restartReport = $null
    if (-not $SkipObsRestart) {
        Stop-PortableObs -Label "before-restart"
        $obsProc = Start-PortableObs -Label "after-restart"
        Invoke-ObsWindowChurn -Process $obsProc
        $restartReport = Invoke-ChaosPhase -Phase "verify" -Label "restart-verify" -RequireMotion
    }

    $offlineReport = $null
    $recoveryReport = $null
    if (-not $SkipPublisherRecovery) {
        Stop-Publisher -Label "initial"
        Start-Sleep -Seconds $OfflineVerifySeconds
        $offlineReport = Invoke-ChaosPhase -Phase "offline-verify"
        $publisherProc = Start-Publisher -Label "recovery"
        Start-Sleep -Seconds $PublisherRecoveryWarmupSeconds
        $recoveryReport = Invoke-ChaosPhase -Phase "verify" -Label "recovery-verify" -RequireMotion
    }

    $cleanupReport = $null
    if (-not $KeepScene) {
        $cleanupReport = Invoke-ChaosPhase -Phase "cleanup"
    }

    Copy-LatestObsLog -Label "final" | Out-Null

    $summary = [ordered]@{
        ok = $true
        runDir = $runDir
        streamId = $StreamId
        obsWebSocketPort = $ObsWebSocketPort
        setupReport = $setupReport
        mutateReport = $mutateReport
        restartReport = $restartReport
        offlineReport = $offlineReport
        recoveryReport = $recoveryReport
        cleanupReport = $cleanupReport
    }
    $summaryPath = Join-Path $runDir "summary.json"
    $summary | ConvertTo-Json -Depth 4 | Set-Content -Path $summaryPath -Encoding UTF8

    Write-Output "VDONINJA_CHAOS_PASS=1"
    Write-Output "RUN_DIR=$runDir"
    Write-Output "STREAM_ID=$StreamId"
    Write-Output "SUMMARY=$summaryPath"
    $summary | ConvertTo-Json -Depth 4
} finally {
    if ($originalObsWebSocketConfig -ne $null) {
        Set-Content -Path $obsWebSocketConfigPath -Value $originalObsWebSocketConfig -Encoding UTF8
    }
    Stop-Publisher -Label "final"
    Stop-PortableObs -Label "final-stop"

    Remove-Item Env:OBS_WEBSOCKET_URL -ErrorAction SilentlyContinue
    Remove-Item Env:OBS_WEBSOCKET_REQUEST_TIMEOUT_MS -ErrorAction SilentlyContinue
    Remove-Item Env:OBS_PLUGINS_PATH -ErrorAction SilentlyContinue
    Remove-Item Env:OBS_PLUGINS_DATA_PATH -ErrorAction SilentlyContinue
    Remove-Item Env:VDONINJA_STREAM_ID -ErrorAction SilentlyContinue
    Remove-Item Env:VDONINJA_PASSWORD -ErrorAction SilentlyContinue
    Remove-Item Env:VDONINJA_ROOM_ID -ErrorAction SilentlyContinue
    Remove-Item Env:VDONINJA_CHAOS_PHASE -ErrorAction SilentlyContinue
    Remove-Item Env:VDONINJA_CHAOS_REPORT_DIR -ErrorAction SilentlyContinue
    Remove-Item Env:VDONINJA_CHAOS_REPORT -ErrorAction SilentlyContinue
    Remove-Item Env:VDONINJA_CHAOS_REQUIRE_MOTION -ErrorAction SilentlyContinue
    Remove-Item Env:VDONINJA_CHAOS_INITIAL_WAIT_MS -ErrorAction SilentlyContinue
    Remove-Item Env:VDONINJA_MIN_SCREENSHOT_BYTES -ErrorAction SilentlyContinue
    Remove-Item Env:PUBLISH_CHURN_PROFILE -ErrorAction SilentlyContinue
    Remove-Item Env:PUBLISH_DATA_FUZZ_PROFILE -ErrorAction SilentlyContinue
    Remove-Item Env:PUBLISH_CHURN_ITERATIONS -ErrorAction SilentlyContinue
    Remove-Item Env:PUBLISH_CHURN_INTERVAL_MS -ErrorAction SilentlyContinue
    Remove-Item Env:PUBLISH_CHURN_EXTRA_AUDIO -ErrorAction SilentlyContinue
    Remove-Item Env:PUBLISH_DURATION_MS -ErrorAction SilentlyContinue
    Remove-Item Env:PUBLISH_STARTUP_WAIT_MS -ErrorAction SilentlyContinue
    Remove-Item Env:VIEW_PROBE_WAIT_MS -ErrorAction SilentlyContinue
    Remove-Item Env:PUBLISHER_RELOADS -ErrorAction SilentlyContinue
    Remove-Item Env:PUBLISHER_RELOAD_INTERVAL_MS -ErrorAction SilentlyContinue
    Remove-Item Env:PUBLISHER_RELOAD_STARTUP_WAIT_MS -ErrorAction SilentlyContinue
}
