$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '../scripts/test-alpha-validation-common.ps1')

$artifacts = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../artifacts'))
$fixture = Join-Path $artifacts ('alpha-helper-test-' + [Guid]::NewGuid().ToString('N'))
$package = Join-Path $fixture 'package'
$dllRelative = 'obs-plugins/64bit/obs-vdoninja.dll'
$dataRelative = 'data/obs-plugins/obs-vdoninja/locale/en-US.ini'

function Write-FixtureFile([string]$Path, [string]$Content) {
    New-Item -ItemType Directory -Force -Path (Split-Path $Path -Parent) | Out-Null
    [IO.File]::WriteAllText($Path, $Content)
}
function Assert-Content([string]$Path, [string]$Expected) {
    if ([IO.File]::ReadAllText($Path) -cne $Expected) { throw "Unexpected contents: $Path" }
}

try {
    $installedDll = Join-Path $fixture "_obs-portable/$dllRelative"
    $installedData = Join-Path $fixture "_obs-portable/$dataRelative"
    Write-FixtureFile (Join-Path $package $dllRelative) 'new DLL'
    Write-FixtureFile $installedDll 'old DLL'
    Write-FixtureFile $installedData 'old locale'

    # Validate the entire input before touching either installed copy.
    $rejected = $false
    try { Sync-PortableObsPluginPayload -RepoRoot $fixture -InstallPrefixPath $package }
    catch {
        if ($_.Exception.Message -notlike '*requires module data*') { throw }
        $rejected = $true
    }
    if (-not $rejected) { throw 'Incomplete package was accepted' }
    Assert-Content $installedDll 'old DLL'

    # Preserve evidence in another runtime even when files share NTFS hardlinks.
    $archivedDll = Join-Path $fixture 'archived.dll'
    $archivedData = Join-Path $fixture 'archived.ini'
    New-Item -ItemType HardLink -Path $archivedDll -Target $installedDll | Out-Null
    New-Item -ItemType HardLink -Path $archivedData -Target $installedData | Out-Null
    Write-FixtureFile (Join-Path $package $dataRelative) 'new locale'
    Sync-PortableObsPluginPayload -RepoRoot $fixture -InstallPrefixPath $package

    Assert-Content $installedDll 'new DLL'
    Assert-Content $installedData 'new locale'
    Assert-Content (Join-Path $fixture '_obs-portable/config/obs-studio/plugins/obs-vdoninja/bin/64bit/obs-vdoninja.dll') 'new DLL'
    Assert-Content (Join-Path $fixture '_obs-portable/config/obs-studio/plugins/obs-vdoninja/data/locale/en-US.ini') 'new locale'
    Assert-Content $archivedDll 'old DLL'
    Assert-Content $archivedData 'old locale'

    # A staged DLL alone is not proof of what the running OBS loaded.
    function Get-Process {
        param($Id, $ErrorAction)
        [pscustomobject]@{ Modules = $script:testModules }
    }
    $goodModule = [pscustomobject]@{ ModuleName = 'obs-vdoninja.dll'; FileName = $installedDll }
    $oldModule = [pscustomobject]@{ ModuleName = 'obs-vdoninja.dll'; FileName = $archivedDll }
    $script:testModules = @($goodModule)
    $evidence = Get-TestObsPluginEvidence -ObsProcessId 123 -InstallPrefixPath $package
    if ($evidence.path -ne $installedDll) { throw 'Loaded module path was not recorded' }
    foreach ($modules in @(@(), @($goodModule, $goodModule), @($oldModule))) {
        $script:testModules = $modules
        $rejected = $false
        try { Get-TestObsPluginEvidence -ObsProcessId 123 -InstallPrefixPath $package | Out-Null }
        catch {
            if ($_.Exception.Message -notmatch 'exactly one|different plugin') { throw }
            $rejected = $true
        }
        if (-not $rejected) { throw 'Missing, duplicate or stale loaded DLL was accepted' }
    }

    # Cleanup must leave a different run's Chrome process alone.
    function Get-CimInstance {
        @(
            [pscustomobject]@{ ProcessId = 101; CommandLine = 'chrome --user-data-dir=C:\test\owned-profile' },
            [pscustomobject]@{ ProcessId = 102; CommandLine = 'chrome --user-data-dir=C:\test\unrelated-profile' }
        )
    }
    $script:stoppedIds = @()
    function Stop-Process { param($Id, [switch]$Force, $ErrorAction) $script:stoppedIds += $Id }
    Stop-AlphaMotionDemoProcesses -ProfilePath 'C:\test\owned-profile'
    if (($script:stoppedIds -join ',') -ne '101') { throw 'Chrome cleanup affected another run' }
    Write-Output 'Portable OBS payload and scoped cleanup tests passed'
} finally {
    $target = [IO.Path]::GetFullPath($fixture)
    if (-not $target.StartsWith($artifacts + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Unsafe test cleanup path: $target"
    }
    if (Test-Path -LiteralPath $target) { Remove-Item -LiteralPath $target -Recurse -Force }
}
