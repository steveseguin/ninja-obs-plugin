param(
    [string]$StreamId = "gcAlphaTeardown1",
    [string]$GameCaptureExe = "C:\\Users\\steve\\Code\\game-capture\\native-qt\\build-review2\\bin\\Release\\game-capture.exe",
    [string]$ChromeExe = "C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe",
    [string]$MotionDemoPath = ".\\tests\\tools\\gc-motion-demo.html",
    [string]$InstallPrefix = ".\\install-obs32",
    [int]$ObsWebSocketPort = 4462,
    [int]$ObsStartupSeconds = 18,
    [int]$PublisherWarmupSeconds = 6,
    [int]$PublisherDurationMs = 50000,
    [int]$ObsWaitMs = 65000,
    [int]$CheckTimeoutSeconds = 160,
    [int]$MaxClearedScreenshotBytes = 6000
)

$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
. (Join-Path $PSScriptRoot "test-alpha-validation-common.ps1")

$gameCaptureExePath = (Resolve-Path $GameCaptureExe).Path
$chromeExePath = (Resolve-Path $ChromeExe).Path
$motionDemoPath = (Resolve-Path $MotionDemoPath).Path
$installPrefixPath = (Resolve-Path $InstallPrefix).Path
$smokeScriptPath = Join-Path $PSScriptRoot "run-vdoninja-source-smoke.ps1"

$publisherOut = Join-Path $repoRoot "artifacts\\alpha-teardown-publisher.out.log"
$publisherErr = Join-Path $repoRoot "artifacts\\alpha-teardown-publisher.err.log"
$smokeOut = Join-Path $repoRoot "artifacts\\alpha-teardown-smoke.out.log"
$smokeErr = Join-Path $repoRoot "artifacts\\alpha-teardown-smoke.err.log"
$sourceCheckOut = Join-Path $repoRoot "artifacts\\obs-source-smoke-native.out.log"
$chromeProfile = Join-Path $repoRoot "artifacts\\chrome-gc-alpha-teardown-profile"

foreach ($path in @($publisherOut, $publisherErr, $smokeOut, $smokeErr)) {
    if (Test-Path $path) {
        Remove-Item $path -Force
    }
}

Sync-PortableObsPluginPayload -RepoRoot $repoRoot -InstallPrefixPath $installPrefixPath
Stop-AlphaMotionDemoProcesses
New-Item -ItemType Directory -Force -Path $chromeProfile | Out-Null

$motionDemoUri = "file:///" + ($motionDemoPath -replace '\\', '/')

$chromeProc = $null
$publisherProc = $null
$previousWaitMs = $env:VDONINJA_WAIT_MS
$previousSkipCapture = $env:VDONINJA_SKIP_CAPTURE
$previousMinScreenshotBytes = $env:VDONINJA_MIN_SCREENSHOT_BYTES

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
            "--password=false",
            "--video-codec=vp9",
            "--alpha-workflow",
            "--resolution=1280x720",
            "--fps=30",
            "--bitrate-kbps=6000",
            "--duration-ms=$PublisherDurationMs",
            "--window=GC Alpha Motion Demo"
        ) -RedirectStandardOutput $publisherOut -RedirectStandardError $publisherErr -PassThru
    Start-Sleep -Seconds $PublisherWarmupSeconds

    $env:VDONINJA_WAIT_MS = [string]$ObsWaitMs
    Remove-Item Env:VDONINJA_SKIP_CAPTURE -ErrorAction SilentlyContinue
    $env:VDONINJA_MIN_SCREENSHOT_BYTES = "1"
    $smokeOutput = & powershell -ExecutionPolicy Bypass -File $smokeScriptPath `
        -Mode native `
        -StreamId $StreamId `
        -Password false `
        -SkipPublisher `
        -InstallPrefix $InstallPrefix `
        -ObsWebSocketPort $ObsWebSocketPort `
        -ObsStartupSeconds $ObsStartupSeconds `
        -CheckTimeoutSeconds $CheckTimeoutSeconds 2>&1
    $smokeOutput | Set-Content -Path $smokeOut -Encoding UTF8
    if ($LASTEXITCODE -ne 0) {
        ($smokeOutput | Out-String) | Set-Content -Path $smokeErr -Encoding UTF8
        throw "OBS native alpha teardown smoke failed with exit code $LASTEXITCODE"
    }

    $sourceCheckJson = Get-Content $sourceCheckOut -Raw | ConvertFrom-Json
    $obsLogPath = Get-LatestPortableObsLogPath -RepoRoot $repoRoot
    $obsLogText = Get-Content $obsLogPath -Raw

    if ($obsLogText -notmatch "Native receiver alpha composition active") {
        throw "OBS log did not reach alpha composition before teardown"
    }
    if ($obsLogText -notmatch "Peer .* disconnected") {
        throw "OBS log did not record peer disconnect after publisher exit"
    }
    if ($obsLogText -notmatch "Clearing native video output \(peer-disconnected\)") {
        throw "OBS log did not clear native video output on peer disconnect"
    }
    if (-not $sourceCheckJson.screenshot) {
        throw "Teardown smoke did not capture a screenshot"
    }
    if ([int64]$sourceCheckJson.screenshot.byteLength -gt $MaxClearedScreenshotBytes) {
        throw "Expected a near-empty cleared-scene screenshot, got $($sourceCheckJson.screenshot.byteLength) bytes"
    }

    [pscustomobject]@{
        ok = $true
        streamId = $StreamId
        obsLog = $obsLogPath
        screenshot = $sourceCheckJson.screenshot
        clearedVideoOutput = $true
        publisherLog = $publisherOut
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

    if ($null -ne $previousMinScreenshotBytes) {
        $env:VDONINJA_MIN_SCREENSHOT_BYTES = $previousMinScreenshotBytes
    } else {
        Remove-Item Env:VDONINJA_MIN_SCREENSHOT_BYTES -ErrorAction SilentlyContinue
    }

    if ($publisherProc -and -not $publisherProc.HasExited) {
        Stop-Process -Id $publisherProc.Id -Force -ErrorAction SilentlyContinue
    }
    if ($chromeProc -and -not $chromeProc.HasExited) {
        Stop-Process -Id $chromeProc.Id -Force -ErrorAction SilentlyContinue
    }
    Stop-AlphaMotionDemoProcesses
}
