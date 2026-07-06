param(
    [string]$StreamId = "",
    [string]$GameCaptureExe = "",
    [string]$SpoutSender = "",
    [string]$InstallPrefix = ".\install",
    [int]$ObsWebSocketPort = 4464,
    [int]$GameCaptureWarmupSeconds = 14,
    [int]$GameCaptureDurationMs = 150000,
    [int]$ObsStartupSeconds = 22,
    [int]$ObsWaitMs = 50000,
    [int]$CheckTimeoutSeconds = 170,
    [int]$OutputWidth = 0,
    [int]$OutputHeight = 0,
    [int]$OutputFps = 0,
    [int]$BitrateKbps = 0,
    [switch]$ControlVTubeStudioWindow,
    [switch]$RequireVTubeStudioWindowControl,
    [string]$VTubeWindowSizes = "1280x720,1600x900,1920x1080",
    [int]$VTubeWindowInitialDelayMs = 6000,
    [int]$VTubeWindowStepWaitMs = 3500,
    [switch]$UseTestSpoutSender,
    [string]$TestSpoutSenderExe = "C:\Users\steve\Code\game-capture\native-qt\build-test\bin\spout_test_sender.exe",
    [string]$TestSpoutSenderName = "",
    [int]$TestSpoutWidth = 640,
    [int]$TestSpoutHeight = 360,
    [int]$TestSpoutFps = 30,
    [int]$TestSpoutResizeAfterMs = 0,
    [int]$TestSpoutResizeWidth = 960,
    [int]$TestSpoutResizeHeight = 540,
    [switch]$SkipAlphaPixelCheck,
    [switch]$AllowQueueDrops,
    [switch]$AllowNativeVideoTimeout
)

$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$artifactsRoot = Join-Path $repoRoot "artifacts"

function Find-GameCaptureExe {
    $distRoot = "C:\Users\steve\Code\game-capture\native-qt\dist"
    if (-not (Test-Path $distRoot)) {
        throw "Game Capture dist directory was not found at $distRoot"
    }

    $candidate = Get-ChildItem $distRoot -Directory -Filter "game-capture-*-win64" -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTimeUtc -Descending |
        ForEach-Object { Join-Path $_.FullName "game-capture.exe" } |
        Where-Object { Test-Path $_ } |
        Select-Object -First 1
    if (-not $candidate) {
        throw "No packaged Game Capture win64 executable found under $distRoot"
    }
    return (Resolve-Path $candidate).Path
}

function Get-LatestPortableObsLogPath {
    $logsDir = Join-Path $repoRoot "_obs-portable\config\obs-studio\logs"
    $latest = Get-ChildItem $logsDir -Filter *.txt -File -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTimeUtc -Descending |
        Select-Object -First 1
    if (-not $latest) {
        throw "No portable OBS log found in $logsDir"
    }
    return $latest.FullName
}

function ConvertFrom-FirstJsonObject {
    param([object[]]$Lines)

    $text = ($Lines | ForEach-Object { [string]$_ }) -join "`n"
    $start = $text.IndexOf("{")
    if ($start -lt 0) {
        return $null
    }

    $depth = 0
    $end = -1
    $inString = $false
    $escaped = $false
    for ($i = $start; $i -lt $text.Length; $i++) {
        $ch = $text[$i]
        if ($escaped) {
            $escaped = $false
            continue
        }
        if ($inString -and $ch -eq [char]92) {
            $escaped = $true
            continue
        }
        if ($ch -eq [char]34) {
            $inString = -not $inString
            continue
        }
        if (-not $inString) {
            if ($ch -eq "{") {
                $depth++
            } elseif ($ch -eq "}") {
                $depth--
                if ($depth -eq 0) {
                    $end = $i
                    break
                }
            }
        }
    }

    if ($end -lt $start) {
        return $null
    }

    $jsonText = $text.Substring($start, $end - $start + 1)
    return $jsonText | ConvertFrom-Json
}

function Wait-FileForPattern {
    param(
        [string]$Path,
        [string]$Pattern,
        [int]$TimeoutSeconds
    )

    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ((Get-Date) -lt $deadline) {
        if (Test-Path $Path) {
            $text = Get-Content $Path -Raw -ErrorAction SilentlyContinue
            if ($text -match $Pattern) {
                return $true
            }
        }
        Start-Sleep -Milliseconds 100
    }
    return $false
}

if ([string]::IsNullOrWhiteSpace($GameCaptureExe)) {
    $GameCaptureExe = Find-GameCaptureExe
} else {
    $GameCaptureExe = (Resolve-Path $GameCaptureExe).Path
}

if ([string]::IsNullOrWhiteSpace($StreamId)) {
    $StreamId = "codexSpoutSmoke" + ((Get-Date -Format "yyyyMMddHHmmss") -replace '[^A-Za-z0-9_]', '')
}

$runDir = Join-Path $artifactsRoot ("gamecapture-spout-smoke-" + (Get-Date -Format "yyyyMMdd-HHmmss"))
New-Item -ItemType Directory -Path $runDir -Force | Out-Null

$gameCaptureDir = Split-Path $GameCaptureExe -Parent
$gameCaptureOut = Join-Path $runDir "game-capture.out.log"
$gameCaptureErr = Join-Path $runDir "game-capture.err.log"
$diagnosticsPath = Join-Path $runDir "game-capture-diagnostics.json"
$discoveryPath = Join-Path $runDir "control.json"
$spoutSourcesPath = Join-Path $runDir "local-control-spout.json"
$smokeOut = Join-Path $runDir "source-smoke.out.log"
$summaryPath = Join-Path $runDir "summary.json"
$testSpoutOut = Join-Path $runDir "spout-test-sender.out.log"
$testSpoutErr = Join-Path $runDir "spout-test-sender.err.log"
$sourceSmokeScript = Join-Path $repoRoot "scripts\run-vdoninja-source-smoke.ps1"
$vtubeWindowChurnScript = Join-Path $repoRoot "scripts\vtube-studio-window-churn.ps1"
$vtubeWindowReportPath = Join-Path $runDir "vtube-window-churn.json"

if (-not (Test-Path $sourceSmokeScript)) {
    throw "OBS source smoke script not found at $sourceSmokeScript"
}
if ($ControlVTubeStudioWindow -and -not (Test-Path $vtubeWindowChurnScript)) {
    throw "VTube Studio window churn script not found at $vtubeWindowChurnScript"
}
if ($UseTestSpoutSender) {
    if (-not (Test-Path $TestSpoutSenderExe)) {
        throw "Test Spout sender was not found at $TestSpoutSenderExe"
    }
    if ([string]::IsNullOrWhiteSpace($TestSpoutSenderName)) {
        $TestSpoutSenderName = "GameCaptureE2EAlpha-" + ((Get-Date -Format "yyyyMMddHHmmss") -replace '[^A-Za-z0-9_]', '')
    }
    $SpoutSender = $TestSpoutSenderName
}

$gameCaptureArgs = @(
    "--headless",
    "--stream=$StreamId",
    "--password=false",
    "--duration-ms=$GameCaptureDurationMs",
    "--source=spout",
    "--video-codec=vp9",
    "--alpha-workflow",
    "--audio-source=none",
    "--local-control",
    "--local-control-port=47634",
    "--local-control-token=codex-spout-test",
    "--local-control-discovery=$discoveryPath",
    "--diagnostics-out=$diagnosticsPath"
)
if (-not [string]::IsNullOrWhiteSpace($SpoutSender)) {
    $gameCaptureArgs += "--spout-sender=$SpoutSender"
}
if ($OutputWidth -gt 0) {
    $gameCaptureArgs += "--width=$OutputWidth"
}
if ($OutputHeight -gt 0) {
    $gameCaptureArgs += "--height=$OutputHeight"
}
if ($OutputFps -gt 0) {
    $gameCaptureArgs += "--fps=$OutputFps"
}
if ($BitrateKbps -gt 0) {
    $gameCaptureArgs += "--bitrate-kbps=$BitrateKbps"
}

Write-Output "GAMECAPTURE_SPOUT_SMOKE_START=1"
Write-Output "RUN_DIR=$runDir"
Write-Output "STREAM_ID=$StreamId"
Write-Output "GAME_CAPTURE_EXE=$GameCaptureExe"

$gameCaptureProc = $null
$testSpoutProc = $null
$oldWait = $env:VDONINJA_WAIT_MS
$oldSkipCapture = $env:VDONINJA_SKIP_CAPTURE
$oldAlphaPixelCheck = $env:VDONINJA_ALPHA_PIXEL_CHECK
$oldAlphaMinBackground = $env:VDONINJA_ALPHA_MIN_BACKGROUND_RATIO
$oldAlphaMinForeground = $env:VDONINJA_ALPHA_MIN_FOREGROUND_RATIO
$oldAlphaMaxGreen = $env:VDONINJA_ALPHA_MAX_GREEN_RATIO
$oldDuringWaitCommand = $env:VDONINJA_DURING_WAIT_COMMAND
$oldRequirePerturb = $env:VDONINJA_REQUIRE_PERTURB_COMMAND
$oldPerturbTimeout = $env:VDONINJA_PERTURB_TIMEOUT_MS

try {
    if ($UseTestSpoutSender) {
        $testSpoutDurationMs = [Math]::Max($GameCaptureDurationMs + 60000, 60000)
        $testSpoutArgs = @(
            "--name=$TestSpoutSenderName",
            "--width=$TestSpoutWidth",
            "--height=$TestSpoutHeight",
            "--fps=$TestSpoutFps",
            "--duration-ms=$testSpoutDurationMs"
        )
        if ($TestSpoutResizeAfterMs -gt 0) {
            $testSpoutArgs += "--resize-after-ms=$TestSpoutResizeAfterMs"
            $testSpoutArgs += "--resize-width=$TestSpoutResizeWidth"
            $testSpoutArgs += "--resize-height=$TestSpoutResizeHeight"
        }

        $testSpoutExePath = (Resolve-Path $TestSpoutSenderExe).Path
        $testSpoutProc = Start-Process -FilePath $testSpoutExePath `
            -ArgumentList $testSpoutArgs `
            -WorkingDirectory (Split-Path $testSpoutExePath -Parent) `
            -RedirectStandardOutput $testSpoutOut `
            -RedirectStandardError $testSpoutErr `
            -WindowStyle Hidden `
            -PassThru
        Write-Output "TEST_SPOUT_SENDER_PID=$($testSpoutProc.Id)"
        if (-not (Wait-FileForPattern -Path $testSpoutOut -Pattern "SPOUT_TEST_SENDER_READY" -TimeoutSeconds 10)) {
            if ($testSpoutProc.HasExited) {
                throw "Test Spout sender exited early with code $($testSpoutProc.ExitCode)"
            }
            throw "Test Spout sender did not report ready; inspect $testSpoutOut"
        }
    }

    $gameCaptureProc = Start-Process -FilePath $GameCaptureExe `
        -ArgumentList $gameCaptureArgs `
        -WorkingDirectory $gameCaptureDir `
        -RedirectStandardOutput $gameCaptureOut `
        -RedirectStandardError $gameCaptureErr `
        -WindowStyle Hidden `
        -PassThru
    Write-Output "GAME_CAPTURE_PID=$($gameCaptureProc.Id)"

    Start-Sleep -Seconds $GameCaptureWarmupSeconds
    if ($gameCaptureProc.HasExited) {
        throw "Game Capture exited early with code $($gameCaptureProc.ExitCode)"
    }

    $spoutSources = $null
    $finalSpoutSources = $null
    if (Test-Path $discoveryPath) {
        $control = Get-Content $discoveryPath -Raw | ConvertFrom-Json
        $headers = @{ Authorization = "Bearer $($control.token)" }
        $spoutSources = Invoke-RestMethod "$($control.base_url)/sources/spout" -Headers $headers
        $spoutSources | ConvertTo-Json -Depth 8 | Set-Content $spoutSourcesPath -Encoding UTF8
    }

    $env:VDONINJA_WAIT_MS = [string]$ObsWaitMs
    if ($SkipAlphaPixelCheck) {
        $env:VDONINJA_ALPHA_PIXEL_CHECK = "0"
    } else {
        $env:VDONINJA_ALPHA_PIXEL_CHECK = "1"
        if (-not $env:VDONINJA_ALPHA_MIN_BACKGROUND_RATIO) {
            $env:VDONINJA_ALPHA_MIN_BACKGROUND_RATIO = "0.03"
        }
        if (-not $env:VDONINJA_ALPHA_MIN_FOREGROUND_RATIO) {
            $env:VDONINJA_ALPHA_MIN_FOREGROUND_RATIO = "0.01"
        }
        if (-not $env:VDONINJA_ALPHA_MAX_GREEN_RATIO) {
            $env:VDONINJA_ALPHA_MAX_GREEN_RATIO = "0.05"
        }
    }
    if ($ControlVTubeStudioWindow) {
        $env:VDONINJA_DURING_WAIT_COMMAND =
            'powershell -NoProfile -ExecutionPolicy Bypass -File "' +
            $vtubeWindowChurnScript +
            '" -OutputJson "' +
            $vtubeWindowReportPath +
            '" -Sizes "' +
            $VTubeWindowSizes +
            '" -InitialDelayMs ' +
            $VTubeWindowInitialDelayMs +
            ' -StepWaitMs ' +
            $VTubeWindowStepWaitMs
        $env:VDONINJA_REQUIRE_PERTURB_COMMAND = if ($RequireVTubeStudioWindowControl) { "1" } else { "0" }
        $env:VDONINJA_PERTURB_TIMEOUT_MS = [string]([Math]::Max($ObsWaitMs + 30000, 45000))
    } else {
        Remove-Item Env:VDONINJA_DURING_WAIT_COMMAND -ErrorAction SilentlyContinue
        Remove-Item Env:VDONINJA_REQUIRE_PERTURB_COMMAND -ErrorAction SilentlyContinue
        Remove-Item Env:VDONINJA_PERTURB_TIMEOUT_MS -ErrorAction SilentlyContinue
    }
    Remove-Item Env:VDONINJA_SKIP_CAPTURE -ErrorAction SilentlyContinue
    $previousErrorActionPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $smokeOutput = & powershell -NoProfile -ExecutionPolicy Bypass -File $sourceSmokeScript `
            -Mode native `
            -StreamId $StreamId `
            -Password false `
            -SkipPublisher `
            -InstallPrefix $InstallPrefix `
            -ObsWebSocketPort $ObsWebSocketPort `
            -ObsStartupSeconds $ObsStartupSeconds `
            -CheckTimeoutSeconds $CheckTimeoutSeconds 2>&1
        $smokeExit = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previousErrorActionPreference
    }
    $smokeOutput | Set-Content $smokeOut -Encoding UTF8
    $sourceCheck = ConvertFrom-FirstJsonObject -Lines $smokeOutput

    if (Test-Path $discoveryPath) {
        $control = Get-Content $discoveryPath -Raw | ConvertFrom-Json
        $headers = @{ Authorization = "Bearer $($control.token)" }
        $finalSpoutSources = Invoke-RestMethod "$($control.base_url)/sources/spout" -Headers $headers
        $finalSpoutSources | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $runDir "local-control-spout-final.json") -Encoding UTF8
    }

    Start-Sleep -Seconds 2
    $obsLogPath = Get-LatestPortableObsLogPath
    $obsLogCopy = Join-Path $runDir ("obs-" + (Split-Path $obsLogPath -Leaf))
    Copy-Item $obsLogPath $obsLogCopy -Force
    $obsLogText = Get-Content $obsLogPath -Raw
    $gameCaptureLogText = Get-Content $gameCaptureOut -Raw -ErrorAction SilentlyContinue
    $sourceScreenshot = if ($sourceCheck -and $sourceCheck.screenshot) {
        $sourceCheck.screenshot.outputPath
    } else {
        $null
    }
    $diagnostics = if (Test-Path $diagnosticsPath) {
        Get-Content $diagnosticsPath -Raw | ConvertFrom-Json
    } else {
        $null
    }
    $vtubeWindowChurn = if (Test-Path $vtubeWindowReportPath) {
        Get-Content $vtubeWindowReportPath -Raw | ConvertFrom-Json
    } else {
        $null
    }

    $alphaPixelCheckOk = if ($SkipAlphaPixelCheck) {
        $true
    } elseif ($sourceCheck -and $sourceCheck.alphaPixelCheck) {
        [bool]$sourceCheck.alphaPixelCheck.ok
    } else {
        $false
    }

    $summary = [ordered]@{
        ok = (
            $smokeExit -eq 0 -and
            $alphaPixelCheckOk -and
            $gameCaptureLogText -match "Found \d+ Spout2 senders" -and
            $gameCaptureLogText -match "VP9 alpha encoder active" -and
            $obsLogText -match "Native receiver alpha composition active" -and
            ($AllowQueueDrops -or $obsLogText -notmatch "Number of media packets dropped due to a full queue") -and
            ($AllowNativeVideoTimeout -or $obsLogText -notmatch "No native video packets")
        )
        streamId = $StreamId
        runDir = $runDir
        gameCaptureExe = $GameCaptureExe
        gameCaptureArgs = $gameCaptureArgs
        gameCaptureRunningDuringValidation = (-not $gameCaptureProc.HasExited)
        gameCaptureExitCode = if ($gameCaptureProc.HasExited) { $gameCaptureProc.ExitCode } else { $null }
        smokeExit = $smokeExit
        sourceCheck = $sourceCheck
        alphaPixelCheckEnabled = (-not [bool]$SkipAlphaPixelCheck)
        alphaPixelCheckOk = $alphaPixelCheckOk
        alphaPixelCheck = if ($sourceCheck) { $sourceCheck.alphaPixelCheck } else { $null }
        controlVTubeStudioWindow = [bool]$ControlVTubeStudioWindow
        requireVTubeStudioWindowControl = [bool]$RequireVTubeStudioWindowControl
        vtubeWindowChurn = $vtubeWindowChurn
        testSpoutSender = [ordered]@{
            enabled = [bool]$UseTestSpoutSender
            exe = if ($UseTestSpoutSender) { (Resolve-Path $TestSpoutSenderExe).Path } else { $null }
            name = if ($UseTestSpoutSender) { $TestSpoutSenderName } else { $null }
            width = if ($UseTestSpoutSender) { $TestSpoutWidth } else { $null }
            height = if ($UseTestSpoutSender) { $TestSpoutHeight } else { $null }
            fps = if ($UseTestSpoutSender) { $TestSpoutFps } else { $null }
            resizeAfterMs = if ($UseTestSpoutSender) { $TestSpoutResizeAfterMs } else { $null }
            resizeWidth = if ($UseTestSpoutSender) { $TestSpoutResizeWidth } else { $null }
            resizeHeight = if ($UseTestSpoutSender) { $TestSpoutResizeHeight } else { $null }
            runningDuringValidation = if ($testSpoutProc) { -not $testSpoutProc.HasExited } else { $null }
            exitCode = if ($testSpoutProc -and $testSpoutProc.HasExited) { $testSpoutProc.ExitCode } else { $null }
            stdout = if (Test-Path $testSpoutOut) { $testSpoutOut } else { $null }
            stderr = if (Test-Path $testSpoutErr) { $testSpoutErr } else { $null }
        }
        spoutSources = $spoutSources
        finalSpoutSources = $finalSpoutSources
        gameCaptureDetectedSpout = ($gameCaptureLogText -match "Found \d+ Spout2 senders")
        gameCaptureVp9AlphaActive = ($gameCaptureLogText -match "VP9 alpha encoder active")
        gameCaptureMetrics = if ($diagnostics) { $diagnostics.metrics } else { $null }
        obsAlphaCompositionActive = ($obsLogText -match "Native receiver alpha composition active")
        obsNoNativeVideoTimeout = ($obsLogText -match "No native video packets")
        obsQueueDrops = ($obsLogText -match "Number of media packets dropped due to a full queue")
        obsLog = $obsLogCopy
        gameCaptureLog = $gameCaptureOut
        gameCaptureDiagnostics = if (Test-Path $diagnosticsPath) { $diagnosticsPath } else { $null }
        sourceSmokeLog = $smokeOut
        sourceScreenshot = $sourceScreenshot
    }
    $summary | ConvertTo-Json -Depth 10 | Set-Content $summaryPath -Encoding UTF8

    Write-Output "SUMMARY=$summaryPath"
    Get-Content $summaryPath

    if ($smokeExit -ne 0) {
        throw "OBS source smoke failed with exit code $smokeExit"
    }
    if (-not $summary.ok) {
        throw "Game Capture Spout smoke failed validation; inspect $summaryPath"
    }
} finally {
    if ($null -ne $oldWait) {
        $env:VDONINJA_WAIT_MS = $oldWait
    } else {
        Remove-Item Env:VDONINJA_WAIT_MS -ErrorAction SilentlyContinue
    }
    if ($null -ne $oldSkipCapture) {
        $env:VDONINJA_SKIP_CAPTURE = $oldSkipCapture
    } else {
        Remove-Item Env:VDONINJA_SKIP_CAPTURE -ErrorAction SilentlyContinue
    }
    if ($null -ne $oldAlphaPixelCheck) {
        $env:VDONINJA_ALPHA_PIXEL_CHECK = $oldAlphaPixelCheck
    } else {
        Remove-Item Env:VDONINJA_ALPHA_PIXEL_CHECK -ErrorAction SilentlyContinue
    }
    if ($null -ne $oldAlphaMinBackground) {
        $env:VDONINJA_ALPHA_MIN_BACKGROUND_RATIO = $oldAlphaMinBackground
    } else {
        Remove-Item Env:VDONINJA_ALPHA_MIN_BACKGROUND_RATIO -ErrorAction SilentlyContinue
    }
    if ($null -ne $oldAlphaMinForeground) {
        $env:VDONINJA_ALPHA_MIN_FOREGROUND_RATIO = $oldAlphaMinForeground
    } else {
        Remove-Item Env:VDONINJA_ALPHA_MIN_FOREGROUND_RATIO -ErrorAction SilentlyContinue
    }
    if ($null -ne $oldAlphaMaxGreen) {
        $env:VDONINJA_ALPHA_MAX_GREEN_RATIO = $oldAlphaMaxGreen
    } else {
        Remove-Item Env:VDONINJA_ALPHA_MAX_GREEN_RATIO -ErrorAction SilentlyContinue
    }
    if ($null -ne $oldDuringWaitCommand) {
        $env:VDONINJA_DURING_WAIT_COMMAND = $oldDuringWaitCommand
    } else {
        Remove-Item Env:VDONINJA_DURING_WAIT_COMMAND -ErrorAction SilentlyContinue
    }
    if ($null -ne $oldRequirePerturb) {
        $env:VDONINJA_REQUIRE_PERTURB_COMMAND = $oldRequirePerturb
    } else {
        Remove-Item Env:VDONINJA_REQUIRE_PERTURB_COMMAND -ErrorAction SilentlyContinue
    }
    if ($null -ne $oldPerturbTimeout) {
        $env:VDONINJA_PERTURB_TIMEOUT_MS = $oldPerturbTimeout
    } else {
        Remove-Item Env:VDONINJA_PERTURB_TIMEOUT_MS -ErrorAction SilentlyContinue
    }
    if ($gameCaptureProc -and -not $gameCaptureProc.HasExited) {
        Stop-Process -Id $gameCaptureProc.Id -Force -ErrorAction SilentlyContinue
        Wait-Process -Id $gameCaptureProc.Id -Timeout 10 -ErrorAction SilentlyContinue
    }
    if ($testSpoutProc -and -not $testSpoutProc.HasExited) {
        Stop-Process -Id $testSpoutProc.Id -Force -ErrorAction SilentlyContinue
        Wait-Process -Id $testSpoutProc.Id -Timeout 10 -ErrorAction SilentlyContinue
    }
}
