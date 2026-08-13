$ErrorActionPreference = "Stop"

$helperPath = Join-Path (Split-Path -Parent $PSScriptRoot) "scripts\redirected-process-output.ps1"
. $helperPath

$lineCount = 2000
$childScript = @"
1..$lineCount | ForEach-Object {
    [Console]::Out.WriteLine('stdout-{0:D4}-xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx' -f `$_)
    [Console]::Error.WriteLine('stderr-{0:D4}-xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx' -f `$_)
}
"@
$encodedChildScript = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($childScript))
$startInfo = [System.Diagnostics.ProcessStartInfo]::new()
$startInfo.FileName = "powershell.exe"
$startInfo.Arguments = "-NoProfile -EncodedCommand $encodedChildScript"
$startInfo.UseShellExecute = $false
$startInfo.CreateNoWindow = $true
$startInfo.RedirectStandardOutput = $true
$startInfo.RedirectStandardError = $true

$process = [System.Diagnostics.Process]::new()
$process.StartInfo = $startInfo
[void]$process.Start()
$drain = Start-RedirectedProcessOutputDrain -Process $process

if (-not $process.WaitForExit(15000)) {
    try { $process.Kill() } catch {}
    throw "Redirected high-volume child process deadlocked"
}

$output = Complete-RedirectedProcessOutputDrain -Drain $drain
if ($process.ExitCode -ne 0) {
    throw "Redirected child failed with exit code $($process.ExitCode): $($output.stderr)"
}
if (($output.stdout -split "`r?`n" | Where-Object { $_ }).Count -ne $lineCount) {
    throw "Did not drain all redirected stdout lines"
}
if (($output.stderr -split "`r?`n" | Where-Object { $_ }).Count -ne $lineCount) {
    throw "Did not drain all redirected stderr lines"
}

Write-Output "Redirected process output regression passed ($lineCount stdout and stderr lines)"
