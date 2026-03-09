param(
    [ValidateSet("browser", "native")]
    [string]$Mode = "native",
    [string]$SourceAStreamId = "nnAiTYE",
    [string]$SourceAPassword = "",
    [string]$SourceARoomId = "",
    [string]$SourceALabel = "Simple",
    [string]$SourceBStreamId = "rKqKhd9",
    [string]$SourceBPassword = "1234",
    [string]$SourceBRoomId = "asfasdfasdff",
    [string]$SourceBLabel = "Room",
    [string]$ObsExe = ".\\_obs-portable\\bin\\64bit\\obs64.exe",
    [string]$ObsWorkingDirectory = ".\\_obs-portable\\bin\\64bit",
    [string]$InstallPrefix = ".\\install",
    [int]$ObsWebSocketPort = 4461,
    [int]$ObsStartupSeconds = 25,
    [int]$WaitMs = 20000,
    [int]$CheckTimeoutSeconds = 150,
    [int]$ObsWebSocketRequestTimeoutMs = 60000
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

$obsStdout = Join-Path $repoRoot "artifacts\\obs-source-concurrent-obs.stdout.log"
$obsStderr = Join-Path $repoRoot "artifacts\\obs-source-concurrent-obs.stderr.log"
$checkOut = Join-Path $repoRoot "artifacts\\obs-source-concurrent-check.out.log"
$checkErr = Join-Path $repoRoot "artifacts\\obs-source-concurrent-check.err.log"

foreach ($path in @($obsStdout, $obsStderr, $checkOut, $checkErr)) {
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
    throw "Refusing to start OBS concurrent smoke because obs64 is already running from $obsExePath (PID(s): $existingIds)"
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

$sourcesJson = @(
    @{
        label = $SourceALabel
        streamId = $SourceAStreamId
        password = $SourceAPassword
        roomId = $SourceARoomId
    },
    @{
        label = $SourceBLabel
        streamId = $SourceBStreamId
        password = $SourceBPassword
        roomId = $SourceBRoomId
    }
) | ConvertTo-Json -Compress

$originalObsWebSocketConfig = Get-Content $obsWebSocketConfigPath -Raw
$obsWebSocketConfig = $originalObsWebSocketConfig | ConvertFrom-Json
$obsWebSocketConfig.server_enabled = $true
$obsWebSocketConfig.auth_required = $false
$obsWebSocketConfig.server_port = $ObsWebSocketPort
$obsWebSocketConfig | ConvertTo-Json | Set-Content -Path $obsWebSocketConfigPath -Encoding UTF8

$env:OBS_WEBSOCKET_URL = "ws://127.0.0.1:$ObsWebSocketPort"
$env:OBS_WEBSOCKET_REQUEST_TIMEOUT_MS = [string]$ObsWebSocketRequestTimeoutMs
$env:OBS_PLUGINS_PATH = $pluginPath
$env:OBS_PLUGINS_DATA_PATH = $dataPath
$env:PATH = "$depsBin;$env:PATH"
$env:VDONINJA_CONCURRENT_SOURCES_JSON = $sourcesJson
$env:VDONINJA_WAIT_MS = [string]$WaitMs

$script:obsProc = $null
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

    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = "node"
    $psi.Arguments = "scripts/obs-websocket-vdoninja-concurrent-check.cjs $Mode"
    $psi.WorkingDirectory = $repoRoot
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true

    $checkProcess = [System.Diagnostics.Process]::Start($psi)
    if (-not $checkProcess.WaitForExit($CheckTimeoutSeconds * 1000)) {
        try {
            $checkProcess.Kill()
        } catch {
        }
        throw "Timed out waiting for concurrent source check after $CheckTimeoutSeconds seconds"
    }
    $checkStdout = $checkProcess.StandardOutput.ReadToEnd()
    $checkStderr = $checkProcess.StandardError.ReadToEnd()
    $checkProcess.WaitForExit()
    Set-Content -Path $checkOut -Value $checkStdout -Encoding UTF8
    Set-Content -Path $checkErr -Value $checkStderr -Encoding UTF8
    $checkExitCode = [int]$checkProcess.ExitCode

    Record-ObsCpuSample
    if ($checkExitCode -ne 0) {
        $stderrText = ""
        if (Test-Path $checkErr) {
            $stderrText = Get-Content $checkErr -Raw
        }
        throw "Concurrent source check failed with exit $checkExitCode. $stderrText"
    }

    $avgCpu = 0
    $maxCpu = 0
    if ($script:cpuSamples.Count -gt 0) {
        $avgCpu = [math]::Round((($script:cpuSamples | Measure-Object -Average).Average), 2)
        $maxCpu = [math]::Round((($script:cpuSamples | Measure-Object -Maximum).Maximum), 2)
    }

    Write-Output "OBS_PID=$($script:obsProc.Id)"
    Write-Output "MODE=$Mode"
    Write-Output "OBS_CPU_SAMPLES=$($script:cpuSamples.Count)"
    Write-Output "OBS_CPU_AVG_PERCENT=$avgCpu"
    Write-Output "OBS_CPU_MAX_PERCENT=$maxCpu"
    if (Test-Path $checkOut) {
        Get-Content $checkOut
    }
} finally {
    if ($originalObsWebSocketConfig) {
        Set-Content -Path $obsWebSocketConfigPath -Value $originalObsWebSocketConfig -Encoding UTF8
    }
    if ($script:obsProc -and -not $script:obsProc.HasExited) {
        Stop-Process -Id $script:obsProc.Id -Force -ErrorAction SilentlyContinue
    }
    Remove-Item Env:OBS_WEBSOCKET_REQUEST_TIMEOUT_MS -ErrorAction SilentlyContinue
    Remove-Item Env:VDONINJA_CONCURRENT_SOURCES_JSON -ErrorAction SilentlyContinue
    Remove-Item Env:VDONINJA_WAIT_MS -ErrorAction SilentlyContinue
}
