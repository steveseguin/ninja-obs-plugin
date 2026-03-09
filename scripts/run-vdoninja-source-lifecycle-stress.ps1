param(
    [ValidateSet("browser", "native")]
    [string]$Mode = "native",
    [string]$StreamId = "codexLifecycleStress1",
    [string]$Password = "",
    [string]$RoomId = "",
    [string]$PushUrl = "https://vdo.ninja/?push=codexLifecycleStress1",
    [string]$ViewUrl = "https://vdo.ninja/?view=codexLifecycleStress1",
    [string]$ObsExe = ".\\_obs-portable\\bin\\64bit\\obs64.exe",
    [string]$ObsWorkingDirectory = ".\\_obs-portable\\bin\\64bit",
    [string]$InstallPrefix = ".\\install",
    [int]$ObsWebSocketPort = 4459,
    [int]$ObsStartupSeconds = 25,
    [int]$PublisherWarmupSeconds = 22,
    [int]$Iterations = 5,
    [int]$IterationWaitMs = 12000,
    [int]$IterationTimeoutSeconds = 90,
    [int]$PauseBetweenIterationsMs = 750,
    [switch]$SkipPublisher
)

$ErrorActionPreference = "Stop"

if ($Iterations -lt 1) {
    throw "Iterations must be at least 1"
}

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

$obsStdout = Join-Path $repoRoot "artifacts\\obs-source-lifecycle-obs.stdout.log"
$obsStderr = Join-Path $repoRoot "artifacts\\obs-source-lifecycle-obs.stderr.log"
$publisherOut = Join-Path $repoRoot "artifacts\\obs-source-lifecycle-publisher.out.log"
$publisherErr = Join-Path $repoRoot "artifacts\\obs-source-lifecycle-publisher.err.log"

foreach ($path in @($obsStdout, $obsStderr, $publisherOut, $publisherErr)) {
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
    throw "Refusing to start OBS lifecycle stress because obs64 is already running from $obsExePath (PID(s): $existingIds)"
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

$script:obsProc = $null
$publisherProc = $null
$script:cpuSamples = New-Object System.Collections.Generic.List[double]
$script:lastCpuSampleAt = $null
$script:lastCpuSampleValue = $null
$iterationResults = New-Object System.Collections.Generic.List[object]

try {
    $script:obsProc = Start-Process -FilePath $obsExePath -ArgumentList "--portable" `
        -WorkingDirectory $obsWorkingDirPath `
        -RedirectStandardOutput $obsStdout `
        -RedirectStandardError $obsStderr `
        -PassThru

    Record-ObsCpuSample
    Sleep-WithCpuSamples -Seconds $ObsStartupSeconds

    if (-not $SkipPublisher) {
        $publisherProc = Start-Process -FilePath "node" `
            -ArgumentList "scripts/playwright-vdo-publish-session.cjs", $PushUrl, $ViewUrl `
            -WorkingDirectory $repoRoot `
            -RedirectStandardOutput $publisherOut `
            -RedirectStandardError $publisherErr `
            -PassThru

        Sleep-WithCpuSamples -Seconds $PublisherWarmupSeconds
    }

    for ($iteration = 1; $iteration -le $Iterations; $iteration++) {
        $checkOut = Join-Path $repoRoot ("artifacts\\obs-source-lifecycle-check-$iteration.out.log")
        $checkErr = Join-Path $repoRoot ("artifacts\\obs-source-lifecycle-check-$iteration.err.log")
        foreach ($path in @($checkOut, $checkErr)) {
            if (Test-Path $path) {
                Remove-Item $path -Force
            }
        }

        $env:VDONINJA_WAIT_MS = [string]$IterationWaitMs
        if ($iteration -lt $Iterations) {
            $env:VDONINJA_SKIP_CAPTURE = "1"
        } else {
            Remove-Item Env:VDONINJA_SKIP_CAPTURE -ErrorAction SilentlyContinue
        }
        Remove-Item Env:VDONINJA_KEEP_SCENE -ErrorAction SilentlyContinue

        $args = @("scripts/obs-websocket-vdoninja-source-check.cjs", $Mode, $StreamId)
        if ($Password -ne "" -or $RoomId -ne "") {
            $args += $Password
        }
        if ($RoomId -ne "") {
            $args += $RoomId
        }

        $startedAt = Get-Date
        $checkProc = Start-Process -FilePath "node" `
            -ArgumentList $args `
            -WorkingDirectory $repoRoot `
            -RedirectStandardOutput $checkOut `
            -RedirectStandardError $checkErr `
            -PassThru
        try {
            Wait-Process -Id $checkProc.Id -Timeout $IterationTimeoutSeconds -ErrorAction Stop
        } catch {
            if ($checkProc -and -not $checkProc.HasExited) {
                Stop-Process -Id $checkProc.Id -Force -ErrorAction SilentlyContinue
            }
            throw "Timed out waiting for lifecycle iteration $iteration after $IterationTimeoutSeconds seconds"
        }
        $checkProc.Refresh()
        $checkExitCode = if ($null -eq $checkProc.ExitCode) { 0 } else { [int]$checkProc.ExitCode }
        Record-ObsCpuSample

        if ($checkExitCode -ne 0) {
            $stderrText = ""
            if (Test-Path $checkErr) {
                $stderrText = (Get-Content $checkErr -Raw)
            }
            throw "Lifecycle iteration $iteration failed with exit $checkExitCode. $stderrText"
        }

        $durationMs = [math]::Round(((Get-Date) - $startedAt).TotalMilliseconds, 0)
        $resultJson = $null
        if (Test-Path $checkOut) {
            $raw = Get-Content $checkOut -Raw
            if ($raw) {
                $resultJson = $raw | ConvertFrom-Json
            }
        }

        [void]$iterationResults.Add([pscustomobject]@{
            iteration = $iteration
            durationMs = $durationMs
            screenshot = if ($resultJson -and $resultJson.screenshot) { $resultJson.screenshot.outputPath } else { $null }
        })

        if ($iteration -lt $Iterations -and $PauseBetweenIterationsMs -gt 0) {
            Start-Sleep -Milliseconds $PauseBetweenIterationsMs
            Record-ObsCpuSample
        }
    }

    Record-ObsCpuSample
    $avgCpu = 0
    $maxCpu = 0
    if ($script:cpuSamples.Count -gt 0) {
        $avgCpu = [math]::Round((($script:cpuSamples | Measure-Object -Average).Average), 2)
        $maxCpu = [math]::Round((($script:cpuSamples | Measure-Object -Maximum).Maximum), 2)
    }

    $avgIterationMs = [math]::Round((($iterationResults | Measure-Object -Property durationMs -Average).Average), 0)
    $maxIterationMs = [math]::Round((($iterationResults | Measure-Object -Property durationMs -Maximum).Maximum), 0)
    $finalResult = $iterationResults | Select-Object -Last 1

    Write-Output "OBS_PID=$($script:obsProc.Id)"
    Write-Output "PUBLISHER_PID=$($publisherProc.Id)"
    Write-Output "ITERATIONS=$Iterations"
    Write-Output "MODE=$Mode"
    Write-Output "STREAM_ID=$StreamId"
    Write-Output "OBS_CPU_SAMPLES=$($script:cpuSamples.Count)"
    Write-Output "OBS_CPU_AVG_PERCENT=$avgCpu"
    Write-Output "OBS_CPU_MAX_PERCENT=$maxCpu"
    Write-Output "ITERATION_AVG_MS=$avgIterationMs"
    Write-Output "ITERATION_MAX_MS=$maxIterationMs"
    if ($finalResult -and $finalResult.screenshot) {
        Write-Output "FINAL_SCREENSHOT=$($finalResult.screenshot)"
    }

    $iterationResults | ConvertTo-Json -Depth 4
} finally {
    if ($originalObsWebSocketConfig) {
        Set-Content -Path $obsWebSocketConfigPath -Value $originalObsWebSocketConfig -Encoding UTF8
    }
    foreach ($proc in @($publisherProc, $script:obsProc)) {
        if ($proc -and -not $proc.HasExited) {
            Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
        }
    }
    Remove-Item Env:VDONINJA_WAIT_MS -ErrorAction SilentlyContinue
    Remove-Item Env:VDONINJA_SKIP_CAPTURE -ErrorAction SilentlyContinue
    Remove-Item Env:VDONINJA_KEEP_SCENE -ErrorAction SilentlyContinue
}
