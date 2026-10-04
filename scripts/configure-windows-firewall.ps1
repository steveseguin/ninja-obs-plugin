param(
    [string]$ObsExe = '',
    [ValidateSet('None', 'Add', 'Remove')][string]$Action = 'None',
    [ValidateSet('Private', 'Public', 'Private,Public')][string]$Profiles = 'Private'
)

$ErrorActionPreference = 'Stop'

function Set-VdoNinjaFirewallRule {
    param(
        [Parameter(Mandatory)][string]$Program,
        [ValidateSet('Private', 'Public', 'Private,Public')][string]$NetworkProfiles = 'Private',
        [switch]$Remove
    )

    $programPath = [System.IO.Path]::GetFullPath($Program)
    if ([System.IO.Path]::GetFileName($programPath) -ine 'obs64.exe') {
        throw 'Firewall access must target the selected obs64.exe.'
    }
    if (-not $Remove -and -not (Test-Path -LiteralPath $programPath -PathType Leaf)) {
        throw "OBS executable not found: $programPath"
    }
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $hash = [BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($programPath.ToLowerInvariant()))).Replace('-', '')
    } finally { $sha.Dispose() }
    $ruleName = 'VDONinja-OBS-UDP-' + $hash
    $existing = Get-NetFirewallRule -Name $ruleName -ErrorAction SilentlyContinue
    if ($Remove) {
        if ($existing) { Remove-NetFirewallRule -Name $ruleName -ErrorAction Stop }
        return
    }
    $rule = @{
        Direction = 'Inbound'; Action = 'Allow'; Enabled = 'True'; Protocol = 'UDP'
        Program = $programPath; Profile = $NetworkProfiles.Split(','); ErrorAction = 'Stop'
    }
    if ($existing) {
        Set-NetFirewallRule -Name $ruleName @rule | Out-Null
    } else {
        New-NetFirewallRule -Name $ruleName -DisplayName 'OBS VDO.Ninja peer-to-peer UDP' @rule | Out-Null
    }
}

if ($Action -ne 'None') {
    Set-VdoNinjaFirewallRule -Program $ObsExe -NetworkProfiles $Profiles -Remove:($Action -eq 'Remove')
}
