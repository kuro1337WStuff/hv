#Requires -Version 5.1
#Requires -RunAsAdministrator
param()

$ErrorActionPreference = 'Stop'
if ($args.Count -or $PSVersionTable.PSEdition -ne 'Desktop' -or ![Environment]::Is64BitProcess) {
    throw 'Run without arguments in 64-bit Windows PowerShell 5.1.'
}

# Never accept a VM, checkout or storage path from the caller. After selecting
# the lane we neither read from nor write to the checkout.
if ($PSCommandPath -notmatch '^[A-Za-z]:\\' -or [IO.Path]::GetFullPath($PSCommandPath) -ne $PSCommandPath -or
    [IO.Path]::GetFileName($PSScriptRoot) -ne 'lab' -or [IO.Path]::GetFileName($PSCommandPath) -ne 'New-HvLane.ps1') {
    throw 'Expected the provisioner under lab in a local dedicated checkout.'
}
$checkout = [IO.Path]::GetDirectoryName($PSScriptRoot)
$name = switch ([IO.Path]::GetFileName($checkout)) {
    'hv-lane-a' { 'HV-SVM-L1-A' }
    'hv-lane-b' { 'HV-SVM-L1-B' }
    default { throw 'Not a dedicated lane runner checkout.' }
}
function Assert-PlainPath([string]$Path) {
    for ($p = $Path; $p; $p = [IO.Path]::GetDirectoryName($p)) {
        if ([IO.File]::GetAttributes($p) -band [IO.FileAttributes]::ReparsePoint) { throw "Linked path: $p" }
    }
}
Assert-PlainPath $PSCommandPath
$windows = [Environment]::GetFolderPath('Windows')
$programs = [Environment]::GetFolderPath('ProgramFiles')
Assert-PlainPath $programs
$env:Path = "$windows\System32;$windows;$PSHOME"
$env:PSModulePath = "$PSHOME\Modules"
Set-Location -LiteralPath $PSHOME
[Environment]::CurrentDirectory = $PSHOME
Import-Module -Name Hyper-V

$leaf = "HvLabMachine-$name"
$store = [IO.Path]::Combine($programs,$leaf)
if (Get-VM | Where-Object Name -eq $name) { throw "VM $name already exists. Not modifying it." }
if ([IO.Directory]::GetFileSystemEntries($programs,$leaf).Length) { throw "Storage already exists: $store" }
$os = Get-CimInstance -ClassName Win32_OperatingSystem -Property FreePhysicalMemory
if ([uint64]$os.FreePhysicalMemory * 1KB -lt 8GB) { throw 'Need at least 8 GiB of available physical memory.' }
$drive = [IO.DriveInfo]::new([IO.Path]::GetPathRoot($store))
if ($drive.DriveType -ne 'Fixed' -or $drive.AvailableFreeSpace -lt 16GB) { throw 'Need at least 16 GiB free on the fixed VM storage volume.' }
$version = (Get-VMHostSupportedVersion -Default).Version
if ([version]$version -lt [version]'9.3') { throw "Config $version is too old for AMD nesting." }

# Publish an empty, protected directory under native Program Files. Move is
# exclusive even if two provisioning attempts pass the preflight together.
$admins = [Security.Principal.SecurityIdentifier]::new('S-1-5-32-544')
$system = [Security.Principal.SecurityIdentifier]::new('S-1-5-18')
$acl = [Security.AccessControl.DirectorySecurity]::new()
$acl.SetOwner($admins)
$acl.SetAccessRuleProtection($true,$false)
foreach ($sid in $admins,$system) {
    $acl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new($sid,'FullControl','ContainerInherit,ObjectInherit','None','Allow'))
}
# Suppress an individual owner's implicit WRITE_DAC on elevated descendants.
$acl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new(
    [Security.Principal.SecurityIdentifier]::new('S-1-3-4'),'ReadPermissions','ContainerInherit,ObjectInherit','None','Allow'))
$reservation = [IO.Path]::Combine($programs,('HvLabProvision-'+[guid]::NewGuid().ToString('N')))
$null = [IO.Directory]::CreateDirectory($reservation,$acl)
try { [IO.Directory]::Move($reservation,$store) }
catch { [IO.Directory]::Delete($reservation,$false); throw }

# From here on any partial VM/store is evidence: never roll it back or retry.
$vm = New-VM -Name $name -Path $store -Generation 2 -Version $version -MemoryStartupBytes 2GB -NoVHD
Get-VMNetworkAdapter -VM $vm | Remove-VMNetworkAdapter -Confirm:$false
Set-VM -VM $vm -AutomaticStartAction Nothing -AutomaticCheckpointsEnabled $false -CheckpointType Standard
Set-VMMemory -VM $vm -DynamicMemoryEnabled $false
Set-VMProcessor -VM $vm -Count 2 -ExposeVirtualizationExtensions $true
$drives = @(Get-VMDvdDrive -VM $vm)
if (!$drives.Count) { $dvd = Add-VMDvdDrive -VM $vm -Passthru }
elseif ($drives.Count -eq 1 -and !$drives[0].Path) { $dvd = $drives[0] }
else { throw 'Unexpected DVD configuration on new VM.' }
Set-VMFirmware -VM $vm -EnableSecureBoot Off -FirstBootDevice $dvd

$vm = Get-VM -Id $vm.Id
$cpu = Get-VMProcessor -VM $vm
$drives = @(Get-VMDvdDrive -VM $vm)
if ($vm.Name -ne $name -or $vm.Generation -ne 2 -or $vm.State -ne 'Off' -or
    $vm.MemoryStartup -ne 2GB -or $vm.DynamicMemoryEnabled -or $cpu.Count -ne 2 -or !$cpu.ExposeVirtualizationExtensions -or
    $vm.AutomaticStartAction -ne 'Nothing' -or $vm.AutomaticCheckpointsEnabled -or $vm.CheckpointType -ne 'Standard' -or
    (Get-VMFirmware -VM $vm).SecureBoot -ne 'Off' -or $drives.Count -ne 1 -or $drives[0].Path -or
    @(Get-VMNetworkAdapter -VM $vm).Count -or @(Get-VMHardDiskDrive -VM $vm).Count -or
    @(Get-VMComPort -VM $vm | Where-Object Path).Count) {
    throw "Unexpected lane configuration; inspect $name ($($vm.Id)) and $store."
}
Write-Host "Provisioned $name ($($vm.Id)), Off. Storage: $store"
