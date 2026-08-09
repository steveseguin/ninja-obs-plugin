function Test-AlphaLoadedPluginEvidence {
    [CmdletBinding()]
    param(
        [AllowNull()][object[]]$LoadedModules,
        [AllowNull()][object]$LoadedPlugin,
        [AllowNull()][object]$StagedPlugin,
        [string]$ExpectedSha256
    )

    $reasons = [System.Collections.Generic.List[string]]::new()
    $expected = ([string]$ExpectedSha256).Trim().ToLowerInvariant()
    if ($expected -notmatch '^[0-9a-f]{64}$') {
        $reasons.Add("expected plugin SHA256 metadata is missing or invalid")
    }
    $modules = @($LoadedModules)
    if ($modules.Count -ne 1) {
        $reasons.Add("loaded plugin module evidence count was $($modules.Count), expected exactly one")
    }

    $module = if ($modules.Count -eq 1) { $modules[0] } else { $null }
    foreach ($bindingCase in @(
        [ordered]@{ name = "loaded module"; value = $module },
        [ordered]@{ name = "loaded plugin"; value = $LoadedPlugin },
        [ordered]@{ name = "staged plugin"; value = $StagedPlugin }
    )) {
        $binding = $bindingCase.value
        if (-not $binding -or [string]::IsNullOrWhiteSpace([string]$binding.path)) {
            $reasons.Add("$($bindingCase.name) path metadata is missing")
        }
        $sha = if ($binding) { ([string]$binding.sha256).Trim().ToLowerInvariant() } else { "" }
        if ($sha -notmatch '^[0-9a-f]{64}$') {
            $reasons.Add("$($bindingCase.name) SHA256 metadata is missing or invalid")
        } elseif ($expected -match '^[0-9a-f]{64}$' -and $sha -ne $expected) {
            $reasons.Add("$($bindingCase.name) SHA256 does not match the expected plugin")
        }
    }

    if ($module -and $LoadedPlugin -and
        ([string]$module.path -ine [string]$LoadedPlugin.path -or
         [string]$module.sha256 -ine [string]$LoadedPlugin.sha256)) {
        $reasons.Add("loaded module list and singular loaded plugin binding disagree")
    }

    return [pscustomobject]@{
        ok = ($reasons.Count -eq 0)
        reasons = @($reasons)
        loadedModuleCount = $modules.Count
    }
}

function Test-AlphaPackagedArtifactEvidence {
    [CmdletBinding()]
    param(
        [AllowNull()][object]$Publisher,
        [AllowNull()][object]$SpoutSender,
        [string]$ExpectedPublisherSha256,
        [string]$ExpectedSpoutSenderSha256
    )

    $reasons = [System.Collections.Generic.List[string]]::new()
    foreach ($artifactCase in @(
        [ordered]@{
            name = "packaged publisher"
            binding = $Publisher
            expected = ([string]$ExpectedPublisherSha256).Trim().ToLowerInvariant()
        },
        [ordered]@{
            name = "Spout fixture"
            binding = $SpoutSender
            expected = ([string]$ExpectedSpoutSenderSha256).Trim().ToLowerInvariant()
        }
    )) {
        if ($artifactCase.expected -notmatch '^[0-9a-f]{64}$') {
            $reasons.Add("$($artifactCase.name) expected SHA256 metadata is missing or invalid")
        }
        if (-not $artifactCase.binding -or
            [string]::IsNullOrWhiteSpace([string]$artifactCase.binding.path)) {
            $reasons.Add("$($artifactCase.name) path metadata is missing")
        }
        $actual = if ($artifactCase.binding) {
            ([string]$artifactCase.binding.sha256).Trim().ToLowerInvariant()
        } else {
            ""
        }
        if ($actual -notmatch '^[0-9a-f]{64}$') {
            $reasons.Add("$($artifactCase.name) SHA256 metadata is missing or invalid")
        } elseif ($artifactCase.expected -match '^[0-9a-f]{64}$' -and
            $actual -ne $artifactCase.expected) {
            $reasons.Add("$($artifactCase.name) SHA256 does not match the expected artifact")
        }
    }

    return [pscustomobject]@{
        ok = ($reasons.Count -eq 0)
        reasons = @($reasons)
    }
}
