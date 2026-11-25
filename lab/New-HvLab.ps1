#Requires -Version 5.1
#Requires -RunAsAdministrator
#Requires -Modules Hyper-V
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$name = 'HV-SVM-L1'
$iso = Join-Path $PSScriptRoot 'media\alpine-virt-3.24.4-x86_64.iso'
$store = Join-Path $PSScriptRoot 'machines\HV-SVM-L1'
$log = Join-Path $PSScriptRoot 'provision.log'
Start-Transcript -Path $log -Append | Out-Null
try {
    if (Get-VM | Where-Object Name -eq $name) { throw "VM $name already exists. Not modifying it." }
    if (Test-Path -LiteralPath $store) { throw "Storage already exists: $store. Not overwriting it." }
    if ((Get-FileHash -Algorithm SHA256 -LiteralPath $iso).Hash -ne '157D353F8E54252CAD2D9812A0D56BB7FA2231FC154A110E1D180D7480EF8B4F') { throw 'Unexpected Alpine ISO checksum.' }
    $version = (Get-VMHostSupportedVersion -Default).Version
    if ([version]$version -lt [version]'9.3') { throw "Config $version is too old for AMD nesting." }

    New-Item -ItemType Directory -Path $store | Out-Null
    $vm = New-VM -Name $name -Generation 2 -Version $version -MemoryStartupBytes 2GB -Path $store -NewVHDPath (Join-Path $store 'lab.vhdx') -NewVHDSizeBytes 16GB
    Get-VMNetworkAdapter -VM $vm | Remove-VMNetworkAdapter -Confirm:$false
    Set-VM -VM $vm -AutomaticStartAction Nothing -AutomaticStopAction ShutDown -AutomaticCheckpointsEnabled $false -CheckpointType Standard
    Set-VMMemory -VM $vm -DynamicMemoryEnabled $false
    Set-VMProcessor -VM $vm -Count 2 -ExposeVirtualizationExtensions $true
    $dvd = Add-VMDvdDrive -VM $vm -Path $iso -Passthru
    Set-VMFirmware -VM $vm -EnableSecureBoot Off -FirstBootDevice $dvd
    Set-VMComPort -VM $vm -Number 1 -Path '\\.\pipe\HV-SVM-L1-COM1'
    Checkpoint-VM -VM $vm -SnapshotName 'Clean - before first boot'
    Start-VM -VM $vm
    $vm = Get-VM -Id $vm.Id
    $cpu = Get-VMProcessor -VM $vm
    $nics = @(Get-VMNetworkAdapter -VM $vm).Count
    if ($vm.State -ne 'Running' -or !$cpu.ExposeVirtualizationExtensions -or $nics -ne 0) { throw 'VM did not start with the requested isolation and CPU settings.' }
    Write-Host "VM=$name ID=$($vm.Id) state=$($vm.State) config=$($vm.Version) vCPUs=$($cpu.Count) nested=$($cpu.ExposeVirtualizationExtensions) NICs=$nics"
    Write-Host "Console: vmconnect.exe localhost $name"
} finally {
    Stop-Transcript | Out-Null
}
