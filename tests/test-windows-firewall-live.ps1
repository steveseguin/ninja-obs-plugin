$ErrorActionPreference = 'Stop'
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = New-Object Security.Principal.WindowsPrincipal($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'The live firewall integration check requires an elevated Windows runner.'
}

$repoRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $repoRoot 'scripts\configure-windows-firewall.ps1')
$fixtureRoot = Join-Path $repoRoot ('artifacts\firewall-live-tests\' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $fixtureRoot -Force | Out-Null
$program = Join-Path $fixtureRoot 'obs64.exe'
# An inert file validates the real application-scoped firewall API without
# granting network access to any running OBS installation.
[IO.File]::WriteAllText($program, 'Firewall integration fixture; not executable.')
$sha = [Security.Cryptography.SHA256]::Create()
try {
    $hash = [BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($program.ToLowerInvariant()))).Replace('-', '')
} finally { $sha.Dispose() }
$ruleName = 'VDONinja-OBS-UDP-' + $hash

try {
    Set-VdoNinjaFirewallRule -Program $program -NetworkProfiles Private
    $rule = Get-NetFirewallRule -Name $ruleName -ErrorAction Stop
    $application = $rule | Get-NetFirewallApplicationFilter
    $ports = $rule | Get-NetFirewallPortFilter
    if ($application.Program -ine $program -or $ports.Protocol -notin @('UDP', '17') -or
        $rule.Direction -ne 'Inbound' -or $rule.Action -ne 'Allow' -or $rule.Profile -ne 'Private') {
        throw 'The installed firewall rule does not match the selected application, protocol, or profile.'
    }
    Set-VdoNinjaFirewallRule -Program $program -NetworkProfiles 'Private,Public'
    $rules = @(Get-NetFirewallRule -Name $ruleName -ErrorAction Stop)
    if ($rules.Count -ne 1 -or [string]$rules[0].Profile -notmatch 'Private' -or [string]$rules[0].Profile -notmatch 'Public') {
        throw 'Firewall profile update did not preserve exactly one scoped rule.'
    }
    Set-VdoNinjaFirewallRule -Program $program -Remove
    if (Get-NetFirewallRule -Name $ruleName -ErrorAction SilentlyContinue) {
        throw 'Firewall rule removal failed.'
    }
    Write-Host 'PASS: real Windows firewall add, update, application/UDP/profile filters, and removal.'
} finally {
    if (Get-NetFirewallRule -Name $ruleName -ErrorAction SilentlyContinue) {
        Remove-NetFirewallRule -Name $ruleName -ErrorAction Stop
    }
}
