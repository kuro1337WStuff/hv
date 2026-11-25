#Requires -Version 5.1
#Requires -RunAsAdministrator
#Requires -Modules Hyper-V
[CmdletBinding()]
param([Parameter(Mandatory)][string]$RunDir)

$ErrorActionPreference = 'Stop'
$vmName = 'HV-SVM-L1'
$root = Split-Path -Parent $PSScriptRoot
$runs = Join-Path $root 'build\runs'
$media = Join-Path $PSScriptRoot 'media'
$store = Join-Path $PSScriptRoot "machines\$vmName"
$stems = @{ l2='hv-l2-smoke'; cpuid='hv-cpuid-resume'; 'npt-write'='hv-npt-write'; 'npt-remap'='hv-npt-remap'; 'npt-cross'='hv-npt-cross'; 'npt-broadcast'='hv-npt-broadcast'; 'forensic-write'='hv-forensic-write'; 'vcpu-gpr'='hv-vcpu-gpr'; 'irq-wake'='hv-irq-wake'; 'irq-shadow'='hv-irq-shadow'; 'irq-priority'='hv-irq-priority'; 'exception-ud'='hv-exception-ud' }
$images = @{}
$events = $null
$mutex = $null
$ownsMutex = $false
$interactive = $false
$activeProfile = ''
$finalState = 'ERROR'
$exitCodes = @{ COMPLETE=0; ERROR=1; STOPPED=2; ATTENTION=3 }

function Write-QueueEvent([string]$State, [string]$Detail) {
    $row = [ordered]@{ time=[datetime]::UtcNow.ToString('o'); state=$State; profile=$activeProfile; detail=$Detail }
    if ($events) { $events.WriteLine(($row | ConvertTo-Json -Compress)) }
    Write-Host ("{0} {1,-12} {2} {3}" -f $row.time, $State, $activeProfile, $Detail)
}

function Request-Attention([string]$Detail) {
    $script:finalState = 'ATTENTION'
    throw $Detail
}

function Assert-QueueActive {
    $quit = Test-Path -LiteralPath $stopFile
    if (!$quit -and $interactive) {
        try {
            if ([Console]::KeyAvailable) { $quit = [Console]::ReadKey($true).Key -eq 'Q' }
        } catch [InvalidOperationException] { $script:interactive = $false }
          catch [IO.IOException] { $script:interactive = $false }
    }
    if ($quit) {
        $script:finalState = 'STOPPED'
        throw 'Operator stop requested. No further VM action.'
    }
}

function Wait-Interruptible([int]$Milliseconds) {
    $timer = [Diagnostics.Stopwatch]::StartNew()
    do {
        Assert-QueueActive
        Start-Sleep -Milliseconds ([Math]::Min(100, [Math]::Max(1, $Milliseconds - [int]$timer.ElapsedMilliseconds)))
    } while ($timer.ElapsedMilliseconds -lt $Milliseconds)
    Assert-QueueActive
}

# Require real, existing filesystem objects; a lexical prefix alone admits junctions.
function Assert-PlainPath([string]$Path) {
    for ($part = $Path; $part; $part = [IO.Path]::GetDirectoryName($part)) {
        if (([IO.File]::GetAttributes($part) -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Reparse point not allowed: $part"
        }
    }
}

function Open-FrozenImage([string]$Profile, [string]$Digest) {
    $path = Join-Path $media ("{0}-{1}.iso" -f $stems[$Profile], $Digest.ToUpperInvariant())
    if (!$images.ContainsKey($path)) {
        Assert-PlainPath $path
        # No write or delete sharing, from hashing through the last VM operation.
        $stream = [IO.File]::Open($path, 'Open', 'Read', 'Read')
        $images.Add($path, $stream)
        if ((Get-FileHash -InputStream $stream -Algorithm SHA256).Hash -ine $Digest) {
            Request-Attention "Digest mismatch for staged $Profile media."
        }
    }
    return $path
}

function Read-Worklist {
    $path = Join-Path $RunDir 'queue.json'
    Assert-PlainPath $path
    $stream = [IO.File]::OpenRead($path)
    try {
        if ($stream.Length -gt 8192) { throw 'Worklist is too large.' }
        $reader = [IO.StreamReader]::new($stream)
        try { $json = $reader.ReadToEnd().Trim() } finally { $reader.Dispose() }
    } finally { $stream.Dispose() }
    if (!$json.StartsWith('[')) { throw 'Expected a JSON array.' }
    $items = ConvertFrom-Json -InputObject $json
    if (!$items -or $items.Count -gt $stems.Count) { throw 'Invalid number of queued profiles.' }
    $seen = @{}
    foreach ($item in $items) {
        Assert-QueueActive
        if ($item -isnot [pscustomobject] -or @($item.PSObject.Properties).Count -ne 2 -or
            $item.profile -isnot [string] -or $item.sha256 -isnot [string] -or
            $stems.Keys -cnotcontains $item.profile -or $seen.ContainsKey($item.profile) -or
            $item.sha256 -cnotmatch '\A[0-9a-fA-F]{64}\z') { throw 'Malformed, unreviewed or duplicate worklist entry.' }
        $seen.Add($item.profile, $true)
        [pscustomobject]@{ Profile=$item.profile; Digest=$item.sha256; Path=(Open-FrozenImage $item.profile $item.sha256) }
    }
}

# All mutations use a fresh view of the originally selected VM and DVD, never a later name match.
function Get-LabView([string]$ExpectedState, [string]$ExpectedPath) {
    $vm = Get-VM -Id $vmId
    $location = [IO.Path]::GetFullPath($vm.ConfigurationLocation).TrimEnd('\')
    if ($vm.Id -ne $vmId -or $vm.Name -ne $vmName -or $vm.Generation -ne 2 -or
        ($location -ine $store -and !$location.StartsWith($store + '\', [StringComparison]::OrdinalIgnoreCase))) {
        Request-Attention 'VM identity, generation or project store does not match.'
    }
    Assert-PlainPath $location
    $cpu = Get-VMProcessor -VM $vm
    $drives = @(Get-VMDvdDrive -VM $vm)
    if ($cpu.Count -ne 2 -or !$cpu.ExposeVirtualizationExtensions -or
        @(Get-VMNetworkAdapter -VM $vm).Count -ne 0 -or $drives.Count -ne 1) {
        Request-Attention 'Expected two nested vCPUs, no network adapters and exactly one DVD.'
    }
    $state = [string]$vm.State
    $path = [string]$drives[0].Path
    if ($state -notin 'Off','Running' -or ($ExpectedState -and
        ($state -ne $ExpectedState -or $path -ine $ExpectedPath -or $drives[0].Id -ne $driveId))) {
        Request-Attention 'VM state or mounted DVD changed unexpectedly. Leaving it alone.'
    }
    [pscustomobject]@{ VM=$vm; DVD=$drives[0]; State=$state; Path=$path }
}

function Get-MountedProfile($View) {
    if (!$View.Path -and $View.State -eq 'Off') { return '' }
    foreach ($profile in $stems.Keys) {
        $pattern = '\A' + [regex]::Escape($stems[$profile]) + '-([0-9a-fA-F]{64})\.iso\z'
        if ([IO.Path]::GetFileName($View.Path) -imatch $pattern) {
            $digest = $Matches[1]
            if ($View.Path -ieq (Join-Path $media ([IO.Path]::GetFileName($View.Path)))) {
                $null = Open-FrozenImage $profile $digest
                return $profile
            }
        }
    }
    Request-Attention 'The mounted DVD is not a reviewed, digest-named image in lab/media.'
}

function Save-Evidence($Frame, [string]$Name) {
    Assert-PlainPath $RunDir
    $path = Join-Path $RunDir $Name
    if (Test-Path -LiteralPath $path) { throw "Refusing to replace evidence: $Name" }
    Save-HvConsoleFrame -Frame $Frame -Path $path
}

function Assert-Terminal($View, [string]$Profile, [switch]$Prior) {
    $frame = Get-HvConsoleFrame -VmId $View.VM.Id
    if ($null -eq $frame) { Request-Attention 'No live frame for the running VM. Will not interrupt it.' }
    $verdict = Get-HvConsoleVerdict -Frame $frame -Profile $Profile
    if ($verdict -notin 'PASS','SKIP') {
        Save-Evidence $frame "guard-$Profile.png"
        Request-Attention "Unrecognized live $Profile console; see guard-$Profile.png. No power-off."
    }
    if ($Prior) {
        Save-Evidence $frame "prior-$Profile.png"
        Write-QueueEvent 'PRIOR' "Mounted $Profile terminal: $verdict (prior-$Profile.png)."
    }
}

function Wait-ConsoleResult($Item) {
    $timer = [Diagnostics.Stopwatch]::StartNew()
    $lastFrame = $null
    $lastIssue = 'No frame returned.'
    do {
        Assert-QueueActive
        $view = Get-LabView 'Running' $Item.Path
        # The display may not be available during UEFI startup. Never reuse a frame for a verdict.
        $frame = $null
        try { $frame = Get-HvConsoleFrame -VmId $view.VM.Id }
        catch { $lastIssue = $_.Exception.Message }
        Assert-QueueActive
        if ($null -ne $frame) {
            $lastFrame = $frame
            $verdict = Get-HvConsoleVerdict -Frame $frame -Profile $Item.Profile
            if ($verdict -in 'PASS','SKIP' -and $timer.Elapsed.TotalSeconds -lt 60) {
                $null = Get-LabView 'Running' $Item.Path
                Save-Evidence $frame "$activeProfile.png"
                Write-QueueEvent $verdict "Fresh VM console: $activeProfile.png; SHA256=$($Item.Digest)"
                if ($verdict -ne 'PASS') { Request-Attention 'Reviewed terminal SKIP. Queue will not advance.' }
                Assert-QueueActive
                return
            }
        }
        Wait-Interruptible 500
    } while ($timer.Elapsed.TotalSeconds -lt 60)
    if ($null -ne $lastFrame) {
        Save-Evidence $lastFrame "$activeProfile.png"
        Request-Attention "No reviewed terminal within 60 seconds. Last live frame: $activeProfile.png."
    }
    Request-Attention "No console frame to save within 60 seconds: $lastIssue"
}

try {
    $RunDir = [IO.Path]::GetFullPath($ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($RunDir)).TrimEnd('\')
    $runId = [IO.Path]::GetFileName($RunDir)
    if ($runId -cnotmatch '\A[0-9a-fA-F]{32}\z' -or [IO.Path]::GetDirectoryName($RunDir) -ine $runs -or
        ![IO.Directory]::Exists($RunDir)) { throw 'Expected an existing build/runs/<32-hex-id> directory.' }
    Assert-PlainPath $RunDir
    $stopFile = Join-Path $RunDir 'STOP'
    $eventPath = Join-Path $RunDir 'events.jsonl'
    if (Test-Path -LiteralPath $eventPath) {
        Assert-PlainPath $eventPath
        if ((Get-Item -LiteralPath $eventPath -Force).LinkType) { throw 'Event log must not be a link.' }
    }
    $logStream = [IO.File]::Open($eventPath, 'Append', 'Write', 'Read')
    $events = [IO.StreamWriter]::new($logStream, [Text.UTF8Encoding]::new($false))
    $events.AutoFlush = $true
    $mutex = [Threading.Mutex]::new($false, "Global\HvQueue-$vmName")
    try { $ownsMutex = $mutex.WaitOne(0) }
    catch [Threading.AbandonedMutexException] { $ownsMutex = $true }
    if (!$ownsMutex) { Request-Attention 'Another worker owns the lab VM. No changes made.' }

    $interactive = $Host.Name -eq 'ConsoleHost' -and [Environment]::UserInteractive -and ![Console]::IsInputRedirected
    Write-QueueEvent 'PREFLIGHT' 'Three-second stop window before any VM change. Press Q or create STOP in this run.'
    Wait-Interruptible 3000
    . (Join-Path $PSScriptRoot 'HvConsole.ps1')
    $queue = @(Read-Worklist)
    $vms = @(Get-VM -Name $vmName)
    if ($vms.Count -ne 1) { Request-Attention 'Expected exactly one lab VM by name.' }
    $vmId = $vms[0].Id
    $view = Get-LabView
    $driveId = $view.DVD.Id
    $mounted = Get-MountedProfile $view
    $point = "HvQueue before $runId"
    if (Get-VMSnapshot -VM $view.VM | Where-Object Name -eq $point) { Request-Attention 'This run already has a checkpoint. Use a new run directory.' }
    if ($view.State -eq 'Running') { Assert-Terminal $view $mounted -Prior }
    Write-QueueEvent 'CHECKPOINT' "Saving prior state as '$point'."
    $view = Get-LabView $view.State $view.Path
    Assert-QueueActive
    Checkpoint-VM -VM $view.VM -SnapshotName $point -Confirm:$false

    foreach ($item in $queue) {
        Assert-QueueActive
        $activeProfile = $item.Profile
        Write-QueueEvent 'NEXT' "Next boot: $($item.Profile). Five seconds to stop before changing the VM."
        Wait-Interruptible 5000
        if ($view.State -eq 'Running') {
            Write-QueueEvent 'GUARD' "Checking the live $mounted terminal before power-off."
            $view = Get-LabView 'Running' $view.Path
            Assert-Terminal $view $mounted
            $view = Get-LabView 'Running' $view.Path
            Assert-QueueActive
            Stop-VM -VM $view.VM -TurnOff -Force -Confirm:$false
        }
        Write-QueueEvent 'MOUNT' "Mounting reviewed $activeProfile media; SHA256=$($item.Digest)"
        $view = Get-LabView 'Off' $view.Path
        Assert-QueueActive
        Set-VMDvdDrive -VMDvdDrive $view.DVD -Path $item.Path -Confirm:$false
        $view = Get-LabView 'Off' $item.Path
        Assert-QueueActive
        Set-VMFirmware -VM $view.VM -FirstBootDevice $view.DVD -Confirm:$false
        Write-QueueEvent 'BOOT' 'Starting the isolated VM.'
        $view = Get-LabView 'Off' $item.Path
        Assert-QueueActive
        Start-VM -VM $view.VM -Confirm:$false
        $view = Get-LabView 'Running' $item.Path
        $mounted = $item.Profile
        Write-QueueEvent 'WATCHING' 'Waiting up to 60 seconds for reviewed live console evidence.'
        Wait-ConsoleResult $item
    }
    Assert-QueueActive
    $finalState = 'COMPLETE'
    $detail = 'All queued profiles passed. Last VM state retained.'
} catch {
    $detail = $_.Exception.Message
} finally {
    try { Write-QueueEvent $finalState $detail }
    finally {
        if ($events) { $events.Dispose() }
        foreach ($stream in $images.Values) { $stream.Dispose() }
        if ($ownsMutex) { $mutex.ReleaseMutex() }
        if ($mutex) { $mutex.Dispose() }
    }
}
exit $exitCodes[$finalState]
