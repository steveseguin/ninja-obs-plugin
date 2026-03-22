param(
    [ValidateSet("status", "prepare", "verify", "verify-metadata", "notes", "cut")]
    [string]$Action = "status",
    [string]$Version = "",
    [ValidateSet("patch", "minor", "major")]
    [string]$Bump = "patch",
    [string]$Date = (Get-Date -Format "yyyy-MM-dd"),
    [string]$BuildDir = "build-release-tests",
    [string]$OutputPath = "",
    [switch]$SkipFormat,
    [switch]$SkipTests,
    [switch]$SkipGitDiff,
    [switch]$AllowDirty,
    [switch]$Push,
    [switch]$Force
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
Set-Location $repoRoot

function Read-TextFile {
    param([string]$Path)

    if (-not (Test-Path $Path)) {
        throw "Missing file: $Path"
    }

    return [System.IO.File]::ReadAllText((Resolve-Path $Path))
}

function Write-TextFile {
    param(
        [string]$Path,
        [string]$Content
    )

    $directory = Split-Path -Parent $Path
    if ($directory) {
        New-Item -ItemType Directory -Force -Path $directory | Out-Null
    }

    $encoding = [System.Text.UTF8Encoding]::new($false)
    $resolvedPath = if ([System.IO.Path]::IsPathRooted($Path)) { $Path } else { (Join-Path $repoRoot $Path) }
    [System.IO.File]::WriteAllText($resolvedPath, $Content, $encoding)
}

function Get-RegexValue {
    param(
        [string]$Text,
        [string]$Pattern,
        [string]$Label
    )

    $match = [System.Text.RegularExpressions.Regex]::Match(
        $Text,
        $Pattern,
        [System.Text.RegularExpressions.RegexOptions]::Multiline
    )
    if (-not $match.Success) {
        throw "Unable to parse $Label."
    }

    return $match.Groups[1].Value
}

function Normalize-ReleaseVersion {
    param([string]$Value)

    if ([string]::IsNullOrWhiteSpace($Value)) {
        return $null
    }

    $normalized = $Value.Trim()
    if ($normalized.StartsWith("v")) {
        $normalized = $normalized.Substring(1)
    }

    $match = [System.Text.RegularExpressions.Regex]::Match(
        $normalized,
        "^(?<core>\d+\.\d+\.\d+)(?<suffix>[-+][0-9A-Za-z.-]+)?$"
    )
    if (-not $match.Success) {
        throw "Version '$Value' must be semver-like, for example 1.2.3 or 1.2.3-rc.1."
    }

    return [PSCustomObject]@{
        Raw          = $normalized
        Core         = $match.Groups["core"].Value
        Suffix       = $match.Groups["suffix"].Value
        IsPrerelease = -not [string]::IsNullOrEmpty($match.Groups["suffix"].Value)
        Tag          = "v$normalized"
    }
}

function Get-NextVersion {
    param(
        [string]$BaseVersion,
        [string]$BumpKind
    )

    $baseInfo = Normalize-ReleaseVersion $BaseVersion
    $parts = $baseInfo.Core.Split(".") | ForEach-Object { [int]$_ }
    $major = $parts[0]
    $minor = $parts[1]
    $patch = $parts[2]

    switch ($BumpKind) {
        "major" {
            $major += 1
            $minor = 0
            $patch = 0
        }
        "minor" {
            $minor += 1
            $patch = 0
        }
        "patch" {
            $patch += 1
        }
        default {
            throw "Unsupported bump kind: $BumpKind"
        }
    }

    return "$major.$minor.$patch"
}

function Invoke-Git {
    param(
        [Parameter(ValueFromRemainingArguments = $true)]
        [string[]]$Arguments
    )

    $output = & git @Arguments 2>&1 | ForEach-Object { "$_" }
    if ($LASTEXITCODE -ne 0) {
        $joined = ($Arguments -join " ")
        throw "git $joined failed.`n$($output -join "`n")"
    }

    return $output
}

function Get-LatestTag {
    $tags = Invoke-Git tag --list "v*" --sort=-version:refname
    if (-not $tags) {
        return $null
    }

    foreach ($line in $tags) {
        if (-not [string]::IsNullOrWhiteSpace($line)) {
            return $line.Trim()
        }
    }

    return $null
}

function Get-VersionState {
    $cmakeText = Read-TextFile "CMakeLists.txt"
    $pluginText = Read-TextFile "src/plugin-main.h"
    $packageJsonText = Read-TextFile "package.json"
    $packageLockText = Read-TextFile "package-lock.json"

    $cmakeVersion = Get-RegexValue $cmakeText "project\(obs-vdoninja VERSION ([0-9]+\.[0-9]+\.[0-9]+)" "CMake version"
    $pluginVersion = Get-RegexValue $pluginText '#define PLUGIN_VERSION "([^"]+)"' "plugin version"
    $packageJsonVersion = Get-RegexValue $packageJsonText '"version"\s*:\s*"([^"]+)"' "package.json version"
    $packageLockVersion = Get-RegexValue $packageLockText '(?s)^\s*\{\s*"name"\s*:\s*"[^"]+"\s*,\s*"version"\s*:\s*"([^"]+)"' "package-lock.json version"
    $packageLockRootVersion = Get-RegexValue $packageLockText '(?s)"packages"\s*:\s*\{\s*""\s*:\s*\{\s*"name"\s*:\s*"[^"]+"\s*,\s*"version"\s*:\s*"([^"]+)"' "package-lock.json root package version"

    return [PSCustomObject]@{
        CMakeVersion           = $cmakeVersion
        PluginVersion          = $pluginVersion
        PackageJsonVersion     = $packageJsonVersion
        PackageLockVersion     = $packageLockVersion
        PackageLockRootVersion = $packageLockRootVersion
        LatestTag              = Get-LatestTag
    }
}

function Get-CurrentSourceVersionInfo {
    $state = Get-VersionState
    return Normalize-ReleaseVersion $state.CMakeVersion
}

function Get-ReleaseTargetVersionInfo {
    if (-not [string]::IsNullOrWhiteSpace($Version)) {
        return Normalize-ReleaseVersion $Version
    }

    $latestTag = Get-LatestTag
    if ($latestTag) {
        return Normalize-ReleaseVersion (Get-NextVersion -BaseVersion $latestTag -BumpKind $Bump)
    }

    return Get-CurrentSourceVersionInfo
}

function Get-ChangelogSection {
    param(
        [string]$Heading,
        [string]$Content,
        [switch]$Required
    )

    if ([string]::IsNullOrEmpty($Content)) {
        $Content = Read-TextFile "CHANGELOG.md"
    }
    $headingPattern = "^## \[$([System.Text.RegularExpressions.Regex]::Escape($Heading))\](?: - (?<date>\d{4}-\d{2}-\d{2}))?\s*$"
    $match = [System.Text.RegularExpressions.Regex]::Match(
        $content,
        $headingPattern,
        [System.Text.RegularExpressions.RegexOptions]::Multiline
    )

    if (-not $match.Success) {
        if ($Required) {
            throw "Missing changelog heading for [$Heading]."
        }
        return $null
    }

    $bodyStart = $match.Index + $match.Length
    $remaining = $content.Substring($bodyStart)
    $nextMatch = [System.Text.RegularExpressions.Regex]::Match(
        $remaining,
        "^## \[",
        [System.Text.RegularExpressions.RegexOptions]::Multiline
    )

    $bodyEnd = if ($nextMatch.Success) { $bodyStart + $nextMatch.Index } else { $content.Length }
    $body = $content.Substring($bodyStart, $bodyEnd - $bodyStart).Trim([char[]]@("`r", "`n"))

    return [PSCustomObject]@{
        Heading = $Heading
        Date    = $match.Groups["date"].Value
        Start   = $match.Index
        End     = $bodyEnd
        Body    = $body
    }
}

function Get-ReleaseChangelogSection {
    param(
        [PSCustomObject]$VersionInfo,
        [string]$Content
    )

    $candidates = @($VersionInfo.Raw)
    if ($VersionInfo.Raw -ne $VersionInfo.Core) {
        $candidates += $VersionInfo.Core
    }

    foreach ($candidate in $candidates) {
        $section = Get-ChangelogSection -Heading $candidate -Content $Content
        if ($section) {
            return $section
        }
    }

    return $null
}

function Get-UnreleasedSection {
    param([string]$Content)
    return Get-ChangelogSection -Heading "Unreleased" -Content $Content -Required
}

function Assert-RegexMatches {
    param(
        [string]$Text,
        [string]$Pattern,
        [string]$Label
    )

    if (-not [System.Text.RegularExpressions.Regex]::IsMatch($Text, $Pattern, [System.Text.RegularExpressions.RegexOptions]::Multiline)) {
        throw "$Label does not contain the expected version pattern."
    }
}

function Set-SourceVersions {
    param([string]$CoreVersion)

    $cmakePattern = "project\(obs-vdoninja VERSION [0-9]+\.[0-9]+\.[0-9]+"
    $cmakeText = Read-TextFile "CMakeLists.txt"
    Assert-RegexMatches -Text $cmakeText -Pattern $cmakePattern -Label "CMakeLists.txt"
    $updatedCMake = [System.Text.RegularExpressions.Regex]::Replace(
        $cmakeText,
        $cmakePattern,
        "project(obs-vdoninja VERSION $CoreVersion"
    )
    Write-TextFile -Path "CMakeLists.txt" -Content $updatedCMake

    $pluginPattern = '#define PLUGIN_VERSION "[^"]+"'
    $pluginText = Read-TextFile "src/plugin-main.h"
    Assert-RegexMatches -Text $pluginText -Pattern $pluginPattern -Label "src/plugin-main.h"
    $updatedPlugin = [System.Text.RegularExpressions.Regex]::Replace(
        $pluginText,
        $pluginPattern,
        "#define PLUGIN_VERSION `"$CoreVersion`""
    )
    Write-TextFile -Path "src/plugin-main.h" -Content $updatedPlugin

    $packageJsonPattern = [regex]'"version"\s*:\s*"[^"]+"'
    $packageJsonText = Read-TextFile "package.json"
    if (-not $packageJsonPattern.IsMatch($packageJsonText)) {
        throw "package.json does not contain the expected version pattern."
    }
    $updatedPackageJson = $packageJsonPattern.Replace($packageJsonText, "`"version`": `"$CoreVersion`"", 1)
    Write-TextFile -Path "package.json" -Content $updatedPackageJson

    $packageLockText = Read-TextFile "package-lock.json"
    $packageLockTopPattern = [regex]'(?s)^(\s*\{\s*"name"\s*:\s*"[^"]+"\s*,\s*"version"\s*:\s*")[^"]+(")'
    if (-not $packageLockTopPattern.IsMatch($packageLockText)) {
        throw "package-lock.json does not contain the expected top-level version pattern."
    }
    $updatedPackageLock = $packageLockTopPattern.Replace($packageLockText, "`${1}$CoreVersion`${2}", 1)

    $packageLockRootPattern = [regex]'(?s)("packages"\s*:\s*\{\s*""\s*:\s*\{\s*"name"\s*:\s*"[^"]+"\s*,\s*"version"\s*:\s*")[^"]+(")'
    if (-not $packageLockRootPattern.IsMatch($updatedPackageLock)) {
        throw "package-lock.json does not contain the expected root package version pattern."
    }
    $updatedPackageLockRoot = $packageLockRootPattern.Replace($updatedPackageLock, "`${1}$CoreVersion`${2}", 1)
    Write-TextFile -Path "package-lock.json" -Content $updatedPackageLockRoot
}

function Promote-UnreleasedChangelog {
    param(
        [PSCustomObject]$VersionInfo,
        [string]$ReleaseDate
    )

    $content = Read-TextFile "CHANGELOG.md"
    if (Get-ReleaseChangelogSection -VersionInfo $VersionInfo -Content $content) {
        if (-not $Force) {
            throw "CHANGELOG.md already has a section for $($VersionInfo.Raw). Use -Force to continue."
        }
        return
    }

    $unreleased = Get-UnreleasedSection -Content $content
    $unreleasedBody = $unreleased.Body.Trim([char[]]@("`r", "`n"))
    if ([string]::IsNullOrWhiteSpace($unreleasedBody) -and -not $Force) {
        throw "The [Unreleased] section is empty. Add release notes or use -Force."
    }

    $replacement = "## [Unreleased]`n`n## [$($VersionInfo.Raw)] - $ReleaseDate"
    if (-not [string]::IsNullOrWhiteSpace($unreleasedBody)) {
        $replacement += "`n`n$unreleasedBody"
    }
    $replacement += "`n`n"

    $newContent = $content.Substring(0, $unreleased.Start) + $replacement + $content.Substring($unreleased.End)
    Write-TextFile -Path "CHANGELOG.md" -Content $newContent
}

function Assert-VersionAlignment {
    param([string]$ExpectedCore)

    $state = Get-VersionState
    $actualVersions = @(
        @{ Label = "CMakeLists.txt"; Value = $state.CMakeVersion }
        @{ Label = "src/plugin-main.h"; Value = $state.PluginVersion }
        @{ Label = "package.json"; Value = $state.PackageJsonVersion }
        @{ Label = "package-lock.json"; Value = $state.PackageLockVersion }
        @{ Label = "package-lock.json packages['']"; Value = $state.PackageLockRootVersion }
    )

    foreach ($entry in $actualVersions) {
        if ($entry.Value -ne $ExpectedCore) {
            throw "$($entry.Label) is $($entry.Value), expected $ExpectedCore."
        }
    }

    return $state
}

function Assert-CleanWorktree {
    if ($AllowDirty) {
        return
    }

    $status = Invoke-Git status --porcelain
    if ($status) {
        throw "Working tree is not clean. Commit or stash changes first, or rerun with -AllowDirty."
    }
}

function Assert-TagMissing {
    param([string]$TagName)

    $existing = Invoke-Git tag --list $TagName
    if ($existing) {
        throw "Tag already exists: $TagName"
    }
}

function Invoke-CheckedCommand {
    param(
        [string]$FilePath,
        [string[]]$Arguments
    )

    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0) {
        $joined = $Arguments -join " "
        throw "$FilePath $joined failed with exit code $LASTEXITCODE."
    }
}

function Resolve-ClangFormat {
    $candidates = @("clang-format-14", "clang-format")
    foreach ($candidate in $candidates) {
        $command = Get-Command $candidate -ErrorAction SilentlyContinue
        if ($command) {
            return $command.Source
        }
    }

    throw "Unable to find clang-format-14 or clang-format in PATH."
}

function Invoke-FormatCheck {
    $formatter = Resolve-ClangFormat
    $files = Get-ChildItem -Path src, tests -Recurse -Include *.cpp, *.h | ForEach-Object { $_.FullName }
    if (-not $files) {
        throw "No C++ source files found for format check."
    }

    Write-Host "Running clang-format dry-run on $($files.Count) files"
    Invoke-CheckedCommand -FilePath $formatter -Arguments (@("--dry-run", "--Werror") + $files)
}

function Invoke-GitDiffCheck {
    Write-Host "Running git diff --check"
    Invoke-CheckedCommand -FilePath "git" -Arguments @("diff", "--check")
    Invoke-CheckedCommand -FilePath "git" -Arguments @("diff", "--cached", "--check")
}

function Invoke-UnitTests {
    param([string]$ReleaseBuildDir)

    Write-Host "Configuring tests in $ReleaseBuildDir"
    Invoke-CheckedCommand -FilePath "cmake" -Arguments @(
        "-S", ".",
        "-B", $ReleaseBuildDir,
        "-DBUILD_TESTS=ON",
        "-DBUILD_PLUGIN=OFF",
        "-DCMAKE_BUILD_TYPE=Release"
    )

    Write-Host "Building vdoninja-tests"
    Invoke-CheckedCommand -FilePath "cmake" -Arguments @("--build", $ReleaseBuildDir, "--target", "vdoninja-tests")

    Write-Host "Running ctest"
    Invoke-CheckedCommand -FilePath "ctest" -Arguments @("--test-dir", $ReleaseBuildDir, "--output-on-failure")
}

function New-ReleaseNotes {
    param([PSCustomObject]$VersionInfo)

    $section = Get-ReleaseChangelogSection -VersionInfo $VersionInfo
    if (-not $section) {
        throw "Cannot build release notes because CHANGELOG.md has no section for $($VersionInfo.Raw)."
    }

    $changesBody = if ([string]::IsNullOrWhiteSpace($section.Body)) {
        "- No changelog notes were recorded for this release."
    } else {
        $section.Body.Trim([char[]]@("`r", "`n"))
    }

    return @"
## Downloads
- Windows installer: `obs-vdoninja-windows-x64-setup.exe`
- Windows portable/manual package: `obs-vdoninja-windows-x64.zip`
- Linux package: `obs-vdoninja-linux-x86_64.tar.gz`
- macOS package: `obs-vdoninja-macos-arm64.zip`

Windows release artifacts are built against OBS `32.0.4` to maximize compatibility across current OBS `32.x` installs.

## Install
- Full install guide: `https://github.com/steveseguin/ninja-obs-plugin/blob/main/INSTALL.md`
- Linux install: `https://github.com/steveseguin/ninja-obs-plugin/blob/main/INSTALL.md#install-linux`
- macOS install: `https://github.com/steveseguin/ninja-obs-plugin/blob/main/INSTALL.md#install-macos`
- Windows installer: run `obs-vdoninja-windows-x64-setup.exe`
- Windows zip: extract, then run `install.cmd`
- Linux/macOS packages: extract, then run `install.sh`
- Post-install usage: `QUICKSTART.md`

## Changes
$changesBody

## Verification
- SHA-256 checksums are provided in `checksums.txt`.
"@
}

function Show-Status {
    $state = Get-VersionState
    $unreleased = Get-UnreleasedSection
    $aligned = ($state.CMakeVersion -eq $state.PluginVersion) -and
        ($state.CMakeVersion -eq $state.PackageJsonVersion) -and
        ($state.CMakeVersion -eq $state.PackageLockVersion) -and
        ($state.CMakeVersion -eq $state.PackageLockRootVersion)

    Write-Host "Repo: $repoRoot"
    Write-Host "CMake version:                $($state.CMakeVersion)"
    Write-Host "Plugin version:               $($state.PluginVersion)"
    Write-Host "package.json version:         $($state.PackageJsonVersion)"
    Write-Host "package-lock version:         $($state.PackageLockVersion)"
    Write-Host "package-lock root version:    $($state.PackageLockRootVersion)"
    Write-Host "Latest tag:                   $($state.LatestTag)"
    if ($state.LatestTag) {
        Write-Host "Next $Bump release:            $(Get-NextVersion -BaseVersion $state.LatestTag -BumpKind $Bump)"
    }
    Write-Host "Unreleased notes present:     $([string]::IsNullOrWhiteSpace($unreleased.Body) -eq $false)"
    Write-Host "Version alignment:            $(if ($aligned) { 'OK' } else { 'MISMATCH' })"
}

function Verify-ReleaseMetadata {
    param([PSCustomObject]$VersionInfo)

    $state = if ($VersionInfo) {
        Assert-VersionAlignment -ExpectedCore $VersionInfo.Core
    } else {
        $currentState = Get-VersionState
        Assert-VersionAlignment -ExpectedCore $currentState.CMakeVersion | Out-Null
        $currentState
    }

    Get-UnreleasedSection | Out-Null
    if ($VersionInfo) {
        $section = Get-ReleaseChangelogSection -VersionInfo $VersionInfo
        if (-not $section) {
            throw "CHANGELOG.md is missing a section for $($VersionInfo.Raw)."
        }
    }

    return $state
}

function Invoke-ReleaseVerification {
    param([PSCustomObject]$VersionInfo)

    Verify-ReleaseMetadata -VersionInfo $VersionInfo | Out-Null
    if (-not $SkipGitDiff) {
        Invoke-GitDiffCheck
    }
    if (-not $SkipFormat) {
        Invoke-FormatCheck
    }
    if (-not $SkipTests) {
        Invoke-UnitTests -ReleaseBuildDir $BuildDir
    }

    if ($VersionInfo) {
        New-ReleaseNotes -VersionInfo $VersionInfo | Out-Null
    }
}

function Prepare-Release {
    param([PSCustomObject]$VersionInfo)

    Set-SourceVersions -CoreVersion $VersionInfo.Core
    Promote-UnreleasedChangelog -VersionInfo $VersionInfo -ReleaseDate $Date
}

function Cut-Release {
    param([PSCustomObject]$VersionInfo)

    Assert-CleanWorktree
    Assert-TagMissing -TagName $VersionInfo.Tag
    Prepare-Release -VersionInfo $VersionInfo
    Invoke-ReleaseVerification -VersionInfo $VersionInfo

    Invoke-Git add -- CMakeLists.txt src/plugin-main.h package.json package-lock.json CHANGELOG.md
    $staged = Invoke-Git diff --cached --name-only
    if (-not $staged) {
        throw "No release changes were staged."
    }

    Invoke-Git commit -m "Release $($VersionInfo.Tag)"
    Invoke-Git tag -a $VersionInfo.Tag -m "Release $($VersionInfo.Tag)"

    if ($Push) {
        $branch = (Invoke-Git rev-parse --abbrev-ref HEAD | Select-Object -First 1).Trim()
        if ([string]::IsNullOrWhiteSpace($branch)) {
            throw "Unable to determine current branch for push."
        }

        Invoke-Git push origin $branch
        Invoke-Git push origin $VersionInfo.Tag
    }
}

$versionInfo = $null
switch ($Action) {
    "prepare" { $versionInfo = Get-ReleaseTargetVersionInfo }
    "cut" { $versionInfo = Get-ReleaseTargetVersionInfo }
    "verify" {
        $versionInfo = if ([string]::IsNullOrWhiteSpace($Version)) { Get-CurrentSourceVersionInfo } else { Normalize-ReleaseVersion $Version }
    }
    "verify-metadata" {
        if (-not [string]::IsNullOrWhiteSpace($Version)) {
            $versionInfo = Normalize-ReleaseVersion $Version
        }
    }
    "notes" {
        $versionInfo = if ([string]::IsNullOrWhiteSpace($Version)) { Get-CurrentSourceVersionInfo } else { Normalize-ReleaseVersion $Version }
    }
}

switch ($Action) {
    "status" {
        Show-Status
    }
    "prepare" {
        Prepare-Release -VersionInfo $versionInfo
        Show-Status
    }
    "verify" {
        Invoke-ReleaseVerification -VersionInfo $versionInfo
        Show-Status
    }
    "verify-metadata" {
        Verify-ReleaseMetadata -VersionInfo $versionInfo | Out-Null
        Show-Status
    }
    "notes" {
        $notes = New-ReleaseNotes -VersionInfo $versionInfo
        if ($OutputPath) {
            Write-TextFile -Path $OutputPath -Content ($notes.TrimEnd() + "`n")
            Write-Host "Wrote release notes to $OutputPath"
        } else {
            Write-Output $notes
        }
    }
    "cut" {
        Cut-Release -VersionInfo $versionInfo
        Show-Status
    }
    default {
        throw "Unsupported action: $Action"
    }
}
