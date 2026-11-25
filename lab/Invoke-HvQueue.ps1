#Requires -Version 5.1
#Requires -RunAsAdministrator
#Requires -Modules Hyper-V
[CmdletBinding(DefaultParameterSetName='Direct')]
param(
    [Parameter(Mandatory, Position=0, ParameterSetName='Direct')]
    [Parameter(Mandatory, Position=0, ParameterSetName='Session')]
    [string]$RunDir,
    [Parameter(Mandatory, ParameterSetName='Session')][string]$SessionLabRoot,
    [Parameter(Mandatory, ParameterSetName='Session')][string]$SessionStopFile,
    [Parameter(Mandatory, ParameterSetName='Session')][guid]$SessionVmId
)

$ErrorActionPreference = 'Stop'
$sessionMode = $PSCmdlet.ParameterSetName -eq 'Session'
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
    if (!$quit -and $sessionMode) {
        # Inspect the directory entry, not the target of a caller-supplied STOP link.
        Assert-PlainPath $stopDir
        $quit = [IO.Directory]::GetFileSystemEntries($stopDir, 'STOP').Length -ne 0
    }
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

# The broker pins the media directory and its ancestors. Open the final entry
# itself, not a reparse target, and validate the same handle we keep for the run.
function Initialize-SessionMedia {
    Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Win32.SafeHandles;

public static class HvSessionImage {
    [StructLayout(LayoutKind.Sequential)]
    struct FileInfo {
        public FileAttributes Attributes;
        public System.Runtime.InteropServices.ComTypes.FILETIME Created, Accessed, Written;
        public uint VolumeSerial, SizeHigh, SizeLow, Links, IndexHigh, IndexLow;
    }

    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, ExactSpelling=true, SetLastError=true)]
    static extern SafeFileHandle CreateFileW(string path, uint access, FileShare share,
        IntPtr security, FileMode mode, uint flags, IntPtr template);
    [DllImport("kernel32.dll", SetLastError=true)]
    static extern bool GetFileInformationByHandle(SafeFileHandle file, out FileInfo info);
    [DllImport("kernel32.dll")]
    static extern uint GetFileType(SafeFileHandle file);
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, ExactSpelling=true, SetLastError=true)]
    static extern uint GetFinalPathNameByHandleW(SafeFileHandle file, StringBuilder path, uint size, uint flags);

    public static FileStream OpenRead(string path) {
        const uint GenericRead = 0x80000000, OpenReparsePoint = 0x00200000, Disk = 1;
        SafeFileHandle handle = CreateFileW(path, GenericRead, FileShare.Read, IntPtr.Zero,
            FileMode.Open, OpenReparsePoint, IntPtr.Zero);
        try {
            if (handle.IsInvalid) throw new Win32Exception(Marshal.GetLastWin32Error());
            FileInfo info;
            if (!GetFileInformationByHandle(handle, out info)) throw new Win32Exception(Marshal.GetLastWin32Error());
            if (GetFileType(handle) != Disk || info.Links != 1 ||
                (info.Attributes & (FileAttributes.Directory | FileAttributes.ReparsePoint)) != 0)
                throw new IOException("Session media must be a regular file with a single link.");
            string expected = @"\\?\" + path;
            var actual = new StringBuilder(expected.Length + 1);
            uint length = GetFinalPathNameByHandleW(handle, actual, (uint)actual.Capacity, 0);
            if (length == 0) throw new Win32Exception(Marshal.GetLastWin32Error());
            if (length != expected.Length || !String.Equals(expected, actual.ToString(), StringComparison.OrdinalIgnoreCase))
                throw new IOException("Unexpected resolved session media path.");
            return new FileStream(handle, FileAccess.Read);
        } catch {
            handle.Dispose();
            throw;
        }
    }
}
'@
}

function Open-FrozenImage([string]$Profile, [string]$Digest) {
    $path = Join-Path $media ("{0}-{1}.iso" -f $profiles[$Profile].stem, $Digest.ToUpperInvariant())
    if (!$images.ContainsKey($path)) {
        # No write or delete sharing, from hashing through the last VM operation.
        if ($sessionMode) {
            $stream = [HvSessionImage]::OpenRead($path)
        } else {
            Assert-PlainPath $path
            $stream = [IO.File]::Open($path, 'Open', 'Read', 'Read')
        }
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
    if (!$items -or $items.Count -gt $profiles.get_Count()) { throw 'Invalid number of queued profiles.' }
    $seen = @{}
    foreach ($item in $items) {
        Assert-QueueActive
        if ($item -isnot [pscustomobject] -or @($item.PSObject.Properties).Count -ne 2 -or
            $item.profile -isnot [string] -or $item.sha256 -isnot [string] -or
            $profiles.get_Keys() -cnotcontains $item.profile -or !$profiles[$item.profile].stem -or $seen.ContainsKey($item.profile) -or
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
    if ($dedicatedLane -and ($vm.MemoryStartup -ne 2GB -or $vm.DynamicMemoryEnabled -ne $false -or
        @(Get-VMHardDiskDrive -VM $vm).Count -ne 0 -or
        @(Get-VMComPort -VM $vm | Where-Object { $_.Path }).Count -ne 0)) {
        Request-Attention 'Lane VMs require fixed 2 GiB RAM, no hard disks and no forwarded COM ports.'
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
    foreach ($profile in $profiles.get_Keys()) {
        $stem = $profiles[$profile].stem
        if (!$stem) { continue } # screen-only, never authorized as boot media
        $pattern = '\A' + [regex]::Escape($stem) + '-([0-9a-fA-F]{64})\.iso\z'
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
    $verdict = Get-HvConsoleVerdict -Frame $frame -Profile $Profile -Profiles $profiles
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
            $verdict = Get-HvConsoleVerdict -Frame $frame -Profile $Item.Profile -Profiles $profiles
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
    $lab = $PSScriptRoot
    if ($sessionMode) {
        $lab = $SessionLabRoot.TrimEnd('\')
        if ($lab -notmatch '\A[A-Za-z]:\\' -or [IO.Path]::GetFullPath($lab) -ine $lab -or
            [IO.Path]::GetFileName($lab) -ine 'lab' -or ![IO.Directory]::Exists($lab)) {
            throw 'SessionLabRoot must be the absolute project lab directory.'
        }
        Assert-PlainPath $lab
        $runs = Join-Path $PSScriptRoot 'runs'
    } else {
        $runs = Join-Path (Split-Path -Parent $lab) 'build\runs'
    }
    # Session workers run from a private copy; bind to the broker's checkout, not that copy.
    $vmName = switch ([IO.Path]::GetFileName([IO.Path]::GetDirectoryName($lab))) {
        'hv-lane-a' { 'HV-SVM-L1-A' }
        'hv-lane-b' { 'HV-SVM-L1-B' }
        default { 'HV-SVM-L1' }
    }
    $dedicatedLane = $vmName -ne 'HV-SVM-L1'
    $media = Join-Path $lab 'media'
    $store = if ($dedicatedLane) {
        Join-Path ([Environment]::GetFolderPath('ProgramFiles')) "HvLabMachine-$vmName"
    } else { Join-Path $lab "machines\$vmName" }
    $RunDir = [IO.Path]::GetFullPath($ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($RunDir)).TrimEnd('\')
    $runId = [IO.Path]::GetFileName($RunDir)
    if ($runId -cnotmatch '\A[0-9a-fA-F]{32}\z' -or [IO.Path]::GetDirectoryName($RunDir) -ine $runs -or
        ![IO.Directory]::Exists($RunDir)) { throw "Expected an existing $runs\<32-hex-id> directory." }
    Assert-PlainPath $RunDir
    $stopFile = Join-Path $RunDir 'STOP'
    if ($sessionMode) {
        $stopDir = Join-Path (Split-Path -Parent $lab) "build\runs\$runId"
        if ($SessionStopFile -ine (Join-Path $stopDir 'STOP')) { throw 'Unexpected session STOP path.' }
        Assert-PlainPath $stopDir
        Initialize-SessionMedia
        $manifest = Join-Path $RunDir 'hv-profiles.json'
    } else {
        $manifest = Join-Path $PSScriptRoot 'hv-profiles.json'
    }
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
    $vms = @(Get-VM -Name $vmName)
    if ($vms.Count -ne 1) { Request-Attention 'Expected exactly one lab VM by name.' }
    if ($sessionMode) {
        if ($vms[0].Id -ne $SessionVmId) { Request-Attention 'Lab VM no longer has the approved session identity.' }
        $vmId = $SessionVmId
    } else {
        $vmId = $vms[0].Id
    }
    $view = Get-LabView
    Write-QueueEvent 'PREFLIGHT' "VM=$vmName ID=$vmId. Three-second stop window before any VM change. Press Q or create STOP in this run."
    Wait-Interruptible 3000
    . (Join-Path $PSScriptRoot 'HvConsole.ps1')
    Assert-PlainPath $manifest
    $profiles = Read-HvProfiles $manifest
    $queue = @(Read-Worklist)
    if ('l1-handoff' -in $queue.Profile) {
        if ($dedicatedLane) { Request-Attention 'Serial handoff is disabled on lane VMs. No VM changes.' }
        $com = Get-VMComPort -VM $view.VM -Number 1
        if ($com.Path -ine '\\.\pipe\HV-SVM-L1-COM1') { Request-Attention 'Expected the lab COM1 pipe. No VM changes.' }
    }
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
        if ($activeProfile -ceq 'l1-handoff') {
            Write-QueueEvent 'SERIAL' 'Requesting the post-handoff transcript from lab COM1.'
            try {
                & (Join-Path $PSScriptRoot 'Receive-HvHandoff.ps1') -OutputPath (Join-Path $RunDir 'l1-handoff.serial') -ShouldStop { Assert-QueueActive; return $false }
            } catch {
                if ($finalState -eq 'STOPPED') { throw }
                $frame = Get-HvConsoleFrame -VmId $view.VM.Id
                if ($null -ne $frame) { Save-Evidence $frame 'l1-handoff.png' }
                Request-Attention "Serial verification failed: $($_.Exception.Message)"
            }
            Write-QueueEvent 'SERIAL' 'Verified four ordered markers from COM1; saved l1-handoff.serial.'
        }
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
