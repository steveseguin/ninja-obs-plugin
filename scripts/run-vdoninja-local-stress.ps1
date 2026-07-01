param(
    [ValidateSet("quick", "standard", "soak")]
    [string]$Profile = "quick",
    [string]$InstallPrefix = ".\install-obs32",
    [string]$ObsExe = ".\_obs-portable\bin\64bit\obs64.exe",
    [string]$ObsWorkingDirectory = ".\_obs-portable\bin\64bit",
    [string]$RunId = "",
    [switch]$SkipBrowser,
    [switch]$SkipPublish,
    [switch]$SkipSource,
    [switch]$SkipLifecycle,
    [switch]$IncludeFaultRecovery,
    [switch]$CaptureSourceScreenshots,
    [int]$LifecycleIterations = 0,
    [int]$LifecycleIterationWaitMs = 0,
    [int]$ObsStartupSeconds = 18,
    [int]$PublisherWarmupSeconds = 22
)

$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$installPrefixPath = (Resolve-Path $InstallPrefix).Path
$obsExePath = (Resolve-Path $ObsExe).Path
$obsWorkingDirPath = (Resolve-Path $ObsWorkingDirectory).Path
$artifactsRoot = Join-Path $repoRoot "artifacts"

if ([string]::IsNullOrWhiteSpace($RunId)) {
    $RunId = "stress" + (Get-Date -Format "yyyyMMddHHmmss")
}

$artifactRunId = $RunId -replace "[^A-Za-z0-9_.-]", "-"
if ([string]::IsNullOrWhiteSpace($artifactRunId)) {
    $artifactRunId = "stress"
}

$signalRunId = $RunId -replace "[^A-Za-z0-9]", ""
if ([string]::IsNullOrWhiteSpace($signalRunId)) {
    $signalRunId = "stress"
}
if ($signalRunId.Length -gt 32) {
    $signalRunId = $signalRunId.Substring(0, 32)
}

$artifactDir = Join-Path $artifactsRoot (Join-Path "local-stress" $artifactRunId)
New-Item -ItemType Directory -Path $artifactDir -Force | Out-Null

$profileDefaults = @{
    quick = @{
        LifecycleIterations = 2
        LifecycleIterationWaitMs = 6000
        LifecycleTimeoutSeconds = 90
        IncludeFaultRecovery = $false
    }
    standard = @{
        LifecycleIterations = 8
        LifecycleIterationWaitMs = 12000
        LifecycleTimeoutSeconds = 90
        IncludeFaultRecovery = $true
    }
    soak = @{
        LifecycleIterations = 40
        LifecycleIterationWaitMs = 15000
        LifecycleTimeoutSeconds = 120
        IncludeFaultRecovery = $true
    }
}

$selectedDefaults = $profileDefaults[$Profile]
if ($LifecycleIterations -le 0) {
    $LifecycleIterations = [int]$selectedDefaults.LifecycleIterations
}
if ($LifecycleIterationWaitMs -le 0) {
    $LifecycleIterationWaitMs = [int]$selectedDefaults.LifecycleIterationWaitMs
}
$lifecycleTimeoutSeconds = [int]$selectedDefaults.LifecycleTimeoutSeconds
if (-not $PSBoundParameters.ContainsKey("IncludeFaultRecovery") -and [bool]$selectedDefaults.IncludeFaultRecovery) {
    $IncludeFaultRecovery = $true
}

$publishStreamId = "${signalRunId}Pub"
$sourceStreamId = "${signalRunId}Src"
$lifecycleStreamId = "${signalRunId}Life"
$faultStreamId = "${signalRunId}Fault"
$roomId = "${signalRunId}Room"

$script:startedAt = Get-Date
$script:results = New-Object System.Collections.Generic.List[object]
$script:summaryPath = Join-Path $artifactDir "summary.json"
$script:ok = $false
$script:errorMessage = $null

function Quote-ProcessArgument {
    param([string]$Argument)

    if ($null -eq $Argument -or $Argument -eq "") {
        return '""'
    }

    return '"' + ($Argument -replace '"', '\"') + '"'
}

function Get-LogTail {
    param([string]$Path)

    if (-not (Test-Path $Path)) {
        return ""
    }

    return ((Get-Content -Path $Path -Tail 30 -ErrorAction SilentlyContinue) -join [Environment]::NewLine)
}

function Copy-StepArtifacts {
    param(
        [datetime]$SinceUtc,
        [string]$Destination
    )

    $copied = New-Object System.Collections.Generic.List[string]
    if (-not (Test-Path $artifactsRoot)) {
        return $copied
    }

    $localStressRoot = Join-Path $artifactsRoot "local-stress"
    $threshold = $SinceUtc.AddSeconds(-2)

    Get-ChildItem -Path $artifactsRoot -File -Recurse -ErrorAction SilentlyContinue |
        Where-Object {
            -not $_.FullName.StartsWith($localStressRoot, [System.StringComparison]::OrdinalIgnoreCase) -and
            $_.LastWriteTimeUtc -ge $threshold
        } |
        ForEach-Object {
            $relative = $_.FullName.Substring($artifactsRoot.Length).TrimStart([char[]]"\/")
            $target = Join-Path $Destination $relative
            $targetDir = Split-Path $target -Parent
            if (-not (Test-Path $targetDir)) {
                New-Item -ItemType Directory -Path $targetDir -Force | Out-Null
            }
            Copy-Item -LiteralPath $_.FullName -Destination $target -Force
            [void]$copied.Add($target)
        }

    return $copied
}

function Invoke-StressStep {
    param(
        [string]$Name,
        [string]$FilePath,
        [string[]]$Arguments,
        [hashtable]$Environment = @{},
        [int]$TimeoutSeconds
    )

    $safeName = $Name -replace "[^A-Za-z0-9_.-]", "-"
    $stepDir = Join-Path $artifactDir $safeName
    New-Item -ItemType Directory -Path $stepDir -Force | Out-Null

    $stdoutPath = Join-Path $stepDir "stdout.log"
    $stderrPath = Join-Path $stepDir "stderr.log"
    $startedAt = Get-Date
    $startedUtc = $startedAt.ToUniversalTime()
    $exitCode = $null
    $timedOut = $false

    Write-Host "== $Name =="
    Write-Host "logs: $stepDir"

    $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $FilePath
    $startInfo.Arguments = ($Arguments | ForEach-Object { Quote-ProcessArgument $_ }) -join " "
    $startInfo.WorkingDirectory = $repoRoot
    $startInfo.UseShellExecute = $false
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $startInfo.CreateNoWindow = $true
    foreach ($entry in $Environment.GetEnumerator()) {
        $startInfo.EnvironmentVariables[$entry.Key] = [string]$entry.Value
    }

    $proc = [System.Diagnostics.Process]::new()
    $proc.StartInfo = $startInfo

    try {
        [void]$proc.Start()
        $stdoutTask = $proc.StandardOutput.ReadToEndAsync()
        $stderrTask = $proc.StandardError.ReadToEndAsync()

        if ($TimeoutSeconds -gt 0) {
            if (-not $proc.WaitForExit($TimeoutSeconds * 1000)) {
                $timedOut = $true
                $proc.Kill()
            }
        } else {
            $proc.WaitForExit()
        }

        $proc.WaitForExit()
        $stdout = $stdoutTask.Result
        $stderr = $stderrTask.Result
        Set-Content -Path $stdoutPath -Value $stdout -Encoding UTF8
        Set-Content -Path $stderrPath -Value $stderr -Encoding UTF8
        $exitCode = if ($null -eq $proc.ExitCode) { -1 } else { [int]$proc.ExitCode }
    } catch {
        Set-Content -Path $stderrPath -Value $_.Exception.Message -Encoding UTF8
        if ($null -eq $exitCode) {
            $exitCode = -1
        }
        throw
    } finally {
        $finishedAt = Get-Date
        $copiedArtifacts = @(Copy-StepArtifacts -SinceUtc $startedUtc -Destination $stepDir)
        [void]$script:results.Add([pscustomobject]@{
            name = $Name
            exitCode = $exitCode
            timedOut = $timedOut
            startedAt = $startedAt.ToString("o")
            finishedAt = $finishedAt.ToString("o")
            durationSeconds = [math]::Round(($finishedAt - $startedAt).TotalSeconds, 2)
            stdout = $stdoutPath
            stderr = $stderrPath
            artifacts = $copiedArtifacts
        })
    }

    if ($timedOut) {
        throw "Step '$Name' timed out after $TimeoutSeconds seconds. See $stepDir"
    }
    if ($exitCode -ne 0) {
        $tail = Get-LogTail -Path $stderrPath
        if ([string]::IsNullOrWhiteSpace($tail)) {
            $tail = Get-LogTail -Path $stdoutPath
        }
        throw "Step '$Name' failed with exit code $exitCode. See $stepDir`n$tail"
    }
}

function Write-StressSummary {
    $summary = [pscustomobject]@{
        ok = $script:ok
        error = $script:errorMessage
        runId = $RunId
        profile = $Profile
        startedAt = $script:startedAt.ToString("o")
        finishedAt = (Get-Date).ToString("o")
        artifactDir = $artifactDir
        inputs = [pscustomobject]@{
            installPrefix = $installPrefixPath
            obsExe = $obsExePath
            obsWorkingDirectory = $obsWorkingDirPath
            obsStartupSeconds = $ObsStartupSeconds
            publisherWarmupSeconds = $PublisherWarmupSeconds
            lifecycleIterations = $LifecycleIterations
            lifecycleIterationWaitMs = $LifecycleIterationWaitMs
            includeFaultRecovery = [bool]$IncludeFaultRecovery
            captureSourceScreenshots = [bool]$CaptureSourceScreenshots
        }
        streamIds = [pscustomobject]@{
            publish = $publishStreamId
            source = $sourceStreamId
            lifecycle = $lifecycleStreamId
            fault = $faultStreamId
            room = $roomId
        }
        steps = @($script:results.ToArray())
    }

    $summary | ConvertTo-Json -Depth 8 | Set-Content -Path $script:summaryPath -Encoding UTF8
}

$powershellExe = (Get-Command powershell -ErrorAction Stop).Source
$npmCommand = Get-Command npm.cmd -ErrorAction SilentlyContinue
if (-not $npmCommand) {
    $npmCommand = Get-Command npm -ErrorAction SilentlyContinue
}

try {
    Write-Host "Local VDO.Ninja stress profile: $Profile"
    Write-Host "Artifacts: $artifactDir"

    if (-not $SkipBrowser) {
        if (-not $npmCommand) {
            throw "npm was not found. Re-run with -SkipBrowser to skip Playwright browser churn."
        }
        Invoke-StressStep -Name "browser-multi-viewer-e2e" `
            -FilePath $npmCommand.Source `
            -Arguments @("run", "test:e2e:multi") `
            -TimeoutSeconds 240
    }

    if (-not $SkipPublish) {
        Invoke-StressStep -Name "obs-publish-smoke" `
            -FilePath $powershellExe `
            -Arguments @(
                "-NoProfile", "-ExecutionPolicy", "Bypass",
                "-File", (Join-Path $repoRoot "scripts\run-vdoninja-publish-smoke.ps1"),
                "-StreamId", $publishStreamId,
                "-Password", "123",
                "-RoomId", $roomId,
                "-ObsExe", $obsExePath,
                "-ObsWorkingDirectory", $obsWorkingDirPath,
                "-InstallPrefix", $installPrefixPath,
                "-ObsWebSocketPort", "4456",
                "-ObsStartupSeconds", [string]$ObsStartupSeconds,
                "-CheckTimeoutSeconds", "150"
            ) `
            -TimeoutSeconds 260
    }

    if (-not $SkipSource) {
        $sourcePushUrl = "https://vdo.ninja/?push=$sourceStreamId&password=false&webcam=1&autostart=1"
        $sourceViewUrl = "https://vdo.ninja/?view=$sourceStreamId&password=false"
        $sourceEnvironment = @{}
        if (-not $CaptureSourceScreenshots) {
            $sourceEnvironment.VDONINJA_SKIP_CAPTURE = "1"
        }
        Invoke-StressStep -Name "obs-source-smoke" `
            -FilePath $powershellExe `
            -Arguments @(
                "-NoProfile", "-ExecutionPolicy", "Bypass",
                "-File", (Join-Path $repoRoot "scripts\run-vdoninja-source-smoke.ps1"),
                "-Mode", "native",
                "-StreamId", $sourceStreamId,
                "-Password", "false",
                "-PushUrl", $sourcePushUrl,
                "-ViewUrl", $sourceViewUrl,
                "-ObsExe", $obsExePath,
                "-ObsWorkingDirectory", $obsWorkingDirPath,
                "-InstallPrefix", $installPrefixPath,
                "-ObsStartupSeconds", [string]$ObsStartupSeconds,
                "-PublisherWarmupSeconds", [string]$PublisherWarmupSeconds,
                "-CheckTimeoutSeconds", "120"
            ) `
            -Environment $sourceEnvironment `
            -TimeoutSeconds 260
    }

    if ($IncludeFaultRecovery) {
        $faultPushUrl = "https://vdo.ninja/?push=$faultStreamId&password=false&webcam=1&autostart=1"
        $faultViewUrl = "https://vdo.ninja/?view=$faultStreamId&password=false"
        $faultArgs = @(
            "-NoProfile", "-ExecutionPolicy", "Bypass",
            "-File", (Join-Path $repoRoot "scripts\run-vdoninja-source-fault-smoke.ps1"),
            "-Mode", "native",
            "-StreamId", $faultStreamId,
            "-Password", "false",
            "-PushUrl", $faultPushUrl,
            "-ViewUrl", $faultViewUrl,
            "-ObsExe", $obsExePath,
            "-ObsWorkingDirectory", $obsWorkingDirPath,
            "-InstallPrefix", $installPrefixPath,
            "-ObsStartupSeconds", [string]$ObsStartupSeconds,
            "-PublisherWarmupSeconds", [string]$PublisherWarmupSeconds
        )
        if (-not $CaptureSourceScreenshots) {
            $faultArgs += "-SkipRecoveryCapture"
        }
        Invoke-StressStep -Name "obs-source-fault-recovery" `
            -FilePath $powershellExe `
            -Arguments $faultArgs `
            -TimeoutSeconds 420
    }

    if (-not $SkipLifecycle) {
        $lifecyclePushUrl = "https://vdo.ninja/?push=$lifecycleStreamId&password=false&webcam=1&autostart=1"
        $lifecycleViewUrl = "https://vdo.ninja/?view=$lifecycleStreamId&password=false"
        $lifecycleStepTimeout = $ObsStartupSeconds + $PublisherWarmupSeconds + ($LifecycleIterations * ($lifecycleTimeoutSeconds + 5)) + 45
        $lifecycleArgs = @(
            "-NoProfile", "-ExecutionPolicy", "Bypass",
            "-File", (Join-Path $repoRoot "scripts\run-vdoninja-source-lifecycle-stress.ps1"),
            "-Mode", "native",
            "-StreamId", $lifecycleStreamId,
            "-PushUrl", $lifecyclePushUrl,
            "-ViewUrl", $lifecycleViewUrl,
            "-ObsExe", $obsExePath,
            "-ObsWorkingDirectory", $obsWorkingDirPath,
            "-InstallPrefix", $installPrefixPath,
            "-ObsStartupSeconds", [string]$ObsStartupSeconds,
            "-PublisherWarmupSeconds", [string]$PublisherWarmupSeconds,
            "-Iterations", [string]$LifecycleIterations,
            "-IterationWaitMs", [string]$LifecycleIterationWaitMs,
            "-IterationTimeoutSeconds", [string]$lifecycleTimeoutSeconds
        )
        if (-not $CaptureSourceScreenshots) {
            $lifecycleArgs += "-SkipFinalCapture"
        }
        Invoke-StressStep -Name "obs-source-lifecycle-stress" `
            -FilePath $powershellExe `
            -Arguments $lifecycleArgs `
            -TimeoutSeconds $lifecycleStepTimeout
    }

    $script:ok = $true
} catch {
    $script:ok = $false
    $script:errorMessage = $_.Exception.Message
    Write-Error $script:errorMessage
} finally {
    Write-StressSummary
    Write-Host "Summary: $script:summaryPath"
}

if (-not $script:ok) {
    exit 1
}
