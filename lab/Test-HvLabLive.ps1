#Requires -Version 5.1
#Requires -RunAsAdministrator
#Requires -Modules Hyper-V
# Run ONLY at the pristine Alpine live ISO's login prompt; uses the VM keyboard.
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$name = 'HV-SVM-L1'
$pipeName = 'HV-SVM-L1-COM1'
$vm = Get-VM -Name $name
$iso = (Resolve-Path (Join-Path $PSScriptRoot 'media\alpine-virt-3.24.4-x86_64.iso')).Path
if ($vm.State -ne 'Running' -or (Get-VMDvdDrive -VM $vm).Path -ne $iso -or @(Get-VMNetworkAdapter -VM $vm).Count -ne 0 -or (Get-VMComPort -VM $vm -Number 1).Path -ne "\\.\pipe\$pipeName") { throw 'Expected the isolated running Alpine lab with its own serial pipe.' }
$system = Get-CimInstance -Namespace root/virtualization/v2 -ClassName Msvm_ComputerSystem -Filter "Name='$($vm.Id)'"
$keyboard = Get-CimAssociatedInstance -InputObject $system -ResultClassName Msvm_Keyboard
function Send-Key([string]$Method, [int]$Code) {
    $result = Invoke-CimMethod -InputObject $keyboard -MethodName $Method -Arguments @{KeyCode=[uint32]$Code}
    if ($result.ReturnValue -ne 0) { throw "Virtual keyboard ${Method}: $($result.ReturnValue)" }
}
function Send-Line([string]$Text) {
    # TypeText uses text injection; the Linux virtual console needs US-layout key events.
    foreach ($c in $Text.ToCharArray()) {
        $shift = $false
        if ($c -cmatch '^[a-zA-Z0-9]$') {
            $key = [int][char]::ToUpperInvariant($c)
            $shift = [char]::IsUpper($c)
        } else {
            switch ($c) {
                ' ' { $key = 0x20 }
                '-' { $key = 0xbd }
                '/' { $key = 0xbf }
                '&' { $key = 0x37; $shift = $true }
                '|' { $key = 0xdc; $shift = $true }
                default { throw "Unsupported console character: $c" }
            }
        }
        if ($shift) { Send-Key PressKey 0x10 }
        try { Send-Key TypeKey $key } finally { if ($shift) { Send-Key ReleaseKey 0x10 } }
    }
    Send-Key TypeKey 13
}
$pipe = [IO.Pipes.NamedPipeClientStream]::new('.', $pipeName, [IO.Pipes.PipeDirection]::InOut, [IO.Pipes.PipeOptions]::Asynchronous)
try {
    $pipe.Connect(10000)
    $reader = [IO.StreamReader]::new($pipe)
    Send-Key TypeKey 13
    Start-Sleep -Seconds 4
    Send-Line 'root'
    Start-Sleep -Seconds 2
    Send-Line 'test -d /sys/firmware/efi && grep -wo svm /proc/cpuinfo | tee /dev/ttyS0'
    $deadline = [DateTime]::UtcNow.AddSeconds(20)
    while ([DateTime]::UtcNow -lt $deadline) {
        $line = $reader.ReadLineAsync()
        if (!$line.Wait([Math]::Max(1,[int]($deadline - [DateTime]::UtcNow).TotalMilliseconds))) { break }
        if ($null -eq $line.Result) { break }
        if ($line.Result -eq 'svm') {
            "$([DateTime]::UtcNow.ToString('o')) $($vm.Id) Alpine guest: UEFI present; svm present in /proc/cpuinfo." | Tee-Object -FilePath (Join-Path $PSScriptRoot 'guest-check.log')
            Send-Line 'exit'
            return
        }
    }
    throw 'No guest proof received; inspect the VM console. Host nesting settings alone are not guest proof.'
} finally { $pipe.Dispose() }
