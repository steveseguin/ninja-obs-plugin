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
    [int]$RapidIterations = 30,
    [int]$RapidWaitMs = 150,
    [int]$CheckTimeoutSeconds = 180,
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
$env:OBS_WEBSOCKET_REQUEST_TIMEOUT_MS = "30000"
$env:VDONINJA_STREAM_ID = $StreamId
$env:VDONINJA_PASSWORD = $Password
$env:VDONINJA_ROOM_ID = $RoomId
$env:VDONINJA_EDGE_RAPID_ITERATIONS = [string]$RapidIterations
$env:VDONINJA_EDGE_RAPID_WAIT_MS = [string]$RapidWaitMs
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
        if ($checkProc -and -not $checkProc.HasExited) {
            Stop-Process -Id $checkProc.Id -Force -ErrorAction SilentlyContinue
        }
        throw "Timed out waiting for edge stress process PID $($checkProc.Id) after $CheckTimeoutSeconds seconds"
    }
    $checkProc.WaitForExit()
    $checkProc.Refresh()
    $checkExitCode = $checkProc.ExitCode
    if ($null -eq $checkExitCode) {
        $checkStderrText = if (Test-Path $checkErr) { Get-Content $checkErr -Raw } else { "" }
        $checkStdoutText = if (Test-Path $checkOut) { Get-Content $checkOut -Raw } else { "" }
        $checkExitCode = if ($checkStderrText -match "(?m)^Error:" -or $checkStdoutText -notmatch '"ok"\s*:\s*true') {
            1
        } else {
            0
        }
    }
    $checkExitCode = [int]$checkExitCode

    $obsProc.Refresh()
    if ($obsProc.HasExited) {
        throw "OBS exited during edge stress with code $($obsProc.ExitCode)"
    }

    Write-Output "OBS_PID=$($obsProc.Id)"
    Write-Output "PUBLISHER_PID=$($publisherProc.Id)"
    Write-Output "CHECK_EXIT=$checkExitCode"
    Write-Output "RAPID_ITERATIONS=$RapidIterations"
    Write-Output "RAPID_WAIT_MS=$RapidWaitMs"
    Write-Output "OBS_STARTED_AT=$($obsStartedAt.ToString("o"))"
    if (Test-Path $checkOut) {
        Get-Content $checkOut
    }
    if (Test-Path $checkErr) {
        Get-Content $checkErr
    }
    if ($checkExitCode -ne 0) {
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
}
