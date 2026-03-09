param(
    [ValidateSet("browser", "native")]
    [string]$Mode = "native",
    [string]$StreamId = "codexNativeSmoke5",
    [string]$Password = "",
    [string]$RoomId = "",
    [string]$PushUrl = "https://vdo.ninja/?push=codexNativeSmoke5",
    [string]$ViewUrl = "https://vdo.ninja/?view=codexNativeSmoke5",
    [string]$ObsExe = ".\\_obs-portable\\bin\\64bit\\obs64.exe",
    [string]$ObsWorkingDirectory = ".\\_obs-portable\\bin\\64bit",
    [string]$InstallPrefix = ".\\install",
    [int]$ObsWebSocketPort = 4456,
    [switch]$ResetPortableSceneCollection = $true,
    [switch]$SkipPublisher,
    [int]$ObsStartupSeconds = 18,
    [int]$PublisherWarmupSeconds = 22,
    [int]$CheckTimeoutSeconds = 90
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
$originalObsWebSocketConfig = $null

$obsStdout = Join-Path $repoRoot "artifacts\\obs-source-smoke-obs.stdout.log"
$obsStderr = Join-Path $repoRoot "artifacts\\obs-source-smoke-obs.stderr.log"
$publisherOut = Join-Path $repoRoot "artifacts\\obs-source-smoke-publisher.out.log"
$publisherErr = Join-Path $repoRoot "artifacts\\obs-source-smoke-publisher.err.log"
$checkOut = Join-Path $repoRoot "artifacts\\obs-source-smoke-$Mode.out.log"
$checkErr = Join-Path $repoRoot "artifacts\\obs-source-smoke-$Mode.err.log"
$sentinelDir = Join-Path $repoRoot "_obs-portable\\config\\obs-studio\\.sentinel"

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

$existingObs = @(Get-RunningProcessByExecutablePath -ExecutablePath $obsExePath)
if ($existingObs.Count -gt 0) {
    $existingIds = ($existingObs | Select-Object -ExpandProperty ProcessId) -join ", "
    throw "Refusing to start OBS smoke test because obs64 is already running from $obsExePath (PID(s): $existingIds)"
}

if (Test-Path $sentinelDir) {
    Get-ChildItem $sentinelDir -File -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue
}

if ($ResetPortableSceneCollection -and (Test-Path $portableScenesDir)) {
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

$obsProc = $null
$publisherProc = $null
$checkProc = $null

try {
    $obsProc = Start-Process -FilePath $obsExePath -ArgumentList "--portable" `
        -WorkingDirectory $obsWorkingDirPath `
        -RedirectStandardOutput $obsStdout `
        -RedirectStandardError $obsStderr `
        -PassThru

    Start-Sleep -Seconds $ObsStartupSeconds

    if (-not $SkipPublisher) {
        $publisherProc = Start-Process -FilePath "node" `
            -ArgumentList "scripts/playwright-vdo-publish-session.cjs", $PushUrl, $ViewUrl `
            -WorkingDirectory $repoRoot `
            -RedirectStandardOutput $publisherOut `
            -RedirectStandardError $publisherErr `
            -PassThru

        Start-Sleep -Seconds $PublisherWarmupSeconds
    }

    $checkArgs = @("scripts/obs-websocket-vdoninja-source-check.cjs", $Mode, $StreamId)
    if ($Password -ne "") {
        $checkArgs += $Password
    }
    if ($RoomId -ne "") {
        if ($Password -eq "") {
            $checkArgs += ""
        }
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
        throw "Timed out waiting for source check process PID $($checkProc.Id) after $CheckTimeoutSeconds seconds"
    }
    $checkExit = $checkProc.ExitCode

    Write-Output "OBS_PID=$($obsProc.Id)"
    Write-Output "PUBLISHER_PID=$($publisherProc.Id)"
    Write-Output "CHECK_EXIT=$checkExit"
    if (Test-Path $checkOut) {
        Get-Content $checkOut
    }
} finally {
    if ($originalObsWebSocketConfig -ne $null) {
        Set-Content -Path $obsWebSocketConfigPath -Value $originalObsWebSocketConfig -Encoding UTF8
    }
    if ($checkProc -and -not $checkProc.HasExited) {
        Stop-Process -Id $checkProc.Id -Force -ErrorAction SilentlyContinue
    }
    if ($publisherProc -and -not $publisherProc.HasExited) {
        Stop-Process -Id $publisherProc.Id -Force -ErrorAction SilentlyContinue
    }
    if ($obsProc -and -not $obsProc.HasExited) {
        Stop-Process -Id $obsProc.Id -Force -ErrorAction SilentlyContinue
    }
}
