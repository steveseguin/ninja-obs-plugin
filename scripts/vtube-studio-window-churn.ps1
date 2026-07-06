param(
    [string]$OutputJson = "",
    [string]$Sizes = "1280x720,1600x900,1920x1080",
    [int]$InitialDelayMs = 3000,
    [int]$StepWaitMs = 3500,
    [switch]$NoRestore
)

$ErrorActionPreference = "Stop"

Add-Type @"
using System;
using System.Runtime.InteropServices;

public static class VTubeWindowNative {
    [StructLayout(LayoutKind.Sequential)]
    public struct RECT {
        public int Left;
        public int Top;
        public int Right;
        public int Bottom;
    }

    [DllImport("user32.dll")]
    public static extern bool GetWindowRect(IntPtr hWnd, out RECT lpRect);

    [DllImport("user32.dll")]
    public static extern bool SetWindowPos(
        IntPtr hWnd,
        IntPtr hWndInsertAfter,
        int X,
        int Y,
        int cx,
        int cy,
        uint uFlags);

    [DllImport("user32.dll")]
    public static extern bool IsIconic(IntPtr hWnd);

    [DllImport("user32.dll")]
    public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);
}
"@

function Convert-Rect {
    param([VTubeWindowNative+RECT]$Rect)

    [ordered]@{
        left = $Rect.Left
        top = $Rect.Top
        right = $Rect.Right
        bottom = $Rect.Bottom
        width = $Rect.Right - $Rect.Left
        height = $Rect.Bottom - $Rect.Top
    }
}

function Get-WindowRectObject {
    param([IntPtr]$Handle)

    $rect = New-Object VTubeWindowNative+RECT
    if (-not [VTubeWindowNative]::GetWindowRect($Handle, [ref]$rect)) {
        throw "GetWindowRect failed for handle $Handle"
    }
    Convert-Rect -Rect $rect
}

function Find-VTubeStudioWindow {
    $candidates = Get-Process -ErrorAction SilentlyContinue |
        Where-Object {
            $_.MainWindowHandle -ne 0 -and (
                $_.ProcessName -match 'VTube|VTubeStudio' -or
                $_.MainWindowTitle -match 'VTube Studio|VTubeStudio'
            )
        } |
        Sort-Object StartTime -Descending -ErrorAction SilentlyContinue

    $candidates | Select-Object -First 1
}

function Parse-SizeList {
    param([string]$Text)

    $result = @()
    foreach ($item in ($Text -split ',')) {
        $trimmed = $item.Trim()
        if ($trimmed -match '^(\d+)x(\d+)$') {
            $width = [int]$matches[1]
            $height = [int]$matches[2]
            if ($width -ge 320 -and $height -ge 240) {
                $result += [pscustomobject]@{ width = $width; height = $height }
            }
        }
    }
    if ($result.Count -eq 0) {
        throw "No valid sizes were provided: $Text"
    }
    $result
}

function Set-WindowSize {
    param(
        [IntPtr]$Handle,
        [int]$Left,
        [int]$Top,
        [int]$Width,
        [int]$Height
    )

    $SWP_NOZORDER = 0x0004
    $SWP_NOACTIVATE = 0x0010
    [void][VTubeWindowNative]::SetWindowPos(
        $Handle,
        [IntPtr]::Zero,
        $Left,
        $Top,
        $Width,
        $Height,
        $SWP_NOZORDER -bor $SWP_NOACTIVATE)
}

function Test-SizeApplied {
    param(
        [object]$Rect,
        [int]$Width,
        [int]$Height
    )

    ([Math]::Abs($Rect.width - $Width) -le 12) -and
        ([Math]::Abs($Rect.height - $Height) -le 12)
}

function Test-RectChanged {
    param(
        [object]$Before,
        [object]$After
    )

    ([Math]::Abs($Before.width - $After.width) -gt 12) -or
        ([Math]::Abs($Before.height - $After.height) -gt 12)
}

if ($InitialDelayMs -gt 0) {
    Start-Sleep -Milliseconds $InitialDelayMs
}

$startedAt = Get-Date
$report = [ordered]@{
    ok = $false
    startedAt = $startedAt.ToString("o")
    finishedAt = $null
    outputJson = $OutputJson
    sizes = $Sizes
    initialDelayMs = $InitialDelayMs
    stepWaitMs = $StepWaitMs
    restored = $false
    process = $null
    wasMinimized = $false
    initialRect = $null
    originalRect = $null
    steps = @()
    error = ""
}

try {
    $process = Find-VTubeStudioWindow
    if (-not $process) {
        throw "No VTube Studio window was found"
    }

    $handle = [IntPtr]$process.MainWindowHandle
    $initial = Get-WindowRectObject -Handle $handle
    $wasMinimized = [VTubeWindowNative]::IsIconic($handle)
    if ($wasMinimized) {
        [void][VTubeWindowNative]::ShowWindow($handle, 9)
        Start-Sleep -Milliseconds 1000
    }
    $original = Get-WindowRectObject -Handle $handle
    $report["process"] = [ordered]@{
        id = $process.Id
        processName = $process.ProcessName
        mainWindowTitle = $process.MainWindowTitle
    }
    $report["wasMinimized"] = $wasMinimized
    $report["initialRect"] = $initial
    $report["originalRect"] = $original

    $appliedCount = 0
    $previousRect = $original
    foreach ($size in (Parse-SizeList -Text $Sizes)) {
        Set-WindowSize -Handle $handle -Left $original.left -Top $original.top -Width $size.width -Height $size.height
        Start-Sleep -Milliseconds $StepWaitMs
        $actual = Get-WindowRectObject -Handle $handle
        $requestedMatch = Test-SizeApplied -Rect $actual -Width $size.width -Height $size.height
        $changed = Test-RectChanged -Before $previousRect -After $actual
        $applied = $requestedMatch -or $changed
        if ($applied) {
            $appliedCount++
        }
        $report["steps"] += [pscustomobject]@{
            requestedWidth = $size.width
            requestedHeight = $size.height
            actualRect = $actual
            requestedMatch = $requestedMatch
            changed = $changed
            applied = $applied
            timestamp = (Get-Date).ToString("o")
        }
        $previousRect = $actual
    }

    $resizeError = if ($appliedCount -eq 0) {
        "VTube Studio window did not accept any resize request"
    } else {
        ""
    }

    if (-not $NoRestore) {
        Set-WindowSize -Handle $handle -Left $original.left -Top $original.top -Width $original.width -Height $original.height
        Start-Sleep -Milliseconds ([Math]::Min($StepWaitMs, 1500))
        if ($wasMinimized) {
            [void][VTubeWindowNative]::ShowWindow($handle, 6)
        }
        $report["restored"] = $true
        $report["restoredRect"] = Get-WindowRectObject -Handle $handle
    }

    if (-not [string]::IsNullOrWhiteSpace($resizeError)) {
        throw $resizeError
    }

    $report["ok"] = $true
} catch {
    $report["error"] = $_.Exception.Message
} finally {
    $report["finishedAt"] = (Get-Date).ToString("o")
    $json = $report | ConvertTo-Json -Depth 8
    if (-not [string]::IsNullOrWhiteSpace($OutputJson)) {
        $parent = Split-Path $OutputJson -Parent
        if (-not [string]::IsNullOrWhiteSpace($parent)) {
            New-Item -ItemType Directory -Path $parent -Force | Out-Null
        }
        Set-Content -Path $OutputJson -Value $json -Encoding UTF8
    }
    Write-Output $json
}

if (-not $report["ok"]) {
    exit 1
}
