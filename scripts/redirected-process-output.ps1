function Start-RedirectedProcessOutputDrain {
    param(
        [Parameter(Mandatory = $true)]
        [System.Diagnostics.Process]$Process
    )

    if (-not $Process.StartInfo.RedirectStandardOutput -or
        -not $Process.StartInfo.RedirectStandardError) {
        throw "The process must redirect both standard output and standard error"
    }

    return [pscustomobject]@{
        stdoutTask = $Process.StandardOutput.ReadToEndAsync()
        stderrTask = $Process.StandardError.ReadToEndAsync()
    }
}

function Complete-RedirectedProcessOutputDrain {
    param(
        [Parameter(Mandatory = $true)]
        [object]$Drain
    )

    return [pscustomobject]@{
        stdout = $Drain.stdoutTask.GetAwaiter().GetResult()
        stderr = $Drain.stderrTask.GetAwaiter().GetResult()
    }
}
