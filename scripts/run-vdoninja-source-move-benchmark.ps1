param(
    [ValidateSet("baseline", "browser", "native")]
    [string]$Mode = "native",
    [string]$StreamId = "nnAiTYE",
    [string]$Password = "",
    [string]$RoomId = "",
    [string]$PushUrl = "https://vdo.ninja/?push=nnAiTYE",
    [string]$ViewUrl = "https://vdo.ninja/?view=nnAiTYE",
    [string]$ObsExe = ".\\_obs-portable\\bin\\64bit\\obs64.exe",
    [string]$ObsWorkingDirectory = ".\\_obs-portable\\bin\\64bit",
    [string]$InstallPrefix = ".\\install",
    [int]$ObsWebSocketPort = 4461,
    [int]$ObsStartupSeconds = 20,
    [int]$PublisherWarmupSeconds = 18,
    [int]$BenchmarkTimeoutSeconds = 120,
    [switch]$SkipPublisher,
    [string]$MovePhasesJson = "",
    [int]$MoveIterations = 60,
    [int]$MoveIntervalMs = 15
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
$obsStdout = Join-Path $repoRoot "artifacts\\obs-move-obs.stdout.log"
$obsStderr = Join-Path $repoRoot "artifacts\\obs-move-obs.stderr.log"
$publisherOut = Join-Path $repoRoot "artifacts\\obs-move-publisher.out.log"
$publisherErr = Join-Path $repoRoot "artifacts\\obs-move-publisher.err.log"
$benchmarkOut = Join-Path $repoRoot ("artifacts\\obs-move-$Mode.out.log")
$benchmarkErr = Join-Path $repoRoot ("artifacts\\obs-move-$Mode.err.log")
$originalObsWebSocketConfig = $null

foreach ($path in @($obsStdout, $obsStderr, $publisherOut, $publisherErr, $benchmarkOut, $benchmarkErr)) {
    if (Test-Path $path) {
        Remove-Item $path -Force
    }
}

function Get-RunningProcessByExecutablePath {
    param([string]$ExecutablePath)

    Get-CimInstance Win32_Process -Filter "Name = 'obs64.exe'" -ErrorAction SilentlyContinue |
        Where-Object { $_.ExecutablePath -and $_.ExecutablePath -ieq $ExecutablePath }
}

function Record-ObsCpuSample {
    if (-not $script:obsProc -or $script:obsProc.HasExited) {
        return
    }

    $proc = Get-Process -Id $script:obsProc.Id -ErrorAction SilentlyContinue
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

$existingObs = @(Get-RunningProcessByExecutablePath -ExecutablePath $obsExePath)
if ($existingObs.Count -gt 0) {
    $existingIds = ($existingObs | Select-Object -ExpandProperty ProcessId) -join ", "
    throw "Refusing to start OBS move benchmark because obs64 is already running from $obsExePath (PID(s): $existingIds)"
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
$env:VDONINJA_MOVE_ITERATIONS = [string]$MoveIterations
$env:VDONINJA_MOVE_INTERVAL_MS = [string]$MoveIntervalMs
if ($MovePhasesJson -ne "") {
    $env:VDONINJA_MOVE_PHASES_JSON = $MovePhasesJson
} else {
    Remove-Item Env:VDONINJA_MOVE_PHASES_JSON -ErrorAction SilentlyContinue
}

$script:obsProc = $null
$publisherProc = $null
$benchmarkProc = $null
$script:cpuSamples = New-Object System.Collections.Generic.List[double]
$script:lastCpuSampleAt = $null
$script:lastCpuSampleValue = $null

try {
    $script:obsProc = Start-Process -FilePath $obsExePath -ArgumentList "--portable" `
        -WorkingDirectory $obsWorkingDirPath `
        -RedirectStandardOutput $obsStdout `
        -RedirectStandardError $obsStderr `
        -PassThru

    Record-ObsCpuSample
    Sleep-WithCpuSamples -Seconds $ObsStartupSeconds

    if (-not $SkipPublisher -and $Mode -ne "baseline") {
        $publisherProc = Start-Process -FilePath "node" `
            -ArgumentList "scripts/playwright-vdo-publish-session.cjs", $PushUrl, $ViewUrl `
            -WorkingDirectory $repoRoot `
            -RedirectStandardOutput $publisherOut `
            -RedirectStandardError $publisherErr `
            -PassThru

        Sleep-WithCpuSamples -Seconds $PublisherWarmupSeconds
    }

    $benchmarkArgs = @("scripts/obs-websocket-vdoninja-move-benchmark.cjs", $Mode)
    if ($Mode -ne "baseline") {
        $benchmarkArgs += $StreamId
        if ($Password -ne "" -or $RoomId -ne "") {
            $benchmarkArgs += $Password
        }
        if ($RoomId -ne "") {
            $benchmarkArgs += $RoomId
        }
    }

    $benchmarkProc = Start-Process -FilePath "node" `
        -ArgumentList $benchmarkArgs `
        -WorkingDirectory $repoRoot `
        -RedirectStandardOutput $benchmarkOut `
        -RedirectStandardError $benchmarkErr `
        -PassThru

    for ($elapsed = 0; $elapsed -lt $BenchmarkTimeoutSeconds; $elapsed++) {
        if ($benchmarkProc.HasExited) {
            break
        }
        Start-Sleep -Seconds 1
        Record-ObsCpuSample
        $benchmarkProc.Refresh()
    }

    if (-not $benchmarkProc.HasExited) {
        throw "Timed out waiting for move benchmark after $BenchmarkTimeoutSeconds seconds"
    }

    $benchmarkProc.WaitForExit()
    $benchmarkProc.Refresh()
    $benchmarkExitCode = if ($null -eq $benchmarkProc.ExitCode) { 0 } else { [int]$benchmarkProc.ExitCode }

    if ($benchmarkExitCode -ne 0) {
        $stderrText = ""
        if (Test-Path $benchmarkErr) {
            $stderrText = (Get-Content $benchmarkErr -Raw)
        }
        throw "Move benchmark failed with exit $benchmarkExitCode. $stderrText"
    }

    Record-ObsCpuSample

    $avgCpu = 0
    $maxCpu = 0
    if ($script:cpuSamples.Count -gt 0) {
        $avgCpu = [math]::Round((($script:cpuSamples | Measure-Object -Average).Average), 2)
        $maxCpu = [math]::Round((($script:cpuSamples | Measure-Object -Maximum).Maximum), 2)
    }

    $benchmarkJson = $null
    if (Test-Path $benchmarkOut) {
        $raw = Get-Content $benchmarkOut -Raw
        if ($raw) {
            $benchmarkJson = $raw | ConvertFrom-Json
        }
    }

    Write-Output "OBS_PID=$($script:obsProc.Id)"
    Write-Output "PUBLISHER_PID=$($publisherProc.Id)"
    Write-Output "MODE=$Mode"
    Write-Output "OBS_CPU_SAMPLES=$($script:cpuSamples.Count)"
    Write-Output "OBS_CPU_AVG_PERCENT=$avgCpu"
    Write-Output "OBS_CPU_MAX_PERCENT=$maxCpu"
    if ($benchmarkJson) {
        $benchmarkJson | Add-Member -NotePropertyName "obsCpuAvgPercent" -NotePropertyValue $avgCpu -Force
        $benchmarkJson | Add-Member -NotePropertyName "obsCpuMaxPercent" -NotePropertyValue $maxCpu -Force
        $benchmarkJson | ConvertTo-Json -Depth 6
    }
} finally {
    if ($originalObsWebSocketConfig -ne $null) {
        Set-Content -Path $obsWebSocketConfigPath -Value $originalObsWebSocketConfig -Encoding UTF8
    }
    foreach ($proc in @($benchmarkProc, $publisherProc, $script:obsProc)) {
        if ($proc -and -not $proc.HasExited) {
            Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
        }
    }
    Remove-Item Env:VDONINJA_MOVE_PHASES_JSON -ErrorAction SilentlyContinue
    Remove-Item Env:VDONINJA_MOVE_ITERATIONS -ErrorAction SilentlyContinue
    Remove-Item Env:VDONINJA_MOVE_INTERVAL_MS -ErrorAction SilentlyContinue
}
