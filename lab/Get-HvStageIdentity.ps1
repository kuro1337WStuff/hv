#Requires -Version 5.1
[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$LaneRoot)

function Assert-StagePlainPath([string]$Path) {
    for ($p = $Path; $p; $p = [IO.Path]::GetDirectoryName($p)) {
        if ([IO.File]::GetAttributes($p) -band [IO.FileAttributes]::ReparsePoint) {
            throw 'Linked paths are not accepted.'
        }
    }
}

function Get-StageTextHash([string]$Text) {
    $utf8 = [Text.UTF8Encoding]::new($false,$true)
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($sha.ComputeHash($utf8.GetBytes($Text))).Replace('-','').ToLowerInvariant() }
    finally { $sha.Dispose() }
}

function Get-StageWorkerHash([string]$Path) {
    Assert-StagePlainPath $Path
    $stream = [IO.File]::Open($Path,'Open','Read','Read')
    try {
        if ($stream.Length -lt 1 -or $stream.Length -gt 65536) { throw 'Worker size outside limits.' }
        # Do not let StreamReader switch to another encoding based on a BOM.
        $reader = [IO.StreamReader]::new($stream,[Text.UTF8Encoding]::new($false,$true),$false)
        try { $text = $reader.ReadToEnd() } finally { $reader.Dispose() }
        if ($text.Length -and $text[0] -eq [char]0xfeff) { $text = $text.Substring(1) }
        return Get-StageTextHash $text.Replace("`r`n","`n")
    } finally { $stream.Dispose() }
}

# Caller must already hold a read guard which shares read and write but not
# delete. Its successful open excludes an exclusive reader as the explanation
# for a subsequent sharing violation when we deny write access.
function Test-StageWriter([string]$Path) {
    try { $probe = [IO.File]::Open($Path,'Open','Read','Read') }
    catch {
        $errorCause = $_.Exception.GetBaseException()
        if ($errorCause -is [IO.IOException] -and $errorCause.HResult -eq -2147024864) { return $true } # HRESULT_FROM_WIN32(32)
        throw
    }
    $probe.Dispose()
    return $false
}

# Internal fixture seam: SessionRoot must already be a trusted broker root.
function Get-CurrentStageIdentity([string]$SessionRoot,[string]$RunnerWorker) {
    $ErrorActionPreference = 'Stop'
    $reviewed = 'b1ec9193b5d93ad851c19a5a1eff545adfb8fd08f47393afcfa3f71c89fea6e2'
    Assert-StagePlainPath $SessionRoot
    $selected = $null
    $selectedGuard = $null
    $count = 0
    try {
        foreach ($dir in [IO.Directory]::EnumerateDirectories($SessionRoot)) {
            $name = [IO.Path]::GetFileName($dir)
            if ($name -cnotmatch '\A[0-9a-f]{32}\z') { continue }
            if (++$count -gt 128) { throw 'Too many candidate stages.' }
            Assert-StagePlainPath $dir
            $lease = [IO.Path]::Combine($dir,'ready.lease')
            try { $attributes = [IO.File]::GetAttributes($lease) }
            catch {
                # Older stages have no lease. Access denied and missing parent
                # directories are unknown states, not evidence of inactivity.
                if ($_.Exception.GetBaseException() -is [IO.FileNotFoundException]) { continue }
                throw
            }
            if ($attributes -band ([IO.FileAttributes]::Directory -bor [IO.FileAttributes]::ReparsePoint)) {
                throw 'Invalid readiness file.'
            }
            $guard = [IO.File]::Open($lease,'Open','Read','ReadWrite')
            try {
                if ($guard.Length -ne 0) { throw 'Invalid readiness file length.' }
                if (Test-StageWriter $lease) {
                    # The publisher renames this file into place as its last
                    # step. Check only its presence, never its contents. On
                    # rollback it closes the writer before removing the file.
                    try {
                        $null = [IO.File]::GetAttributes([IO.Path]::Combine($dir,'session.pending'))
                        throw 'Stage publication is incomplete.'
                    } catch {
                        if ($_.Exception.GetBaseException() -isnot [IO.FileNotFoundException]) { throw }
                    }
                    if ($selected) { throw 'Multiple ready stages.' }
                    $selected = $dir
                    $selectedLease = $lease
                    $selectedGuard = $guard
                    $guard = $null
                }
            } finally { if ($guard) { $guard.Dispose() } }
        }
        if (!$selected) { throw 'No current ready stage.' }
        if ((Get-StageWorkerHash ([IO.Path]::Combine($selected,'Invoke-HvQueue.ps1'))) -cne $reviewed -or
            (Get-StageWorkerHash $RunnerWorker) -cne $reviewed) { throw 'Unreviewed worker source.' }
        if (!(Test-StageWriter $selectedLease)) { throw 'Stage readiness ended during observation.' }
        # A point-in-time observation only; this conveys no release authority.
        return [pscustomobject]@{ Stage = [IO.Path]::GetFileName($selected); WorkerSha256 = $reviewed }
    } finally { if ($selectedGuard) { $selectedGuard.Dispose() } }
}

function Assert-StageRootAcl([string]$Path) {
    # Mirror New-PrivateRoot in Start-HvLabSession.ps1 without creating or
    # changing anything. In particular, OWNER RIGHTS limits a filtered owner.
    $admins = [Security.Principal.SecurityIdentifier]::new('S-1-5-32-544')
    $system = [Security.Principal.SecurityIdentifier]::new('S-1-5-18')
    $acl = [Security.AccessControl.DirectorySecurity]::new()
    $acl.SetAccessRuleProtection($true,$false)
    $acl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new(
        [Security.Principal.SecurityIdentifier]::new('S-1-3-4'),'ReadPermissions','ContainerInherit,ObjectInherit','None','Allow'))
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    try {
        foreach ($sid in $admins,$system,$identity.User) {
            $rights = if ($sid -eq $identity.User) { 'ReadAndExecute' } else { 'FullControl' }
            $acl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new($sid,$rights,'ContainerInherit,ObjectInherit','None','Allow'))
        }
    } finally { $identity.Dispose() }
    $actual = [IO.Directory]::GetAccessControl($Path)
    if ($actual.GetOwner([Security.Principal.SecurityIdentifier]) -ne $admins -or
        $actual.GetSecurityDescriptorSddlForm('Access') -cne $acl.GetSecurityDescriptorSddlForm('Access')) {
        throw 'Session directory does not have the required protected ACL.'
    }
}

$ErrorActionPreference = 'Stop'
if ($PSVersionTable.PSEdition -ne 'Desktop' -or ![Environment]::Is64BitProcess) {
    throw 'Use 64-bit Windows PowerShell 5.1.'
}
function Assert-StageLocalVolume([string]$Path) {
    if ($Path -notmatch '\A[A-Za-z]:\\') { throw 'Expected a local volume path.' }
    $drive = [IO.Path]::GetPathRoot($Path)
    if ([IO.DriveInfo]::new($drive).DriveType -ne 'Fixed') { throw 'Expected a fixed local volume.' }
    # DriveType alone reports Fixed even for SUBST. /L queries the volume
    # mount point; a substituted directory is not a volume mount point.
    $mountvol = [IO.Path]::Combine([Environment]::SystemDirectory,'mountvol.exe')
    $volume = & $mountvol $drive /L
    if ($LASTEXITCODE -ne 0 -or ([string]$volume).Trim() -notmatch '\A\\\\\?\\Volume\{[0-9a-f-]{36}\}\\\z') {
        throw 'Expected a volume-mounted drive, not a redirected drive.'
    }
}

if ($LaneRoot -notmatch '\A[A-Za-z]:\\' -or $LaneRoot.Substring(2).Contains(':')) { throw 'Expected a full local checkout path.' }
$root = [IO.Path]::GetFullPath($LaneRoot).TrimEnd('\')
if ($root -ine $LaneRoot.TrimEnd('\') -or [IO.Path]::GetFileName($root) -notin 'hv-lane-a','hv-lane-b' -or
    ![IO.Directory]::Exists($root)) {
    throw 'Expected a canonical, existing dedicated lane checkout.'
}
Assert-StageLocalVolume $root
Assert-StagePlainPath $root
$programs = [Environment]::GetFolderPath('ProgramFiles')
Assert-StageLocalVolume $programs
$sessionRoot = [IO.Path]::Combine($programs,('HvLabSession-'+(Get-StageTextHash $root.ToLowerInvariant()).Substring(0,20)))
Assert-StagePlainPath $sessionRoot
Assert-StageRootAcl $sessionRoot
Get-CurrentStageIdentity $sessionRoot ([IO.Path]::Combine($root,'lab','Invoke-HvQueue.ps1'))
