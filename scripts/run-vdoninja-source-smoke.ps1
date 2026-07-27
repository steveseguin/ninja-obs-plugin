param(
    [ValidateSet("browser", "native")]
    [string]$Mode = "native",
    [string]$StreamId = "codexNativeSmoke5",
    [string]$Password = "false",
    [string]$RoomId = "",
    [string]$PushUrl = "https://vdo.ninja/?push=codexNativeSmoke5&password=false",
    [string]$ViewUrl = "https://vdo.ninja/?view=codexNativeSmoke5&password=false",
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
. (Join-Path $PSScriptRoot "test-alpha-validation-common.ps1")
$obsExePath = (Resolve-Path $ObsExe).Path
$obsWorkingDirPath = (Resolve-Path $ObsWorkingDirectory).Path
$installPrefixPath = (Resolve-Path $InstallPrefix).Path
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
$checkResultJson = Join-Path $repoRoot "artifacts\\obs-source-smoke-$Mode.result.json"
$sentinelDir = Join-Path $repoRoot "_obs-portable\\config\\obs-studio\\.sentinel"

foreach ($path in @($obsStdout, $obsStderr, $publisherOut, $publisherErr, $checkOut, $checkErr, $checkResultJson)) {
    if (Test-Path $path) {
        Remove-Item $path -Force
    }
}

function Get-RunningProcessByExecutablePath {
    param([string]$ExecutablePath)

    Get-CimInstance Win32_Process -Filter "Name = 'obs64.exe'" -ErrorAction SilentlyContinue |
        Where-Object { $_.ExecutablePath -and $_.ExecutablePath -ieq $ExecutablePath }
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
    throw "Refusing to start OBS smoke test because obs64 is already running from $obsExePath (PID(s): $existingIds)"
}

if (Test-Path $sentinelDir) {
    Get-ChildItem $sentinelDir -File -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue
}

if ($ResetPortableSceneCollection -and (Test-Path $portableScenesDir)) {
    Get-ChildItem $portableScenesDir -Filter "Untitled.json*" -File -ErrorAction SilentlyContinue |
        Remove-Item -Force -ErrorAction SilentlyContinue
}

Sync-PortableObsPluginPayload -RepoRoot $repoRoot -InstallPrefixPath $installPrefixPath

if (-not (Test-Path $obsWebSocketConfigPath)) {
    throw "obs-websocket config not found at $obsWebSocketConfigPath"
}

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
Remove-Item Env:OBS_PLUGINS_PATH -ErrorAction SilentlyContinue
$env:OBS_PLUGINS_DATA_PATH = $dataPath
$env:PATH = "$depsBin;$env:PATH"

$obsProc = $null
$publisherProc = $null
$checkProc = $null

try {
    $obsProc = Start-Process -FilePath $obsExePath -ArgumentList "--portable" `
        -WorkingDirectory $obsWorkingDirPath `
        -WindowStyle Hidden `
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

    $checkStartInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $checkStartInfo.FileName = "node"
    $checkStartInfo.Arguments = ($checkArgs | ForEach-Object { Quote-ProcessArgument $_ }) -join " "
    $checkStartInfo.WorkingDirectory = $repoRoot
    $checkStartInfo.UseShellExecute = $false
    $checkStartInfo.RedirectStandardOutput = $true
    $checkStartInfo.RedirectStandardError = $true
    $checkStartInfo.Environment["VDONINJA_SOURCE_CHECK_RESULT_JSON"] = $checkResultJson

    $checkProc = [System.Diagnostics.Process]::new()
    $checkProc.StartInfo = $checkStartInfo
    [void]$checkProc.Start()
    $checkTimedOutAfterResult = $false
    if (-not $checkProc.WaitForExit($CheckTimeoutSeconds * 1000)) {
        $timedOutPid = $checkProc.Id
        try {
            $checkProc.Kill()
        } catch {
        }
        [void]$checkProc.WaitForExit(10000)
        $checkStdout = ""
        $checkStderr = ""
        if ($checkProc.HasExited) {
            $checkStdout = $checkProc.StandardOutput.ReadToEnd()
            $checkStderr = $checkProc.StandardError.ReadToEnd()
            Set-Content -Path $checkOut -Value $checkStdout -Encoding UTF8
            Set-Content -Path $checkErr -Value $checkStderr -Encoding UTF8
        }
        if (Test-Path $checkResultJson) {
            $checkTimedOutAfterResult = $true
            $checkExit = 0
            if ([string]::IsNullOrWhiteSpace($checkStdout)) {
                $checkStdout = Get-Content $checkResultJson -Raw
                Set-Content -Path $checkOut -Value $checkStdout -Encoding UTF8
            }
        } else {
            throw "Timed out waiting for source check process PID $timedOutPid after $CheckTimeoutSeconds seconds"
        }
    } else {
        $checkStdout = $checkProc.StandardOutput.ReadToEnd()
        $checkStderr = $checkProc.StandardError.ReadToEnd()
        $checkExit = $checkProc.ExitCode
        Set-Content -Path $checkOut -Value $checkStdout -Encoding UTF8
        Set-Content -Path $checkErr -Value $checkStderr -Encoding UTF8
    }

    Write-Output "OBS_PID=$($obsProc.Id)"
    Write-Output "PUBLISHER_PID=$($publisherProc.Id)"
    Write-Output "CHECK_EXIT=$checkExit"
    Write-Output "CHECK_TIMEOUT_AFTER_RESULT=$checkTimedOutAfterResult"
    if (Test-Path $checkOut) {
        Get-Content $checkOut
    }
    if (Test-Path $checkErr) {
        Get-Content $checkErr
    }
    if ($checkExit -ne 0) {
        throw "Source check failed with exit code $checkExit"
    }

    $expectedPluginHash =
        (Get-FileHash (Join-Path $installPrefixPath "obs-plugins\64bit\obs-vdoninja.dll") -Algorithm SHA256).Hash
    $loadedPluginModules = @(
        (Get-Process -Id $obsProc.Id -ErrorAction Stop).Modules |
            Where-Object { $_.ModuleName -ieq "obs-vdoninja.dll" } |
            ForEach-Object {
                [pscustomobject]@{
                    path = $_.FileName
                    sha256 = (Get-FileHash $_.FileName -Algorithm SHA256).Hash
                }
            }
    )
    if ($loadedPluginModules.Count -ne 1) {
        $loadedPaths = ($loadedPluginModules | ForEach-Object { $_.path }) -join ", "
        throw "Portable OBS must load exactly one obs-vdoninja.dll; found: $loadedPaths"
    }
    if ($loadedPluginModules[0].sha256 -ne $expectedPluginHash) {
        throw "Portable OBS loaded a stale plugin DLL from $($loadedPluginModules[0].path)"
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
}
