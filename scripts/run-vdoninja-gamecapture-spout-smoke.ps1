param(
    [string]$StreamId = "",
    [string]$RoomId = "",
    [switch]$RequestRoomQuality,
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
    [string]$VideoCodec = "vp9",
    [string]$VideoEncoder = "",
    [string]$FfmpegOptions = "",
    [switch]$NoAlphaWorkflow,
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
    [string]$TestSpoutPattern = "",
    [int]$TestSpoutResizeAfterMs = 0,
    [int]$TestSpoutResizeWidth = 960,
    [int]$TestSpoutResizeHeight = 540,
    [switch]$SkipAlphaPixelCheck,
    [long]$AlphaBackgroundColor = 0,
    [ValidateRange(0, 20)]
    [int]$AlphaSampleCount = 0,
    [ValidateRange(0, 5000)]
    [int]$AlphaSampleIntervalMs = 0,
    [ValidateSet("none", "source-toggle", "source-recreate", "command", "same-peer-ice-rebuild", "publisher-restart")]
    [string]$AlphaTransitionMode = "source-toggle",
    [string]$AlphaTransitionCommand = "",
    [string]$AlphaTransitionLabel = "transition",
    [ValidateRange(0, 20)]
    [int]$AlphaTransitionAfterSample = 0,
    [ValidateRange(100, 5000)]
    [int]$AlphaTransitionHoldMs = 350,
    [ValidateRange(1, 120)]
    [int]$AlphaTransitionTimeoutSeconds = 30,
    [string]$AlphaReceiverProbePath = "",
    [int]$AlphaReceiverProbeTimeoutSeconds = 90,
    [string]$ExpectedGameCaptureSha256 = "",
    [string]$ExpectedPluginSha256 = "",
    [string]$ExpectedSpoutSenderSha256 = "",
    [switch]$AllowQueueDrops,
    [switch]$AllowNativeVideoTimeout
)

$ErrorActionPreference = "Stop"

$expectedHqOnlyRuntimeExplanation =
    "Room Quality is unavailable with VP9; continuing HQ-only without changing the selected codec or alpha workflow."

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$artifactsRoot = Join-Path $repoRoot "artifacts"
$alphaHarnessContractsScript = Join-Path $PSScriptRoot "alpha-harness-contracts.ps1"
if (-not (Test-Path -LiteralPath $alphaHarnessContractsScript -PathType Leaf)) {
    throw "Production alpha harness contracts were not found: $alphaHarnessContractsScript"
}
. $alphaHarnessContractsScript

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

function Get-SmokeMetadataValue {
    param(
        [object[]]$Lines,
        [string]$Name
    )

    $prefix = "$Name="
    foreach ($lineObject in $Lines) {
        $line = [string]$lineObject
        if ($line.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)) {
            return $line.Substring($prefix.Length).Trim()
        }
    }
    return $null
}

function ConvertFrom-Base64Json {
    param([string]$Value, [string]$Label)

    if ([string]::IsNullOrWhiteSpace($Value)) {
        throw "$Label metadata is missing"
    }
    try {
        $json = [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($Value))
        return $json | ConvertFrom-Json
    } catch {
        throw "$Label metadata is not valid base64 JSON: $($_.Exception.Message)"
    }
}

function Get-FileBinding {
    param([string]$Path)

    if ([string]::IsNullOrWhiteSpace($Path) -or -not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return $null
    }
    $resolved = (Resolve-Path -LiteralPath $Path).Path
    return [ordered]@{
        path = $resolved
        sha256 = (Get-FileHash -LiteralPath $resolved -Algorithm SHA256).Hash.ToLowerInvariant()
    }
}

function Normalize-Sha256 {
    param([string]$Value, [string]$Label)

    if ([string]::IsNullOrWhiteSpace($Value)) {
        return ""
    }
    $normalized = $Value.Trim().ToLowerInvariant()
    if ($normalized -notmatch '^[0-9a-f]{64}$') {
        throw "$Label must be a 64-character SHA256 value"
    }
    return $normalized
}

if ([string]::IsNullOrWhiteSpace($GameCaptureExe)) {
    $GameCaptureExe = Find-GameCaptureExe
} else {
    $GameCaptureExe = (Resolve-Path $GameCaptureExe).Path
}

if ([string]::IsNullOrWhiteSpace($StreamId)) {
    $StreamId = "codexSpoutSmoke" + ((Get-Date -Format "yyyyMMddHHmmss") -replace '[^A-Za-z0-9_]', '')
}
if ($RequestRoomQuality -and [string]::IsNullOrWhiteSpace($RoomId)) {
    throw "-RequestRoomQuality requires a non-empty -RoomId"
}
if (-not [string]::IsNullOrWhiteSpace($AlphaReceiverProbePath)) {
    $AlphaReceiverProbePath = (Resolve-Path -LiteralPath $AlphaReceiverProbePath).Path
}
$gameCaptureSha256 = (Get-FileHash -LiteralPath $GameCaptureExe -Algorithm SHA256).Hash.ToLowerInvariant()
$ExpectedGameCaptureSha256 = Normalize-Sha256 -Value $ExpectedGameCaptureSha256 -Label "ExpectedGameCaptureSha256"
$ExpectedPluginSha256 = Normalize-Sha256 -Value $ExpectedPluginSha256 -Label "ExpectedPluginSha256"
$ExpectedSpoutSenderSha256 = Normalize-Sha256 -Value $ExpectedSpoutSenderSha256 -Label "ExpectedSpoutSenderSha256"
if ($ExpectedGameCaptureSha256 -and $gameCaptureSha256 -ne $ExpectedGameCaptureSha256) {
    throw "Packaged Game Capture hash does not match ExpectedGameCaptureSha256"
}
if ($AlphaTransitionMode -eq "publisher-restart" -and -not $ExpectedGameCaptureSha256) {
    throw "publisher-restart requires ExpectedGameCaptureSha256 to bind both publisher processes"
}
if ($AlphaTransitionMode -eq "publisher-restart" -and -not [string]::IsNullOrWhiteSpace($AlphaTransitionCommand)) {
    throw "publisher-restart uses the packaged restart helper and does not accept AlphaTransitionCommand overrides"
}

$runDir = Join-Path $artifactsRoot ("gamecapture-spout-smoke-" + (Get-Date -Format "yyyyMMdd-HHmmss"))
New-Item -ItemType Directory -Path $runDir -Force | Out-Null

$gameCaptureDir = Split-Path $GameCaptureExe -Parent
$gameCaptureOut = Join-Path $runDir "game-capture.out.log"
$gameCaptureErr = Join-Path $runDir "game-capture.err.log"
$diagnosticsPath = Join-Path $runDir "game-capture-diagnostics.json"
$liveDiagnosticsPath = Join-Path $runDir "game-capture-live-diagnostics.json"
$discoveryPath = Join-Path $runDir "control.json"
$spoutSourcesPath = Join-Path $runDir "local-control-spout.json"
$smokeOut = Join-Path $runDir "source-smoke.out.log"
$sourceCheckResultPath = Join-Path $runDir "source-check-result.json"
$summaryPath = Join-Path $runDir "summary.json"
$testSpoutOut = Join-Path $runDir "spout-test-sender.out.log"
$testSpoutErr = Join-Path $runDir "spout-test-sender.err.log"
$alphaReceiverProbeOut = Join-Path $runDir "room-alpha-receiver-probe.out.log"
$alphaReceiverProbeErr = Join-Path $runDir "room-alpha-receiver-probe.err.log"
$alphaReceiverProbeReportPath = Join-Path $runDir "room-alpha-receiver-probe.json"
$alphaEpochFile = Join-Path $runDir "alpha-visual-epoch.txt"
$sourceSmokeScript = Join-Path $repoRoot "scripts\run-vdoninja-source-smoke.ps1"
$sourceCheckScript = Join-Path $repoRoot "scripts\obs-websocket-vdoninja-source-check.cjs"
$driverScript = (Resolve-Path -LiteralPath $MyInvocation.MyCommand.Path).Path
$vtubeWindowChurnScript = Join-Path $repoRoot "scripts\vtube-studio-window-churn.ps1"
$vtubeWindowReportPath = Join-Path $runDir "vtube-window-churn.json"
$publisherRestartHelper = Join-Path $PSScriptRoot "restart-game-capture-for-alpha-e2e.ps1"
$publisherRestartSpecPath = Join-Path $runDir "publisher-restart-spec.json"
$publisherRestartResultPath = Join-Path $runDir "publisher-restart-result.json"
$publisherRestartOut = Join-Path $runDir "game-capture-restarted.out.log"
$publisherRestartErr = Join-Path $runDir "game-capture-restarted.err.log"

if (-not (Test-Path $sourceSmokeScript)) {
    throw "OBS source smoke script not found at $sourceSmokeScript"
}
if (-not (Test-Path $sourceCheckScript)) {
    throw "OBS source checker was not found at $sourceCheckScript"
}
if ($AlphaTransitionMode -eq "publisher-restart" -and
    -not (Test-Path -LiteralPath $publisherRestartHelper -PathType Leaf)) {
    throw "Publisher restart helper was not found at $publisherRestartHelper"
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
$testSpoutSenderSha256 = if ($UseTestSpoutSender) {
    (Get-FileHash -LiteralPath $TestSpoutSenderExe -Algorithm SHA256).Hash.ToLowerInvariant()
} else {
    $null
}
if ($ExpectedSpoutSenderSha256 -and $testSpoutSenderSha256 -ne $ExpectedSpoutSenderSha256) {
    throw "Spout fixture hash does not match ExpectedSpoutSenderSha256"
}

$gameCaptureArgs = @(
    "--headless",
    "--stream=$StreamId",
    "--password=false",
    "--duration-ms=$GameCaptureDurationMs",
    "--source=spout",
    "--video-codec=$VideoCodec",
    "--audio-source=none",
    "--local-control",
    "--local-control-port=47634",
    "--local-control-token=codex-spout-test",
    "--local-control-discovery=$discoveryPath",
    "--diagnostics-out=$diagnosticsPath"
)
if (-not [string]::IsNullOrWhiteSpace($RoomId)) {
    $gameCaptureArgs += "--room=$RoomId"
    $gameCaptureArgs += "--label=Room Alpha Native Receiver E2E"
}
if (-not $NoAlphaWorkflow) {
    $gameCaptureArgs += "--alpha-workflow"
}
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
if (-not [string]::IsNullOrWhiteSpace($VideoEncoder)) {
    $gameCaptureArgs += "--video-encoder=$VideoEncoder"
}
if (-not [string]::IsNullOrWhiteSpace($FfmpegOptions)) {
    $escapedFfmpegOptions = $FfmpegOptions.Replace('"', '\"')
    $gameCaptureArgs += "--ffmpeg-options=`"$escapedFfmpegOptions`""
}

Write-Output "GAMECAPTURE_SPOUT_SMOKE_START=1"
Write-Output "RUN_DIR=$runDir"
Write-Output "STREAM_ID=$StreamId"
Write-Output "ROOM_ID=$RoomId"
Write-Output "ROOM_QUALITY_REQUESTED=$([bool]$RequestRoomQuality)"
Write-Output "GAME_CAPTURE_EXE=$GameCaptureExe"

$gameCaptureProc = $null
$testSpoutProc = $null
$alphaReceiverProbeProc = $null
$oldWait = $env:VDONINJA_WAIT_MS
$oldSkipCapture = $env:VDONINJA_SKIP_CAPTURE
$oldAlphaPixelCheck = $env:VDONINJA_ALPHA_PIXEL_CHECK
$oldAlphaMinBackground = $env:VDONINJA_ALPHA_MIN_BACKGROUND_RATIO
$oldAlphaMinForeground = $env:VDONINJA_ALPHA_MIN_FOREGROUND_RATIO
$oldAlphaMaxDark = $env:VDONINJA_ALPHA_MAX_DARK_RATIO
$oldAlphaMaxGreen = $env:VDONINJA_ALPHA_MAX_GREEN_RATIO
$oldAlphaBackgroundColor = $env:VDONINJA_ALPHA_BACKGROUND_COLOR
$oldAlphaPattern = $env:VDONINJA_ALPHA_PATTERN
$oldAlphaSampleCount = $env:VDONINJA_ALPHA_SAMPLE_COUNT
$oldAlphaSampleInterval = $env:VDONINJA_ALPHA_SAMPLE_INTERVAL_MS
$oldAlphaSampleStep = $env:VDONINJA_ALPHA_SAMPLE_STEP
$oldAlphaTransitionMode = $env:VDONINJA_ALPHA_TRANSITION_MODE
$oldAlphaTransitionCommand = $env:VDONINJA_ALPHA_TRANSITION_COMMAND
$oldAlphaTransitionLabel = $env:VDONINJA_ALPHA_TRANSITION_LABEL
$oldAlphaTransitionAfterSample = $env:VDONINJA_ALPHA_TRANSITION_AFTER_SAMPLE
$oldAlphaTransitionHoldMs = $env:VDONINJA_ALPHA_TRANSITION_HOLD_MS
$oldAlphaTransitionTimeoutMs = $env:VDONINJA_ALPHA_TRANSITION_TIMEOUT_MS
$oldAlphaEpochFile = $env:VDONINJA_ALPHA_EPOCH_FILE
$oldDuringWaitCommand = $env:VDONINJA_DURING_WAIT_COMMAND
$oldRequirePerturb = $env:VDONINJA_REQUIRE_PERTURB_COMMAND
$oldPerturbTimeout = $env:VDONINJA_PERTURB_TIMEOUT_MS
$oldGameCaptureControlDiscovery = $env:VDONINJA_GAME_CAPTURE_CONTROL_DISCOVERY
$publisherRestartResult = $null
$effectiveAlphaTransitionCommand = $AlphaTransitionCommand
$artifactIdentityContract = [ordered]@{
    required = $false
    ok = $true
    plugin = $null
    packaged = $null
    evaluator = Get-FileBinding -Path $alphaHarnessContractsScript
}
$actualTestSpoutPattern = $null
$effectiveAlphaPattern = "generic"
$requiredAlphaSampleCount = 3
$effectiveAlphaSampleCount = 3
$requiredAlphaSampleIntervalMs = 100
$effectiveAlphaSampleIntervalMs = 75

try {
    if ($UseTestSpoutSender) {
        "pre" | Set-Content -LiteralPath $alphaEpochFile -Encoding ascii
        $testSpoutDurationMs = [Math]::Max($GameCaptureDurationMs + 60000, 60000)
        $testSpoutArgs = @(
            "--name=$TestSpoutSenderName",
            "--width=$TestSpoutWidth",
            "--height=$TestSpoutHeight",
            "--fps=$TestSpoutFps",
            "--duration-ms=$testSpoutDurationMs"
        )
        if (-not $SkipAlphaPixelCheck -and -not $NoAlphaWorkflow) {
            $testSpoutArgs += "--epoch-file=$alphaEpochFile"
        }
        if (-not [string]::IsNullOrWhiteSpace($TestSpoutPattern)) {
            $testSpoutArgs += "--pattern=$TestSpoutPattern"
        }
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
        $testSpoutReadyText = Get-Content -LiteralPath $testSpoutOut -Raw
        $readyMatch = [regex]::Match($testSpoutReadyText, 'SPOUT_TEST_SENDER_READY[^\r\n]*\bpattern=([^\s]+)')
        if (-not $readyMatch.Success) {
            throw "Test Spout sender READY line did not report its actual pattern; inspect $testSpoutOut"
        }
        $actualTestSpoutPattern = $readyMatch.Groups[1].Value
        $expectedTestSpoutPattern = if ([string]::IsNullOrWhiteSpace($TestSpoutPattern)) {
            "animated"
        } else {
            $TestSpoutPattern.ToLowerInvariant()
        }
        if ($actualTestSpoutPattern -ne $expectedTestSpoutPattern) {
            throw "Test Spout sender pattern mismatch: requested '$expectedTestSpoutPattern', running '$actualTestSpoutPattern'"
        }
    }

    $gameCaptureProc = Start-Process -FilePath $GameCaptureExe `
        -ArgumentList $gameCaptureArgs `
        -WorkingDirectory $gameCaptureDir `
        -RedirectStandardOutput $gameCaptureOut `
        -RedirectStandardError $gameCaptureErr `
        -WindowStyle Hidden `
        -PassThru
    $initialGameCapturePid = $gameCaptureProc.Id
    Write-Output "GAME_CAPTURE_PID=$($gameCaptureProc.Id)"

    Start-Sleep -Seconds $GameCaptureWarmupSeconds
    if ($gameCaptureProc.HasExited) {
        throw "Game Capture exited early with code $($gameCaptureProc.ExitCode)"
    }

    if (-not [string]::IsNullOrWhiteSpace($AlphaReceiverProbePath)) {
        $alphaReceiverProbeArgs = @(
            $AlphaReceiverProbePath,
            "--stream=$StreamId",
            "--room=$RoomId",
            "--password=false",
            "--output=$alphaReceiverProbeReportPath",
            "--timeout-ms=$([Math]::Max(10000, ($AlphaReceiverProbeTimeoutSeconds - 10) * 1000))"
        )
        $alphaReceiverProbeProc = Start-Process -FilePath "node" `
            -ArgumentList $alphaReceiverProbeArgs `
            -WorkingDirectory (Split-Path $AlphaReceiverProbePath -Parent) `
            -RedirectStandardOutput $alphaReceiverProbeOut `
            -RedirectStandardError $alphaReceiverProbeErr `
            -WindowStyle Hidden `
            -PassThru
        Write-Output "ALPHA_RECEIVER_PROBE_PID=$($alphaReceiverProbeProc.Id)"
    }

    $spoutSources = $null
    $finalSpoutSources = $null
    $control = $null
    if (Test-Path $discoveryPath) {
        $control = Get-Content $discoveryPath -Raw | ConvertFrom-Json
        $headers = @{ Authorization = "Bearer $($control.token)" }
        $spoutSources = Invoke-RestMethod "$($control.base_url)/sources/spout" -Headers $headers
        $spoutSources | ConvertTo-Json -Depth 8 | Set-Content $spoutSourcesPath -Encoding UTF8
    }
    if ($AlphaTransitionMode -eq "publisher-restart") {
        if (-not $control -or [int]$control.pid -ne $gameCaptureProc.Id) {
            throw "publisher-restart requires a discovery file bound to the initial packaged publisher PID"
        }
        $publisherRestartSpec = [ordered]@{
            schema = "game-capture-alpha-publisher-restart-spec-v1"
            runDir = $runDir
            executablePath = $GameCaptureExe
            expectedSha256 = $gameCaptureSha256
            arguments = @($gameCaptureArgs)
            workingDirectory = $gameCaptureDir
            oldPid = $gameCaptureProc.Id
            discoveryPath = $discoveryPath
            stdoutPath = $publisherRestartOut
            stderrPath = $publisherRestartErr
            resultPath = $publisherRestartResultPath
            discoveryTimeoutSeconds = [Math]::Max(15, $AlphaTransitionTimeoutSeconds)
        }
        $publisherRestartSpec | ConvertTo-Json -Depth 12 | Set-Content `
            -LiteralPath $publisherRestartSpecPath -Encoding UTF8
        $effectiveAlphaTransitionCommand =
            'powershell -NoProfile -ExecutionPolicy Bypass -File "' +
            $publisherRestartHelper +
            '" -SpecPath "' +
            $publisherRestartSpecPath +
            '"'
    }

    $effectiveAlphaPattern = if (-not [string]::IsNullOrWhiteSpace($actualTestSpoutPattern)) {
        $actualTestSpoutPattern.ToLowerInvariant()
    } elseif (-not [string]::IsNullOrWhiteSpace($TestSpoutPattern)) {
        $TestSpoutPattern.ToLowerInvariant()
    } else {
        "generic"
    }
    $requiredAlphaSampleCount = if ($effectiveAlphaPattern -eq "alpha-moving-edge") {
        10
    } elseif ($effectiveAlphaPattern -in @("alpha-checker", "alpha-opaque", "alpha-half")) {
        4
    } else {
        3
    }
    $requiredAlphaSampleIntervalMs = 100
    $effectiveAlphaSampleCount = if ($AlphaSampleCount -gt 0) {
        [Math]::Max($requiredAlphaSampleCount, $AlphaSampleCount)
    } else {
        $requiredAlphaSampleCount
    }
    $effectiveAlphaSampleIntervalMs = if ($AlphaSampleIntervalMs -gt 0) {
        [Math]::Min($requiredAlphaSampleIntervalMs, $AlphaSampleIntervalMs)
    } else {
        75
    }

    $env:VDONINJA_WAIT_MS = [string]$ObsWaitMs
    $env:VDONINJA_GAME_CAPTURE_CONTROL_DISCOVERY = $discoveryPath
    if ($OutputWidth -gt 0) {
        $env:VDONINJA_SOURCE_WIDTH = [string]$OutputWidth
        $env:VDONINJA_CANVAS_WIDTH = [string]$OutputWidth
    } else {
        Remove-Item Env:VDONINJA_SOURCE_WIDTH -ErrorAction SilentlyContinue
        Remove-Item Env:VDONINJA_CANVAS_WIDTH -ErrorAction SilentlyContinue
    }
    if ($OutputHeight -gt 0) {
        $env:VDONINJA_SOURCE_HEIGHT = [string]$OutputHeight
        $env:VDONINJA_CANVAS_HEIGHT = [string]$OutputHeight
    } else {
        Remove-Item Env:VDONINJA_SOURCE_HEIGHT -ErrorAction SilentlyContinue
        Remove-Item Env:VDONINJA_CANVAS_HEIGHT -ErrorAction SilentlyContinue
    }
    if ($SkipAlphaPixelCheck -or $NoAlphaWorkflow) {
        $env:VDONINJA_ALPHA_PIXEL_CHECK = "0"
        Remove-Item Env:VDONINJA_ALPHA_TRANSITION_COMMAND -ErrorAction SilentlyContinue
        Remove-Item Env:VDONINJA_ALPHA_TRANSITION_MODE -ErrorAction SilentlyContinue
        Remove-Item Env:VDONINJA_ALPHA_TRANSITION_LABEL -ErrorAction SilentlyContinue
        Remove-Item Env:VDONINJA_ALPHA_TRANSITION_AFTER_SAMPLE -ErrorAction SilentlyContinue
        Remove-Item Env:VDONINJA_ALPHA_TRANSITION_HOLD_MS -ErrorAction SilentlyContinue
        Remove-Item Env:VDONINJA_ALPHA_TRANSITION_TIMEOUT_MS -ErrorAction SilentlyContinue
        Remove-Item Env:VDONINJA_ALPHA_EPOCH_FILE -ErrorAction SilentlyContinue
    } else {
        $env:VDONINJA_ALPHA_PIXEL_CHECK = "1"
        $env:VDONINJA_ALPHA_PATTERN = $effectiveAlphaPattern
        $env:VDONINJA_ALPHA_SAMPLE_COUNT = [string]$effectiveAlphaSampleCount
        $env:VDONINJA_ALPHA_SAMPLE_INTERVAL_MS = [string]$effectiveAlphaSampleIntervalMs
        $env:VDONINJA_ALPHA_SAMPLE_STEP = "2"
        $env:VDONINJA_ALPHA_TRANSITION_MODE = $AlphaTransitionMode
        $env:VDONINJA_ALPHA_TRANSITION_LABEL = $AlphaTransitionLabel
        $env:VDONINJA_ALPHA_TRANSITION_HOLD_MS = [string]$AlphaTransitionHoldMs
        $env:VDONINJA_ALPHA_TRANSITION_TIMEOUT_MS = [string]($AlphaTransitionTimeoutSeconds * 1000)
        $env:VDONINJA_ALPHA_EPOCH_FILE = $alphaEpochFile
        if (-not [string]::IsNullOrWhiteSpace($effectiveAlphaTransitionCommand)) {
            $env:VDONINJA_ALPHA_TRANSITION_COMMAND = $effectiveAlphaTransitionCommand
        } else {
            Remove-Item Env:VDONINJA_ALPHA_TRANSITION_COMMAND -ErrorAction SilentlyContinue
        }
        if ($AlphaTransitionAfterSample -gt 0) {
            $env:VDONINJA_ALPHA_TRANSITION_AFTER_SAMPLE = [string]$AlphaTransitionAfterSample
        } else {
            Remove-Item Env:VDONINJA_ALPHA_TRANSITION_AFTER_SAMPLE -ErrorAction SilentlyContinue
        }
        $env:VDONINJA_ALPHA_MIN_BACKGROUND_RATIO = if ($effectiveAlphaPattern -eq "alpha-checker") { "0.35" } else { "0.03" }
        $env:VDONINJA_ALPHA_MIN_FOREGROUND_RATIO = if ($effectiveAlphaPattern -eq "alpha-checker") { "0.35" } else { "0.01" }
        $env:VDONINJA_ALPHA_MAX_DARK_RATIO = "0.002"
        $env:VDONINJA_ALPHA_MAX_GREEN_RATIO = "0.05"
        if ($AlphaBackgroundColor -gt 0) {
            $env:VDONINJA_ALPHA_BACKGROUND_COLOR = [string]$AlphaBackgroundColor
        } else {
            Remove-Item Env:VDONINJA_ALPHA_BACKGROUND_COLOR -ErrorAction SilentlyContinue
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
        $sourceSmokeArgs = @(
            "-Mode", "native",
            "-StreamId", $StreamId,
            "-Password", "false",
            "-SkipPublisher",
            "-InstallPrefix", $InstallPrefix,
            "-ObsWebSocketPort", [string]$ObsWebSocketPort,
            "-ObsStartupSeconds", [string]$ObsStartupSeconds,
            "-CheckTimeoutSeconds", [string]$CheckTimeoutSeconds
        )
        if (-not [string]::IsNullOrWhiteSpace($RoomId)) {
            $sourceSmokeArgs += @("-RoomId", $RoomId)
        }
        $smokeOutput = & powershell -NoProfile -ExecutionPolicy Bypass -File $sourceSmokeScript @sourceSmokeArgs 2>&1
        $smokeExit = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previousErrorActionPreference
    }
    $smokeOutput | Set-Content $smokeOut -Encoding UTF8
    $sharedSourceCheckResultPath = Join-Path $repoRoot "artifacts\obs-source-smoke-native.result.json"
    if (Test-Path -LiteralPath $sharedSourceCheckResultPath -PathType Leaf) {
        Copy-Item -LiteralPath $sharedSourceCheckResultPath -Destination $sourceCheckResultPath -Force
    }
    $sourceCheck = if (Test-Path -LiteralPath $sourceCheckResultPath -PathType Leaf) {
        Get-Content -LiteralPath $sourceCheckResultPath -Raw | ConvertFrom-Json
    } else {
        ConvertFrom-FirstJsonObject -Lines $smokeOutput
    }
    $reportedObsExePath = Get-SmokeMetadataValue -Lines $smokeOutput -Name "OBS_EXE_PATH"
    $reportedObsExeSha256 = Get-SmokeMetadataValue -Lines $smokeOutput -Name "OBS_EXE_SHA256"
    $reportedExpectedPluginPath = Get-SmokeMetadataValue -Lines $smokeOutput -Name "EXPECTED_PLUGIN_PATH"
    $reportedExpectedPluginSha256 = Get-SmokeMetadataValue -Lines $smokeOutput -Name "EXPECTED_PLUGIN_SHA256"
    $reportedLoadedPluginPath = Get-SmokeMetadataValue -Lines $smokeOutput -Name "LOADED_PLUGIN_PATH"
    $reportedLoadedPluginSha256 = Get-SmokeMetadataValue -Lines $smokeOutput -Name "LOADED_PLUGIN_SHA256"
    $reportedLoadedPluginModulesBase64 = Get-SmokeMetadataValue `
        -Lines $smokeOutput -Name "LOADED_PLUGIN_MODULES_JSON_BASE64"
    $loadedPluginModulesEvidence = @()
    if (-not [string]::IsNullOrWhiteSpace($reportedLoadedPluginModulesBase64)) {
        $loadedPluginModulesEvidence = @(
            ConvertFrom-Base64Json -Value $reportedLoadedPluginModulesBase64 `
                -Label "LOADED_PLUGIN_MODULES_JSON_BASE64"
        )
    }
    $portableObsBinding = Get-FileBinding -Path $reportedObsExePath
    $expectedPluginBinding = Get-FileBinding -Path $reportedExpectedPluginPath
    $loadedPluginBinding = Get-FileBinding -Path $reportedLoadedPluginPath
    $artifactIdentityContractRequired = [bool]$UseTestSpoutSender -and
        -not $SkipAlphaPixelCheck -and -not $NoAlphaWorkflow
    if ($artifactIdentityContractRequired) {
        $pluginContract = Test-AlphaLoadedPluginEvidence `
            -LoadedModules @($loadedPluginModulesEvidence) `
            -LoadedPlugin $loadedPluginBinding `
            -StagedPlugin $expectedPluginBinding `
            -ExpectedSha256 $ExpectedPluginSha256
        $packagedContract = Test-AlphaPackagedArtifactEvidence `
            -Publisher (Get-FileBinding -Path $GameCaptureExe) `
            -SpoutSender (Get-FileBinding -Path $TestSpoutSenderExe) `
            -ExpectedPublisherSha256 $ExpectedGameCaptureSha256 `
            -ExpectedSpoutSenderSha256 $ExpectedSpoutSenderSha256
        $artifactIdentityContract = [ordered]@{
            required = $true
            ok = ([bool]$pluginContract.ok -and [bool]$packagedContract.ok)
            plugin = $pluginContract
            packaged = $packagedContract
            evaluator = Get-FileBinding -Path $alphaHarnessContractsScript
        }
    }
    if ($smokeExit -eq 0) {
        if (-not $portableObsBinding -or -not $expectedPluginBinding -or -not $loadedPluginBinding) {
            throw "Successful OBS source smoke did not report resolvable OBS/plugin artifact bindings"
        }
        if ($portableObsBinding.sha256 -ne ([string]$reportedObsExeSha256).ToLowerInvariant()) {
            throw "Portable OBS hash changed or was reported incorrectly: $reportedObsExePath"
        }
        if ($expectedPluginBinding.sha256 -ne ([string]$reportedExpectedPluginSha256).ToLowerInvariant()) {
            throw "Expected plugin hash changed or was reported incorrectly: $reportedExpectedPluginPath"
        }
        if ($loadedPluginBinding.sha256 -ne ([string]$reportedLoadedPluginSha256).ToLowerInvariant()) {
            throw "Loaded plugin hash changed or was reported incorrectly: $reportedLoadedPluginPath"
        }
        if ($expectedPluginBinding.sha256 -ne $loadedPluginBinding.sha256) {
            throw "Portable OBS loaded plugin does not match the staged plugin payload"
        }
        if ($ExpectedPluginSha256 -and $loadedPluginBinding.sha256 -ne $ExpectedPluginSha256) {
            throw "Actually loaded OBS plugin hash does not match ExpectedPluginSha256"
        }
        if ($loadedPluginModulesEvidence.Count -ne 1) {
            throw "Successful OBS source smoke did not bind exactly one loaded plugin module"
        }
        if ($artifactIdentityContractRequired -and -not [bool]$artifactIdentityContract.ok) {
            throw "Production alpha artifact identity contract rejected the packaged workflow evidence"
        }
        if ([string]$loadedPluginModulesEvidence[0].path -ine $loadedPluginBinding.path -or
            ([string]$loadedPluginModulesEvidence[0].sha256).ToLowerInvariant() -ne $loadedPluginBinding.sha256) {
            throw "Loaded plugin module list does not match the singular loaded plugin binding"
        }
    }

    if ($AlphaTransitionMode -eq "publisher-restart") {
        if (-not (Test-Path -LiteralPath $publisherRestartResultPath -PathType Leaf)) {
            throw "Publisher restart did not write its structured result artifact"
        }
        $publisherRestartResult = Get-Content -LiteralPath $publisherRestartResultPath -Raw | ConvertFrom-Json
        if (-not [bool]$publisherRestartResult.ok -or
            [int]$publisherRestartResult.oldPublisher.pid -ne $gameCaptureProc.Id -or
            [int]$publisherRestartResult.newPublisher.pid -eq $gameCaptureProc.Id -or
            [string]$publisherRestartResult.executable.sha256 -ne $gameCaptureSha256 -or
            [string]$publisherRestartResult.oldPublisher.sha256 -ne $gameCaptureSha256 -or
            [string]$publisherRestartResult.newPublisher.sha256 -ne $gameCaptureSha256) {
            throw "Publisher restart result did not bind distinct old/new PIDs to the packaged executable hash"
        }
        $restartSpecBinding = Get-FileBinding -Path $publisherRestartSpecPath
        if (-not $restartSpecBinding -or
            [string]$publisherRestartResult.spec.sha256 -ne $restartSpecBinding.sha256) {
            throw "Publisher restart result is not bound to the exact restart specification"
        }
        $newPublisherProcess = Get-Process -Id ([int]$publisherRestartResult.newPublisher.pid) -ErrorAction Stop
        if ($newPublisherProcess.HasExited) {
            throw "Restarted packaged publisher exited before validation completed"
        }
        $gameCaptureProc = $newPublisherProcess
    }

    $alphaReceiverProbeExit = $null
    $alphaReceiverProbeTimedOut = $false
    if ($alphaReceiverProbeProc) {
        if (-not $alphaReceiverProbeProc.WaitForExit($AlphaReceiverProbeTimeoutSeconds * 1000)) {
            $alphaReceiverProbeTimedOut = $true
            Stop-Process -Id $alphaReceiverProbeProc.Id -Force -ErrorAction SilentlyContinue
            Wait-Process -Id $alphaReceiverProbeProc.Id -Timeout 10 -ErrorAction SilentlyContinue
        }
        $alphaReceiverProbeProc.Refresh()
        if ($alphaReceiverProbeProc.HasExited) {
            $alphaReceiverProbeExit = $alphaReceiverProbeProc.ExitCode
        }
    }
    $alphaReceiverProbe = if (Test-Path -LiteralPath $alphaReceiverProbeReportPath) {
        Get-Content -LiteralPath $alphaReceiverProbeReportPath -Raw | ConvertFrom-Json
    } else {
        $null
    }

    if (Test-Path $discoveryPath) {
        $control = Get-Content $discoveryPath -Raw | ConvertFrom-Json
        $headers = @{ Authorization = "Bearer $($control.token)" }
        $finalSpoutSources = Invoke-RestMethod "$($control.base_url)/sources/spout" -Headers $headers
        $finalSpoutSources | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $runDir "local-control-spout-final.json") -Encoding UTF8
        if (-not $gameCaptureProc.HasExited) {
            $liveDiagnostics = Invoke-RestMethod "$($control.base_url)/diagnostics" -Headers $headers
            $liveDiagnostics | ConvertTo-Json -Depth 20 | Set-Content $liveDiagnosticsPath -Encoding UTF8
        }
    }

    Start-Sleep -Seconds 2
    $obsLogPath = Get-LatestPortableObsLogPath
    $obsLogCopy = Join-Path $runDir ("obs-" + (Split-Path $obsLogPath -Leaf))
    Copy-Item $obsLogPath $obsLogCopy -Force
    $obsLogText = Get-Content $obsLogPath -Raw
    $gameCaptureLogPaths = @($gameCaptureOut)
    if (Test-Path -LiteralPath $publisherRestartOut -PathType Leaf) {
        $gameCaptureLogPaths += $publisherRestartOut
    }
    $gameCaptureLogText = ($gameCaptureLogPaths | ForEach-Object {
        Get-Content -LiteralPath $_ -Raw -ErrorAction SilentlyContinue
    }) -join "`n"
    $testSpoutLogText = if (Test-Path -LiteralPath $testSpoutOut -PathType Leaf) {
        Get-Content -LiteralPath $testSpoutOut -Raw -ErrorAction SilentlyContinue
    } else {
        ""
    }
    $fixturePostEpochObserved =
        -not $UseTestSpoutSender -or
        $SkipAlphaPixelCheck -or
        $NoAlphaWorkflow -or
        $AlphaTransitionMode -eq "none" -or
        $testSpoutLogText -match 'SPOUT_TEST_SENDER_EPOCH[^\r\n]*\bepoch=post\b'
    $sourceScreenshot = if ($sourceCheck -and $sourceCheck.screenshot) {
        $sourceCheck.screenshot.outputPath
    } else {
        $null
    }
    $preferredDiagnosticsPath = if ((Test-Path $liveDiagnosticsPath) -and ((Get-Item $liveDiagnosticsPath).Length -gt 0)) {
        $liveDiagnosticsPath
    } else {
        $diagnosticsPath
    }
    $diagnostics = if ((Test-Path $preferredDiagnosticsPath) -and ((Get-Item $preferredDiagnosticsPath).Length -gt 0)) {
        Get-Content $preferredDiagnosticsPath -Raw | ConvertFrom-Json
    } else {
        $null
    }
    $vtubeWindowChurn = if (Test-Path $vtubeWindowReportPath) {
        Get-Content $vtubeWindowReportPath -Raw | ConvertFrom-Json
    } else {
        $null
    }

    $alphaPixelCheckOk = if ($SkipAlphaPixelCheck -or $NoAlphaWorkflow) {
        $true
    } elseif ($sourceCheck -and $sourceCheck.alphaPixelCheck) {
        [bool]$sourceCheck.alphaPixelCheck.ok
    } else {
        $false
    }

    $transitionRequested = -not ($SkipAlphaPixelCheck -or $NoAlphaWorkflow) -and
        $AlphaTransitionMode -ne "none"
    $observedTransitionEvidence = if ($sourceCheck -and $sourceCheck.alphaPixelCheck -and
        $sourceCheck.alphaPixelCheck.transition -and
        $sourceCheck.alphaPixelCheck.transition.result) {
        $sourceCheck.alphaPixelCheck.transition.result.observedTransition
    } else {
        $null
    }
    $diagnosticsEvidenceOk = if ($SkipAlphaPixelCheck -or $NoAlphaWorkflow) {
        $true
    } else {
        $sourceCheck -and $sourceCheck.diagnosticsEvidence -and
            [int]$sourceCheck.diagnosticsEvidence.snapshotCount -gt 0
    }
    $oldTransportRetiredBeforeEpochChange = if (-not $transitionRequested) {
        $true
    } else {
        $observedTransitionEvidence -and [bool]$observedTransitionEvidence.ok -and
            [long]$observedTransitionEvidence.before.observedAtMs -le
                [long]$observedTransitionEvidence.retired.observedAtMs -and
            [long]$observedTransitionEvidence.retired.observedAtMs -le
                [long]$sourceCheck.alphaPixelCheck.transition.result.epochWrittenAtMs
    }
    $firstPostUsefulSample = if ($transitionRequested -and $sourceCheck -and
        $sourceCheck.alphaPixelCheck -and $sourceCheck.alphaPixelCheck.samples) {
        @($sourceCheck.alphaPixelCheck.samples | Where-Object {
            [string]$_.checkpoint -like "post:*" -and
            [string]$_.classification -ne "waiting-background"
        } | Select-Object -First 1)[0]
    } else {
        $null
    }
    $firstPostDiagnostics = if ($firstPostUsefulSample -and $firstPostUsefulSample.screenshot) {
        $firstPostUsefulSample.screenshot.diagnosticsAtCaptureStart
    } else {
        $null
    }
    $newTransportObservedBeforePostCapture = if (-not $transitionRequested) {
        $true
    } else {
        $firstPostUsefulSample -and $firstPostDiagnostics -and $observedTransitionEvidence -and
            [string]$firstPostUsefulSample.connectionEpoch -eq "post" -and
            [string]$firstPostDiagnostics.peer.transportKey -eq
                [string]$observedTransitionEvidence.after.peer.transportKey -and
            [int]$firstPostDiagnostics.publisherPid -eq
                [int]$observedTransitionEvidence.after.publisherPid -and
            [long]$observedTransitionEvidence.after.observedAtMs -le
                [long]$firstPostUsefulSample.screenshot.captureStartedAtMs
    }
    $transitionActionBound = if (-not $transitionRequested) {
        $true
    } elseif ($AlphaTransitionMode -eq "same-peer-ice-rebuild") {
        $transitionResult = $sourceCheck.alphaPixelCheck.transition.result
        $transitionResult.controlRequest -and
            [string]$transitionResult.controlRequest.request.command -eq "refresh_peer_transports" -and
            [bool]$transitionResult.controlRequest.response.ok -and
            [int]$transitionResult.controlRequest.response.accepted_peer_count -gt 0 -and
            [int]$transitionResult.controlRequest.publisherPid -eq
                [int]$observedTransitionEvidence.before.publisherPid
    } elseif ($AlphaTransitionMode -eq "publisher-restart") {
        $transitionResult = $sourceCheck.alphaPixelCheck.transition.result
        $publisherRestartResult -and [bool]$publisherRestartResult.ok -and
            [string]$transitionResult.commandResult.command -eq $effectiveAlphaTransitionCommand -and
            [int]$transitionResult.commandResult.exitCode -eq 0 -and
            [int]$publisherRestartResult.oldPublisher.pid -eq
                [int]$observedTransitionEvidence.before.publisherPid -and
            [int]$publisherRestartResult.newPublisher.pid -eq
                [int]$observedTransitionEvidence.after.publisherPid
    } elseif ($AlphaTransitionMode -eq "command") {
        [string]$sourceCheck.alphaPixelCheck.transition.result.commandResult.command -eq
            $effectiveAlphaTransitionCommand -and
            [int]$sourceCheck.alphaPixelCheck.transition.result.commandResult.exitCode -eq 0
    } else {
        [bool]$sourceCheck.alphaPixelCheck.transition.result.ok
    }
    $transitionClaimsOk = $diagnosticsEvidenceOk -and
        $oldTransportRetiredBeforeEpochChange -and
        $newTransportObservedBeforePostCapture -and
        $transitionActionBound

    $probeRequired = -not [string]::IsNullOrWhiteSpace($AlphaReceiverProbePath)
    $alphaReceiverProbeHarnessOk = if ($probeRequired) {
        $null -ne $alphaReceiverProbe -and
            -not $alphaReceiverProbeTimedOut -and
            [bool]$alphaReceiverProbe.harness.ok
    } else {
        $true
    }
    $alphaReceiverProbeProductOk = if ($probeRequired) {
        $null -ne $alphaReceiverProbe -and [bool]$alphaReceiverProbe.product.ok
    } else {
        $true
    }

    $alphaSampleHashes = @()
    $alphaSamplesUseRequestedRed = $true
    if ($sourceCheck -and $sourceCheck.alphaPixelCheck -and $sourceCheck.alphaPixelCheck.samples) {
        $alphaSampleHashes = @(
            $sourceCheck.alphaPixelCheck.samples |
                ForEach-Object { [string]$_.screenshot.sha256 } |
                Where-Object { -not [string]::IsNullOrWhiteSpace($_) } |
                Select-Object -Unique
        )
        if ($AlphaBackgroundColor -gt 0) {
            $alphaSamplesUseRequestedRed = @(
                $sourceCheck.alphaPixelCheck.samples |
                    Where-Object {
                        [int]$_.expectedBackground.r -ne 255 -or
                        [int]$_.expectedBackground.g -ne 0 -or
                        [int]$_.expectedBackground.b -ne 0
                    }
            ).Count -eq 0
        }
    } elseif ($AlphaBackgroundColor -gt 0 -and -not ($SkipAlphaPixelCheck -or $NoAlphaWorkflow)) {
        $alphaSamplesUseRequestedRed = $false
    }
    $movingTransparencyObserved =
        -not $UseTestSpoutSender -or
        $TestSpoutPattern -ne "alpha-moving-edge" -or
        (
            $sourceCheck -and
            $sourceCheck.alphaPixelCheck -and
            $sourceCheck.alphaPixelCheck.sequence -and
            [bool]$sourceCheck.alphaPixelCheck.sequence.ok -and
            [int]$sourceCheck.alphaPixelCheck.sequence.pre.usefulSampleCount -ge 10 -and
            [int]$sourceCheck.alphaPixelCheck.sequence.post.usefulSampleCount -ge 10
        )

    $publisherHashStable =
        (Get-FileHash -LiteralPath $GameCaptureExe -Algorithm SHA256).Hash.ToLowerInvariant() -eq $gameCaptureSha256
    $spoutSenderHashStable = -not $UseTestSpoutSender -or (
        (Get-FileHash -LiteralPath $TestSpoutSenderExe -Algorithm SHA256).Hash.ToLowerInvariant() -eq
            $testSpoutSenderSha256
    )
    $pluginModuleSetValid = $loadedPluginModulesEvidence.Count -eq 1 -and
        $loadedPluginBinding -and
        [string]$loadedPluginModulesEvidence[0].path -ieq $loadedPluginBinding.path -and
        ([string]$loadedPluginModulesEvidence[0].sha256).ToLowerInvariant() -eq
            $loadedPluginBinding.sha256
    $pluginHashStable = $pluginModuleSetValid -and $loadedPluginBinding -and (
        (Get-FileHash -LiteralPath $loadedPluginBinding.path -Algorithm SHA256).Hash.ToLowerInvariant() -eq
            $loadedPluginBinding.sha256
    )

    $roomRequested = -not [string]::IsNullOrWhiteSpace($RoomId)
    $escapedRoomId = [regex]::Escape($RoomId)
    $roomJoinLogged = -not $roomRequested -or $gameCaptureLogText -match "Joining room:\s*$escapedRoomId"
    $roomPeerInitLogged = -not $roomRequested -or $gameCaptureLogText -match "Peer init .*roomMode=1"
    $roomQualityEnabled = -not $RequestRoomQuality -or (
        $gameCaptureArgs -notcontains "--disable-room-lq" -and
        $gameCaptureLogText -match "Auto-starting .*room=$escapedRoomId .*roomModeLq=true"
    )
    $obsPrimaryTrackDecoded = $obsLogText -match "Native receiver decoded first video frame"
    $obsAlphaTrackDecoded = $obsLogText -match "Native receiver decoded first alpha frame"
    $obsPrimaryAndAlphaNegotiated =
        $obsLogText -match "video\(mid=video codecs=VP9" -and
        $obsLogText -match "video\(mid=video-alpha codecs=VP9" -and
        $obsLogText -match "Received VP9 alpha video track"
    $selectedCodecAuthorityVp9 = -not $roomRequested -or (
        $gameCaptureLogText -notmatch "Room Quality uses H\.264" -and
        $alphaReceiverProbe -and
        [bool]$alphaReceiverProbe.assertions.selectedCodecAuthorityVp9
    )
    $allRoomVideoPeersHq = -not $RequestRoomQuality -or (
        $gameCaptureLogText -match "Peer init .*roomMode=1 role=guest .*tier=hq" -and
        $gameCaptureLogText -notmatch "Peer init .*roomMode=1 .*tier=lq"
    )
    $hqOnlyRuntimeExplanationLogged = -not $RequestRoomQuality -or
        $gameCaptureLogText.Contains($expectedHqOnlyRuntimeExplanation)
    $roomAlphaProductOk = -not $roomRequested -or (
        $roomJoinLogged -and
        $roomPeerInitLogged -and
        $roomQualityEnabled -and
        $allRoomVideoPeersHq -and
        $hqOnlyRuntimeExplanationLogged -and
        $selectedCodecAuthorityVp9 -and
        $obsPrimaryAndAlphaNegotiated -and
        $obsPrimaryTrackDecoded -and
        $obsAlphaTrackDecoded -and
        $alphaReceiverProbeProductOk -and
        $alphaSamplesUseRequestedRed -and
        $movingTransparencyObserved
    )
    $harnessOk =
        $null -ne $sourceCheck -and
        $gameCaptureLogText -match "Found \d+ Spout2 senders" -and
        (-not $UseTestSpoutSender -or $actualTestSpoutPattern -eq $expectedTestSpoutPattern) -and
        $publisherHashStable -and
        $spoutSenderHashStable -and
        $pluginHashStable -and
        [bool]$artifactIdentityContract.ok -and
        $transitionClaimsOk -and
        $fixturePostEpochObserved -and
        $alphaReceiverProbeHarnessOk
    $productOk =
        $alphaPixelCheckOk -and
        $alphaSamplesUseRequestedRed -and
        $movingTransparencyObserved -and
        $fixturePostEpochObserved -and
        $alphaReceiverProbeProductOk -and
        ($NoAlphaWorkflow -or $gameCaptureLogText -match "VP9 alpha encoder active") -and
        ($NoAlphaWorkflow -or $obsLogText -match "Native receiver alpha composition active") -and
        ($AllowQueueDrops -or $obsLogText -notmatch "Number of media packets dropped due to a full queue") -and
        ($AllowNativeVideoTimeout -or $obsLogText -notmatch "No native video packets") -and
        $roomAlphaProductOk

    $alphaScreenshotPaths = @()
    if ($sourceCheck -and $sourceCheck.alphaPixelCheck -and $sourceCheck.alphaPixelCheck.samples) {
        $alphaScreenshotPaths = @(
            $sourceCheck.alphaPixelCheck.samples |
                ForEach-Object { @([string]$_.backgroundPngPath, [string]$_.finalPngPath) } |
                Where-Object { -not [string]::IsNullOrWhiteSpace($_) } |
                Select-Object -Unique
        )
    }
    $alphaScreenshotBindings = @(
        $alphaScreenshotPaths |
            ForEach-Object { Get-FileBinding -Path $_ } |
            Where-Object { $null -ne $_ }
    )
    $evidenceBindings = @(
        @(
            $gameCaptureOut,
            $gameCaptureErr,
            $preferredDiagnosticsPath,
            $obsLogCopy,
            $smokeOut,
            $testSpoutOut,
            $testSpoutErr,
            $alphaReceiverProbeReportPath,
            $alphaEpochFile,
            $discoveryPath,
            $publisherRestartSpecPath,
            $publisherRestartResultPath,
            $publisherRestartOut,
            $publisherRestartErr,
            $sourceCheckResultPath
        ) |
            ForEach-Object { Get-FileBinding -Path $_ } |
            Where-Object { $null -ne $_ }
    )
    $artifactBinding = [ordered]@{
        gameCapture = Get-FileBinding -Path $GameCaptureExe
        spoutSender = if ($UseTestSpoutSender) { Get-FileBinding -Path $TestSpoutSenderExe } else { $null }
        portableObs = $portableObsBinding
        stagedPlugin = $expectedPluginBinding
        loadedPlugin = $loadedPluginBinding
        loadedPluginModules = @($loadedPluginModulesEvidence)
        controlDiscovery = Get-FileBinding -Path $discoveryPath
        publisherRestart = [ordered]@{
            helper = Get-FileBinding -Path $publisherRestartHelper
            spec = Get-FileBinding -Path $publisherRestartSpecPath
            result = Get-FileBinding -Path $publisherRestartResultPath
        }
        sourceCheckResult = Get-FileBinding -Path $sourceCheckResultPath
        scripts = @(
            Get-FileBinding -Path $driverScript
            Get-FileBinding -Path $sourceSmokeScript
            Get-FileBinding -Path $sourceCheckScript
        )
        screenshots = $alphaScreenshotBindings
        evidence = $evidenceBindings
    }

    $summary = [ordered]@{
        ok = ($harnessOk -and $productOk)
        harnessOk = $harnessOk
        productOk = $productOk
        failureClass = if (-not $harnessOk) { "harness" } elseif (-not $productOk) { "product" } else { "none" }
        streamId = $StreamId
        roomId = $RoomId
        roomQualityRequested = [bool]$RequestRoomQuality
        runDir = $runDir
        gameCaptureExe = $GameCaptureExe
        gameCaptureSha256 = $gameCaptureSha256
        gameCaptureArgs = $gameCaptureArgs
        initialGameCapturePid = $initialGameCapturePid
        alphaWorkflow = (-not [bool]$NoAlphaWorkflow)
        gameCaptureRunningDuringValidation = (-not $gameCaptureProc.HasExited)
        gameCaptureExitCode = if ($gameCaptureProc.HasExited) { $gameCaptureProc.ExitCode } else { $null }
        smokeExit = $smokeExit
        sourceCheck = $sourceCheck
        alphaPixelCheckEnabled = (-not [bool]$SkipAlphaPixelCheck)
        alphaPixelCheckOk = $alphaPixelCheckOk
        alphaPixelCheck = if ($sourceCheck) { $sourceCheck.alphaPixelCheck } else { $null }
        diagnosticsEvidence = if ($sourceCheck) { $sourceCheck.diagnosticsEvidence } else { $null }
        validatedTransitionClaims = [ordered]@{
            ok = $transitionClaimsOk
            diagnosticsEvidencePresent = $diagnosticsEvidenceOk
            oldTransportRetiredBeforeVisualEpochChange = $oldTransportRetiredBeforeEpochChange
            newTransportObservedBeforeFirstPostCapture = $newTransportObservedBeforePostCapture
            exactTransitionActionBound = $transitionActionBound
            observedTransition = $observedTransitionEvidence
            firstPostDiagnostics = $firstPostDiagnostics
        }
        publisherRestart = [ordered]@{
            requested = ($AlphaTransitionMode -eq "publisher-restart")
            command = if ($AlphaTransitionMode -eq "publisher-restart") {
                $effectiveAlphaTransitionCommand
            } else {
                $null
            }
            result = $publisherRestartResult
        }
        alphaPattern = $effectiveAlphaPattern
        alphaSampling = [ordered]@{
            requestedCount = $AlphaSampleCount
            requiredCount = $requiredAlphaSampleCount
            effectiveCount = $effectiveAlphaSampleCount
            requestedIntervalMs = $AlphaSampleIntervalMs
            requiredIntervalMs = $requiredAlphaSampleIntervalMs
            effectiveIntervalMs = $effectiveAlphaSampleIntervalMs
            transitionRequested = ($AlphaTransitionMode -ne "none")
            transitionMode = $AlphaTransitionMode
            transitionLabel = if ($AlphaTransitionMode -ne "none") {
                $AlphaTransitionLabel
            } else {
                $null
            }
            transitionAfterSampleRequested = if ($AlphaTransitionAfterSample -gt 0) {
                $AlphaTransitionAfterSample
            } else {
                $null
            }
            transitionSettleMs = 0
            transitionHoldMs = $AlphaTransitionHoldMs
            transitionTimeoutSeconds = $AlphaTransitionTimeoutSeconds
            sequenceOk = if ($sourceCheck -and $sourceCheck.alphaPixelCheck) {
                [bool]$sourceCheck.alphaPixelCheck.sequence.ok
            } else {
                $null
            }
            transition = if ($sourceCheck -and $sourceCheck.alphaPixelCheck) {
                $sourceCheck.alphaPixelCheck.transition
            } else {
                $null
            }
        }
        alphaBackgroundColorRequested = if ($AlphaBackgroundColor -gt 0) { $AlphaBackgroundColor } else { $null }
        alphaEpochEvidence = Get-FileBinding -Path $alphaEpochFile
        fixturePostEpochObserved = $fixturePostEpochObserved
        alphaSamplesUseRequestedRed = $alphaSamplesUseRequestedRed
        movingTransparencyObserved = $movingTransparencyObserved
        uniqueAlphaCompositeSampleHashes = $alphaSampleHashes
        controlVTubeStudioWindow = [bool]$ControlVTubeStudioWindow
        requireVTubeStudioWindowControl = [bool]$RequireVTubeStudioWindowControl
        vtubeWindowChurn = $vtubeWindowChurn
        testSpoutSender = [ordered]@{
            enabled = [bool]$UseTestSpoutSender
            exe = if ($UseTestSpoutSender) { (Resolve-Path $TestSpoutSenderExe).Path } else { $null }
            sha256 = $testSpoutSenderSha256
            name = if ($UseTestSpoutSender) { $TestSpoutSenderName } else { $null }
            width = if ($UseTestSpoutSender) { $TestSpoutWidth } else { $null }
            height = if ($UseTestSpoutSender) { $TestSpoutHeight } else { $null }
            fps = if ($UseTestSpoutSender) { $TestSpoutFps } else { $null }
            requestedPattern = if ($UseTestSpoutSender) { $TestSpoutPattern } else { $null }
            actualPattern = if ($UseTestSpoutSender) { $actualTestSpoutPattern } else { $null }
            resizeAfterMs = if ($UseTestSpoutSender) { $TestSpoutResizeAfterMs } else { $null }
            resizeWidth = if ($UseTestSpoutSender) { $TestSpoutResizeWidth } else { $null }
            resizeHeight = if ($UseTestSpoutSender) { $TestSpoutResizeHeight } else { $null }
            runningDuringValidation = if ($testSpoutProc) { -not $testSpoutProc.HasExited } else { $null }
            exitCode = if ($testSpoutProc -and $testSpoutProc.HasExited) { $testSpoutProc.ExitCode } else { $null }
            stdout = if (Test-Path $testSpoutOut) { $testSpoutOut } else { $null }
            stderr = if (Test-Path $testSpoutErr) { $testSpoutErr } else { $null }
        }
        alphaReceiverProbe = [ordered]@{
            required = $probeRequired
            script = if ($probeRequired) { $AlphaReceiverProbePath } else { $null }
            exitCode = $alphaReceiverProbeExit
            timedOut = $alphaReceiverProbeTimedOut
            harnessOk = $alphaReceiverProbeHarnessOk
            productOk = $alphaReceiverProbeProductOk
            report = if ($alphaReceiverProbe) { $alphaReceiverProbe } else { $null }
            reportPath = if (Test-Path -LiteralPath $alphaReceiverProbeReportPath) { $alphaReceiverProbeReportPath } else { $null }
            stdout = if (Test-Path -LiteralPath $alphaReceiverProbeOut) { $alphaReceiverProbeOut } else { $null }
            stderr = if (Test-Path -LiteralPath $alphaReceiverProbeErr) { $alphaReceiverProbeErr } else { $null }
        }
        roomAlphaAssertions = [ordered]@{
            expectedHqOnlyRuntimeExplanation = $expectedHqOnlyRuntimeExplanation
            roomJoinLogged = $roomJoinLogged
            roomPeerInitLogged = $roomPeerInitLogged
            roomQualityEnabled = $roomQualityEnabled
            allRoomVideoPeersHq = $allRoomVideoPeersHq
            hqOnlyRuntimeExplanationLogged = $hqOnlyRuntimeExplanationLogged
            selectedCodecAuthorityVp9 = $selectedCodecAuthorityVp9
            obsPrimaryAndAlphaNegotiated = $obsPrimaryAndAlphaNegotiated
            obsPrimaryTrackDecoded = $obsPrimaryTrackDecoded
            obsAlphaTrackDecoded = $obsAlphaTrackDecoded
            obsAlphaCompositionActive = ($obsLogText -match "Native receiver alpha composition active")
            redCompositeSamplesPass = ($alphaPixelCheckOk -and $alphaSamplesUseRequestedRed)
            movingTransparencyObserved = $movingTransparencyObserved
            browserTrackCountersAdvance = $alphaReceiverProbeProductOk
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
        gameCaptureLogs = $gameCaptureLogPaths
        gameCaptureDiagnostics = if ((Test-Path $preferredDiagnosticsPath) -and ((Get-Item $preferredDiagnosticsPath).Length -gt 0)) { $preferredDiagnosticsPath } else { $null }
        sourceSmokeLog = $smokeOut
        sourceScreenshot = $sourceScreenshot
        artifactBinding = $artifactBinding
        artifactIdentityContract = $artifactIdentityContract
        expectedArtifactHashes = [ordered]@{
            gameCapture = if ($ExpectedGameCaptureSha256) { $ExpectedGameCaptureSha256 } else { $null }
            plugin = if ($ExpectedPluginSha256) { $ExpectedPluginSha256 } else { $null }
            spoutSender = if ($ExpectedSpoutSenderSha256) { $ExpectedSpoutSenderSha256 } else { $null }
        }
        artifactHashesStableThroughRun = [ordered]@{
            gameCapture = $publisherHashStable
            plugin = $pluginHashStable
            spoutSender = $spoutSenderHashStable
        }
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
    if ($null -ne $oldAlphaMaxDark) {
        $env:VDONINJA_ALPHA_MAX_DARK_RATIO = $oldAlphaMaxDark
    } else {
        Remove-Item Env:VDONINJA_ALPHA_MAX_DARK_RATIO -ErrorAction SilentlyContinue
    }
    if ($null -ne $oldAlphaMaxGreen) {
        $env:VDONINJA_ALPHA_MAX_GREEN_RATIO = $oldAlphaMaxGreen
    } else {
        Remove-Item Env:VDONINJA_ALPHA_MAX_GREEN_RATIO -ErrorAction SilentlyContinue
    }
    if ($null -ne $oldAlphaBackgroundColor) {
        $env:VDONINJA_ALPHA_BACKGROUND_COLOR = $oldAlphaBackgroundColor
    } else {
        Remove-Item Env:VDONINJA_ALPHA_BACKGROUND_COLOR -ErrorAction SilentlyContinue
    }
    if ($null -ne $oldAlphaPattern) {
        $env:VDONINJA_ALPHA_PATTERN = $oldAlphaPattern
    } else {
        Remove-Item Env:VDONINJA_ALPHA_PATTERN -ErrorAction SilentlyContinue
    }
    if ($null -ne $oldAlphaSampleCount) {
        $env:VDONINJA_ALPHA_SAMPLE_COUNT = $oldAlphaSampleCount
    } else {
        Remove-Item Env:VDONINJA_ALPHA_SAMPLE_COUNT -ErrorAction SilentlyContinue
    }
    if ($null -ne $oldAlphaSampleInterval) {
        $env:VDONINJA_ALPHA_SAMPLE_INTERVAL_MS = $oldAlphaSampleInterval
    } else {
        Remove-Item Env:VDONINJA_ALPHA_SAMPLE_INTERVAL_MS -ErrorAction SilentlyContinue
    }
    if ($null -ne $oldAlphaSampleStep) {
        $env:VDONINJA_ALPHA_SAMPLE_STEP = $oldAlphaSampleStep
    } else {
        Remove-Item Env:VDONINJA_ALPHA_SAMPLE_STEP -ErrorAction SilentlyContinue
    }
    if ($null -ne $oldAlphaTransitionCommand) {
        $env:VDONINJA_ALPHA_TRANSITION_COMMAND = $oldAlphaTransitionCommand
    } else {
        Remove-Item Env:VDONINJA_ALPHA_TRANSITION_COMMAND -ErrorAction SilentlyContinue
    }
    if ($null -ne $oldAlphaTransitionMode) {
        $env:VDONINJA_ALPHA_TRANSITION_MODE = $oldAlphaTransitionMode
    } else {
        Remove-Item Env:VDONINJA_ALPHA_TRANSITION_MODE -ErrorAction SilentlyContinue
    }
    if ($null -ne $oldAlphaTransitionLabel) {
        $env:VDONINJA_ALPHA_TRANSITION_LABEL = $oldAlphaTransitionLabel
    } else {
        Remove-Item Env:VDONINJA_ALPHA_TRANSITION_LABEL -ErrorAction SilentlyContinue
    }
    if ($null -ne $oldAlphaTransitionAfterSample) {
        $env:VDONINJA_ALPHA_TRANSITION_AFTER_SAMPLE = $oldAlphaTransitionAfterSample
    } else {
        Remove-Item Env:VDONINJA_ALPHA_TRANSITION_AFTER_SAMPLE -ErrorAction SilentlyContinue
    }
    if ($null -ne $oldAlphaTransitionHoldMs) {
        $env:VDONINJA_ALPHA_TRANSITION_HOLD_MS = $oldAlphaTransitionHoldMs
    } else {
        Remove-Item Env:VDONINJA_ALPHA_TRANSITION_HOLD_MS -ErrorAction SilentlyContinue
    }
    if ($null -ne $oldAlphaTransitionTimeoutMs) {
        $env:VDONINJA_ALPHA_TRANSITION_TIMEOUT_MS = $oldAlphaTransitionTimeoutMs
    } else {
        Remove-Item Env:VDONINJA_ALPHA_TRANSITION_TIMEOUT_MS -ErrorAction SilentlyContinue
    }
    if ($null -ne $oldAlphaEpochFile) {
        $env:VDONINJA_ALPHA_EPOCH_FILE = $oldAlphaEpochFile
    } else {
        Remove-Item Env:VDONINJA_ALPHA_EPOCH_FILE -ErrorAction SilentlyContinue
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
    if ($null -ne $oldGameCaptureControlDiscovery) {
        $env:VDONINJA_GAME_CAPTURE_CONTROL_DISCOVERY = $oldGameCaptureControlDiscovery
    } else {
        Remove-Item Env:VDONINJA_GAME_CAPTURE_CONTROL_DISCOVERY -ErrorAction SilentlyContinue
    }
    if ($gameCaptureProc -and -not $gameCaptureProc.HasExited) {
        Stop-Process -Id $gameCaptureProc.Id -Force -ErrorAction SilentlyContinue
        Wait-Process -Id $gameCaptureProc.Id -Timeout 10 -ErrorAction SilentlyContinue
    }
    if ($alphaReceiverProbeProc -and -not $alphaReceiverProbeProc.HasExited) {
        Stop-Process -Id $alphaReceiverProbeProc.Id -Force -ErrorAction SilentlyContinue
        Wait-Process -Id $alphaReceiverProbeProc.Id -Timeout 10 -ErrorAction SilentlyContinue
    }
    if ($testSpoutProc -and -not $testSpoutProc.HasExited) {
        Stop-Process -Id $testSpoutProc.Id -Force -ErrorAction SilentlyContinue
        Wait-Process -Id $testSpoutProc.Id -Timeout 10 -ErrorAction SilentlyContinue
    }
}
