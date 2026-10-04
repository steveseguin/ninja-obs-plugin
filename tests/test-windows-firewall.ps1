$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '..\scripts\configure-windows-firewall.ps1')

# Exercise the production helper without changing the machine's firewall.
$script:rules = @{}
$script:creates = 0
$script:updates = 0
function Get-NetFirewallRule {
    [CmdletBinding()] param([string]$Name)
    if ($script:rules.ContainsKey($Name)) { return $script:rules[$Name] }
}
function New-NetFirewallRule {
    [CmdletBinding()] param($Name, $DisplayName, $Direction, $Action, $Enabled, $Protocol, $Program, $Profile)
    $script:rules[$Name] = @{} + $PSBoundParameters
    $script:creates++
}
function Set-NetFirewallRule {
    [CmdletBinding()] param($Name, $Direction, $Action, $Enabled, $Protocol, $Program, $Profile)
    $script:rules[$Name] = @{} + $PSBoundParameters
    $script:updates++
}
function Remove-NetFirewallRule {
    [CmdletBinding()] param($Name)
    $script:rules.Remove($Name)
}
function Assert-True($Value, $Message) { if (-not $Value) { throw $Message } }

$root = Join-Path $PSScriptRoot ('..\artifacts\firewall-tests\' + [guid]::NewGuid().ToString('N'))
foreach ($folder in @('OBS [portable]', 'Other OBS')) {
    $directory = New-Item -ItemType Directory -Path (Join-Path $root $folder) -Force
    [IO.File]::WriteAllText((Join-Path $directory.FullName 'obs64.exe'), 'inert fixture')
}
$first = [IO.Path]::GetFullPath((Join-Path $root 'OBS [portable]\obs64.exe'))
$second = [IO.Path]::GetFullPath((Join-Path $root 'Other OBS\obs64.exe'))
Set-VdoNinjaFirewallRule -Program $first -NetworkProfiles Private
$rule = @($script:rules.Values)[0]
Assert-True ($rule.Program -ceq $first) 'The exact selected executable path was not preserved'
Assert-True ($rule.Direction -eq 'Inbound' -and $rule.Protocol -eq 'UDP' -and $rule.Action -eq 'Allow') 'Rule is not restricted to inbound UDP'
Assert-True (($rule.Profile -join ',') -eq 'Private') 'Private opt-in allowed another profile'
Set-VdoNinjaFirewallRule -Program $first -NetworkProfiles 'Private,Public'
Assert-True ($script:creates -eq 1 -and $script:updates -eq 1 -and $script:rules.Count -eq 1) 'Reinstall duplicated its rule'
Assert-True ((@($script:rules.Values)[0].Profile -join ',') -eq 'Private,Public') 'Profile update failed'
Set-VdoNinjaFirewallRule -Program $second -NetworkProfiles Public
Assert-True ($script:rules.Count -eq 2) 'Different OBS installations share a rule'
Set-VdoNinjaFirewallRule -Program $first -Remove
Assert-True ($script:rules.Count -eq 1 -and @($script:rules.Values)[0].Program -eq $second) 'Uninstall changed another installation'
Set-VdoNinjaFirewallRule -Program $first -Remove
Assert-True ($script:rules.Count -eq 1) 'Repeated uninstall changed another rule'
$rejected = $false
try { Set-VdoNinjaFirewallRule -Program (Join-Path $root 'unrelated.exe') } catch { $rejected = $true }
Assert-True $rejected 'Unexpected executable was accepted'
Write-Host 'PASS: firewall path, protocol, profile, upgrade, and removal checks (mocked Windows firewall).'
