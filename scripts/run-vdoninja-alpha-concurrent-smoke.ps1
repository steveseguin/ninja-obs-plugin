param(
    [string]$StreamId = "gcAlphaConcurrent1",
    [string]$GameCaptureExe = "C:\\Users\\steve\\Code\\game-capture\\native-qt\\build-review2\\bin\\Release\\game-capture.exe",
    [string]$ChromeExe = "C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe",
    [string]$MotionDemoPath = ".\\tests\\tools\\gc-motion-demo.html",
    [string]$InstallPrefix = ".\\install-obs32",
    [int]$ObsWebSocketPort = 4462,
    [int]$ObsStartupSeconds = 18,
    [int]$PublisherWarmupSeconds = 8,
    [int]$ObsWaitMs = 30000,
    [int]$PublisherDurationMs = 90000,
    [int]$CheckTimeoutSeconds = 120
)

$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
. (Join-Path $PSScriptRoot "test-alpha-validation-common.ps1")

$gameCaptureExePath = (Resolve-Path $GameCaptureExe).Path
$chromeExePath = (Resolve-Path $ChromeExe).Path
$motionDemoPath = (Resolve-Path $MotionDemoPath).Path
$installPrefixPath = (Resolve-Path $InstallPrefix).Path
$smokeScriptPath = Join-Path $PSScriptRoot "run-vdoninja-source-smoke.ps1"

$publisherOut = Join-Path $repoRoot "artifacts\\alpha-concurrent-publisher.out.log"
$publisherErr = Join-Path $repoRoot "artifacts\\alpha-concurrent-publisher.err.log"
$viewerOut = Join-Path $repoRoot "artifacts\\alpha-concurrent-browser.out.json"
$viewerErr = Join-Path $repoRoot "artifacts\\alpha-concurrent-browser.err.log"
$smokeOut = Join-Path $repoRoot "artifacts\\alpha-concurrent-smoke.out.log"
$smokeErr = Join-Path $repoRoot "artifacts\\alpha-concurrent-smoke.err.log"
$chromeProfile = Join-Path $repoRoot "artifacts\\chrome-gc-alpha-concurrent-profile"

foreach ($path in @($publisherOut, $publisherErr, $viewerOut, $viewerErr, $smokeOut, $smokeErr)) {
    if (Test-Path $path) {
        Remove-Item $path -Force
    }
}

Sync-PortableObsPluginPayload -RepoRoot $repoRoot -InstallPrefixPath $installPrefixPath
Stop-AlphaMotionDemoProcesses
New-Item -ItemType Directory -Force -Path $chromeProfile | Out-Null

$motionDemoUri = "file:///" + ($motionDemoPath -replace '\\', '/')
$viewUrl = "https://vdo.ninja/?view=$([Uri]::EscapeDataString($StreamId))&cleanoutput=1"

$chromeProc = $null
$publisherProc = $null
$viewerProc = $null
$previousWaitMs = $env:VDONINJA_WAIT_MS
$previousSkipCapture = $env:VDONINJA_SKIP_CAPTURE

try {
    $chromeProc = Start-Process -FilePath $chromeExePath -ArgumentList @(
            "--user-data-dir=$chromeProfile",
            "--autoplay-policy=no-user-gesture-required",
            "--app=$motionDemoUri",
            "--window-size=1280,720"
        ) -PassThru
    Start-Sleep -Seconds 2

    $publisherProc = Start-Process -FilePath $gameCaptureExePath -ArgumentList @(
            "--headless",
            "--stream=$StreamId",
            "--video-codec=vp9",
            "--alpha-workflow",
            "--resolution=1280x720",
            "--fps=30",
            "--bitrate-kbps=6000",
            "--duration-ms=$PublisherDurationMs",
            "--window=GC Alpha Motion Demo"
        ) -RedirectStandardOutput $publisherOut -RedirectStandardError $publisherErr -PassThru
    Start-Sleep -Seconds $PublisherWarmupSeconds

    $viewerProc = Start-Process -FilePath "node" -ArgumentList @(
            "scripts/playwright-vdo-view-check.cjs",
            $viewUrl
        ) -WorkingDirectory $repoRoot -RedirectStandardOutput $viewerOut -RedirectStandardError $viewerErr -PassThru

    $env:VDONINJA_WAIT_MS = [string]$ObsWaitMs
    $env:VDONINJA_SKIP_CAPTURE = "1"
    $smokeOutput = & powershell -ExecutionPolicy Bypass -File $smokeScriptPath `
        -Mode native `
        -StreamId $StreamId `
        -SkipPublisher `
        -InstallPrefix $InstallPrefix `
        -ObsWebSocketPort $ObsWebSocketPort `
        -ObsStartupSeconds $ObsStartupSeconds `
        -CheckTimeoutSeconds $CheckTimeoutSeconds 2>&1
    $smokeOutput | Set-Content -Path $smokeOut -Encoding UTF8
    if ($LASTEXITCODE -ne 0) {
        ($smokeOutput | Out-String) | Set-Content -Path $smokeErr -Encoding UTF8
        throw "OBS native alpha smoke failed with exit code $LASTEXITCODE"
    }

    if ($viewerProc -and -not $viewerProc.HasExited) {
        Wait-Process -Id $viewerProc.Id -Timeout 90 -ErrorAction Stop
    }

    $viewerJson = Get-Content $viewerOut -Raw | ConvertFrom-Json
    $browserVideoTrackCount = (($viewerJson.videoElements | Measure-Object -Property videoTracks -Sum).Sum)
    $browserInboundVideoBytes = (($viewerJson.pcStats | Measure-Object -Property inboundVideoBytes -Sum).Sum)
    $browserVideoElements =
        @($viewerJson.videoElements | Where-Object { $_.videoTracks -ge 1 -and $_.videoWidth -gt 0 -and $_.currentTime -gt 0 })

    if ($viewerJson.containsWaitingText) {
        throw "Browser viewer stayed on the waiting state"
    }
    if ($browserVideoTrackCount -ne 1) {
        throw "Expected browser viewer to receive exactly one video track, got $browserVideoTrackCount"
    }
    if ($browserInboundVideoBytes -le 0) {
        throw "Browser viewer did not receive inbound video bytes"
    }
    if ($browserVideoElements.Count -lt 1) {
        throw "Browser viewer did not render a playable video element"
    }

    $obsLogPath = Get-LatestPortableObsLogPath -RepoRoot $repoRoot
    $obsLogText = Get-Content $obsLogPath -Raw
    if ($obsLogText -notmatch "Native receiver alpha composition active") {
        throw "OBS log did not reach alpha composition"
    }
    if ($obsLogText -match "No native video packets") {
        throw "OBS log hit a stale native video timeout during concurrent alpha validation"
    }
    if ($obsLogText -match "Number of media packets dropped due to a full queue") {
        throw "OBS log hit libdatachannel queue drops during concurrent alpha validation"
    }

    [pscustomobject]@{
        ok = $true
        streamId = $StreamId
        browserViewUrl = $viewUrl
        browserVideoTrackCount = $browserVideoTrackCount
        browserInboundVideoBytes = $browserInboundVideoBytes
        obsLog = $obsLogPath
        obsAlphaCompositionActive = $true
        publisherLog = $publisherOut
        browserLog = $viewerOut
        smokeLog = $smokeOut
    } | ConvertTo-Json -Depth 4
}
finally {
    if ($null -ne $previousWaitMs) {
        $env:VDONINJA_WAIT_MS = $previousWaitMs
    } else {
        Remove-Item Env:VDONINJA_WAIT_MS -ErrorAction SilentlyContinue
    }

    if ($null -ne $previousSkipCapture) {
        $env:VDONINJA_SKIP_CAPTURE = $previousSkipCapture
    } else {
        Remove-Item Env:VDONINJA_SKIP_CAPTURE -ErrorAction SilentlyContinue
    }

    foreach ($proc in @($viewerProc, $publisherProc)) {
        if ($proc -and -not $proc.HasExited) {
            Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
        }
    }
    if ($chromeProc -and -not $chromeProc.HasExited) {
        Stop-Process -Id $chromeProc.Id -Force -ErrorAction SilentlyContinue
    }
    Stop-AlphaMotionDemoProcesses
}
