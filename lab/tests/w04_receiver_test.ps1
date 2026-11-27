#Requires -Version 5.1
# Run in a clean powershell.exe -NoProfile. Never import Hyper-V or execute the worker script.
$ErrorActionPreference = 'Stop'
Import-Module "$PSHOME\Modules\Microsoft.PowerShell.Management\Microsoft.PowerShell.Management.psd1"
Import-Module "$PSHOME\Modules\Microsoft.PowerShell.Utility\Microsoft.PowerShell.Utility.psd1"
$PSModuleAutoLoadingPreference = 'None'
$worker = [IO.Path]::Combine([IO.Path]::GetDirectoryName($PSScriptRoot), 'Invoke-HvQueue.ps1')
$tokens = $errors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile($worker, [ref]$tokens, [ref]$errors)
if ($errors.Count) { throw ($errors -join "`n") }
foreach ($name in 'Request-Attention','Get-LabView','Test-W04Request','Assert-W04Lineage','Invoke-W04Boot') {
    $def = @($ast.EndBlock.Statements | Where-Object {
        $_ -is [Management.Automation.Language.FunctionDefinitionAst] -and $_.Name -ceq $name
    })
    if ($def.Count -ne 1) { throw "Expected one top-level definition of $name" }
    # Preserve $PSScriptRoot/$PSCommandPath for the function, without evaluating
    # anything else in the file (including its #Requires or its entry point).
    $isolated = [Management.Automation.Language.Parser]::ParseInput($def[0].Extent.Text,$worker,[ref]$tokens,[ref]$errors)
    if ($errors.Count) { throw ($errors -join "`n") }
    . ($isolated.GetScriptBlock())
}

$fixedRun = '1f0eceeb4492541ab6a0b8bc6b49bd17'
$otherRun = '00000000000000000000000000000001'
$loader = 'l1-w04-loader'; $candidate = 'l1-w04-return'
$loaderHash = 'd4ae62551bed9d40024fb8572183d88fcc6cc67db3b37a11c7509c4c18710e98'
$returnHash = 'd4f15d71d3866648983ecb5298afbe69ae50e9a49c1b3e82452d03e40645de0e'
$otherHash = '0' * 64
$generic = [pscustomobject]@{ Profile='ordinary'; Digest=$otherHash }
foreach ($r in @(
    @{Run=$fixedRun; Items=@($generic); Expected=$true}
    @{Run=$otherRun; Items=@($generic,[pscustomobject]@{Profile=$candidate; Digest=$otherHash}); Expected=$true}
    @{Run=$otherRun; Items=@([pscustomobject]@{Profile='ordinary'; Digest=$returnHash}); Expected=$true}
    @{Run=$otherRun; Items=@($generic); Expected=$false}
)) {
    $route = Test-W04Request $r.Run $r.Items
    if ($route -isnot [bool] -or $route -ne $r.Expected) { throw 'Incorrect W04 request routing' }
}

# Model reads independently of what the production guard expects. Only the synthetic
# VM changes when a mutator runs. Log attempts BEFORE validating their arguments.
function Get-VM($Id) { if ($Id -ne $fx.VM.Id) { throw 'Fixture: wrong VM lookup' }; $fx.VM }
function Assert-PlainPath($Path) { if ($Path -ine $store) { throw 'Fixture: unexpected store' } }
function Get-VMProcessor($VM) { [pscustomobject]@{ Count=2; ExposeVirtualizationExtensions=$true } }
function Get-VMNetworkAdapter($VM) {}
function Get-VMHardDiskDrive($VM) {}
function Get-VMComPort($VM) {}
function Get-VMFirmware($VM) { [pscustomobject]@{ SecureBoot='Off'; BootOrder=@([pscustomobject]@{ Device=$fx.First }) } }
function Get-VMSnapshot($VM) {
    if ($VM -ne $fx.VM) { throw 'Fixture: wrong snapshot lookup' }
    $fx.Snapshots.ToArray()
}
function Get-VMDvdDrive($VM,$VMSnapshot) {
    if ($null -ne $VMSnapshot) {
        if (!$fx.Snapshots.Contains($VMSnapshot)) { throw 'Fixture: unknown snapshot' }
        return [pscustomobject]@{ Id=$fx.Disc.Id; Path=$VMSnapshot.DvdPath }
    }
    if ($VM -ne $fx.VM) { throw 'Fixture: wrong DVD lookup' }
    $fx.Disc
}
function Open-FrozenImage($Profile,$Digest) {
    if ($fx.FreezeError -eq $Profile) { throw 'Synthetic media hash mismatch' }
    Join-Path $media ('{0}-{1}.iso' -f $profiles[$Profile].stem,$Digest.ToUpperInvariant())
}
function Open-W04Release($SessionRoot,$Operations,$WorkerPath) {
    if (!$SessionRoot -or !$Operations -or $WorkerPath -ine $worker) { throw 'Fixture: missing release context' }
    if ($fx.Spent) { throw 'Synthetic spent reservation' }
    $fx.Release
}
function Assert-W04Authority($Release) {
    if ($Release -ne $fx.Release) { throw 'Fixture: wrong authority release' }
    if (!$fx.Authorized -or $fx.Revoked) { throw 'Synthetic missing or revoked authority' }
    if ($fx.LeaseOccupied) { throw 'Synthetic occupied lease' }
}
function Assert-QueueActive { if ($fx.Stopped) { throw 'Synthetic run STOP' } }
function Wait-Interruptible([int]$Milliseconds) {
    if ($Milliseconds -ne 5000 -or $fx.Waited -or ($fx.Trace -join ':') -cne 'receipt:intent:checkpoint:receipt:checkpoint') {
        throw 'Fixture: expected one stop window between checkpoint and mount'
    }
    $fx.Waited = $true
    Assert-QueueActive
}
function Write-W04Receipt($Release,$Phase,$CheckpointId) {
    if ($Release -ne $fx.Release) { throw 'Fixture: wrong receipt release' }
    if ($Phase -eq 'intent') {
        if ($PSBoundParameters.ContainsKey('CheckpointId')) { throw 'Fixture: premature checkpoint identity' }
        if ($fx.Spent -or $fx.Contended) { throw 'Synthetic CreateNew: intent already exists' }
    } elseif ($CheckpointId -ne $fx.VM.ParentSnapshotId) { throw 'Fixture: wrong receipt checkpoint' }
    if ($fx.Receipts.Contains($Phase)) { throw 'Fixture: duplicate receipt' }
    $fx.Spent = $true
    $fx.Receipts.Add($Phase)
    $fx.Trace.Add("receipt:$Phase")
}
function Write-QueueEvent($State,$Detail) {
    if ($State -eq 'PASS') { throw 'Fixture: boot is not a console verdict' }
}
function New-Snapshot($Id,$Parent,$State,$Name,$Path) {
    [pscustomobject]@{ Id=$Id; VMId=$fx.VM.Id; ParentSnapshotId=$Parent; State=$State;
        SnapshotType='Standard'; Name=$Name; DvdPath=$Path }
}
function Checkpoint-VM($VM,$SnapshotName,[switch]$Passthru,$Confirm) {
    $fx.Actions.Add('checkpoint'); $fx.Trace.Add('checkpoint')
    if ($VM -ne $fx.VM -or !$Passthru -or $SnapshotName -cne $point) { throw 'Fixture: wrong checkpoint arguments' }
    $s = New-Snapshot ([guid]::NewGuid()) $VM.ParentSnapshotId $VM.State $SnapshotName $fx.Disc.Path
    $fx.Snapshots.Add($s); $VM.ParentSnapshotId = $s.Id
    if ($fx.RevokeAtCheckpoint) { $fx.Revoked = $true }
    if ($fx.ReturnStaleId) { return $fx.Snapshots[0] }
    $s
}
function Set-VMDvdDrive($VMDvdDrive,$Path,$Confirm) {
    $fx.Actions.Add('mount'); $fx.Trace.Add('mount')
    if ($VMDvdDrive -ne $fx.Disc -or $Path -ine $returnPath) { throw 'Fixture: wrong mount arguments' }
    $fx.Disc.Path = $Path
}
function Set-VMFirmware($VM,$FirstBootDevice,$Confirm) {
    $fx.Actions.Add('firmware'); $fx.Trace.Add('firmware')
    if ($VM -ne $fx.VM -or $FirstBootDevice -ne $fx.Disc) { throw 'Fixture: wrong boot device' }
    $fx.First = $FirstBootDevice
}
function Start-VM($VM,$Confirm) {
    $fx.Actions.Add('boot'); $fx.Trace.Add('boot')
    if ($VM -ne $fx.VM -or $fx.First -ne $fx.Disc -or $VM.State -cne 'Off') { throw 'Fixture: wrong boot target' }
    $VM.State = 'Running'
}

function Invoke-Case($Case) {
    $ownsMutex = $true; $sessionMode = $true; $dedicatedLane = $true
    $runId = $fixedRun; $vmName = 'HV-SVM-L1-A'; $vmId = [guid]::NewGuid(); $driveId = 'fixture-dvd'
    $lab = [IO.Path]::Combine($PSScriptRoot,'hv-lane-a','lab'); $store = $lab; $media = Join-Path $lab 'media'
    $profiles = @{}; foreach ($p in $loader,$candidate) { $profiles[$p] = @{ stem="hv-$p"; pass=$null } }
    $priorPath = Join-Path $media ("hv-$loader-" + $loaderHash.ToUpperInvariant() + '.iso')
    $returnPath = Join-Path $media ("hv-$candidate-" + $returnHash.ToUpperInvariant() + '.iso')
    $queue = @([pscustomobject]@{ Profile=$candidate; Digest=$returnHash; Path=$returnPath })
    $fx = @{
        Authorized=$true; Revoked=$false; Stopped=$false; Spent=$false; RevokeAtCheckpoint=$false
        Actions=[Collections.Generic.List[string]]::new(); Trace=[Collections.Generic.List[string]]::new()
        Receipts=[Collections.Generic.List[string]]::new(); Snapshots=[Collections.Generic.List[object]]::new()
        Disc=[pscustomobject]@{ Id=$driveId; Path=$priorPath }
        VM=[pscustomobject]@{ Id=$vmId; Name=$vmName; State='Off'; ParentSnapshotId=$null; Generation=2;
            ConfigurationLocation=$store; MemoryStartup=2GB; DynamicMemoryEnabled=$false; AutomaticStartAction='Nothing';
            AutomaticCheckpointsEnabled=$false; CheckpointType='Standard' }
    }
    $ids = @(1..18 | ForEach-Object { [guid]::NewGuid() })
    for ($i = 0; $i -lt 17; ++$i) {
        $parent = if ($i -gt 0) { $ids[$i-1] } else { [guid]::Empty }
        $fx.Snapshots.Add((New-Snapshot $ids[$i] $parent 'Off' "fixture-$i" ''))
    }
    $saved = New-Snapshot $ids[17] $ids[16] 'Saved' 'W04 146-R2 recovery' $priorPath
    $fx.Snapshots.Add($saved); $fx.VM.ParentSnapshotId = $saved.Id
    $fx.Release = @{ Ids=[string[]]$ids; VmId=$vmId; SavedId=$ids[17]; SavedParent=$ids[16] }
    switch ($Case) {
        'empty drive' { $fx.Disc.Path = '' }
        'different DVD' { $fx.Disc.Path = $returnPath }
        'same stem wrong digest' { $fx.Disc.Path = Join-Path $media "hv-$loader-$otherHash.iso" }
        'extra ID' { $fx.Snapshots.Add((New-Snapshot ([guid]::NewGuid()) $ids[0] 'Off' 'extra' '')) }
        'substituted ID' { $fx.Snapshots[0].Id = [guid]::NewGuid() }
        'wrong saved parent' { $saved.ParentSnapshotId = $ids[0] }
        'wrong live parent' { $fx.VM.ParentSnapshotId = $ids[0] }
        'wrong saved DVD' { $saved.DvdPath = $returnPath }
        'wrong VM' { $fx.Release.VmId = [guid]::NewGuid() }
        'wrong snapshot owner' { $fx.Snapshots[0].VMId = [guid]::NewGuid() }
        'running VM' { $fx.VM.State = 'Running' }
        'unapproved' { $fx.Authorized = $false }
        'revoked' { $fx.Revoked = $true }
        'run STOP' { $fx.Stopped = $true }
        'busy mutex' { $ownsMutex = $false }
        'abandoned mutex' { $abandonedLease = $true }
        'spent intent' { $fx.Spent = $true }
        'intent race' { $fx.Contended = $true }
        'prior run checkpoint' { $fx.Snapshots[0].Name = "HvQueue before $runId" }
        'wrong run' { $runId = $otherRun }
        'occupied lease' { $fx.LeaseOccupied = $true }
        'direct mode' { $sessionMode = $false }
        'different lane' { $vmName = 'fixture-other-lane'; $fx.VM.Name = $vmName }
        'diagnostic PASS pin' { $profiles[$candidate].pass = $otherHash }
        'wrong digest' { $queue[0].Digest = $otherHash }
        'wrong candidate path' { $queue[0].Path = $priorPath }
        'wrong profile' { $queue[0].Profile = $loader }
        'extra item' { $queue += $generic }
        'bad loader bytes' { $fx.FreezeError = $loader }
        'bad return bytes' { $fx.FreezeError = $candidate }
        'ambiguous child receipt' { $fx.ReturnStaleId = $true }
        'revoke after checkpoint' { $fx.RevokeAtCheckpoint = $true }
    }
    $point = "HvQueue before $runId"
    $failed = $false
    try { Invoke-W04Boot } catch {
        if ($Case -eq 'valid' -or $_.FullyQualifiedErrorId -ne $_.Exception.Message -or $_.Exception.Message -like 'Fixture:*') { throw }
        if ($Case -eq 'revoke after checkpoint' -and $_.Exception.Message -cne 'Synthetic missing or revoked authority') { throw }
        $failed = $true
    }
    $actions = $fx.Actions -join ':'
    if ($Case -eq 'valid') {
        $expected = 'receipt:intent:checkpoint:receipt:checkpoint:mount:receipt:mount:firmware:receipt:firmware:boot:receipt:boot'
        if (!$fx.Waited -or ($fx.Trace -join ':') -cne $expected -or $fx.VM.State -cne 'Running' -or $fx.Disc.Path -ine $returnPath) {
            throw "Invalid successful transition: $($fx.Trace -join ':')"
        }
    } elseif ($Case -eq 'ambiguous child receipt') {
        if (!$failed -or ($fx.Trace -join ':') -cne 'receipt:intent:checkpoint') {
            throw 'Uncorroborated checkpoint result received a durable identity receipt'
        }
    } elseif ($Case -eq 'revoke after checkpoint') {
        if (!$failed -or ($fx.Trace -join ':') -cne 'receipt:intent:checkpoint:receipt:checkpoint') {
            throw "Checkpoint revocation was not contained: $($fx.Trace -join ':')"
        }
    } elseif (!$failed -or $actions) { throw "Expected refusal with zero actions for '${Case}', got '$actions'" }
}
foreach ($c in 'valid','empty drive','different DVD','same stem wrong digest','extra ID','substituted ID',
    'wrong saved parent','wrong live parent','wrong saved DVD','wrong VM','wrong snapshot owner','running VM',
    'unapproved','revoked','run STOP','busy mutex','abandoned mutex','spent intent','intent race','prior run checkpoint','wrong run','wrong digest',
    'direct mode','different lane','diagnostic PASS pin','occupied lease',
    'wrong candidate path','wrong profile','extra item','bad loader bytes','bad return bytes','ambiguous child receipt','revoke after checkpoint') {
    Invoke-Case $c
}
'W04 isolated receiver: routing, four ordered mutations and receipts, zero-action refusals and checkpoint revocation verified.'
