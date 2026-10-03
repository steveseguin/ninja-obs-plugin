param(
    [string]$IsccPath = "",
    [switch]$RequireCompiler
)

$ErrorActionPreference = "Stop"
$repositoryRoot = Split-Path -Parent $PSScriptRoot
$testRoot = Join-Path $repositoryRoot ("artifacts\windows-installer-tests\" + [guid]::NewGuid().ToString('N'))
$packageRoot = Join-Path $testRoot 'package [test]'
$installerScript = Join-Path $packageRoot 'install.ps1'
$savedAppData = $env:APPDATA
$script:promptResponses = New-Object 'System.Collections.Generic.Queue[string]'
$script:passed = 0

function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

function Write-Fixture([string]$Path, [string]$Content) {
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Path) | Out-Null
    [System.IO.File]::WriteAllText($Path, $Content)
}

function New-ObsFixture([string]$Name) {
    $root = Join-Path $testRoot $Name
    Write-Fixture (Join-Path $root 'bin\64bit\obs64.exe') 'OBS executable sentinel'
    Write-Fixture (Join-Path $root 'bin\64bit\obs.dll') 'OBS core sentinel'
    return $root
}

function Assert-Installed([string]$Root) {
    Assert-True (([System.IO.File]::ReadAllText((Join-Path $Root 'obs-plugins\64bit\obs-vdoninja.dll'))) -eq 'plugin fixture') 'Plugin missing or incorrect'
    Assert-True (([System.IO.File]::ReadAllText((Join-Path $Root 'obs-plugins\64bit\dependency.dll'))) -eq 'dependency fixture') 'Bundled dependency missing'
    Assert-True (([System.IO.File]::ReadAllText((Join-Path $Root 'data\obs-plugins\obs-vdoninja\locale\en-US.ini'))) -eq 'locale fixture') 'Plugin data missing'
    Assert-True (([System.IO.File]::ReadAllText((Join-Path $Root 'bin\64bit\obs64.exe'))) -eq 'OBS executable sentinel') 'OBS executable was changed'
    Assert-True (-not (Test-Path -LiteralPath (Join-Path $Root 'obs-studio'))) 'Unexpected appended obs-studio folder'
}

function Assert-NoPlugin([string]$Root) {
    Assert-True (-not (Test-Path -LiteralPath (Join-Path $Root 'obs-plugins'))) "Unexpected plugin directory: $Root"
    Assert-True (-not (Test-Path -LiteralPath (Join-Path $Root 'data'))) "Unexpected data directory: $Root"
}

# Mock only console answers. The packaged script still detects OBS and performs
# real validation and copying, with every selected destination in this fixture.
function Read-Host {
    param([string]$Prompt)
    # Use the inherited queue: $script: would refer to the invoked installer.
    if ($promptResponses.Count -eq 0) { throw "Unexpected prompt: $Prompt" }
    return $promptResponses.Dequeue()
}

function Invoke-Package([hashtable]$Options, [string[]]$Answers = @(), [string]$ExpectedError = '') {
    $script:promptResponses.Clear()
    foreach ($answer in $Answers) { $script:promptResponses.Enqueue($answer) }
    $failure = ''
    try {
        & $installerScript @Options -NoQuickStartPopup 6>$null
    } catch {
        $failure = $_.Exception.Message
    }
    if ($ExpectedError) {
        Assert-True ($failure -like "*$ExpectedError*") "Expected '$ExpectedError', got '$failure'"
    } else {
        Assert-True (-not $failure) "Scenario $($script:passed + 1) failed: $failure (answers: $($Answers.Count))"
    }
    Assert-True ($script:promptResponses.Count -eq 0) 'Not all confirmation answers were consumed'
    $script:passed++
}

function Invoke-Setup([string]$Root, [bool]$ExpectSuccess) {
    $log = Join-Path $testRoot ("setup-" + [guid]::NewGuid().ToString('N') + '.log')
    $process = Start-Process -FilePath $script:setupExe -ArgumentList @(
        '/CURRENTUSER', '/VERYSILENT', '/SUPPRESSMSGBOXES', '/SP-', '/NORESTART', '/NOCLOSEAPPLICATIONS',
        ('/DIR="{0}"' -f $Root), ('/LOG="{0}"' -f $log)
    ) -WindowStyle Hidden -PassThru
    if (-not $process.WaitForExit(30000)) {
        $process.Kill()
        throw "Installer timed out; see $log"
    }
    $process.Refresh()
    Assert-True (($process.ExitCode -eq 0) -eq $ExpectSuccess) "Unexpected setup exit code $($process.ExitCode); see $log"
    if (-not $ExpectSuccess) {
        Assert-True ((Get-Content -LiteralPath $log -Raw) -match 'OBS Studio was not found') 'Setup failed for a reason other than OBS folder validation'
    }
    $script:passed++
}

try {
    Write-Fixture (Join-Path $packageRoot 'obs-plugins\64bit\obs-vdoninja.dll') 'plugin fixture'
    Write-Fixture (Join-Path $packageRoot 'obs-plugins\64bit\dependency.dll') 'dependency fixture'
    Write-Fixture (Join-Path $packageRoot 'data\obs-plugins\obs-vdoninja\locale\en-US.ini') 'locale fixture'
    foreach ($name in @('LICENSE', 'INSTALL.md', 'QUICKSTART.md', 'README.md', 'THIRD_PARTY_LICENSES.md')) {
        Write-Fixture (Join-Path $packageRoot $name) 'documentation fixture'
    }
    Copy-Item -LiteralPath (Join-Path $repositoryRoot 'scripts\install-package-windows.ps1') -Destination $installerScript
    $env:APPDATA = Join-Path $testRoot 'appdata'

    $custom = New-ObsFixture 'Custom OBS [portable]'
    Invoke-Package @{ ObsRoot = $custom; Yes = $true }
    Assert-Installed $custom

    Write-Fixture (Join-Path $custom 'obs-plugins\64bit\obs-vdoninja.dll') 'old plugin'
    Invoke-Package @{ ObsRoot = $custom; Yes = $true }
    Assert-Installed $custom

    $missing = Join-Path $testRoot 'missing OBS'
    Invoke-Package @{ ObsRoot = $missing; Yes = $true } -ExpectedError 'OBS Studio was not found'
    Assert-True (-not (Test-Path -LiteralPath $missing)) 'Invalid target was created'

    $empty = Join-Path $testRoot 'unrelated folder'
    New-Item -ItemType Directory -Path $empty | Out-Null
    Invoke-Package @{ ObsRoot = $empty; Yes = $true } -ExpectedError 'OBS Studio was not found'
    Assert-NoPlugin $empty

    $bin = Join-Path $custom 'bin\64bit'
    Invoke-Package @{ ObsRoot = $bin; Yes = $true } -ExpectedError 'OBS Studio was not found'
    Assert-NoPlugin $bin

    $incomplete = Join-Path $testRoot 'incomplete OBS'
    Write-Fixture (Join-Path $incomplete 'bin\64bit\obs64.exe') 'OBS executable sentinel'
    Invoke-Package @{ ObsRoot = $incomplete; Yes = $true } -ExpectedError 'OBS Studio was not found'
    Assert-NoPlugin $incomplete

    Invoke-Package @{ Yes = $true } -ExpectedError 'specify -ObsRoot'
    Invoke-Package @{ ObsRoot = $custom; CurrentUser = $true; Yes = $true } -ExpectedError 'Use either'

    $cancelled = New-ObsFixture 'Cancelled OBS'
    Invoke-Package @{ ObsRoot = $cancelled } -Answers @('', 'n')
    Assert-NoPlugin $cancelled
    Invoke-Package @{ ObsRoot = $cancelled } -Answers @('', '')
    Assert-NoPlugin $cancelled

    $chosen = New-ObsFixture 'Chosen OBS'
    Invoke-Package @{ ObsRoot = $cancelled } -Answers @(('"{0}"' -f $chosen), 'y')
    Assert-Installed $chosen
    Assert-NoPlugin $cancelled

    $autoChosen = New-ObsFixture 'Override detected OBS'
    Invoke-Package @{} -Answers @($autoChosen, 'yes')
    Assert-Installed $autoChosen

    Invoke-Package @{ CurrentUser = $true } -Answers @('n')
    Assert-True (-not (Test-Path -LiteralPath $env:APPDATA)) 'Cancelled per-user install created files'
    Invoke-Package @{ CurrentUser = $true; Yes = $true }
    Assert-True (Test-Path -LiteralPath (Join-Path $env:APPDATA 'obs-studio\plugins\obs-vdoninja\bin\64bit\obs-vdoninja.dll')) 'Per-user plugin missing'
    Assert-True (Test-Path -LiteralPath (Join-Path $env:APPDATA 'obs-studio\plugins\obs-vdoninja\data\locale\en-US.ini')) 'Per-user data missing'

    if (-not $IsccPath) {
        $compiler = Get-Command ISCC.exe -ErrorAction SilentlyContinue
        if ($compiler) { $IsccPath = $compiler.Source }
        foreach ($candidate in @(
            "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
            "${env:ProgramFiles}\Inno Setup 6\ISCC.exe",
            "${env:LOCALAPPDATA}\Programs\Inno Setup 6\ISCC.exe"
        )) {
            if (-not $IsccPath -and (Test-Path -LiteralPath $candidate)) { $IsccPath = $candidate }
        }
    }
    if (-not $IsccPath) {
        if ($RequireCompiler) { throw 'Inno Setup compiler is required for setup regression tests' }
        Write-Warning 'Inno Setup not found: compiled setup tests skipped. Use -RequireCompiler in CI.'
    } else {
        # Exercise production code with an inert payload. Disable uninstall
        # registration and previous-path lookup; use a separate app ID as well.
        $source = [System.IO.File]::ReadAllText((Join-Path $repositoryRoot 'packaging\windows\installer-Windows.iss'))
        $source = $source.Replace('[Setup]', "[Setup]`nUninstallable=no`nUsePreviousAppDir=no")
        $source = $source.Replace('AppId={{A95D1933-7F52-44D5-89B2-67FE58DC4C52}', ('AppId=vdoninja-installer-test-' + [guid]::NewGuid().ToString('N')))
        $fixtureScript = Join-Path $testRoot 'test-installer.iss'
        [System.IO.File]::WriteAllText($fixtureScript, $source)
        $compileLog = Join-Path $testRoot 'compile.log'
        & $IsccPath "/DMySourceDir=$packageRoot" "/DMyOutputDir=$testRoot" '/DMyOutputBaseFilename=test-setup' $fixtureScript >$compileLog
        if ($LASTEXITCODE -ne 0) { throw "Inno Setup compile failed; see $compileLog" }
        $script:setupExe = Join-Path $testRoot 'test-setup.exe'

        Invoke-Setup $missing $false
        Assert-True (-not (Test-Path -LiteralPath $missing)) 'Setup created invalid target'
        Invoke-Setup $empty $false
        Assert-NoPlugin $empty
        Invoke-Setup $bin $false
        Assert-NoPlugin $bin
        Invoke-Setup $incomplete $false
        Assert-NoPlugin $incomplete
        $guiTarget = New-ObsFixture 'Setup custom OBS [portable]'
        Invoke-Setup $guiTarget $true
        Assert-Installed $guiTarget
        Write-Fixture (Join-Path $guiTarget 'obs-plugins\64bit\obs-vdoninja.dll') 'old plugin'
        Invoke-Setup $guiTarget $true
        Assert-Installed $guiTarget
        Assert-NoPlugin $cancelled
    }
    Write-Host "PASS: $script:passed Windows package scenarios. Fixtures/logs: $testRoot"
} finally {
    $env:APPDATA = $savedAppData
}
