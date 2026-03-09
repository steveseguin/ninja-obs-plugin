param(
    [ValidateSet("native", "browser")]
    [string]$Mode = "native",
    [string]$StreamId = "codexFaultRecover1",
    [string]$PushUrl = "https://vdo.ninja/?push=codexFaultRecover1",
    [string]$ViewUrl = "https://vdo.ninja/?view=codexFaultRecover1",
    [string]$ObsExe = ".\\_obs-portable\\bin\\64bit\\obs64.exe",
    [string]$ObsWorkingDirectory = ".\\_obs-portable\\bin\\64bit",
    [string]$InstallPrefix = ".\\install",
    [int]$ObsWebSocketPort = 4458,
    [int]$ObsStartupSeconds = 25,
    [int]$PublisherWarmupSeconds = 22,
    [int]$InitialCheckTimeoutSeconds = 150,
    [int]$DisconnectPauseSeconds = 12,
    [int]$RecoveryWarmupSeconds = 25,
    [int]$PostRecoverySeconds = 10
)

$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$obsExePath = (Resolve-Path $ObsExe).Path
$obsWorkingDirPath = (Resolve-Path $ObsWorkingDirectory).Path
$installPrefixPath = (Resolve-Path $InstallPrefix).Path
$pluginPath = (Resolve-Path (Join-Path $installPrefixPath "obs-plugins\\64bit")).Path
$dataPath = (Resolve-Path (Join-Path $installPrefixPath "data\\obs-plugins")).Path
$depsBin = "C:\\Users\\steve\\Code\\obs-build-dependencies\\windows-deps-2023-06-01-x64\\bin"
$obsWebSocketConfigPath = Join-Path $repoRoot "_obs-portable\\config\\obs-studio\\plugin_config\\obs-websocket\\config.json"
$portableScenesDir = Join-Path $repoRoot "_obs-portable\\config\\obs-studio\\basic\\scenes"
$sentinelDir = Join-Path $repoRoot "_obs-portable\\config\\obs-studio\\.sentinel"
$logDir = Join-Path $repoRoot "_obs-portable\\config\\obs-studio\\logs"

$obsStdout = Join-Path $repoRoot "artifacts\\obs-source-fault-obs.stdout.log"
$obsStderr = Join-Path $repoRoot "artifacts\\obs-source-fault-obs.stderr.log"
$publisherOut1 = Join-Path $repoRoot "artifacts\\obs-source-fault-publisher1.out.log"
$publisherErr1 = Join-Path $repoRoot "artifacts\\obs-source-fault-publisher1.err.log"
$publisherOut2 = Join-Path $repoRoot "artifacts\\obs-source-fault-publisher2.out.log"
$publisherErr2 = Join-Path $repoRoot "artifacts\\obs-source-fault-publisher2.err.log"
$checkOut = Join-Path $repoRoot "artifacts\\obs-source-fault-check.out.log"
$checkErr = Join-Path $repoRoot "artifacts\\obs-source-fault-check.err.log"

foreach ($path in @($obsStdout, $obsStderr, $publisherOut1, $publisherErr1, $publisherOut2, $publisherErr2, $checkOut, $checkErr)) {
    if (Test-Path $path) {
        Remove-Item $path -Force
    }
}

function Get-RunningProcessByExecutablePath {
    param([string]$ExecutablePath)

    Get-CimInstance Win32_Process -Filter "Name = 'obs64.exe'" -ErrorAction SilentlyContinue |
        Where-Object { $_.ExecutablePath -and $_.ExecutablePath -ieq $ExecutablePath }
}

$existingObs = @(Get-RunningProcessByExecutablePath -ExecutablePath $obsExePath)
if ($existingObs.Count -gt 0) {
    $existingIds = ($existingObs | Select-Object -ExpandProperty ProcessId) -join ", "
    throw "Refusing to start OBS fault smoke because obs64 is already running from $obsExePath (PID(s): $existingIds)"
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
$env:OBS_PLUGINS_PATH = $pluginPath
$env:OBS_PLUGINS_DATA_PATH = $dataPath
$env:PATH = "$depsBin;$env:PATH"
$env:VDONINJA_KEEP_SCENE = "1"
$env:VDONINJA_SKIP_CAPTURE = "1"
$env:VDONINJA_WAIT_MS = "5000"

$obsProc = $null
$publisherProc1 = $null
$publisherProc2 = $null
$script:cpuSamples = New-Object System.Collections.Generic.List[double]
$script:lastCpuSampleAt = $null
$script:lastCpuSampleValue = $null

function Get-LatestObsLogPath {
    if (-not (Test-Path $logDir)) {
        throw "OBS log directory not found at $logDir"
    }

    $latestLog = Get-ChildItem $logDir -File | Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
    if (-not $latestLog) {
        throw "No OBS log file found in $logDir"
    }

    return $latestLog.FullName
}

function Get-PatternCount {
    param(
        [string]$Path,
        [string]$Pattern
    )

    if (-not (Test-Path $Path)) {
        return 0
    }

    $matches = Select-String -Path $Path -Pattern $Pattern -ErrorAction SilentlyContinue
    if (-not $matches) {
        return 0
    }

    return @($matches).Count
}

function Wait-ForPatternCount {
    param(
        [string]$Path,
        [string]$Pattern,
        [int]$MinimumCount,
        [int]$TimeoutSeconds,
        [string]$Description
    )

    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ((Get-Date) -lt $deadline) {
        Record-ObsCpuSample
        if ((Get-PatternCount -Path $Path -Pattern $Pattern) -ge $MinimumCount) {
            Record-ObsCpuSample
            return
        }
        Start-Sleep -Seconds 1
    }

    throw "Timed out waiting for $Description in $Path"
}

function Record-ObsCpuSample {
    if (-not $obsProc -or $obsProc.HasExited) {
        return
    }

    $proc = Get-Process -Id $obsProc.Id -ErrorAction SilentlyContinue
    if (-not $proc) {
        return
    }

    $now = Get-Date
    $cpuValue = [double]$proc.CPU
    if ($script:lastCpuSampleAt -ne $null -and $script:lastCpuSampleValue -ne $null) {
        $elapsedSeconds = ($now - $script:lastCpuSampleAt).TotalSeconds
        if ($elapsedSeconds -gt 0) {
            $percent = (($cpuValue - $script:lastCpuSampleValue) / ($elapsedSeconds * [Environment]::ProcessorCount)) * 100.0
            if ($percent -ge 0) {
                [void]$script:cpuSamples.Add([math]::Round($percent, 2))
            }
        }
    }

    $script:lastCpuSampleAt = $now
    $script:lastCpuSampleValue = $cpuValue
}

function Sleep-WithCpuSamples {
    param([int]$Seconds)

    for ($i = 0; $i -lt $Seconds; $i++) {
        Start-Sleep -Seconds 1
        Record-ObsCpuSample
    }
}

try {
    $obsProc = Start-Process -FilePath $obsExePath -ArgumentList "--portable" `
        -WorkingDirectory $obsWorkingDirPath `
        -RedirectStandardOutput $obsStdout `
        -RedirectStandardError $obsStderr `
        -PassThru

    Record-ObsCpuSample
    Sleep-WithCpuSamples -Seconds $ObsStartupSeconds
    $latestLogPath = Get-LatestObsLogPath

    $initialConnectCount = Get-PatternCount -Path $latestLogPath -Pattern "Connected to publisher"

    $publisherProc1 = Start-Process -FilePath "node" `
        -ArgumentList "scripts/playwright-vdo-publish-session.cjs", $PushUrl, $ViewUrl `
        -WorkingDirectory $repoRoot `
        -RedirectStandardOutput $publisherOut1 `
        -RedirectStandardError $publisherErr1 `
        -PassThru

    Sleep-WithCpuSamples -Seconds $PublisherWarmupSeconds

    Push-Location $repoRoot
    try {
        cmd /c "node scripts\\obs-websocket-vdoninja-source-check.cjs $Mode $StreamId 1> `"$checkOut`" 2> `"$checkErr`""
        $checkExitCode = $LASTEXITCODE
    } finally {
        Pop-Location
    }
    if ($checkExitCode -ne 0) {
        throw "Initial source check failed with exit $checkExitCode"
    }

    Wait-ForPatternCount -Path $latestLogPath -Pattern "Connected to publisher" -MinimumCount ($initialConnectCount + 1) `
        -TimeoutSeconds 60 -Description "initial publisher connection"

    $disconnectCount = Get-PatternCount -Path $latestLogPath -Pattern "Disconnected from publisher"
    Stop-Process -Id $publisherProc1.Id -Force -ErrorAction SilentlyContinue
    Wait-Process -Id $publisherProc1.Id -Timeout 15 -ErrorAction SilentlyContinue
    Wait-ForPatternCount -Path $latestLogPath -Pattern "Disconnected from publisher" -MinimumCount ($disconnectCount + 1) `
        -TimeoutSeconds 60 -Description "publisher disconnect"
    Sleep-WithCpuSamples -Seconds $DisconnectPauseSeconds

    $reconnectCount = Get-PatternCount -Path $latestLogPath -Pattern "Connected to publisher"
    $publisherProc2 = Start-Process -FilePath "node" `
        -ArgumentList "scripts/playwright-vdo-publish-session.cjs", $PushUrl, $ViewUrl `
        -WorkingDirectory $repoRoot `
        -RedirectStandardOutput $publisherOut2 `
        -RedirectStandardError $publisherErr2 `
        -PassThru

    Wait-ForPatternCount -Path $latestLogPath -Pattern "Connected to publisher" -MinimumCount ($reconnectCount + 1) `
        -TimeoutSeconds $RecoveryWarmupSeconds -Description "publisher recovery"
    Sleep-WithCpuSamples -Seconds $PostRecoverySeconds

    $reconnectShot = Join-Path $repoRoot ("artifacts\\obs-source-$Mode-reconnect-" + [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds() + ".png")
    $captureResult = node scripts/obs-websocket-capture-scene.cjs "ws://127.0.0.1:$ObsWebSocketPort" $reconnectShot
    if ($LASTEXITCODE -ne 0) {
        throw "Reconnect scene capture failed with exit $LASTEXITCODE"
    }

    Record-ObsCpuSample
    $avgCpu = 0
    $maxCpu = 0
    if ($script:cpuSamples.Count -gt 0) {
        $avgCpu = [math]::Round((($script:cpuSamples | Measure-Object -Average).Average), 2)
        $maxCpu = [math]::Round((($script:cpuSamples | Measure-Object -Maximum).Maximum), 2)
    }

    Write-Output "OBS_PID=$($obsProc.Id)"
    Write-Output "INITIAL_PUBLISHER_PID=$($publisherProc1.Id)"
    Write-Output "RECOVERY_PUBLISHER_PID=$($publisherProc2.Id)"
    Write-Output "OBS_CPU_SAMPLES=$($script:cpuSamples.Count)"
    Write-Output "OBS_CPU_AVG_PERCENT=$avgCpu"
    Write-Output "OBS_CPU_MAX_PERCENT=$maxCpu"
    Write-Output "INITIAL_CHECK_LOG=$checkOut"
    Write-Output "RECONNECT_SCREENSHOT=$reconnectShot"
    Write-Output "LATEST_OBS_LOG=$latestLogPath"
    Get-Content $checkOut
    $captureResult
} finally {
    if ($originalObsWebSocketConfig) {
        Set-Content -Path $obsWebSocketConfigPath -Value $originalObsWebSocketConfig -Encoding UTF8
    }
    foreach ($proc in @($publisherProc2, $publisherProc1, $obsProc)) {
        if ($proc -and -not $proc.HasExited) {
            Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
        }
    }
}
