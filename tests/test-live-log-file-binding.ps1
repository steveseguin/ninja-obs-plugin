$ErrorActionPreference = "Stop"

$driverPath = Join-Path (Split-Path -Parent $PSScriptRoot) "scripts\run-vdoninja-gamecapture-spout-smoke.ps1"
$driverText = Get-Content -LiteralPath $driverPath -Raw
$functionMatch = [regex]::Match(
    $driverText,
    '(?ms)^function Get-FileBinding \{.*?^\}'
)
if (-not $functionMatch.Success) {
    throw "Get-FileBinding was not found in the driver"
}
Invoke-Expression $functionMatch.Value

$tempPath = Join-Path ([IO.Path]::GetTempPath()) ("vdoninja-live-log-{0}.txt" -f [guid]::NewGuid())
$writer = [IO.File]::Open($tempPath, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::Read)
try {
    $bytes = [Text.Encoding]::UTF8.GetBytes("live redirected log evidence`n")
    $writer.Write($bytes, 0, $bytes.Length)
    $writer.Flush()

    $binding = Get-FileBinding -Path $tempPath
    if (-not $binding -or $binding.sha256 -notmatch '^[0-9a-f]{64}$') {
        throw "A live redirected log did not produce a valid file binding"
    }
} finally {
    $writer.Dispose()
    Remove-Item -LiteralPath $tempPath -Force -ErrorAction SilentlyContinue
}

Write-Output "Live redirected log file-binding regression passed"
