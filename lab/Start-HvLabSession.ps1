#Requires -Version 5.1
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
if ($PSVersionTable.PSEdition -ne 'Desktop' -or ![Environment]::Is64BitProcess) {
    throw 'Launch with 64-bit Windows PowerShell 5.1.'
}
$exe = Join-Path $PSHOME 'powershell.exe'
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)

# Only the broker and its startup-pinned scripts execute elevated. Do not
# resolve host tools or PowerShell modules from the caller's checkout or PATH.
$windows = [Environment]::GetFolderPath('Windows')
$programs = [Environment]::GetFolderPath('ProgramFiles')
$lab = $PSScriptRoot
$root = [IO.Path]::GetDirectoryName($lab)
$vmName = switch ([IO.Path]::GetFileName($root)) {
    'hv-lane-a' { 'HV-SVM-L1-A' }
    'hv-lane-b' { 'HV-SVM-L1-B' }
    default { 'HV-SVM-L1' }
}
$utf8 = [Text.UTF8Encoding]::new($false, $true)
function Get-TextDigest([string]$Text) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($sha.ComputeHash($utf8.GetBytes($Text))).Replace('-', '').ToLowerInvariant() }
    finally { $sha.Dispose() }
}
function Assert-Plain([string]$Path) {
    for ($p = $Path; $p; $p = [IO.Path]::GetDirectoryName($p)) {
        if ([IO.File]::GetAttributes($p) -band [IO.FileAttributes]::ReparsePoint) { throw 'Linked paths are not accepted.' }
    }
}
function Read-InputText([string]$Path, [int]$Limit) {
    Assert-Plain $Path
    $s = [IO.File]::Open($Path, 'Open', 'Read', 'Read')
    try {
        if ($s.Length -eq 0 -or $s.Length -gt $Limit) { throw 'Input size outside limits.' }
        $r = [IO.StreamReader]::new($s, $utf8, $true)
        try { return $r.ReadToEnd() } finally { $r.Dispose() }
    } finally { $s.Dispose() }
}
function New-PrivateRoot([string]$Path) {
    $admins = [Security.Principal.SecurityIdentifier]::new('S-1-5-32-544')
    $system = [Security.Principal.SecurityIdentifier]::new('S-1-5-18')
    $acl = [Security.AccessControl.DirectorySecurity]::new()
    $acl.SetOwner($admins)
    # Descendants created by elevated Windows processes may be owned by the
    # individual SID. Suppress its implicit WRITE_DAC under a filtered token.
    $acl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new(
        [Security.Principal.SecurityIdentifier]::new('S-1-3-4'),'ReadPermissions','ContainerInherit,ObjectInherit','None','Allow'))
    $acl.SetAccessRuleProtection($true, $false)
    foreach ($sid in $admins,$system,$identity.User) {
        $rights = if ($sid -eq $identity.User) { 'ReadAndExecute' } else { 'FullControl' }
        $acl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new($sid,$rights,'ContainerInherit,ObjectInherit','None','Allow'))
    }
    $null = [IO.Directory]::CreateDirectory($Path,$acl)
    Assert-Plain $Path
    $actual = [IO.Directory]::GetAccessControl($Path)
    if ($actual.GetOwner([Security.Principal.SecurityIdentifier]) -ne $admins -or
        $actual.GetSecurityDescriptorSddlForm('Access') -ne $acl.GetSecurityDescriptorSddlForm('Access')) {
        throw 'Session directory does not have the required protected ACL.'
    }
}
function Lock-Directory([string]$Path) {
    $handle = [HvSessionDirectory]::CreateFile($Path,0x80,3,[IntPtr]::Zero,3,0x02200000,[IntPtr]::Zero)
    if ($handle.IsInvalid) { throw "Cannot pin directory: $Path" }
    try { Assert-Plain $Path; return $handle } catch { $handle.Dispose(); throw }
}
function Read-Fixed($Stream,[int]$Length) {
    $buf = [byte[]]::new($Length)
    for ($pos=0; $pos -lt $buf.Length;) {
        $Stream.ReadTimeout = [Math]::Max(1, 3000 - [int]$requestTimer.ElapsedMilliseconds)
        $n = $Stream.Read($buf,$pos,$buf.Length-$pos)
        if (!$n -or $requestTimer.ElapsedMilliseconds -gt 3000) { throw 'Incomplete request.' }
        $pos += $n
    }
    return ,$buf
}
function Read-Body($Stream,[int]$Limit) {
    $size = [Net.IPAddress]::NetworkToHostOrder([BitConverter]::ToInt32((Read-Fixed $Stream 4),0))
    if ($size -le 0 -or $size -gt $Limit) { throw 'Invalid request size.' }
    return $utf8.GetString((Read-Fixed $Stream $size))
}
function Read-Request($Stream) {
    $line = [Text.Encoding]::ASCII.GetString((Read-Fixed $Stream 66))
    if ($line -cnotmatch '\A([a-f0-9]{32}) ([a-f0-9]{32})\n\z' -or $Matches[1] -cne $token) { throw 'Unauthorized or malformed request.' }

    return $Matches[2]
}
function Send-Events([string]$Path, [ref]$Offset, $Writer) {
    if (![IO.File]::Exists($Path)) { return }
    $s = [IO.File]::Open($Path,'Open','Read','ReadWrite')
    try {
        $s.Position = $Offset.Value
        if ($s.Position -ge $s.Length) { return }
        $r = [IO.StreamReader]::new($s,$utf8)
        try { $text = $r.ReadToEnd() } finally { $r.Dispose() }
        $end = $text.LastIndexOf("`n")
        if ($end -lt 0) { return }
        $complete = $text.Substring(0,$end+1)
        foreach ($line in $complete.TrimEnd("`r","`n").Split("`n")) { $Writer.WriteLine('EVENT '+$line.TrimEnd("`r")) }
        $Offset.Value += $utf8.GetByteCount($complete)
    } finally { $s.Dispose() }
}
function Invoke-SessionBatch($Client,$Stream,$Writer,[string]$Id) {
    $src = Join-Path $root "build\runs\$Id"
    $dst = Join-Path $privateRuns $Id
    if ([IO.Directory]::Exists($dst)) { throw 'This run was already submitted in this session.' }
    $queue = Read-Body $Stream 8192
    $profiles = Read-Body $Stream 32768
    $null = [IO.Directory]::CreateDirectory($dst)
    [IO.File]::WriteAllText((Join-Path $dst 'queue.json'),$queue,$utf8)
    [IO.File]::WriteAllText((Join-Path $dst 'hv-profiles.json'),$profiles,$utf8)
    $Writer.WriteLine('ACCEPTED')
    $stopPath = Join-Path $src 'STOP'
    $argsLine = '-NoProfile -File "{0}" -RunDir "{1}" -SessionLabRoot "{2}" -SessionStopFile "{3}" -SessionVmId {4}' -f $worker,$dst,$lab,$stopPath,$vm.Id
    $child = Start-Process -FilePath $exe -WorkingDirectory $private -NoNewWindow -ArgumentList $argsLine -PassThru
    $null = $child.Handle # Retain the native process handle until exit status is read.
    $offset = 0L
    $events = Join-Path $dst 'events.jsonl'
    try {
        while (!$child.WaitForExit(200)) {
            if ($Client.Client.Poll(0,[Net.Sockets.SelectMode]::SelectRead) -and !$Client.Client.Available) { throw 'Client disconnected.' }
            Send-Events $events ([ref]$offset) $Writer
        }
        Send-Events $events ([ref]$offset) $Writer
        foreach ($image in [IO.Directory]::EnumerateFiles($dst,'*.png')) {
            $Writer.WriteLine('IMAGE '+[IO.Path]::GetFileName($image)+' '+[Convert]::ToBase64String([IO.File]::ReadAllBytes($image)))
        }
        $serial = Join-Path $dst 'l1-handoff.serial'
        if ([IO.File]::Exists($serial)) {
            $Writer.WriteLine('SERIAL '+[Convert]::ToBase64String([IO.File]::ReadAllBytes($serial)))
        }
        $Writer.WriteLine('END '+$child.ExitCode)
        return $child.ExitCode
    } finally {
        if (!$child.HasExited) {
            [IO.File]::WriteAllText((Join-Path $dst 'STOP'),'')
            $child.WaitForExit()
        }
        $child.Dispose()
    }
}

$sessionRoot = Join-Path $programs ('HvLabSession-'+(Get-TextDigest $root.ToLowerInvariant()).Substring(0,20))
$descriptor = Join-Path $sessionRoot 'session.json'
if (!$principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    $child = Start-Process -FilePath $exe -WorkingDirectory $PSHOME -Verb RunAs -PassThru -ArgumentList ('-NoProfile -File "' + $PSCommandPath + '"')
    $timer = [Diagnostics.Stopwatch]::StartNew()
    while (!$child.WaitForExit(200)) {
        if ([IO.File]::Exists($descriptor)) {
            try { $active = [IO.File]::ReadAllText($descriptor) | ConvertFrom-Json } catch { continue }
            if ($active.pid -eq $child.Id) { Write-Host "Delegated lab session ready (PID $($child.Id))."; return }
        }
        if ($timer.Elapsed.TotalSeconds -gt 45) { throw 'Worker not ready; inspect the elevated console.' }
    }
    throw "Session did not start (exit $($child.ExitCode)). Inspect the console or $sessionRoot\startup-error.txt."
}
$env:Path = "$windows\System32;$windows;$PSHOME"
$env:PSModulePath = "$PSHOME\Modules;$programs\WindowsPowerShell\Modules"
Set-Location -LiteralPath $PSHOME
New-PrivateRoot $sessionRoot
$lock = [IO.File]::Open((Join-Path $sessionRoot 'active.lock'),'OpenOrCreate','Write','None')
$private = Join-Path $sessionRoot ([guid]::NewGuid().ToString('N'))
$directoryLocks = [Collections.Generic.List[IDisposable]]::new()
$listener = $null
try {
    $vm = Get-VM -Name $vmName -ErrorAction Stop
    if (@($vm).Count -ne 1 -or $vm.Name -ne $vmName -or $vm.Generation -ne 2) {
        throw "Expected one Generation 2 VM named $vmName."
    }
    $store = if ($vmName -eq 'HV-SVM-L1') { Join-Path $lab "machines\$vmName" }
             else { Join-Path $programs "HvLabMachine-$vmName" }
    $location = [IO.Path]::GetFullPath($vm.ConfigurationLocation).TrimEnd('\')
    if ($location -ine $store -and !$location.StartsWith($store + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw 'VM configuration is outside the expected checkout store.'
    }
    Assert-Plain $location
    $privateRuns = Join-Path $private 'runs'
    $null = [IO.Directory]::CreateDirectory($privateRuns)
    $env:TEMP = $private; $env:TMP = $private
    # Keep the ISO path rooted in the same real directories until revocation.
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
public static class HvSessionDirectory {
    [DllImport("kernel32", CharSet=CharSet.Unicode, SetLastError=true)]
    public static extern SafeFileHandle CreateFile(string name, uint access, uint share,
        IntPtr security, uint disposition, uint flags, IntPtr template);
}
'@
    $chain = [Collections.Generic.Stack[string]]::new()
    for ($p = (Join-Path $lab 'media'); $p; $p = [IO.Path]::GetDirectoryName($p)) { $chain.Push($p) }
    foreach ($p in $chain) {
        $directoryLocks.Add((Lock-Directory $p))
    }
    $directoryLocks.Add((Lock-Directory (Join-Path $root 'build')))
    $directoryLocks.Add((Lock-Directory (Join-Path $root 'build\runs')))
    $worker = Join-Path $private 'Invoke-HvQueue.ps1'
    $trusted = @{
        'Invoke-HvQueue.ps1'='b3a255e3850484c9053927e66fc9b26a7bbe020d59ac4ab7e62871bb896ddb9f'
        'Receive-HvHandoff.ps1'='87780e5797d51dd3f74ba1815491dbcf52f74d767d8a301f6b9fd2eb1084641b'
        'HvConsole.ps1'='921c4638be60b02d76930a7a881bb376a8fe599c61c6d8f5b3b3d36ca2510275'
    }
    foreach ($name in $trusted.Keys) {
        $source = Read-InputText (Join-Path $lab $name) 65536
        if ((Get-TextDigest $source.Replace("`r`n","`n")) -cne $trusted[$name]) { throw "Unreviewed worker source: $name" }
        [IO.File]::WriteAllText((Join-Path $private $name),$source,[Text.UTF8Encoding]::new($true))
    }
    $listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0)
    $listener.ExclusiveAddressUse = $true
    $listener.Start(1)
    $token = [guid]::NewGuid().ToString('N')
    $data = @{port=$listener.LocalEndpoint.Port;token=$token;pid=$PID;handoffSerial=[int]($vmName -eq 'HV-SVM-L1')} | ConvertTo-Json -Compress
    [IO.File]::WriteAllText($descriptor,$data,$utf8)
    Write-Host "LAB SESSION READY: $($vm.Name) / $($vm.Id). Press Q or close this window to revoke."
    while ($true) {
        if ([Console]::KeyAvailable -and [Console]::ReadKey($true).Key -eq 'Q') { break }
        if (!$listener.Pending()) { Start-Sleep -Milliseconds 150; continue }
        $client = $listener.AcceptTcpClient()
        $writer = $null
        try {
            $stream = $client.GetStream(); $stream.WriteTimeout = 5000
            $writer = [IO.StreamWriter]::new($stream,$utf8); $writer.AutoFlush=$true; $writer.NewLine="`n"
            $requestTimer = [Diagnostics.Stopwatch]::StartNew()
            $id = Read-Request $stream
            Write-Host "Delegated lab batch $id"
            $runLock = Lock-Directory (Join-Path $root "build\runs\$id")
            try { $result = Invoke-SessionBatch $client $stream $writer $id } finally { $runLock.Dispose() }
            if ($result -eq 2) { break }
        } catch {
            Write-Warning $_.Exception.Message
            if ($writer) { try { $writer.WriteLine('ERROR Request failed; inspect the session console.') } catch {} }
        } finally { $client.Close() }
    }
} catch {
    [IO.File]::WriteAllText((Join-Path $sessionRoot 'startup-error.txt'),$_.ToString(),$utf8)
    throw
} finally {
    if ($listener) { $listener.Stop() }
    [IO.File]::Delete($descriptor)
    foreach ($handle in $directoryLocks) { $handle.Dispose() }
    $lock.Dispose()
    Write-Host 'Lab session closed. No further batches accepted.'
}
