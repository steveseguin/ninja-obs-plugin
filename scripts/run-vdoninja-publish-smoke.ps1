param(
    [string]$StreamId = "codexObsPublish1",
    [string]$Password = "123",
    [string]$RoomId = "1",
    [string]$ObsExe = ".\_obs-portable\bin\64bit\obs64.exe",
    [string]$ObsWorkingDirectory = ".\_obs-portable\bin\64bit",
    [string]$InstallPrefix = ".\install-obs32",
    [int]$ObsWebSocketPort = 4456,
    [int]$ObsStartupSeconds = 18,
    [int]$CheckTimeoutSeconds = 150
)

$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
. (Join-Path $PSScriptRoot "test-alpha-validation-common.ps1")

$obsExePath = (Resolve-Path $ObsExe).Path
$obsWorkingDirPath = (Resolve-Path $ObsWorkingDirectory).Path
$installPrefixPath = (Resolve-Path $InstallPrefix).Path
$pluginPath = (Resolve-Path (Join-Path $installPrefixPath "obs-plugins\64bit")).Path
$dataPath = (Resolve-Path (Join-Path $installPrefixPath "data\obs-plugins")).Path
$depsBin = "C:\Users\steve\Code\obs-build-dependencies\windows-deps-2023-06-01-x64\bin"
$obsWebSocketConfigPath = Join-Path $repoRoot "_obs-portable\config\obs-studio\plugin_config\obs-websocket\config.json"
$obsStdout = Join-Path $repoRoot "artifacts\obs-publish-smoke-obs.stdout.log"
$obsStderr = Join-Path $repoRoot "artifacts\obs-publish-smoke-obs.stderr.log"
$checkOut = Join-Path $repoRoot "artifacts\obs-publish-smoke.out.json"
$checkErr = Join-Path $repoRoot "artifacts\obs-publish-smoke.err.log"
$sentinelDir = Join-Path $repoRoot "_obs-portable\config\obs-studio\.sentinel"

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

$existingObs = @(Get-RunningProcessByExecutablePath -ExecutablePath $obsExePath)
if ($existingObs.Count -gt 0) {
    $existingIds = ($existingObs | Select-Object -ExpandProperty ProcessId) -join ", "
    throw "Refusing to start OBS publish smoke because obs64 is already running from $obsExePath (PID(s): $existingIds)"
}

if (-not (Test-Path $obsWebSocketConfigPath)) {
    throw "obs-websocket config not found at $obsWebSocketConfigPath"
}

if (Test-Path $sentinelDir) {
    Get-ChildItem $sentinelDir -File -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue
}

Sync-PortableObsPluginPayload -RepoRoot $repoRoot -InstallPrefixPath $installPrefixPath

$originalObsWebSocketConfig = Get-Content $obsWebSocketConfigPath -Raw
$obsWebSocketConfig = $originalObsWebSocketConfig | ConvertFrom-Json
$obsWebSocketConfig.server_enabled = $true
$obsWebSocketConfig.auth_required = $false
$obsWebSocketConfig.server_port = $ObsWebSocketPort
$obsWebSocketConfig | ConvertTo-Json | Set-Content -Path $obsWebSocketConfigPath -Encoding UTF8

$previousObsWebSocketUrl = $env:OBS_WEBSOCKET_URL
$previousObsPluginsPath = $env:OBS_PLUGINS_PATH
$previousObsPluginsDataPath = $env:OBS_PLUGINS_DATA_PATH
$previousPath = $env:PATH

$env:OBS_WEBSOCKET_URL = "ws://127.0.0.1:$ObsWebSocketPort"
$env:OBS_PLUGINS_PATH = $pluginPath
$env:OBS_PLUGINS_DATA_PATH = $dataPath
$env:PATH = "$depsBin;$env:PATH"

$obsProc = $null
$checkProc = $null

try {
    $obsProc = Start-Process -FilePath $obsExePath -ArgumentList "--portable", "--disable-shutdown-check", "--disable-updater" `
        -WorkingDirectory $obsWorkingDirPath `
        -RedirectStandardOutput $obsStdout `
        -RedirectStandardError $obsStderr `
        -PassThru

    Start-Sleep -Seconds $ObsStartupSeconds

    $checkStartInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $checkStartInfo.FileName = "node"
    $checkStartInfo.Arguments = "scripts/obs-websocket-vdoninja-publish-check.cjs `"$StreamId`" `"$Password`" `"$RoomId`""
    $checkStartInfo.WorkingDirectory = $repoRoot
    $checkStartInfo.UseShellExecute = $false
    $checkStartInfo.RedirectStandardOutput = $true
    $checkStartInfo.RedirectStandardError = $true

    $checkProc = [System.Diagnostics.Process]::new()
    $checkProc.StartInfo = $checkStartInfo
    [void]$checkProc.Start()
    if (-not $checkProc.WaitForExit($CheckTimeoutSeconds * 1000)) {
        $checkProc.Kill()
        throw "Timed out waiting for OBS publish check PID $($checkProc.Id) after $CheckTimeoutSeconds seconds"
    }

    $checkStdout = $checkProc.StandardOutput.ReadToEnd()
    $checkStderr = $checkProc.StandardError.ReadToEnd()
    Set-Content -Path $checkOut -Value $checkStdout -Encoding UTF8
    Set-Content -Path $checkErr -Value $checkStderr -Encoding UTF8

    Write-Output "OBS_PID=$($obsProc.Id)"
    Write-Output "CHECK_EXIT=$($checkProc.ExitCode)"
    Get-Content $checkOut
    Get-Content $checkErr
    if ($checkProc.ExitCode -ne 0) {
        throw "OBS publish check failed with exit code $($checkProc.ExitCode)"
    }

    $obsLogPath = Get-LatestPortableObsLogPath -RepoRoot $repoRoot
    $obsProcess = Get-CimInstance Win32_Process -Filter "ProcessId = $($obsProc.Id)" -ErrorAction SilentlyContinue
    [pscustomobject]@{
        ok = $true
        streamId = $StreamId
        password = $Password
        roomId = $RoomId
        obsLog = $obsLogPath
        obsProcessPath = if ($obsProcess) { $obsProcess.ExecutablePath } else { $obsExePath }
        pluginDll = Join-Path $repoRoot "_obs-portable\config\obs-studio\plugins\obs-vdoninja\bin\64bit\obs-vdoninja.dll"
        report = $checkOut
        stderr = $checkErr
    } | ConvertTo-Json -Depth 4
}
finally {
    Set-Content -Path $obsWebSocketConfigPath -Value $originalObsWebSocketConfig -Encoding UTF8

    if ($null -ne $previousObsWebSocketUrl) {
        $env:OBS_WEBSOCKET_URL = $previousObsWebSocketUrl
    } else {
        Remove-Item Env:OBS_WEBSOCKET_URL -ErrorAction SilentlyContinue
    }
    if ($null -ne $previousObsPluginsPath) {
        $env:OBS_PLUGINS_PATH = $previousObsPluginsPath
    } else {
        Remove-Item Env:OBS_PLUGINS_PATH -ErrorAction SilentlyContinue
    }
    if ($null -ne $previousObsPluginsDataPath) {
        $env:OBS_PLUGINS_DATA_PATH = $previousObsPluginsDataPath
    } else {
        Remove-Item Env:OBS_PLUGINS_DATA_PATH -ErrorAction SilentlyContinue
    }
    $env:PATH = $previousPath

    if ($checkProc -and -not $checkProc.HasExited) {
        Stop-Process -Id $checkProc.Id -Force -ErrorAction SilentlyContinue
    }
    if ($obsProc -and -not $obsProc.HasExited) {
        Stop-Process -Id $obsProc.Id -Force -ErrorAction SilentlyContinue
    }
}
