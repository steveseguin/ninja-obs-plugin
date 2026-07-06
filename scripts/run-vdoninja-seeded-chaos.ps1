param(
    [string]$Seed = "",
    [int]$Iterations = 3,
    [int]$BaseObsWebSocketPort = 4482,
    [string]$InstallPrefix = ".\install",
    [switch]$IncludePublisherReloads,
    [switch]$IncludeTerminalDataFuzz,
    [switch]$Quick,
    [switch]$StopOnFailure
)

$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$chaosScript = Join-Path $repoRoot "scripts\run-vdoninja-chaos-stress.ps1"
$artifactsRoot = Join-Path $repoRoot "artifacts"
$crashDumpDir = Join-Path $env:LOCALAPPDATA "CrashDumps"

if (-not (Test-Path $chaosScript)) {
    throw "Chaos script not found at $chaosScript"
}

if ([string]::IsNullOrWhiteSpace($Seed)) {
    $Seed = "seed-" + (Get-Date -Format "yyyyMMdd-HHmmss")
}

if ($Iterations -lt 1) {
    throw "Iterations must be at least 1"
}

function Convert-SeedToInt {
    param([string]$Text)

    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $bytes = [System.Text.Encoding]::UTF8.GetBytes($Text)
        $hash = $sha.ComputeHash($bytes)
        return [BitConverter]::ToInt32($hash, 0) -band 0x7fffffff
    } finally {
        $sha.Dispose()
    }
}

function Sanitize-Token {
    param([string]$Text)

    $clean = ($Text -replace '[^A-Za-z0-9_-]', '')
    if ([string]::IsNullOrWhiteSpace($clean)) {
        return "seed"
    }
    if ($clean.Length -gt 24) {
        return $clean.Substring(0, 24)
    }
    return $clean
}

function Sanitize-StreamToken {
    param([string]$Text)

    $clean = ($Text -replace '[^A-Za-z0-9]', '_').Trim('_')
    if ([string]::IsNullOrWhiteSpace($clean)) {
        return "seed"
    }
    if ($clean.Length -gt 24) {
        return $clean.Substring(0, 24).Trim('_')
    }
    return $clean
}

function Pick-Item {
    param(
        [System.Random]$Random,
        [object[]]$Items
    )

    return $Items[$Random.Next(0, $Items.Count)]
}

function Get-ObsCrashDumps {
    if (-not (Test-Path $crashDumpDir)) {
        return @()
    }
    return @(Get-ChildItem $crashDumpDir -Filter "obs64*.dmp" -File -ErrorAction SilentlyContinue |
        Select-Object FullName, Name, Length, LastWriteTimeUtc)
}

function Copy-NewCrashDumps {
    param(
        [object[]]$Before,
        [string]$Destination
    )

    $beforeKeys = @{}
    foreach ($dump in $Before) {
        $beforeKeys[$dump.FullName] = $dump.LastWriteTimeUtc
    }

    $after = Get-ObsCrashDumps
    $newDumps = @()
    foreach ($dump in $after) {
        if (-not $beforeKeys.ContainsKey($dump.FullName) -or $beforeKeys[$dump.FullName] -ne $dump.LastWriteTimeUtc) {
            $newDumps += $dump
        }
    }

    if ($newDumps.Count -gt 0) {
        New-Item -ItemType Directory -Path $Destination -Force | Out-Null
        foreach ($dump in $newDumps) {
            Copy-Item -Path $dump.FullName -Destination (Join-Path $Destination $dump.Name) -Force -ErrorAction SilentlyContinue
        }
    }

    return $newDumps
}

function Get-ChaosRunNames {
    if (-not (Test-Path $artifactsRoot)) {
        return @{}
    }
    $names = @{}
    Get-ChildItem $artifactsRoot -Directory -Filter "vdoninja-chaos-*" -ErrorAction SilentlyContinue |
        ForEach-Object { $names[$_.Name] = $true }
    return $names
}

function Get-NewChaosRuns {
    param([hashtable]$Before)

    if (-not (Test-Path $artifactsRoot)) {
        return @()
    }
    return @(Get-ChildItem $artifactsRoot -Directory -Filter "vdoninja-chaos-*" -ErrorAction SilentlyContinue |
        Where-Object { -not $Before.ContainsKey($_.Name) } |
        Sort-Object LastWriteTimeUtc)
}

function New-IterationPlan {
    param(
        [System.Random]$Random,
        [int]$Index,
        [int]$Port,
        [string]$StreamId
    )

    $churnChoices = @("basic", "aggressive", "aggressive")
    $dataChoices = @("official", "aggressive", "aggressive")
    if ($IncludeTerminalDataFuzz -and $Random.NextDouble() -lt 0.20) {
        $dataChoices += "terminal"
    }

    $churn = Pick-Item -Random $Random -Items $churnChoices
    $dataFuzz = Pick-Item -Random $Random -Items $dataChoices
    $iterations = if ($Quick) { $Random.Next(12, 31) } else { $Random.Next(35, 91) }
    $interval = $Random.Next(350, 1201)
    $warmup = if ($Quick) { $Random.Next(14, 22) } else { $Random.Next(20, 31) }
    $recoveryWarmup = if ($Quick) { $Random.Next(14, 22) } else { $Random.Next(20, 34) }
    $keepAlive = if ($Quick) { $Random.Next(180000, 260001) } else { $Random.Next(300000, 480001) }
    $phaseTimeout = if ($Quick) { 180 } else { 240 }
    $reloads = 0
    if ($IncludePublisherReloads -and $Random.NextDouble() -lt 0.35) {
        $reloads = 1
    }

    $skipObsRestart = $Random.NextDouble() -lt 0.15
    $skipPublisherRecovery = $Random.NextDouble() -lt 0.15
    $skipWindowChurn = $Random.NextDouble() -lt 0.20

    if ($dataFuzz -eq "terminal") {
        $skipPublisherRecovery = $true
    }

    return [ordered]@{
        iteration = $Index
        streamId = $StreamId
        port = $Port
        publisherChurn = $churn
        publisherDataFuzz = $dataFuzz
        publisherChurnIterations = $iterations
        publisherChurnIntervalMs = $interval
        publisherWarmupSeconds = $warmup
        publisherRecoveryWarmupSeconds = $recoveryWarmup
        publisherKeepAliveMs = $keepAlive
        publisherReloads = $reloads
        publisherReloadIntervalMs = $Random.Next(35000, 70001)
        publisherReloadStartupWaitMs = $Random.Next(8000, 15001)
        chaosPhaseTimeoutSeconds = $phaseTimeout
        skipObsRestart = $skipObsRestart
        skipPublisherRecovery = $skipPublisherRecovery
        skipWindowChurn = $skipWindowChurn
    }
}

function Build-ReplayArgs {
    param([object]$Plan)

    $args = @(
        "-NoProfile",
        "-ExecutionPolicy", "Bypass",
        "-File", "scripts\run-vdoninja-chaos-stress.ps1",
        "-StreamId", $Plan.streamId,
        "-ObsWebSocketPort", [string]$Plan.port,
        "-InstallPrefix", $InstallPrefix,
        "-PublisherWarmupSeconds", [string]$Plan.publisherWarmupSeconds,
        "-PublisherRecoveryWarmupSeconds", [string]$Plan.publisherRecoveryWarmupSeconds,
        "-PublisherKeepAliveMs", [string]$Plan.publisherKeepAliveMs,
        "-PublisherChurn", $Plan.publisherChurn,
        "-PublisherDataFuzz", $Plan.publisherDataFuzz,
        "-PublisherChurnIterations", [string]$Plan.publisherChurnIterations,
        "-PublisherChurnIntervalMs", [string]$Plan.publisherChurnIntervalMs,
        "-PublisherReloads", [string]$Plan.publisherReloads,
        "-PublisherReloadIntervalMs", [string]$Plan.publisherReloadIntervalMs,
        "-PublisherReloadStartupWaitMs", [string]$Plan.publisherReloadStartupWaitMs,
        "-ChaosPhaseTimeoutSeconds", [string]$Plan.chaosPhaseTimeoutSeconds
    )

    if ($Plan.skipObsRestart) {
        $args += "-SkipObsRestart"
    }
    if ($Plan.skipPublisherRecovery) {
        $args += "-SkipPublisherRecovery"
    }
    if ($Plan.skipWindowChurn) {
        $args += "-SkipWindowChurn"
    }

    return $args
}

function ConvertTo-CommandLine {
    param([string[]]$ArgumentList)

    $quoted = foreach ($arg in $ArgumentList) {
        if ($arg -match '[\s"`]') {
            '"' + ($arg -replace '"', '\"') + '"'
        } else {
            $arg
        }
    }
    return "powershell " + ($quoted -join " ")
}

$seedInt = Convert-SeedToInt -Text $Seed
$rng = [System.Random]::new($seedInt)
$runId = "vdoninja-seeded-chaos-" + (Get-Date -Format "yyyyMMdd-HHmmss") + "-" + (Sanitize-Token $Seed)
$runDir = Join-Path $artifactsRoot $runId
New-Item -ItemType Directory -Path $runDir -Force | Out-Null

$startedAt = Get-Date
$safeSeed = Sanitize-Token $Seed
$streamSeed = Sanitize-StreamToken $Seed
$results = @()

Write-Output "VDONINJA_SEEDED_CHAOS_START=1"
Write-Output "SEED=$Seed"
Write-Output "SEED_INT=$seedInt"
Write-Output "RUN_DIR=$runDir"

for ($i = 1; $i -le $Iterations; $i++) {
    $port = $BaseObsWebSocketPort + (($i - 1) % 20)
    $streamId = "seed" + $streamSeed + "i" + $i
    $plan = New-IterationPlan -Random $rng -Index $i -Port $port -StreamId $streamId
    $args = Build-ReplayArgs -Plan $plan
    $replay = ConvertTo-CommandLine -ArgumentList $args
    $stdout = Join-Path $runDir ("iteration-$i.stdout.log")
    $stderr = Join-Path $runDir ("iteration-$i.stderr.log")
    $beforeRuns = Get-ChaosRunNames
    $beforeDumps = Get-ObsCrashDumps
    $started = Get-Date

    Write-Output "ITERATION_$i=$replay"

    $exitCode = $null
    $errorText = ""
    $newRuns = @()
    $newDumps = @()

    try {
        $proc = Start-Process -FilePath "powershell" `
            -ArgumentList $args `
            -WorkingDirectory $repoRoot `
            -RedirectStandardOutput $stdout `
            -RedirectStandardError $stderr `
            -WindowStyle Hidden `
            -PassThru
        [void]$proc.WaitForExit()
        $exitCode = [int]$proc.ExitCode
    } catch {
        $exitCode = 1
        $errorText = $_.Exception.Message
    } finally {
        $newRuns = Get-NewChaosRuns -Before $beforeRuns
        $newDumps = Copy-NewCrashDumps -Before $beforeDumps -Destination (Join-Path $runDir "iteration-$i-crash-dumps")
    }

    if ([string]::IsNullOrWhiteSpace($errorText) -and (Test-Path $stderr)) {
        $errorText = (Get-Content $stderr -Raw -ErrorAction SilentlyContinue)
    }

    $childRun = $null
    $childSummary = $null
    if ($newRuns.Count -gt 0) {
        $childRun = $newRuns[-1].FullName
        $possibleSummary = Join-Path $childRun "summary.json"
        if (Test-Path $possibleSummary) {
            $childSummary = $possibleSummary
        }
    }

    $childFailed = $false
    if ($childRun -and -not $childSummary) {
        $childFailed = $true
    }
    if ([string]::IsNullOrWhiteSpace($errorText) -and $childFailed) {
        $errorText = "Child chaos run did not produce summary.json: $childRun"
    }

    $result = [ordered]@{
        iteration = $i
        ok = ($exitCode -eq 0 -and -not $childFailed -and $newDumps.Count -eq 0)
        exitCode = $exitCode
        startedAt = $started.ToString("o")
        finishedAt = (Get-Date).ToString("o")
        plan = $plan
        replayCommand = $replay
        stdout = $stdout
        stderr = $stderr
        childRuns = @($newRuns | ForEach-Object { $_.FullName })
        childRun = $childRun
        childSummary = $childSummary
        newCrashDumps = @($newDumps | ForEach-Object { $_.FullName })
        errorText = if ($exitCode -eq 0 -and -not $childFailed) { "" } else { $errorText }
    }

    $results += [pscustomobject]$result
    $iterationPath = Join-Path $runDir ("iteration-$i.json")
    $result | ConvertTo-Json -Depth 8 | Set-Content -Path $iterationPath -Encoding UTF8

    if (-not $result.ok -and $StopOnFailure) {
        break
    }
}

$failed = @($results | Where-Object { -not $_.ok })
$summary = [ordered]@{
    ok = ($failed.Count -eq 0)
    seed = $Seed
    seedInt = $seedInt
    iterationsRequested = $Iterations
    iterationsCompleted = $results.Count
    startedAt = $startedAt.ToString("o")
    finishedAt = (Get-Date).ToString("o")
    runDir = $runDir
    includePublisherReloads = [bool]$IncludePublisherReloads
    includeTerminalDataFuzz = [bool]$IncludeTerminalDataFuzz
    quick = [bool]$Quick
    failedIterations = @($failed | ForEach-Object { $_.iteration })
    results = $results
}

$summaryPath = Join-Path $runDir "summary.json"
$summary | ConvertTo-Json -Depth 10 | Set-Content -Path $summaryPath -Encoding UTF8

Write-Output "VDONINJA_SEEDED_CHAOS_PASS=$([int]$summary.ok)"
Write-Output "SUMMARY=$summaryPath"
$summary | ConvertTo-Json -Depth 10

if (-not $summary.ok) {
    exit 1
}
