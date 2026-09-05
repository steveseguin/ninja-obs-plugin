[CmdletBinding()]
param(
    [string]$RepositoryRoot = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($RepositoryRoot)) {
    $RepositoryRoot = Split-Path -Parent $PSScriptRoot
}
$repositoryPath = (Resolve-Path -LiteralPath $RepositoryRoot).Path
$results = New-Object System.Collections.Generic.List[object]

function Add-PolicyCheck {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Name,
        [Parameter(Mandatory = $true)]
        [bool]$Passed,
        [Parameter(Mandatory = $true)]
        [string]$FailureDetail
    )

    $results.Add([pscustomobject]@{
            Name = $Name
            Passed = $Passed
            FailureDetail = $FailureDetail
        }) | Out-Null
}

function Read-RepositoryText {
    param([string]$RelativePath)

    $path = Join-Path $repositoryPath $RelativePath
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        return ""
    }

    return [System.IO.File]::ReadAllText($path)
}

function Test-ExactSequenceSet {
    param(
        [string[]]$Actual,
        [string[]]$Expected
    )

    $actualSorted = @($Actual | Sort-Object)
    $expectedSorted = @($Expected | Sort-Object)
    if ($actualSorted.Count -ne $expectedSorted.Count) {
        return $false
    }

    return (($actualSorted -join "`n") -ceq ($expectedSorted -join "`n"))
}

function Format-SetDifference {
    param(
        [string[]]$Actual,
        [string[]]$Expected
    )

    $missing = @($Expected | Where-Object { $Actual -cnotcontains $_ } | Sort-Object)
    $unexpected = @($Actual | Where-Object { $Expected -cnotcontains $_ } | Sort-Object)
    $duplicate = @($Actual | Group-Object | Where-Object Count -gt 1 | ForEach-Object Name | Sort-Object)
    return "missing=[$($missing -join ', ')]; unexpected=[$($unexpected -join ', ')]; duplicate=[$($duplicate -join ', ')]"
}

function Get-YamlJobBlock {
    param(
        [string]$Yaml,
        [string]$JobName
    )

    $escapedName = [regex]::Escape($JobName)
    $match = [regex]::Match(
        $Yaml,
        "(?ms)^  ${escapedName}:\s*\r?\n.*?(?=^  [A-Za-z0-9_-]+:\s*(?:#.*)?$|\z)"
    )
    if (-not $match.Success) {
        return ""
    }
    return $match.Value
}

function Get-YamlSteps {
    param([string]$JobBlock)

    $lines = @($JobBlock -split "`r?`n")
    $steps = New-Object System.Collections.Generic.List[object]
    $currentLines = $null
    $currentName = ""
    $currentIndex = -1

    foreach ($line in $lines) {
        if ($line -match '^      -\s+(.+)$') {
            if ($null -ne $currentLines) {
                $steps.Add([pscustomobject]@{
                        Index = $currentIndex
                        Name = $currentName
                        Text = ($currentLines -join "`n")
                    }) | Out-Null
            }

            $currentLines = New-Object System.Collections.Generic.List[string]
            $currentIndex = $steps.Count
            $currentName = ""
            if ($Matches[1] -match '^name:\s*(.+?)\s*$') {
                $currentName = $Matches[1].Trim('"', "'")
            }
        }

        if ($null -ne $currentLines) {
            $currentLines.Add($line) | Out-Null
        }
    }

    if ($null -ne $currentLines) {
        $steps.Add([pscustomobject]@{
                Index = $currentIndex
                Name = $currentName
                Text = ($currentLines -join "`n")
            }) | Out-Null
    }

    return @($steps | ForEach-Object { $_ })
}

function Get-YamlStepRunText {
    param([string]$StepText)

    $lines = @($StepText -split "`r?`n")
    $runLines = New-Object System.Collections.Generic.List[string]
    $insideRunBlock = $false
    foreach ($line in $lines) {
        if (-not $insideRunBlock) {
            if ($line -match '^        run:\s*(.*?)\s*$') {
                $insideRunBlock = $true
                $inlineRun = $Matches[1]
                if ($inlineRun -and $inlineRun -notin @('|', '>', '|-', '>-')) {
                    $runLines.Add($inlineRun) | Out-Null
                }
            }
            continue
        }

        if ($line -match '^        [A-Za-z0-9_-]+:\s*' -or $line -match '^      -\s+') {
            break
        }
        if ($line -match '^          (.*)$') {
            $runLines.Add($Matches[1]) | Out-Null
        }
    }

    return ($runLines -join "`n")
}

function Remove-PowerShellComments {
    param([string]$Text)

    if ([string]::IsNullOrEmpty($Text)) {
        return ""
    }

    $tokens = $null
    $parseErrors = $null
    [void][System.Management.Automation.Language.Parser]::ParseInput(
        $Text,
        [ref]$tokens,
        [ref]$parseErrors
    )
    $characters = $Text.ToCharArray()
    foreach ($token in @($tokens | Where-Object Kind -eq 'Comment')) {
        for ($index = $token.Extent.StartOffset; $index -lt $token.Extent.EndOffset; $index++) {
            if ($characters[$index] -notin @("`r", "`n")) {
                $characters[$index] = ' '
            }
        }
    }
    return (-join $characters)
}

function Remove-CMakeComments {
    param([string]$Text)

    if ([string]::IsNullOrEmpty($Text)) {
        return ""
    }

    $withoutBracketComments = [regex]::Replace($Text, '(?s)#\[\[.*?\]\]', '')
    $cleanLines = New-Object System.Collections.Generic.List[string]
    foreach ($line in @($withoutBracketComments -split "`r?`n")) {
        $inQuote = $false
        $escaped = $false
        $cutIndex = -1
        for ($index = 0; $index -lt $line.Length; $index++) {
            $character = $line[$index]
            if ($escaped) {
                $escaped = $false
                continue
            }
            if ($character -eq '\') {
                $escaped = $true
                continue
            }
            if ($character -eq '"') {
                $inQuote = -not $inQuote
                continue
            }
            if ($character -eq '#' -and -not $inQuote) {
                $cutIndex = $index
                break
            }
        }
        $cleanLines.Add($(if ($cutIndex -ge 0) { $line.Substring(0, $cutIndex) } else { $line })) | Out-Null
    }
    return ($cleanLines -join "`n")
}

function Remove-CMakeStaticallyDisabledBlocks {
    param([string]$Text)

    $output = New-Object System.Collections.Generic.List[string]
    $disabledStack = New-Object System.Collections.Generic.List[bool]
    foreach ($line in @($Text -split "`r?`n")) {
        if ($line -match '^\s*if\s*\(\s*(?<condition>.*?)\s*\)\s*$') {
            $normalizedCondition = $Matches['condition'].Trim().ToUpperInvariant()
            $parentDisabled = $disabledStack.Count -gt 0 -and $disabledStack[$disabledStack.Count - 1]
            $literalFalse = $normalizedCondition -match '^(?:FALSE|OFF|NO|N|IGNORE|NOTFOUND|0|NOT\s+(?:TRUE|ON|YES|Y|1))$'
            $disabledStack.Add($parentDisabled -or $literalFalse) | Out-Null
            if (-not $disabledStack[$disabledStack.Count - 1]) {
                $output.Add($line) | Out-Null
            }
            continue
        }
        if ($line -match '^\s*endif\s*\(') {
            if ($disabledStack.Count -gt 0) {
                $wasDisabled = $disabledStack[$disabledStack.Count - 1]
                $disabledStack.RemoveAt($disabledStack.Count - 1)
                if (-not $wasDisabled) {
                    $output.Add($line) | Out-Null
                }
            } else {
                $output.Add($line) | Out-Null
            }
            continue
        }
        $currentlyDisabled = $disabledStack.Count -gt 0 -and $disabledStack[$disabledStack.Count - 1]
        if (-not $currentlyDisabled) {
            $output.Add($line) | Out-Null
        }
    }
    return ($output -join "`n")
}

function Get-CMakeConditionalBlock {
    param(
        [string]$Text,
        [string]$Condition
    )

    $lines = @($Text -split "`r?`n")
    $escapedCondition = [regex]::Escape($Condition)
    for ($start = 0; $start -lt $lines.Count; $start++) {
        if ($lines[$start] -notmatch "^\s*if\s*\(\s*${escapedCondition}\s*\)\s*$") {
            continue
        }
        $depth = 1
        $body = New-Object System.Collections.Generic.List[string]
        for ($index = $start + 1; $index -lt $lines.Count; $index++) {
            if ($lines[$index] -match '^\s*if\s*\(') {
                $depth++
            } elseif ($lines[$index] -match '^\s*endif\s*\(') {
                $depth--
                if ($depth -eq 0) {
                    return ($body -join "`n")
                }
            }
            $body.Add($lines[$index]) | Out-Null
        }
    }
    return ""
}

function Get-CMakeAddTests {
    param([string]$CMakeText)

    return @(
        [regex]::Matches(
            $CMakeText,
            '(?is)\badd_test\s*\(\s*NAME\s+(?<name>[A-Za-z0-9_.-]+)\s+COMMAND\s+(?<command>.*?)\)'
        ) | ForEach-Object {
            [pscustomobject]@{
                Name = $_.Groups['name'].Value
                Command = ([regex]::Replace($_.Groups['command'].Value, '\s+', ' ')).Trim()
            }
        }
    )
}

function Get-WindowsMatrixRows {
    param([string]$WindowsJob)

    $rows = New-Object System.Collections.Generic.List[object]
    $matches = [regex]::Matches(
        $WindowsJob,
        '(?ms)^          - label:\s*(?<label>[^\r\n#]+?)\s*\r?\n(?<body>.*?)(?=^          - label:|^    [A-Za-z0-9_-]+:|\z)'
    )
    foreach ($match in $matches) {
        $fields = @{}
        $fields['label'] = $match.Groups['label'].Value.Trim().Trim('"', "'")
        foreach ($fieldMatch in [regex]::Matches($match.Groups['body'].Value, '(?m)^            (?<key>[A-Za-z0-9_]+):\s*(?<value>.*?)\s*$')) {
            $fields[$fieldMatch.Groups['key'].Value] = $fieldMatch.Groups['value'].Value.Trim().Trim('"', "'")
        }
        $rows.Add($fields) | Out-Null
    }
    return @($rows | ForEach-Object { $_ })
}

function Get-PowerShellFunctionText {
    param(
        $Ast,
        [string]$Name
    )

    if ($null -eq $Ast) {
        return ""
    }
    $functions = @($Ast.FindAll({
                param($node)
                $node -is [System.Management.Automation.Language.FunctionDefinitionAst]
            }, $true) | Where-Object Name -CEQ $Name)
    if ($functions.Count -ne 1) {
        return ""
    }
    return $functions[0].Body.Extent.Text
}

function Get-PowerShellCommandTexts {
    param(
        $Ast,
        [string]$CommandName
    )

    if ($null -eq $Ast) {
        return @()
    }
    return @(
        $Ast.FindAll({
                param($node)
                $node -is [System.Management.Automation.Language.CommandAst]
            }, $true) | Where-Object {
            $_.GetCommandName() -and $_.GetCommandName() -ieq $CommandName
        } | ForEach-Object { $_.Extent.Text }
    )
}

function Get-PowerShellForeachBodies {
    param(
        $Ast,
        [string]$CollectionVariable
    )

    if ($null -eq $Ast) {
        return @()
    }
    $variablePattern = '\$' + [regex]::Escape($CollectionVariable) + '\b'
    return @(
        $Ast.FindAll({
                param($node)
                $node -is [System.Management.Automation.Language.ForEachStatementAst]
            }, $true) | Where-Object {
            [regex]::IsMatch($_.Condition.Extent.Text, $variablePattern)
        } | ForEach-Object { $_.Body.Extent.Text }
    )
}

function Get-ContainingPowerShellFunctionName {
    param($Node)

    $current = $Node.Parent
    while ($null -ne $current) {
        if ($current -is [System.Management.Automation.Language.FunctionDefinitionAst]) {
            return $current.Name
        }
        $current = $current.Parent
    }
    return ""
}

function Test-IsUnconditionalTopLevelPowerShellCommand {
    param($CommandAst)

    $current = $CommandAst.Parent
    while ($null -ne $current) {
        if ($current -is [System.Management.Automation.Language.FunctionDefinitionAst] -or
            $current -is [System.Management.Automation.Language.IfStatementAst] -or
            $current -is [System.Management.Automation.Language.LoopStatementAst] -or
            $current -is [System.Management.Automation.Language.SwitchStatementAst] -or
            $current -is [System.Management.Automation.Language.TryStatementAst] -or
            $current -is [System.Management.Automation.Language.TrapStatementAst]) {
            return $false
        }
        $current = $current.Parent
    }
    return $true
}

function Get-UnconditionalTopLevelPowerShellCommands {
    param($Ast)

    if ($null -eq $Ast) {
        return @()
    }
    return @(
        $Ast.FindAll({
                param($node)
                $node -is [System.Management.Automation.Language.CommandAst]
            }, $true) | Where-Object { Test-IsUnconditionalTopLevelPowerShellCommand $_ }
    )
}

function Get-ReachablePowerShellFunctionNames {
    param($Ast)

    if ($null -eq $Ast) {
        return @()
    }
    $functionMap = @{}
    foreach ($function in @($Ast.FindAll({
                    param($node)
                    $node -is [System.Management.Automation.Language.FunctionDefinitionAst]
                }, $true))) {
        $functionMap[$function.Name.ToLowerInvariant()] = $function
    }
    $reachable = @{}
    $queue = New-Object System.Collections.Generic.Queue[string]
    $rootCommands = @(
        $Ast.FindAll({
                param($node)
                $node -is [System.Management.Automation.Language.CommandAst]
            }, $true) | Where-Object { -not (Get-ContainingPowerShellFunctionName $_) }
    )
    foreach ($command in $rootCommands) {
        $name = $command.GetCommandName()
        if ($name -and $functionMap.ContainsKey($name.ToLowerInvariant())) {
            $queue.Enqueue($name.ToLowerInvariant())
        }
    }
    while ($queue.Count -gt 0) {
        $name = $queue.Dequeue()
        if ($reachable.ContainsKey($name)) {
            continue
        }
        $reachable[$name] = $true
        $functionAst = $functionMap[$name]
        foreach ($command in @($functionAst.Body.FindAll({
                        param($node)
                        $node -is [System.Management.Automation.Language.CommandAst]
                    }, $true))) {
            $calledName = $command.GetCommandName()
            if ($calledName -and $functionMap.ContainsKey($calledName.ToLowerInvariant()) -and
                -not $reachable.ContainsKey($calledName.ToLowerInvariant())) {
                $queue.Enqueue($calledName.ToLowerInvariant())
            }
        }
    }
    return @($reachable.Keys)
}

function Test-IsReachablePowerShellNode {
    param(
        $Node,
        [string[]]$ReachableFunctionNames
    )

    $functionName = Get-ContainingPowerShellFunctionName $Node
    return (-not $functionName -or $ReachableFunctionNames -icontains $functionName)
}

function Get-ReachablePowerShellCommandTexts {
    param(
        $Ast,
        [string]$CommandName,
        [string[]]$ReachableFunctionNames
    )

    if ($null -eq $Ast) {
        return @()
    }
    return @(
        $Ast.FindAll({
                param($node)
                $node -is [System.Management.Automation.Language.CommandAst]
            }, $true) | Where-Object {
            $_.GetCommandName() -and
            $_.GetCommandName() -ieq $CommandName -and
            (Test-IsReachablePowerShellNode $_ $ReachableFunctionNames)
        } | ForEach-Object { $_.Extent.Text }
    )
}

function Get-ReachablePowerShellFunctionText {
    param(
        $Ast,
        [string]$Name,
        [string[]]$ReachableFunctionNames
    )

    if ($ReachableFunctionNames -inotcontains $Name) {
        return ""
    }
    return Get-PowerShellFunctionText $Ast $Name
}

function Get-ReachablePowerShellForeachRecords {
    param(
        $Ast,
        [string]$CollectionVariable,
        [string[]]$ReachableFunctionNames
    )

    if ($null -eq $Ast) {
        return @()
    }
    $variablePattern = '\$' + [regex]::Escape($CollectionVariable) + '\b'
    return @(
        $Ast.FindAll({
                param($node)
                $node -is [System.Management.Automation.Language.ForEachStatementAst]
            }, $true) | Where-Object {
            (Test-IsReachablePowerShellNode $_ $ReachableFunctionNames) -and
            [regex]::IsMatch($_.Condition.Extent.Text, $variablePattern)
        } | ForEach-Object {
            [pscustomobject]@{
                Iterator = $_.Variable.VariablePath.UserPath
                Body = $_.Body.Extent.Text
                Text = $_.Extent.Text
            }
        }
    )
}

function Get-ExactPowerShellCommandArgument {
    param(
        $CommandAst,
        [string]$ParameterName
    )

    $elements = @($CommandAst.CommandElements)
    for ($index = 0; $index -lt $elements.Count; $index++) {
        if ($elements[$index] -is [System.Management.Automation.Language.CommandParameterAst] -and
            $elements[$index].ParameterName -ieq $ParameterName -and
            $index + 1 -lt $elements.Count) {
            return $elements[$index + 1].Extent.Text.Trim('"', "'")
        }
    }
    return $null
}

function Get-TopLevelWorkflowInvocationCommands {
    param(
        [string]$RunText,
        [string]$ScriptRelativePattern
    )

    if ([string]::IsNullOrWhiteSpace($RunText)) {
        return @()
    }
    $parseText = $RunText
    foreach ($expressionMatch in @([regex]::Matches($RunText, '\$\{\{\s*(?<expression>.*?)\s*\}\}'))) {
        $token = '__GH_' + ([regex]::Replace($expressionMatch.Groups['expression'].Value.Trim(), '[^A-Za-z0-9]+', '_')) + '__'
        $parseText = $parseText.Replace($expressionMatch.Value, $token)
    }
    $tokens = $null
    $parseErrors = $null
    $ast = [System.Management.Automation.Language.Parser]::ParseInput(
        $parseText,
        [ref]$tokens,
        [ref]$parseErrors
    )
    if (@($parseErrors).Count -gt 0) {
        return @()
    }
    $fileInvocationPattern = '(?is)-File\s+["'']?' + $ScriptRelativePattern + '["'']?(?:\s|$)'
    $directCommandPattern = '(?i)^(?:\.[\\/])?' + $ScriptRelativePattern + '$'
    return @(
        Get-UnconditionalTopLevelPowerShellCommands $ast | Where-Object {
            $commandName = $_.GetCommandName()
            $commandText = $_.Extent.Text
            (($commandName -in @('pwsh', 'pwsh.exe', 'powershell', 'powershell.exe')) -and
                [regex]::IsMatch($commandText, $fileInvocationPattern)) -or
            ($commandName -and [regex]::IsMatch($commandName, $directCommandPattern))
        }
    )
}

function Get-CMakeTestProperties {
    param([string]$CMakeText)

    $propertiesByTest = @{}
    $blocks = [regex]::Matches($CMakeText, '(?is)\bset_tests_properties\s*\((?<body>.*?)\)')
    foreach ($block in $blocks) {
        $body = $block.Groups['body'].Value
        $propertiesMarker = [regex]::Match($body, '(?i)\bPROPERTIES\b')
        if (-not $propertiesMarker.Success) {
            continue
        }

        $testNameText = $body.Substring(0, $propertiesMarker.Index)
        $propertyText = $body.Substring($propertiesMarker.Index + $propertiesMarker.Length)
        $labelsMatch = [regex]::Match($propertyText, '(?is)\bLABELS\s+(?:"(?<quoted>[^"]*)"|(?<plain>[^\s\)]+))')
        $timeoutMatch = [regex]::Match($propertyText, '(?i)\bTIMEOUT\s+"?(?<timeout>\d+)"?')
        $labels = ""
        $timeout = $null
        if ($labelsMatch.Success) {
            $labels = if ($labelsMatch.Groups['quoted'].Success) {
                $labelsMatch.Groups['quoted'].Value
            } else {
                $labelsMatch.Groups['plain'].Value
            }
        }
        if ($timeoutMatch.Success) {
            $timeout = [int]$timeoutMatch.Groups['timeout'].Value
        }

        foreach ($nameMatch in [regex]::Matches($testNameText, '[A-Za-z0-9_.-]+')) {
            $propertiesByTest[$nameMatch.Value] = [pscustomobject]@{
                Labels = @($labels -split ';' | ForEach-Object Trim | Where-Object { $_ })
                Timeout = $timeout
            }
        }
    }

    return $propertiesByTest
}

function Get-DoubleQuotedArrayValues {
    param(
        [string]$Text,
        [string]$VariableName
    )

    $escapedName = [regex]::Escape($VariableName)
    $pattern = '(?is)\$' + $escapedName + '\s*=\s*@\((?<body>.*?)\)'
    $arrayMatch = [regex]::Match($Text, $pattern)
    if (-not $arrayMatch.Success) {
        return @()
    }

    return @(
        [regex]::Matches($arrayMatch.Groups['body'].Value, '"([^"]+)"') |
            ForEach-Object { $_.Groups[1].Value }
    )
}

function Get-DollarVariableArrayValues {
    param(
        [string]$Text,
        [string]$VariableName
    )

    $escapedName = [regex]::Escape($VariableName)
    $pattern = '(?is)\$' + $escapedName + '\s*=\s*@\((?<body>.*?)\)'
    $arrayMatch = [regex]::Match($Text, $pattern)
    if (-not $arrayMatch.Success) {
        return @()
    }

    return @(
        [regex]::Matches($arrayMatch.Groups['body'].Value, '\$([A-Za-z_][A-Za-z0-9_]*)') |
            ForEach-Object { $_.Groups[1].Value }
    )
}

$cmakeRawText = Read-RepositoryText "CMakeLists.txt"
$cmakeCommentFreeText = Remove-CMakeComments $cmakeRawText
$cmakeText = Remove-CMakeStaticallyDisabledBlocks $cmakeCommentFreeText
$buildWorkflowText = Read-RepositoryText ".github/workflows/build.yml"
$normalCiText = Read-RepositoryText ".github/workflows/ci.yml"
$wrapperRawText = Read-RepositoryText "scripts/run-release-linked-gates.ps1"
$wrapperTokens = $null
$wrapperParseErrors = $null
$wrapperAst = [System.Management.Automation.Language.Parser]::ParseInput(
    $wrapperRawText,
    [ref]$wrapperTokens,
    [ref]$wrapperParseErrors
)
$wrapperText = Remove-PowerShellComments $wrapperRawText
$wrapperReachableFunctionNames = @(Get-ReachablePowerShellFunctionNames $wrapperAst)

Add-PolicyCheck "CMakeLists.txt is available" ($cmakeText.Length -gt 0) "CMakeLists.txt is missing or empty."
Add-PolicyCheck "Windows release workflow is available" ($buildWorkflowText.Length -gt 0) ".github/workflows/build.yml is missing or empty."
Add-PolicyCheck "Normal CI workflow is available" ($normalCiText.Length -gt 0) ".github/workflows/ci.yml is missing or empty."

$expectedNativeTests = @(
    "vdoninja-native-media-linked-owner-lifetime-track",
    "vdoninja-native-media-linked-owner-lifetime-data-channel",
    "vdoninja-native-media-linked-owner-order-peer-connection",
    "vdoninja-native-media-linked-owner-order-same-handle-data-channel",
    "vdoninja-native-media-linked-owner-hook-pre-permit",
    "vdoninja-native-media-linked-owner-hook-registration-failure",
    "vdoninja-native-media-linked-owner-hook-track-kinds",
    "vdoninja-native-media-linked-owner-hook-feedback",
    "vdoninja-native-media-linked-full"
)
$moduleTestName = "vdoninja-module-lifecycle-linked"
$expectedReleaseLinkedTests = @($expectedNativeTests) + @($moduleTestName)

$cmakeAddTests = @(Get-CMakeAddTests $cmakeText)
$registeredTests = @($cmakeAddTests | ForEach-Object Name)
$registeredNativeTests = @($registeredTests | Where-Object { $_ -like 'vdoninja-native-media-linked-*' })
Add-PolicyCheck `
    "CMake registers the exact nine native linked tests" `
    (Test-ExactSequenceSet $registeredNativeTests $expectedNativeTests) `
    ("Native CTest inventory mismatch: " + (Format-SetDifference $registeredNativeTests $expectedNativeTests))

$nativeGateCMake = Get-CMakeConditionalBlock $cmakeText "BUILD_NATIVE_MEDIA_LINKED_GATE"
$moduleGateCMake = Get-CMakeConditionalBlock $cmakeText "BUILD_MODULE_LIFECYCLE_LINKED_GATE"
$nativeGateRegisteredTests = @(Get-CMakeAddTests $nativeGateCMake | ForEach-Object Name)
$moduleGateRegisteredTests = @(Get-CMakeAddTests $moduleGateCMake | ForEach-Object Name)
Add-PolicyCheck `
    "Native CTests are active inside BUILD_NATIVE_MEDIA_LINKED_GATE" `
    (Test-ExactSequenceSet $nativeGateRegisteredTests $expectedNativeTests) `
    ("Active native gate inventory mismatch: " + (Format-SetDifference $nativeGateRegisteredTests $expectedNativeTests))
Add-PolicyCheck `
    "Module CTest is active inside BUILD_MODULE_LIFECYCLE_LINKED_GATE" `
    (Test-ExactSequenceSet $moduleGateRegisteredTests @($moduleTestName)) `
    ("Active module gate inventory mismatch: " + (Format-SetDifference $moduleGateRegisteredTests @($moduleTestName)))

$moduleRegistrationCount = @($registeredTests | Where-Object { $_ -ceq $moduleTestName }).Count
Add-PolicyCheck `
    "CMake registers the module linked test exactly once" `
    ($moduleRegistrationCount -eq 1) `
    "Expected one add_test(NAME $moduleTestName ...); found $moduleRegistrationCount."

$expectedNativeFilters = [ordered]@{
    "vdoninja-native-media-linked-owner-lifetime-track" = "Track in-flight callback is safe during manager destruction"
    "vdoninja-native-media-linked-owner-lifetime-data-channel" = "DataChannel in-flight callback is safe during manager destruction"
    "vdoninja-native-media-linked-owner-order-peer-connection" = "PeerConnection description and feedback functions share one owner session"
    "vdoninja-native-media-linked-owner-order-same-handle-data-channel" = "same-handle DataChannel replacement drains before detaching functions"
    "vdoninja-native-media-linked-owner-hook-pre-permit" = "completion delayed until after owner shutdown is rejected"
    "vdoninja-native-media-linked-owner-hook-registration-failure" = "installed function registration failure detaches setter"
    "vdoninja-native-media-linked-owner-hook-track-kinds" = "video alpha audio Track completions share one owner session"
    "vdoninja-native-media-linked-owner-hook-feedback" = "admitted feedback completion drains before owner state"
    "vdoninja-native-media-linked-full" = ""
}
foreach ($entry in $expectedNativeFilters.GetEnumerator()) {
    $registrations = @($cmakeAddTests | Where-Object Name -CEQ $entry.Key)
    $expectedCommand = '$<TARGET_FILE:vdoninja-native-media-linked-gate>'
    if ($entry.Value) {
        $expectedCommand += " `"$($entry.Value)`""
    }
    $actualCommands = @($registrations | ForEach-Object Command)
    Add-PolicyCheck `
        "CTest command mapping: $($entry.Key)" `
        ($registrations.Count -eq 1 -and $actualCommands[0] -ceq $expectedCommand) `
        "Expected COMMAND $expectedCommand exactly once; actual=[$($actualCommands -join ' | ')]."
}

$moduleRegistrations = @($cmakeAddTests | Where-Object Name -CEQ $moduleTestName)
$moduleExpectedCommand = '$<TARGET_FILE:vdoninja-module-lifecycle-linked-gate>'
$moduleActualCommands = @($moduleRegistrations | ForEach-Object Command)
Add-PolicyCheck `
    "CTest command mapping: $moduleTestName" `
    ($moduleRegistrations.Count -eq 1 -and $moduleActualCommands[0] -ceq $moduleExpectedCommand) `
    "Expected COMMAND $moduleExpectedCommand exactly once with no filter; actual=[$($moduleActualCommands -join ' | ')]."

$testProperties = Get-CMakeTestProperties $cmakeText
$expectedTimeouts = [ordered]@{
    "vdoninja-native-media-linked-owner-lifetime-track" = 30
    "vdoninja-native-media-linked-owner-lifetime-data-channel" = 30
    "vdoninja-native-media-linked-owner-order-peer-connection" = 60
    "vdoninja-native-media-linked-owner-order-same-handle-data-channel" = 60
    "vdoninja-native-media-linked-owner-hook-pre-permit" = 60
    "vdoninja-native-media-linked-owner-hook-registration-failure" = 60
    "vdoninja-native-media-linked-owner-hook-track-kinds" = 60
    "vdoninja-native-media-linked-owner-hook-feedback" = 60
    "vdoninja-native-media-linked-full" = 600
    "vdoninja-module-lifecycle-linked" = 60
}
foreach ($entry in $expectedTimeouts.GetEnumerator()) {
    $hasProperties = $testProperties.ContainsKey($entry.Key)
    $actualTimeout = if ($hasProperties) { $testProperties[$entry.Key].Timeout } else { $null }
    Add-PolicyCheck `
        "CTest timeout: $($entry.Key)" `
        ($hasProperties -and $actualTimeout -eq $entry.Value) `
        "Expected timeout $($entry.Value)s; actual=$actualTimeout."
}

foreach ($nativeTest in $expectedNativeTests) {
    $hasProperties = $testProperties.ContainsKey($nativeTest)
    $labels = if ($hasProperties) { @($testProperties[$nativeTest].Labels) } else { @() }
    $expectedLabels = if ($nativeTest -ceq "vdoninja-native-media-linked-full") {
        @("native-media-linked", "release-linked")
    } else {
        @("native-media-linked", "owner-lifetime", "release-linked")
    }
    Add-PolicyCheck `
        "Native CTest exact labels: $nativeTest" `
        (Test-ExactSequenceSet $labels $expectedLabels) `
        "Expected exact labels [$($expectedLabels -join ', ')]; actual=[$($labels -join ', ')]."
}

$moduleLabels = if ($testProperties.ContainsKey($moduleTestName)) {
    @($testProperties[$moduleTestName].Labels)
} else {
    @()
}
Add-PolicyCheck `
    "Module CTest carries the exact release labels" `
    (Test-ExactSequenceSet $moduleLabels @("module-lifecycle-linked", "release-linked")) `
    "Expected exact labels [module-lifecycle-linked, release-linked]; actual=[$($moduleLabels -join ', ')]."

Add-PolicyCheck `
    "CMake declares LIBDATACHANNEL_PLOG_INCLUDE_DIR as an explicit cache path" `
    ([regex]::IsMatch($cmakeText, '(?is)\bset\s*\(\s*LIBDATACHANNEL_PLOG_INCLUDE_DIR\b.*?\bCACHE\s+PATH\b')) `
    "Declare LIBDATACHANNEL_PLOG_INCLUDE_DIR as a CACHE PATH so clean CI cannot silently use a workspace-vendored header."
Add-PolicyCheck `
    "Module linked target consumes LIBDATACHANNEL_PLOG_INCLUDE_DIR" `
    ([regex]::IsMatch($cmakeText, '(?is)target_include_directories\s*\(\s*vdoninja-module-lifecycle-linked-gate\b.*?\$\{LIBDATACHANNEL_PLOG_INCLUDE_DIR\}')) `
    "The module linked target must include the explicitly supplied LIBDATACHANNEL_PLOG_INCLUDE_DIR."
Add-PolicyCheck `
    "CMake rejects a missing explicit plog include directory" `
    ([regex]::IsMatch($cmakeText, '(?is)(?:EXISTS|IS_DIRECTORY)[^\r\n\)]*LIBDATACHANNEL_PLOG_INCLUDE_DIR.*?FATAL_ERROR|LIBDATACHANNEL_PLOG_INCLUDE_DIR.*?(?:EXISTS|IS_DIRECTORY).*?FATAL_ERROR')) `
    "When BUILD_MODULE_LIFECYCLE_LINKED_GATE is ON, an absent plog include directory must fail configuration."
Add-PolicyCheck `
    "CMake has no vendored plog fallback" `
    (-not [regex]::IsMatch($cmakeText, '(?i)(?:\$\{CMAKE_CURRENT_SOURCE_DIR\}[\\/])?deps[\\/]libdatachannel[\\/]deps[\\/]plog[\\/]include')) `
    "Do not directly or conditionally fall back to deps/libdatachannel/deps/plog/include; release wiring must supply the clean-checkout path explicitly."

$linuxJob = Get-YamlJobBlock $buildWorkflowText "build-linux"
$linuxCallerJob = $linuxJob
# Linux packaging is shared by PR builds and the release workflow. Follow only
# the repository-owned reusable workflow, retaining the caller's required gate.
if ([regex]::IsMatch($linuxJob, '(?m)^    uses: \./\.github/workflows/linux\.yml\s*$')) {
    $linuxJob = Get-YamlJobBlock (Read-RepositoryText ".github/workflows/linux.yml") "build-linux"
}
$linuxSteps = @(Get-YamlSteps $linuxJob)
$linuxConfigureSteps = @($linuxSteps | Where-Object Name -ceq "Configure Plugin")
$linuxConfigureText = if ($linuxConfigureSteps.Count -eq 1) { $linuxConfigureSteps[0].Text } else { "" }
$linuxConfigureRunText = if ($linuxConfigureSteps.Count -eq 1) {
    Remove-PowerShellComments (Get-YamlStepRunText $linuxConfigureText)
} else {
    ""
}
$linuxConfigureShells = @(
    [regex]::Matches($linuxConfigureText, '(?im)^        shell:\s*(?<shell>[^\r\n#]+?)\s*$') |
        ForEach-Object { $_.Groups['shell'].Value.Trim().Trim('"', "'") }
)
$linuxConfigureUsesBashShell = (
    $linuxConfigureShells.Count -eq 0 -or
    ($linuxConfigureShells.Count -eq 1 -and $linuxConfigureShells[0] -ceq "bash")
)
Add-PolicyCheck `
    "Linux Configure Plugin remains an unconditional Bash step" `
    ($linuxConfigureSteps.Count -eq 1 -and
        -not [regex]::IsMatch($linuxCallerJob, '(?m)^    if:\s*') -and
        -not [regex]::IsMatch($linuxCallerJob, '(?i)continue-on-error\s*:') -and
        $linuxConfigureUsesBashShell -and
        -not [regex]::IsMatch($linuxJob, '(?m)^    if:\s*') -and
        -not [regex]::IsMatch($linuxConfigureText, '(?m)^        if:\s*') -and
        -not [regex]::IsMatch($linuxConfigureText, '(?i)continue-on-error\s*:') -and
        [regex]::IsMatch($linuxConfigureRunText, '(?im)^\s*export\s+OBS_LIB_DIR=') -and
        [regex]::IsMatch($linuxConfigureRunText, '(?im)^\s*cmake\s+-B\s+build\b') -and
        [regex]::IsMatch($linuxConfigureRunText, '\$\{OBS_LIB_DIR\}')) `
    "Keep the Linux Configure Plugin run block unconditional under the Ubuntu default shell or explicit shell: bash; shell: pwsh is incompatible with its export and braced OBS_LIB_DIR Bash syntax."

$windowsJob = Get-YamlJobBlock $buildWorkflowText "build-windows"
$windowsSteps = @(Get-YamlSteps $windowsJob)
$matrixRows = @(Get-WindowsMatrixRows $windowsJob)
$matrixLabels = @($matrixRows | ForEach-Object { $_['label'] })
$expectedMatrixLabels = @("OBS 32.2.x", "OBS 32.0-32.1 legacy")
Add-PolicyCheck `
    "Windows release matrix contains exactly the current and legacy rows" `
    (Test-ExactSequenceSet $matrixLabels $expectedMatrixLabels) `
    ("Windows matrix mismatch: " + (Format-SetDifference $matrixLabels $expectedMatrixLabels))

$expectedMatrixRows = @(
    [ordered]@{
        label = "OBS 32.2.x"
        obs_version = "32.2.0"
        obs_source_ref = "32.2.0"
        obs_source_tag = "32.2.0"
        asset_suffix = ""
        ffmpeg_imports = "avcodec-62.dll,avutil-60.dll,swscale-9.dll,swresample-6.dll"
    },
    [ordered]@{
        label = "OBS 32.0-32.1 legacy"
        obs_version = "32.0.4"
        obs_source_ref = "32.0.4"
        obs_source_tag = "32.0.4"
        asset_suffix = "-obs32.0-32.1"
        ffmpeg_imports = "avcodec-61.dll,avutil-59.dll,swscale-8.dll,swresample-5.dll"
    }
)
foreach ($expectedRow in $expectedMatrixRows) {
    $actualRows = @($matrixRows | Where-Object { $_['label'] -ceq $expectedRow['label'] })
    $mismatchedFields = New-Object System.Collections.Generic.List[string]
    if ($actualRows.Count -eq 1) {
        foreach ($field in $expectedRow.Keys) {
            $actualValue = if ($actualRows[0].ContainsKey($field)) { $actualRows[0][$field] } else { $null }
            if ($actualValue -cne $expectedRow[$field]) {
                $mismatchedFields.Add("${field}: expected='$($expectedRow[$field])' actual='$actualValue'") | Out-Null
            }
        }
    }
    Add-PolicyCheck `
        "Windows matrix exact OBS/FFmpeg mapping: $($expectedRow['label'])" `
        ($actualRows.Count -eq 1 -and $mismatchedFields.Count -eq 0) `
        "Expected one exact row; mismatches=[$($mismatchedFields -join '; ')]."
}

$configureSteps = @($windowsSteps | Where-Object Name -ceq "Configure Plugin")
$configureText = if ($configureSteps.Count -eq 1) { $configureSteps[0].Text } else { "" }
$configureRunText = if ($configureSteps.Count -eq 1) {
    Remove-PowerShellComments (Get-YamlStepRunText $configureText)
} else {
    ""
}
Add-PolicyCheck `
    "Windows matrix has one common Configure Plugin step" `
    ($configureSteps.Count -eq 1 -and
        -not [regex]::IsMatch($windowsJob, '(?m)^    if:\s*') -and
        -not [regex]::IsMatch($configureText, '(?m)^        if:\s*')) `
    "Expected one unconditional Configure Plugin step shared by both Windows matrix rows; found $($configureSteps.Count)."

foreach ($flag in @("BUILD_PLUGIN", "BUILD_NATIVE_MEDIA_LINKED_GATE", "BUILD_MODULE_LIFECYCLE_LINKED_GATE")) {
    Add-PolicyCheck `
        "Windows matrix explicitly configures $flag=ON" `
        ($configureSteps.Count -eq 1 -and [regex]::IsMatch($configureRunText, "(?i)-D${flag}(?::BOOL)?=ON\b")) `
        "The common Configure Plugin step must pass -D${flag}=ON for both release rows."
}

Add-PolicyCheck `
    "Windows configure resolves plog from the clean libdatachannel checkout" `
    ($configureSteps.Count -eq 1 -and
        [regex]::IsMatch($configureRunText, '(?i)Resolve-Path[^\r\n]*libdatachannel-src[\\/]deps[\\/]plog[\\/]include') -and
        [regex]::IsMatch($configureRunText, '(?i)-DLIBDATACHANNEL_PLOG_INCLUDE_DIR\s*=\s*"?\$plogIncludeDir\b')) `
    "Resolve libdatachannel-src/deps/plog/include in the job and pass it as -DLIBDATACHANNEL_PLOG_INCLUDE_DIR."

$buildSteps = @($windowsSteps | Where-Object Name -ceq "Build Plugin")
$packageSteps = @($windowsSteps | Where-Object Name -ceq "Package")
$releaseWrapperScriptPattern = 'scripts[\\/]run-release-linked-gates\.ps1'
$linkedGateSteps = @($windowsSteps | Where-Object {
        $candidateRunText = Remove-PowerShellComments (Get-YamlStepRunText $_.Text)
        @(Get-TopLevelWorkflowInvocationCommands $candidateRunText $releaseWrapperScriptPattern).Count -eq 1
    })
$linkedGateText = if ($linkedGateSteps.Count -eq 1) { $linkedGateSteps[0].Text } else { "" }
$linkedGateRunText = if ($linkedGateSteps.Count -eq 1) {
    Remove-PowerShellComments (Get-YamlStepRunText $linkedGateText)
} else {
    ""
}
$linkedGateInvocationCommands = @(Get-TopLevelWorkflowInvocationCommands $linkedGateRunText $releaseWrapperScriptPattern)
$linkedGateInvocation = if ($linkedGateInvocationCommands.Count -eq 1) { $linkedGateInvocationCommands[0] } else { $null }
$linkedGateInvocationText = if ($null -ne $linkedGateInvocation) { $linkedGateInvocation.Extent.Text } else { "" }
Add-PolicyCheck `
    "Windows workflow executes the checked release-linked wrapper exactly once" `
    ($linkedGateSteps.Count -eq 1 -and
        $linkedGateInvocationCommands.Count -eq 1) `
    "Expected exactly one Windows step invoking scripts/run-release-linked-gates.ps1; found $($linkedGateSteps.Count)."

$gateOrderIsCorrect = (
    $buildSteps.Count -eq 1 -and
    $packageSteps.Count -eq 1 -and
    $linkedGateSteps.Count -eq 1 -and
    $linkedGateSteps[0].Index -gt $buildSteps[0].Index -and
    $linkedGateSteps[0].Index -lt $packageSteps[0].Index
)
Add-PolicyCheck `
    "Release-linked wrapper runs after build and before Package" `
    $gateOrderIsCorrect `
    "The linked gate must block packaging and run strictly between Build Plugin and Package."
Add-PolicyCheck `
    "Release-linked workflow step is common and unconditional" `
    ($linkedGateSteps.Count -eq 1 -and
        [regex]::IsMatch($linkedGateText, '(?im)^        shell:\s*pwsh\s*$') -and
        -not [regex]::IsMatch($windowsJob, '(?m)^    if:\s*') -and
        -not [regex]::IsMatch($linkedGateText, '(?m)^        if:\s*') -and
        -not [regex]::IsMatch($linkedGateText, '(?i)continue-on-error\s*:')) `
    "Use one required pwsh step for every Windows matrix row; job/step if conditions and continue-on-error are forbidden."
Add-PolicyCheck `
    "Release-linked workflow step has an exact 20-minute limit" `
    ($linkedGateSteps.Count -eq 1 -and [regex]::IsMatch($linkedGateText, '(?im)^\s+timeout-minutes:\s*20\s*$')) `
    "Set timeout-minutes: 20 on the release-linked gate step."

$workflowRuntimeArguments = [ordered]@{
    "ObsRundir" = '(?is)-ObsRundir\s+[^\r\n]*obs-studio[\\/]build_x64[\\/]rundir'
    "LibobsRuntimeDirectory" = '(?is)-LibobsRuntimeDirectory\s+[^\r\n]*obs-studio[\\/]build_x64[\\/]libobs'
    "PthreadsRuntimeDirectory" = '(?is)-PthreadsRuntimeDirectory\s+[^\r\n]*w32-pthreads'
    "ObsDepsBinDirectory" = '(?is)-ObsDepsBinDirectory\s+[^\r\n]*(?:obs-deps|ffmpegDepsRoot|obsDeps)'
    "ExpectedFfmpegImports" = '(?i)-ExpectedFfmpegImports\s+"?__GH_matrix_ffmpeg_imports__\b'
}
Add-PolicyCheck `
    "Release-linked invocation supplies exact BuildDirectory=build" `
    ($null -ne $linkedGateInvocation -and
        (Get-ExactPowerShellCommandArgument $linkedGateInvocation "BuildDirectory") -ceq "build") `
    "The top-level wrapper command must pass the exact scalar -BuildDirectory build; stale/suffixed paths are forbidden."
foreach ($runtimeArgument in $workflowRuntimeArguments.GetEnumerator()) {
    Add-PolicyCheck `
        "Release-linked invocation supplies $($runtimeArgument.Key)" `
        ($linkedGateSteps.Count -eq 1 -and [regex]::IsMatch($linkedGateInvocationText, $runtimeArgument.Value)) `
        "The wrapper invocation must explicitly supply -$($runtimeArgument.Key) from the clean Windows job."
}

$releaseJob = Get-YamlJobBlock $buildWorkflowText "release"
Add-PolicyCheck `
    "Published releases depend on build-windows" `
    ([regex]::IsMatch($releaseJob, '(?im)^    needs:\s*(?:\[[^\]]*\bbuild-windows\b[^\]]*\]|build-windows\s*)$')) `
    "The release job must list build-windows in needs so failed linked gates prevent publication."

$allNormalCiSteps = @(Get-YamlSteps $normalCiText)
$normalCiExecutableText = @($allNormalCiSteps | ForEach-Object {
        Remove-PowerShellComments (Get-YamlStepRunText $_.Text)
    }) -join "`n"
$normalCiEnablesLinkedGates = [regex]::IsMatch(
    $normalCiExecutableText,
    '(?i)-D(?:BUILD_NATIVE_MEDIA_LINKED_GATE|BUILD_MODULE_LIFECYCLE_LINKED_GATE)(?::BOOL)?=ON\b'
)
Add-PolicyCheck `
    "Dependency-light normal CI does not enable release linked gates" `
    (-not $normalCiEnablesLinkedGates) `
    ".github/workflows/ci.yml must remain independent of the OBS/FFmpeg/libdatachannel linked release gates."

$normalTestJob = Get-YamlJobBlock $normalCiText "test"
$normalCiSteps = @(Get-YamlSteps $normalTestJob)
$normalPolicyScriptPattern = 'tests[\\/]test-release-linked-wiring\.ps1'
$normalCiPolicySteps = @($normalCiSteps | Where-Object {
        $candidateRunText = Remove-PowerShellComments (Get-YamlStepRunText $_.Text)
        @(Get-TopLevelWorkflowInvocationCommands $candidateRunText $normalPolicyScriptPattern).Count -eq 1
    })
$normalCiPolicyText = if ($normalCiPolicySteps.Count -eq 1) { $normalCiPolicySteps[0].Text } else { "" }
$normalCiPolicyRunText = if ($normalCiPolicySteps.Count -eq 1) {
    Remove-PowerShellComments (Get-YamlStepRunText $normalCiPolicyText)
} else {
    ""
}
$normalCiPolicyInvocations = @(Get-TopLevelWorkflowInvocationCommands $normalCiPolicyRunText $normalPolicyScriptPattern)
Add-PolicyCheck `
    "Dependency-light normal CI unconditionally executes the static policy once" `
    ($normalCiPolicySteps.Count -eq 1 -and
        [regex]::IsMatch($normalCiPolicyText, '(?im)^\s+shell:\s*pwsh\s*$') -and
        $normalCiPolicyInvocations.Count -eq 1 -and
        -not [regex]::IsMatch($normalTestJob, '(?m)^    if:\s*') -and
        -not [regex]::IsMatch($normalCiPolicyText, '(?m)^        if:\s*') -and
        -not [regex]::IsMatch($normalCiPolicyText, '(?i)continue-on-error\s*:')) `
    "Invoke tests/test-release-linked-wiring.ps1 exactly once from a required pwsh step in .github/workflows/ci.yml."

$normalMutationPolicyScriptPattern = 'tests[\\/]test-release-linked-wiring-policy-mutations\.ps1'
$normalCiMutationPolicySteps = @($normalCiSteps | Where-Object {
        $candidateRunText = Remove-PowerShellComments (Get-YamlStepRunText $_.Text)
        @(Get-TopLevelWorkflowInvocationCommands $candidateRunText $normalMutationPolicyScriptPattern).Count -eq 1
    })
$normalCiMutationPolicyText = if ($normalCiMutationPolicySteps.Count -eq 1) {
    $normalCiMutationPolicySteps[0].Text
} else {
    ""
}
$normalCiMutationPolicyRunText = if ($normalCiMutationPolicySteps.Count -eq 1) {
    Remove-PowerShellComments (Get-YamlStepRunText $normalCiMutationPolicyText)
} else {
    ""
}
$normalCiMutationPolicyInvocations = @(
    Get-TopLevelWorkflowInvocationCommands $normalCiMutationPolicyRunText $normalMutationPolicyScriptPattern
)
Add-PolicyCheck `
    "Dependency-light normal CI unconditionally executes the policy mutation harness once" `
    ($normalCiMutationPolicySteps.Count -eq 1 -and
        [regex]::IsMatch($normalCiMutationPolicyText, '(?im)^\s+shell:\s*pwsh\s*$') -and
        $normalCiMutationPolicyInvocations.Count -eq 1 -and
        -not [regex]::IsMatch($normalTestJob, '(?m)^    if:\s*') -and
        -not [regex]::IsMatch($normalCiMutationPolicyText, '(?m)^        if:\s*') -and
        -not [regex]::IsMatch($normalCiMutationPolicyText, '(?i)continue-on-error\s*:')) `
    "Invoke tests/test-release-linked-wiring-policy-mutations.ps1 exactly once from a required pwsh step in .github/workflows/ci.yml."

$wrapperPath = Join-Path $repositoryPath "scripts/run-release-linked-gates.ps1"
Add-PolicyCheck `
    "Checked release-linked wrapper exists" `
    (Test-Path -LiteralPath $wrapperPath -PathType Leaf) `
    "Create scripts/run-release-linked-gates.ps1; the release workflow must not hide this policy in inline YAML."
Add-PolicyCheck `
    "Wrapper is valid strict PowerShell" `
    ($wrapperRawText.Length -gt 0 -and
        @($wrapperParseErrors).Count -eq 0 -and
        [regex]::IsMatch($wrapperText, '(?im)^\s*Set-StrictMode\s+-Version\s+Latest\s*$') -and
        [regex]::IsMatch($wrapperText, '(?im)^\s*\$ErrorActionPreference\s*=\s*["'']Stop["'']\s*$')) `
    "The checked wrapper must parse cleanly, enable Set-StrictMode -Version Latest, and set ErrorActionPreference to Stop."

$checkedCommandFunction = Get-ReachablePowerShellFunctionText $wrapperAst "Invoke-CheckedCommand" $wrapperReachableFunctionNames
Add-PolicyCheck `
    "Wrapper centralizes external tools with fatal exit propagation" `
    ($checkedCommandFunction.Length -gt 0 -and
        [regex]::IsMatch(
            $checkedCommandFunction,
            '(?is)(?:\$commandOutput\s*=\s*)?&\s*\$FilePath\s+@ArgumentList\s*\r?\n\s*\$commandExitCode\s*=\s*\$LASTEXITCODE\s*\r?\n\s*if\s*\(\s*\$commandExitCode\s+-ne\s+0\s*\)\s*\{.*?\bthrow\b'
        ) -and
        -not [regex]::IsMatch($checkedCommandFunction, '(?i)\$LASTEXITCODE\s*=')) `
    "Invoke-CheckedCommand must immediately capture LASTEXITCODE into commandExitCode and throw from that value; assigning/resetting LASTEXITCODE is forbidden."

$checkedCommandCalls = @(Get-ReachablePowerShellCommandTexts $wrapperAst "Invoke-CheckedCommand" $wrapperReachableFunctionNames)
$ctestCalls = @($checkedCommandCalls | Where-Object {
        [regex]::IsMatch($_, '(?i)(?:-FilePath\s+)?["'']?ctest(?:\.exe)?["'']?(?:\s|,|$)')
    })
$ctestDiscoveryCalls = @($ctestCalls | Where-Object {
        [regex]::IsMatch($_, '(?i)--show-only(?:=|["'']?\s*,?\s*["''])json-v1')
    })
$ctestExecutionCalls = @($ctestCalls | Where-Object {
        -not [regex]::IsMatch($_, '(?i)--show-only')
    })
$importToolCalls = @($checkedCommandCalls | Where-Object {
        [regex]::IsMatch($_, '(?is)(?:-FilePath\s+)?["'']?(?:dumpbin(?:\.exe)?|llvm-readobj(?:\.exe)?)["'']?.*?/(?:DEPENDENTS|IMPORTS)|--coff-imports')
    })
$directExternalToolCalls = @(
    foreach ($toolName in @("ctest", "ctest.exe", "dumpbin", "dumpbin.exe", "llvm-readobj", "llvm-readobj.exe")) {
        Get-ReachablePowerShellCommandTexts $wrapperAst $toolName $wrapperReachableFunctionNames
    }
)
Add-PolicyCheck `
    "CTest and import tooling only run through Invoke-CheckedCommand" `
    ($ctestCalls.Count -eq 2 -and $importToolCalls.Count -ge 1 -and $directExternalToolCalls.Count -eq 0) `
    "Expected exactly two checked ctest calls and at least one checked import-tool call, with no direct invocations; checked ctest=$($ctestCalls.Count), checked imports=$($importToolCalls.Count), direct=$($directExternalToolCalls.Count)."

$wrapperExpectedTests = @(Get-DoubleQuotedArrayValues $wrapperText "ExpectedReleaseLinkedTests")
Add-PolicyCheck `
    "Wrapper declares the exact ten-test release inventory" `
    (Test-ExactSequenceSet $wrapperExpectedTests $expectedReleaseLinkedTests) `
    ("ExpectedReleaseLinkedTests mismatch: " + (Format-SetDifference $wrapperExpectedTests $expectedReleaseLinkedTests))
Add-PolicyCheck `
    "Wrapper discovers CTest inventory as JSON" `
    ($ctestDiscoveryCalls.Count -eq 1 -and $ctestExecutionCalls.Count -eq 1) `
    "Use ctest --show-only=json-v1 so the wrapper can reject missing, extra, or renamed release tests."
Add-PolicyCheck `
    "Wrapper compares discovered tests with the exact inventory" `
    (@(Get-ReachablePowerShellCommandTexts $wrapperAst "Assert-ExactSet" $wrapperReachableFunctionNames | Where-Object {
                [regex]::IsMatch($_, '(?i)\$ExpectedReleaseLinkedTests\b') -and
                [regex]::IsMatch($_, '(?i)\$(?:discovered|actual)(?:ReleaseLinked)?Tests\b')
            }).Count -eq 1) `
    "Compare the discovered CTest names against ExpectedReleaseLinkedTests before executing them."
Add-PolicyCheck `
    "CTest discovery JSON feeds the compared inventory" `
    (@(Get-ReachablePowerShellCommandTexts $wrapperAst "ConvertFrom-Json" $wrapperReachableFunctionNames).Count -eq 1 -and
        [regex]::IsMatch($wrapperText, '(?is)\$(?:ctestDiscoveryJson|discoveryJson)\b.*?ConvertFrom-Json') -and
        [regex]::IsMatch($wrapperText, '(?is)ConvertFrom-Json.*?\$(?:discovered|actual)(?:ReleaseLinked)?Tests\b')) `
    "Parse the checked --show-only=json-v1 output and derive the discovered test names used by Assert-ExactSet."
Add-PolicyCheck `
    "Wrapper makes an empty CTest selection fatal" `
    ($ctestExecutionCalls.Count -eq 1 -and [regex]::IsMatch($ctestExecutionCalls[0], '(?i)--no-tests=error')) `
    "The linked ctest invocation must include --no-tests=error."
Add-PolicyCheck `
    "Wrapper serializes linked CTest execution" `
    ($ctestExecutionCalls.Count -eq 1 -and [regex]::IsMatch($ctestExecutionCalls[0], '(?i)--parallel["'']?\s*,?\s*["'']?1\b')) `
    "The linked ctest invocation must include --parallel 1 for deterministic lifetime coverage."
Add-PolicyCheck `
    "Wrapper emits linked CTest failures" `
    ($ctestExecutionCalls.Count -eq 1 -and [regex]::IsMatch($ctestExecutionCalls[0], '(?i)--output-on-failure')) `
    "The linked ctest invocation must include --output-on-failure."
Add-PolicyCheck `
    "Both CTest calls select only the exact release-linked label" `
    ($ctestCalls.Count -eq 2 -and @($ctestCalls | Where-Object {
                [regex]::Matches($_, '(?i)(?:["'']-L["'']|-L)\s*(?:,\s*)?(?:["'']\^release-linked\$["'']|\^release-linked\$)').Count -eq 1 -and
                [regex]::Matches($_, '(?i)(?:["'']-L["'']|(?<![A-Za-z0-9_-])-L(?![A-Za-z0-9_-]))').Count -eq 1
            }).Count -eq 2) `
    "Discovery and execution must each use exactly one -L '^release-linked$' argument."
Add-PolicyCheck `
    "Wrapper stops on the first linked failure" `
    ($ctestExecutionCalls.Count -eq 1 -and [regex]::IsMatch($ctestExecutionCalls[0], '(?i)--stop-on-failure')) `
    "The linked ctest invocation must include --stop-on-failure."

$exactSetFunction = Get-ReachablePowerShellFunctionText $wrapperAst "Assert-ExactSet" $wrapperReachableFunctionNames
Add-PolicyCheck `
    "Exact-set mismatches are fatal" `
    ($exactSetFunction.Length -gt 0 -and
        [regex]::IsMatch($exactSetFunction, '(?i)\bCompare-Object\b') -and
        [regex]::IsMatch($exactSetFunction, '(?is)\bif\s*\(.*?\).*?\bthrow\b')) `
    "Assert-ExactSet must test Compare-Object output and throw on every mismatch."

$expectedCMakeCacheEntries = @(
    "BUILD_PLUGIN:BOOL=ON",
    "BUILD_NATIVE_MEDIA_LINKED_GATE:BOOL=ON",
    "BUILD_MODULE_LIFECYCLE_LINKED_GATE:BOOL=ON"
)
$wrapperCMakeCacheEntries = @(Get-DoubleQuotedArrayValues $wrapperText "ExpectedCMakeCacheEntries")
Add-PolicyCheck `
    "Wrapper declares the exact required CMake cache options" `
    (Test-ExactSequenceSet $wrapperCMakeCacheEntries $expectedCMakeCacheEntries) `
    ("ExpectedCMakeCacheEntries mismatch: " + (Format-SetDifference $wrapperCMakeCacheEntries $expectedCMakeCacheEntries))
$cacheValidationFunction = Get-ReachablePowerShellFunctionText $wrapperAst "Assert-CMakeCacheContract" $wrapperReachableFunctionNames
$cacheEntryLoopRecords = @(Get-ReachablePowerShellForeachRecords $wrapperAst "ExpectedCMakeCacheEntries" $wrapperReachableFunctionNames)
$cacheEntryLoopBodies = @($cacheEntryLoopRecords | ForEach-Object Body)
Add-PolicyCheck `
    "Wrapper validates the configured CMake cache" `
    ($cacheValidationFunction.Length -gt 0 -and
        [regex]::IsMatch($cacheValidationFunction, '(?i)CMakeCache\.txt') -and
        $cacheEntryLoopBodies.Count -eq 1 -and
        [regex]::IsMatch($cacheEntryLoopBodies[0], '(?i)(?:Select-String|-contains|Contains|IndexOf)') -and
        [regex]::IsMatch($cacheEntryLoopBodies[0], '(?i)\bthrow\b') -and
        @(Get-ReachablePowerShellCommandTexts $wrapperAst "Assert-CMakeCacheContract" $wrapperReachableFunctionNames).Count -eq 1) `
    "Read CMakeCache.txt and reject any missing BUILD_PLUGIN/native/module =ON entry."
Add-PolicyCheck `
    "Wrapper resolves the explicit plog cache path" `
    ($cacheValidationFunction.Length -gt 0 -and
        [regex]::IsMatch($cacheValidationFunction, '(?i)LIBDATACHANNEL_PLOG_INCLUDE_DIR:PATH=') -and
        [regex]::IsMatch($cacheValidationFunction, '(?i)(?:Resolve-Path|Test-Path)') -and
        [regex]::IsMatch($cacheValidationFunction, '(?i)\bthrow\b')) `
    "Extract LIBDATACHANNEL_PLOG_INCLUDE_DIR:PATH from CMakeCache.txt and require its resolved directory to exist."

$expectedGateExecutables = @(
    "vdoninja-module-lifecycle-linked-gate.exe",
    "vdoninja-native-media-linked-gate.exe"
)
$wrapperGateExecutables = @(Get-DoubleQuotedArrayValues $wrapperText "ExpectedReleaseLinkedExecutables")
Add-PolicyCheck `
    "Wrapper declares the exact two linked executables" `
    (Test-ExactSequenceSet $wrapperGateExecutables $expectedGateExecutables) `
    ("ExpectedReleaseLinkedExecutables mismatch: " + (Format-SetDifference $wrapperGateExecutables $expectedGateExecutables))
$gateExecutableLoopRecords = @(Get-ReachablePowerShellForeachRecords $wrapperAst "ExpectedReleaseLinkedExecutables" $wrapperReachableFunctionNames)
$gateExecutableLoopBodies = @($gateExecutableLoopRecords | ForEach-Object Body)
Add-PolicyCheck `
    "Wrapper fatally validates both linked executable paths" `
    ($gateExecutableLoopBodies.Count -eq 1 -and
        [regex]::IsMatch($gateExecutableLoopBodies[0], '(?i)\bTest-Path\b') -and
        [regex]::IsMatch($gateExecutableLoopBodies[0], '(?i)\bthrow\b')) `
    "Iterate ExpectedReleaseLinkedExecutables, require every built executable to exist, and throw when one is absent."
$expectedPeArtifacts = @(
    "obs-vdoninja.dll",
    "vdoninja-module-lifecycle-linked-gate.exe",
    "vdoninja-native-media-linked-gate.exe"
)
$wrapperPeArtifacts = @(Get-DoubleQuotedArrayValues $wrapperText "ExpectedReleasePeArtifacts")
Add-PolicyCheck `
    "Wrapper declares the plugin and both gates as PE policy artifacts" `
    (Test-ExactSequenceSet $wrapperPeArtifacts $expectedPeArtifacts) `
    ("ExpectedReleasePeArtifacts mismatch: " + (Format-SetDifference $wrapperPeArtifacts $expectedPeArtifacts))
$peValidationFunction = Get-ReachablePowerShellFunctionText $wrapperAst "Assert-PeAmd64" $wrapperReachableFunctionNames
$peArtifactLoopRecords = @(Get-ReachablePowerShellForeachRecords $wrapperAst "ExpectedReleasePeArtifacts" $wrapperReachableFunctionNames)
$peArtifactLoopBodies = @($peArtifactLoopRecords | ForEach-Object Body)
$peArtifactLoopUsesExactArtifact = (
    $peArtifactLoopRecords.Count -eq 1 -and
    $peArtifactLoopRecords[0].Iterator -ceq "artifact" -and
    [regex]::IsMatch($peArtifactLoopRecords[0].Body, '(?i)\$artifactPath\s*=\s*Join-Path\s+\$BuildDirectory\s+\$artifact\b') -and
    [regex]::IsMatch($peArtifactLoopRecords[0].Body, '(?i)Assert-PeAmd64\s+-Path\s+\$artifactPath\b') -and
    [regex]::IsMatch(
        $peArtifactLoopRecords[0].Body,
        '(?is)\$importsByArtifact\s*\[\s*\$artifact\s*\]\s*=\s*@?\(\s*Get-PeImports\s+-Path\s+\$artifactPath\s*\)'
    )
)
Add-PolicyCheck `
    "Wrapper enforces PE AMD64 for the plugin and both linked executables" `
    ($peValidationFunction.Length -gt 0 -and
        [regex]::IsMatch($peValidationFunction, '(?i)(?:0x8664|34404)') -and
        [regex]::IsMatch($peValidationFunction, '(?i)BitConverter\]::ToInt32\s*\([^\)]*(?:0x3c|60)') -and
        [regex]::IsMatch($peValidationFunction, '(?i)BitConverter\]::ToUInt16') -and
        [regex]::IsMatch($peValidationFunction, '(?i)\bthrow\b') -and
        $peArtifactLoopUsesExactArtifact) `
    "Read obs-vdoninja.dll and both linked executable PE headers and require IMAGE_FILE_MACHINE_AMD64 (0x8664)."
Add-PolicyCheck `
    "Wrapper inspects imports for the plugin and both linked executables" `
    ($importToolCalls.Count -ge 1 -and
        $peArtifactLoopUsesExactArtifact) `
    "Inspect imports for obs-vdoninja.dll and both linked executables with an available Windows tool before running them."

$expectedBlockedImports = @("datachannel.dll", "libcrypto-3-x64.dll", "libssl-3-x64.dll")
$wrapperBlockedImports = @(Get-DoubleQuotedArrayValues $wrapperText "BlockedDynamicImports")
Add-PolicyCheck `
    "Wrapper rejects the exact forbidden dynamic imports" `
    (Test-ExactSequenceSet $wrapperBlockedImports $expectedBlockedImports) `
    ("BlockedDynamicImports mismatch: " + (Format-SetDifference $wrapperBlockedImports $expectedBlockedImports))
$blockedImportLoopRecords = @(Get-ReachablePowerShellForeachRecords $wrapperAst "BlockedDynamicImports" $wrapperReachableFunctionNames)
$blockedImportLoopBodies = @($blockedImportLoopRecords | ForEach-Object Body)
Add-PolicyCheck `
    "Forbidden imports are consumed by a fatal validation loop" `
    ($blockedImportLoopBodies.Count -eq 1 -and
        [regex]::IsMatch($blockedImportLoopBodies[0], '(?i)(?:importsByArtifact|actualImports|artifactImports)') -and
        [regex]::IsMatch($blockedImportLoopBodies[0], '(?i)\bthrow\b')) `
    "Iterate BlockedDynamicImports against inspected artifact imports and throw on every match."
$reachableExactSetCalls = @(Get-ReachablePowerShellCommandTexts $wrapperAst "Assert-ExactSet" $wrapperReachableFunctionNames)
Add-PolicyCheck `
    "Wrapper compares actual and matrix-expected FFmpeg imports" `
    ([regex]::IsMatch($wrapperText, '(?is)\$expectedFfmpegSet\s*=\s*@?\(\s*\$ExpectedFfmpegImports\s+-split\s*["''],["'']\s*\)') -and
        @($reachableExactSetCalls | Where-Object {
                [regex]::IsMatch($_, '(?i)\$expectedFfmpegSet\b')
            }).Count -ge 1 -and
        @($reachableExactSetCalls | Where-Object {
                [regex]::IsMatch($_, '(?i)\$ExpectedFfmpegImports\b')
            }).Count -eq 0) `
    "Split ExpectedFfmpegImports into expectedFfmpegSet and compare that parsed four-element set; the raw comma string is forbidden in Assert-ExactSet."

$expectedObsImportArtifacts = @(
    "obs-vdoninja.dll",
    "vdoninja-native-media-linked-gate.exe"
)
$wrapperObsImportArtifacts = @(Get-DoubleQuotedArrayValues $wrapperText "ExpectedObsImportArtifacts")
Add-PolicyCheck `
    "Plugin and native gate must import obs.dll" `
    (Test-ExactSequenceSet $wrapperObsImportArtifacts $expectedObsImportArtifacts) `
    ("ExpectedObsImportArtifacts mismatch: " + (Format-SetDifference $wrapperObsImportArtifacts $expectedObsImportArtifacts))
$obsImportLoopRecords = @(Get-ReachablePowerShellForeachRecords $wrapperAst "ExpectedObsImportArtifacts" $wrapperReachableFunctionNames)
$obsImportLoopBodies = @($obsImportLoopRecords | ForEach-Object Body)
Add-PolicyCheck `
    "Wrapper requires the OBS runtime import" `
    ($obsImportLoopBodies.Count -eq 1 -and
        $obsImportLoopRecords[0].Iterator -ceq "artifact" -and
        [regex]::IsMatch($obsImportLoopBodies[0], '(?is)\$actualImports\s*=\s*@?\(\s*\$importsByArtifact\s*\[\s*\$artifact\s*\]\s*\)') -and
        [regex]::IsMatch($obsImportLoopBodies[0], '(?i)obs\.dll') -and
        [regex]::IsMatch($obsImportLoopBodies[0], '(?i)(?:importsByArtifact|actualImports|artifactImports)') -and
        [regex]::IsMatch($obsImportLoopBodies[0], '(?i)\bthrow\b')) `
    "Require obs.dll on obs-vdoninja.dll and vdoninja-native-media-linked-gate.exe."

$expectedFfmpegImportArtifacts = @(
    "obs-vdoninja.dll",
    "vdoninja-native-media-linked-gate.exe"
)
$wrapperFfmpegImportArtifacts = @(Get-DoubleQuotedArrayValues $wrapperText "ExpectedFfmpegImportArtifacts")
Add-PolicyCheck `
    "Plugin and native gate use the exact matrix FFmpeg import set" `
    (Test-ExactSequenceSet $wrapperFfmpegImportArtifacts $expectedFfmpegImportArtifacts) `
    ("ExpectedFfmpegImportArtifacts mismatch: " + (Format-SetDifference $wrapperFfmpegImportArtifacts $expectedFfmpegImportArtifacts))
$ffmpegImportLoopRecords = @(Get-ReachablePowerShellForeachRecords $wrapperAst "ExpectedFfmpegImportArtifacts" $wrapperReachableFunctionNames)
$ffmpegImportLoopBodies = @($ffmpegImportLoopRecords | ForEach-Object Body)
Add-PolicyCheck `
    "Wrapper applies the exact FFmpeg comparison to both artifacts" `
    ($ffmpegImportLoopBodies.Count -eq 1 -and
        $ffmpegImportLoopRecords[0].Iterator -ceq "artifact" -and
        [regex]::IsMatch($ffmpegImportLoopBodies[0], '(?is)\$actualImports\s*=\s*@?\(\s*\$importsByArtifact\s*\[\s*\$artifact\s*\]\s*\)') -and
        [regex]::IsMatch($ffmpegImportLoopBodies[0], '(?i)\$expectedFfmpegSet\b') -and
        [regex]::IsMatch($ffmpegImportLoopBodies[0], '(?i)(?:importsByArtifact|actualImports|artifactImports)') -and
        [regex]::IsMatch($ffmpegImportLoopBodies[0], '(?i)\bAssert-ExactSet\b')) `
    "Compare both obs-vdoninja.dll and the native linked gate against the matrix ExpectedFfmpegImports set."

$expectedModuleIsolatedArtifacts = @("vdoninja-module-lifecycle-linked-gate.exe")
$wrapperModuleIsolatedArtifacts = @(Get-DoubleQuotedArrayValues $wrapperText "NoObsOrFfmpegImportArtifacts")
Add-PolicyCheck `
    "Module linked gate is isolated from OBS and FFmpeg" `
    (Test-ExactSequenceSet $wrapperModuleIsolatedArtifacts $expectedModuleIsolatedArtifacts) `
    ("NoObsOrFfmpegImportArtifacts mismatch: " + (Format-SetDifference $wrapperModuleIsolatedArtifacts $expectedModuleIsolatedArtifacts))
$moduleImportLoopRecords = @(Get-ReachablePowerShellForeachRecords $wrapperAst "NoObsOrFfmpegImportArtifacts" $wrapperReachableFunctionNames)
$moduleImportLoopBodies = @($moduleImportLoopRecords | ForEach-Object Body)

$knownFfmpegImports = @(
    "avcodec-61.dll",
    "avutil-59.dll",
    "swscale-8.dll",
    "swresample-5.dll",
    "avcodec-62.dll",
    "avutil-60.dll",
    "swscale-9.dll",
    "swresample-6.dll"
)
$wrapperKnownFfmpegImports = @(Get-DoubleQuotedArrayValues $wrapperText "KnownFfmpegImports")
Add-PolicyCheck `
    "Wrapper knows every FFmpeg ABI in the two-row release matrix" `
    (Test-ExactSequenceSet $wrapperKnownFfmpegImports $knownFfmpegImports) `
    ("KnownFfmpegImports mismatch: " + (Format-SetDifference $wrapperKnownFfmpegImports $knownFfmpegImports))
Add-PolicyCheck `
    "Wrapper rejects OBS or any matrix FFmpeg ABI on the module gate" `
    ($moduleImportLoopBodies.Count -eq 1 -and
        $moduleImportLoopRecords[0].Iterator -ceq "artifact" -and
        [regex]::IsMatch($moduleImportLoopBodies[0], '(?is)\$actualImports\s*=\s*@?\(\s*\$importsByArtifact\s*\[\s*\$artifact\s*\]\s*\)') -and
        [regex]::IsMatch($moduleImportLoopBodies[0], '(?i)\$KnownFfmpegImports\b') -and
        [regex]::IsMatch($moduleImportLoopBodies[0], '(?i)obs\.dll') -and
        [regex]::IsMatch($moduleImportLoopBodies[0], '(?i)(?:importsByArtifact|actualImports|artifactImports)') -and
        [regex]::IsMatch($moduleImportLoopBodies[0], '(?i)\bthrow\b')) `
    "Require the module lifecycle gate to import neither obs.dll nor any known FFmpeg ABI DLL."

$expectedRuntimeVariables = @(
    "ObsRundir",
    "LibobsRuntimeDirectory",
    "PthreadsRuntimeDirectory",
    "ObsDepsBinDirectory"
)
$wrapperRuntimeVariables = @(Get-DollarVariableArrayValues $wrapperText "RequiredRuntimeDirectories")
Add-PolicyCheck `
    "Wrapper declares the exact runtime directory set" `
    (Test-ExactSequenceSet $wrapperRuntimeVariables $expectedRuntimeVariables) `
    ("RequiredRuntimeDirectories mismatch: " + (Format-SetDifference $wrapperRuntimeVariables $expectedRuntimeVariables))
foreach ($runtimeVariable in $expectedRuntimeVariables) {
    $runtimeParameterPattern = '(?is)\bparam\s*\(.*?\$' + [regex]::Escape($runtimeVariable) + '\b'
    Add-PolicyCheck `
        "Wrapper accepts runtime directory: $runtimeVariable" `
        ([regex]::IsMatch($wrapperText, $runtimeParameterPattern)) `
        "Declare -$runtimeVariable as a required wrapper parameter."
}
$runtimeDirectoryLoopRecords = @(Get-ReachablePowerShellForeachRecords $wrapperAst "RequiredRuntimeDirectories" $wrapperReachableFunctionNames)
$runtimeDirectoryLoopBodies = @($runtimeDirectoryLoopRecords | ForEach-Object Body)
Add-PolicyCheck `
    "Wrapper validates every runtime directory" `
    ($runtimeDirectoryLoopBodies.Count -eq 1 -and
        $runtimeDirectoryLoopRecords[0].Iterator -ceq "runtimeDirectory" -and
        [regex]::IsMatch($runtimeDirectoryLoopBodies[0], '(?i)\bTest-Path\b[^\r\n]*\$runtimeDirectory\b') -and
        [regex]::IsMatch($runtimeDirectoryLoopBodies[0], '(?i)\bResolve-Path\b[^\r\n]*\$runtimeDirectory\b') -and
        [regex]::IsMatch($runtimeDirectoryLoopBodies[0], '(?i)\bthrow\b')) `
    "Reject any missing OBS rundir, libobs, pthreads, or non-Qt OBS dependency bin directory."
$reachablePathAssignments = @(
    $wrapperAst.FindAll({
            param($node)
            $node -is [System.Management.Automation.Language.AssignmentStatementAst]
        }, $true) | Where-Object {
        (Test-IsReachablePowerShellNode $_ $wrapperReachableFunctionNames) -and
        $_.Left.Extent.Text -ieq '$env:PATH'
    }
)
$pathRuntimeArrayReferences = if ($reachablePathAssignments.Count -eq 1) {
    @(
        [regex]::Matches($reachablePathAssignments[0].Right.Extent.Text, '(?i)\$([A-Za-z_][A-Za-z0-9_]*RuntimeDirectories[A-Za-z0-9_]*)') |
            ForEach-Object { $_.Groups[1].Value }
    )
} else {
    @()
}
Add-PolicyCheck `
    "Wrapper prepends all runtime directories to PATH" `
    ($reachablePathAssignments.Count -eq 1 -and
        (Test-ExactSequenceSet $pathRuntimeArrayReferences @("RequiredRuntimeDirectories")) -and
        [regex]::IsMatch($reachablePathAssignments[0].Right.Extent.Text, '(?i)\$RequiredRuntimeDirectories\b\s+-join\s+\[IO\.Path\]::PathSeparator')) `
    "Prepend the validated runtime directory set to PATH before invoking either linked executable."

$passedCount = @($results | Where-Object Passed).Count
$failed = @($results | Where-Object { -not $_.Passed })
foreach ($result in $results) {
    if ($result.Passed) {
        Write-Host "[PASS] $($result.Name)"
    } else {
        Write-Host "[FAIL] $($result.Name) :: $($result.FailureDetail)"
    }
}

Write-Host "Release-linked wiring policy: $passedCount passed, $($failed.Count) failed, $($results.Count) total."
if ($failed.Count -gt 0) {
    exit 1
}

exit 0
