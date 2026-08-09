[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$SpecPath
)

$ErrorActionPreference = "Stop"

function Get-Sha256 {
    param([Parameter(Mandatory = $true)][string]$Path)
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Get-TextSha256 {
    param([Parameter(Mandatory = $true)][string]$Text)
    $hasher = [Security.Cryptography.SHA256]::Create()
    try {
        $hashBytes = $hasher.ComputeHash([Text.Encoding]::UTF8.GetBytes($Text))
        return ([BitConverter]::ToString($hashBytes) -replace '-', '').ToLowerInvariant()
    } finally {
        $hasher.Dispose()
    }
}

function Test-PathWithin {
    param(
        [Parameter(Mandatory = $true)][string]$Candidate,
        [Parameter(Mandatory = $true)][string]$Parent
    )
    $candidateFull = [System.IO.Path]::GetFullPath($Candidate)
    $parentFull = [System.IO.Path]::GetFullPath($Parent).TrimEnd('\') + '\'
    return $candidateFull.StartsWith($parentFull, [System.StringComparison]::OrdinalIgnoreCase)
}

function Quote-ProcessArgument {
    param([AllowNull()][string]$Argument)
    if ($null -eq $Argument) {
        return '""'
    }
    return '"' + ($Argument -replace '"', '\"') + '"'
}

$resolvedSpecPath = (Resolve-Path -LiteralPath $SpecPath).Path
$specBytes = [System.IO.File]::ReadAllBytes($resolvedSpecPath)
$spec = [Text.Encoding]::UTF8.GetString($specBytes) | ConvertFrom-Json
$resultPath = [System.IO.Path]::GetFullPath([string]$spec.resultPath)
$result = [ordered]@{
    schema = "game-capture-alpha-publisher-restart-v1"
    ok = $false
    error = $null
    startedAt = (Get-Date).ToUniversalTime().ToString("o")
    completedAt = $null
    spec = [ordered]@{
        path = $resolvedSpecPath
        sha256 = (Get-Sha256 -Path $resolvedSpecPath)
    }
    executable = $null
    argumentSha256 = $null
    oldPublisher = $null
    newPublisher = $null
    oldDiscovery = $null
    newDiscovery = $null
}
$newProcess = $null

try {
    $runDir = [System.IO.Path]::GetFullPath([string]$spec.runDir)
    $executablePath = (Resolve-Path -LiteralPath ([string]$spec.executablePath)).Path
    $workingDirectory = (Resolve-Path -LiteralPath ([string]$spec.workingDirectory)).Path
    $discoveryPath = [System.IO.Path]::GetFullPath([string]$spec.discoveryPath)
    $stdoutPath = [System.IO.Path]::GetFullPath([string]$spec.stdoutPath)
    $stderrPath = [System.IO.Path]::GetFullPath([string]$spec.stderrPath)
    foreach ($ownedPath in @($discoveryPath, $stdoutPath, $stderrPath, $resultPath)) {
        if (-not (Test-PathWithin -Candidate $ownedPath -Parent $runDir)) {
            throw "Restart-owned path is outside the run directory: $ownedPath"
        }
    }

    $expectedSha256 = ([string]$spec.expectedSha256).Trim().ToLowerInvariant()
    if ($expectedSha256 -notmatch '^[0-9a-f]{64}$') {
        throw "expectedSha256 is missing or invalid"
    }
    $actualSha256 = Get-Sha256 -Path $executablePath
    if ($actualSha256 -ne $expectedSha256) {
        throw "Publisher executable hash does not match the restart specification"
    }
    $arguments = @($spec.arguments | ForEach-Object { [string]$_ })
    if ($arguments.Count -eq 0) {
        throw "Publisher restart specification has no arguments"
    }
    $argumentJson = ConvertTo-Json -InputObject $arguments -Compress
    $argumentLine = ($arguments | ForEach-Object { Quote-ProcessArgument $_ }) -join " "
    $argumentSha256 = Get-TextSha256 -Text $argumentJson
    $result.executable = [ordered]@{ path = $executablePath; sha256 = $actualSha256 }
    $result.argumentSha256 = $argumentSha256

    $oldPid = [int]$spec.oldPid
    $oldProcess = Get-CimInstance Win32_Process -Filter "ProcessId = $oldPid" -ErrorAction Stop
    if (-not $oldProcess -or -not $oldProcess.ExecutablePath -or
        [System.IO.Path]::GetFullPath([string]$oldProcess.ExecutablePath) -ine $executablePath) {
        throw "The requested old publisher PID does not run the bound executable"
    }
    $result.oldPublisher = [ordered]@{
        pid = $oldPid
        path = [System.IO.Path]::GetFullPath([string]$oldProcess.ExecutablePath)
        sha256 = $actualSha256
    }

    if (-not (Test-Path -LiteralPath $discoveryPath -PathType Leaf)) {
        throw "Old control discovery file is missing"
    }
    $oldDiscoveryText = Get-Content -LiteralPath $discoveryPath -Raw
    $oldDiscovery = $oldDiscoveryText | ConvertFrom-Json
    if ([int]$oldDiscovery.pid -ne $oldPid) {
        throw "Old control discovery PID does not match the process being restarted"
    }
    $result.oldDiscovery = [ordered]@{
        path = $discoveryPath
        pid = [int]$oldDiscovery.pid
        baseUrl = [string]$oldDiscovery.base_url
        sha256 = Get-TextSha256 -Text $oldDiscoveryText
    }

    Stop-Process -Id $oldPid -Force -ErrorAction Stop
    Wait-Process -Id $oldPid -Timeout 15 -ErrorAction SilentlyContinue
    if (Get-Process -Id $oldPid -ErrorAction SilentlyContinue) {
        throw "Old publisher PID $oldPid did not exit"
    }
    if (Test-Path -LiteralPath $discoveryPath -PathType Leaf) {
        Remove-Item -LiteralPath $discoveryPath -Force
    }

    $newProcess = Start-Process -FilePath $executablePath `
        -ArgumentList $argumentLine `
        -WorkingDirectory $workingDirectory `
        -RedirectStandardOutput $stdoutPath `
        -RedirectStandardError $stderrPath `
        -WindowStyle Hidden `
        -PassThru
    $result.newPublisher = [ordered]@{
        pid = $newProcess.Id
        path = $executablePath
        sha256 = (Get-Sha256 -Path $executablePath)
    }
    if ($newProcess.Id -eq $oldPid) {
        throw "Restart reused the old publisher PID"
    }

    $deadline = (Get-Date).AddSeconds([Math]::Max(10, [int]$spec.discoveryTimeoutSeconds))
    $newDiscovery = $null
    $newDiscoveryText = $null
    while ((Get-Date) -lt $deadline) {
        $newProcess.Refresh()
        if ($newProcess.HasExited) {
            throw "Restarted publisher exited early with code $($newProcess.ExitCode)"
        }
        if (Test-Path -LiteralPath $discoveryPath -PathType Leaf) {
            try {
                $candidateText = Get-Content -LiteralPath $discoveryPath -Raw
                $candidate = $candidateText | ConvertFrom-Json
                if ([int]$candidate.pid -eq $newProcess.Id -and $candidate.base_url -and $candidate.token) {
                    $health = Invoke-RestMethod -Uri "$($candidate.base_url)/health" -TimeoutSec 2
                    if ([bool]$health.ok -and [int]$health.pid -eq $newProcess.Id) {
                        $newDiscovery = $candidate
                        $newDiscoveryText = $candidateText
                        break
                    }
                }
            } catch {
            }
        }
        Start-Sleep -Milliseconds 100
    }
    if (-not $newDiscovery) {
        throw "Restarted publisher did not publish a healthy new control discovery identity"
    }
    $newDiscoverySha256 = Get-TextSha256 -Text $newDiscoveryText
    if ($newDiscoverySha256 -eq [string]$result.oldDiscovery.sha256) {
        throw "Restarted publisher reused the old discovery content"
    }
    $result.newDiscovery = [ordered]@{
        path = $discoveryPath
        pid = [int]$newDiscovery.pid
        baseUrl = [string]$newDiscovery.base_url
        sha256 = $newDiscoverySha256
        tokenSha256 = Get-TextSha256 -Text ([string]$newDiscovery.token)
    }
    $result.ok = $true
} catch {
    $result.error = $_.Exception.Message
} finally {
    if (-not $result.ok -and $newProcess) {
        $newProcess.Refresh()
        if (-not $newProcess.HasExited) {
            Stop-Process -Id $newProcess.Id -Force -ErrorAction SilentlyContinue
            Wait-Process -Id $newProcess.Id -Timeout 10 -ErrorAction SilentlyContinue
        }
    }
    $result.completedAt = (Get-Date).ToUniversalTime().ToString("o")
    $resultParent = Split-Path -Parent $resultPath
    if ($resultParent) {
        New-Item -ItemType Directory -Path $resultParent -Force | Out-Null
    }
    $result | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $resultPath -Encoding UTF8
    $result | ConvertTo-Json -Depth 12
}

if (-not $result.ok) {
    exit 1
}
