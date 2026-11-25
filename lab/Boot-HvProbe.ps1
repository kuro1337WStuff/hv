#Requires -Version 5.1
#Requires -RunAsAdministrator
#Requires -Modules Hyper-V
[CmdletBinding(DefaultParameterSetName='Halt')]
param(
    [Parameter(ParameterSetName='Halt')][switch]$L2Smoke,
    [Parameter(ParameterSetName='Resume')][switch]$CpuidResume,
    [Parameter(ParameterSetName='Protection')][switch]$NptWrite
)
$ErrorActionPreference = 'Stop'
$vm = Get-VM -Name 'HV-SVM-L1'
$store = Join-Path $PSScriptRoot 'machines\HV-SVM-L1'
$inStore = ($vm.ConfigurationLocation.TrimEnd('\') + '\').StartsWith($store + '\', [StringComparison]::OrdinalIgnoreCase)
if (!$inStore -or $vm.Generation -ne 2 -or @(Get-VMNetworkAdapter -VM $vm).Count -ne 0) { throw "Not the isolated project VM ($($vm.ConfigurationLocation)); refusing to change its boot media." }
$dvd = @(Get-VMDvdDrive -VM $vm)
if ($dvd.Count -ne 1) { throw 'Expected exactly one lab DVD drive.' }
$alpine = Join-Path $PSScriptRoot 'media\alpine-virt-3.24.4-x86_64.iso'
$stem = if ($NptWrite) { 'hv-npt-write' } elseif ($CpuidResume) { 'hv-cpuid-resume' } elseif ($L2Smoke) { 'hv-l2-smoke' } else { 'hv-probe' }
$image = (Resolve-Path (Join-Path $PSScriptRoot "..\build\$stem.iso")).Path
$hash = (Get-FileHash $image).Hash
$media = Join-Path $PSScriptRoot "media\$stem-$hash.iso"
if (!(Test-Path -LiteralPath $media)) { Copy-Item -LiteralPath $image -Destination $media }
if ((Get-FileHash $media).Hash -ne $hash) { throw 'Staged media checksum mismatch.' }
if ($vm.State -eq 'Running' -and $dvd[0].Path -eq $alpine) {
    # Preserve the live RAM disk before requesting an orderly Alpine shutdown.
    $point = 'Alpine before UEFI probe'
    if (!(Get-VMSnapshot -VM $vm | Where-Object Name -eq $point)) { Checkpoint-VM -VM $vm -SnapshotName $point }
    Stop-VM -VM $vm -Confirm:$false
}
if ((Get-VM -Id $vm.Id).State -ne 'Off') { throw 'Power off the lab VM before replacing its current image. No forced stop was requested.' }
Set-VMDvdDrive -VMDvdDrive $dvd[0] -Path $media
Set-VMFirmware -VM $vm -FirstBootDevice $dvd[0]
Start-VM -VM $vm
Write-Output "Started $($vm.Name) with $media"
