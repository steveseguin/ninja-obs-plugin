param(
    [string]$Repo = "steveseguin/ninja-obs-plugin",
    [string]$CodeSigningRepo = "$env:USERPROFILE\Code\code-signing",
    [switch]$SetBundlePassphrase,
    [string]$BundlePassphrase = ""
)

$ErrorActionPreference = "Stop"

function Require-Command {
    param([string]$Name)
    if (-not (Get-Command $Name -ErrorAction SilentlyContinue)) {
        throw "Required command not found: $Name"
    }
}

function Parse-EnvFile {
    param([string]$Path)
    $result = @{}
    Get-Content $Path | ForEach-Object {
        $line = $_.Trim()
        if (-not $line -or $line.StartsWith("#")) {
            return
        }
        if ($line -notmatch "^[A-Za-z_][A-Za-z0-9_]*=") {
            return
        }
        $parts = $line.Split("=", 2)
        if ($parts.Count -eq 2) {
            $result[$parts[0]] = $parts[1]
        }
    }
    return $result
}

function Set-RepoSecret {
    param(
        [string]$RepoName,
        [string]$SecretName,
        [string]$SecretValue
    )
    if ([string]::IsNullOrWhiteSpace($SecretValue)) {
        throw "Refusing to set empty secret: $SecretName"
    }

    $SecretValue | gh secret set $SecretName --repo $RepoName
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to set secret '$SecretName' for '$RepoName'."
    }
}

Require-Command -Name "gh"

$configPath = Join-Path $CodeSigningRepo "secrets\decrypted\build-config.env"
$pfxPath = Join-Path $CodeSigningRepo "secrets\decrypted\certs\socialstream.pfx"

if (-not (Test-Path $configPath)) {
    throw "Missing config file: $configPath"
}
if (-not (Test-Path $pfxPath)) {
    throw "Missing signing certificate: $pfxPath"
}

$config = Parse-EnvFile -Path $configPath

if (-not $config.ContainsKey("WIN_CSC_KEY_PASSWORD") -or
    [string]::IsNullOrWhiteSpace($config["WIN_CSC_KEY_PASSWORD"])) {
    throw "WIN_CSC_KEY_PASSWORD missing in $configPath"
}

Write-Host "Syncing release secrets to $Repo ..."
Set-RepoSecret -RepoName $Repo -SecretName "WIN_CSC_KEY_PASSWORD" -SecretValue $config["WIN_CSC_KEY_PASSWORD"]

$certB64 = [Convert]::ToBase64String([System.IO.File]::ReadAllBytes($pfxPath))
Set-RepoSecret -RepoName $Repo -SecretName "WIN_SIGN_CERT_B64" -SecretValue $certB64

if ($config.ContainsKey("VT_API_KEY") -and -not [string]::IsNullOrWhiteSpace($config["VT_API_KEY"])) {
    Set-RepoSecret -RepoName $Repo -SecretName "VT_API_KEY" -SecretValue $config["VT_API_KEY"]
    Write-Host "Set: WIN_CSC_KEY_PASSWORD, WIN_SIGN_CERT_B64, VT_API_KEY"
} else {
    Write-Warning "VT_API_KEY not found in build-config.env; VirusTotal step will be skipped."
    Write-Host "Set: WIN_CSC_KEY_PASSWORD, WIN_SIGN_CERT_B64"
}

if ($SetBundlePassphrase) {
    if ([string]::IsNullOrWhiteSpace($BundlePassphrase)) {
        throw "SetBundlePassphrase specified but BundlePassphrase is empty."
    }
    Set-RepoSecret -RepoName $Repo -SecretName "CODESIGN_BUNDLE_PASSPHRASE" -SecretValue $BundlePassphrase
    Write-Host "Set: CODESIGN_BUNDLE_PASSPHRASE"
}

Write-Host ""
Write-Host "Current secrets:"
gh secret list --repo $Repo
