#Requires -Version 5.1
# Capture one l1-handoff boot. The VM owns the pipe server and halts after one reply.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidateNotNullOrEmpty()][string]$OutputPath,
    [ValidateRange(1,300)][int]$TimeoutSeconds = 60,
    [scriptblock]$ShouldStop
)
$ErrorActionPreference = 'Stop'
$destination = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputPath)
$directory = [IO.Path]::GetDirectoryName($destination)
if (![IO.Directory]::Exists($directory)) { throw 'Capture directory must already exist.' }
if ([IO.File]::Exists($destination) -or [IO.Directory]::Exists($destination)) { throw "Capture path already exists: $destination" }

$lines = 'HV:HANDOFF','HV:OWNED','HV:EXPECTED_OOM','HV:HALT'
$bytes = [byte[]]::new(256)
$request = [byte[]]@(0x3F)
$clock = [Diagnostics.Stopwatch]::StartNew()
$status = 'waiting for the pipe'
function Get-PollMilliseconds {
    if ($ShouldStop -and (& $ShouldStop)) { throw [OperationCanceledException]::new('Operator stopped COM1 capture.') }
    $left = $TimeoutSeconds * 1000 - $clock.ElapsedMilliseconds
    if ($left -le 0) { throw [TimeoutException]::new("COM1 capture timed out after ${TimeoutSeconds}s ($status).") }
    return [int][Math]::Min(200L, $left)
}

$complete = $false
while (!$complete) {
    # Never combine evidence from either side of a pipe disconnect or VM reset.
    $length = 0
    $lineStart = 0
    $index = 0
    $pipe = [IO.Pipes.NamedPipeClientStream]::new('.', 'HV-SVM-L1-COM1',
        [IO.Pipes.PipeDirection]::InOut, [IO.Pipes.PipeOptions]::Asynchronous,
        [Security.Principal.TokenImpersonationLevel]::Anonymous)
    try {
        $poll = Get-PollMilliseconds
        try { $pipe.Connect($poll) } catch [TimeoutException] { continue }
        $status = 'connected, waiting for HV:HANDOFF'
        $nextRequest = 0L
        while (!$complete) {
            $read = $pipe.ReadAsync($bytes, $length, $bytes.Length - $length)
            do {
                # During startup the UART may not yet be listening. Keep the read
                # pending while retrying the fixed request; never retry after data.
                if ($length -eq 0 -and $clock.ElapsedMilliseconds -ge $nextRequest) {
                    $send = $pipe.WriteAsync($request, 0, 1)
                    while (!$send.Wait((Get-PollMilliseconds))) { }
                    $nextRequest = $clock.ElapsedMilliseconds + 750
                }
            } while (!$read.Wait((Get-PollMilliseconds)))
            $count = $read.Result
            if ($count -eq 0) { break }
            $end = $length + $count
            for ($i = $length; $i -lt $end; $i++) {
                if ($bytes[$i] -ne 10) { continue }
                $line = [Text.Encoding]::ASCII.GetString($bytes, $lineStart, $i + 1 - $lineStart)
                if (!$line.Equals($lines[$index] + "`r`n", [StringComparison]::Ordinal)) {
                    $observed = $line.TrimEnd("`n").Replace("`r", '\r') -replace '[^\x20-\x7e]', '.'
                    throw "Invalid COM1 line $($index + 1): expected $($lines[$index]) with CRLF; received '$observed'."
                }
                $lineStart = $i + 1
                $index++
                if ($index -eq $lines.Length) {
                    if ($lineStart -ne $end) { throw 'Unexpected bytes following HV:HALT in COM1 reply.' }
                    $complete = $true
                } else { $status = "waiting for $($lines[$index])" }
            }
            $length = $end
            if ($length -eq $bytes.Length -and !$complete) { throw 'COM1 reply exceeds capture limit.' }
        }
    } catch {
        $cause = $_.Exception.GetBaseException()
        # Broken pipe, busy pipe, closing pipe, or not connected during boot.
        if ($cause -isnot [IO.IOException] -or ($cause.HResult -band 0xffff) -notin 109,231,232,233) { throw }
    } finally { $pipe.Dispose() }
    if (!$complete) {
        $status = "pipe disconnected at $length bytes, awaiting fresh connection"
        Start-Sleep -Milliseconds (Get-PollMilliseconds)
    }
}

# Publish only bytes actually received, never generated expected text. Move in
# the same directory is atomic and refuses to replace an existing capture.
$null = Get-PollMilliseconds
$temp = [IO.Path]::Combine($directory, [IO.Path]::GetRandomFileName())
$file = [IO.File]::Open($temp, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
try {
    try { $file.Write($bytes, 0, $length) } finally { $file.Dispose() }
    [IO.File]::Move($temp, $destination)
} finally { if ([IO.File]::Exists($temp)) { [IO.File]::Delete($temp) } }
