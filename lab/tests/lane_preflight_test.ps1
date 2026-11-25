#Requires -Version 5.1
# From the checkout root: powershell.exe -NoProfile -File lab\tests\lane_preflight_test.ps1
$ErrorActionPreference = 'Stop'
$PSModuleAutoLoadingPreference = 'None'

# Load only the two function definitions, never the privileged worker's entry point.
$worker = [IO.Path]::Combine([IO.Path]::GetDirectoryName($PSScriptRoot), 'Invoke-HvQueue.ps1')
$tokens = $errors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile($worker, [ref]$tokens, [ref]$errors)
if ($errors.Count) { throw ($errors -join "`n") }
foreach ($name in 'Get-LabView','Request-Attention') {
    $definition = @($ast.EndBlock.Statements | Where-Object {
        $_ -is [Management.Automation.Language.FunctionDefinitionAst] -and $_.Name -eq $name
    })
    if ($definition.Count -ne 1) { throw "Expected one top-level definition of $name" }
    . ([scriptblock]::Create($definition[0].Extent.Text))
}

$dedicatedLane = $true
$vmName = 'HV-SVM-L1-A'
$vmId = [guid]::NewGuid()
$store = $PSScriptRoot
$driveId = 'dvd1'
$disc = [pscustomobject]@{ Id=$driveId; Path='reviewed.iso' }
$laneVm = [pscustomobject]@{
    Id=$vmId; Name=$vmName; Generation=2; ConfigurationLocation=$store; State='Off'
    MemoryStartup=2GB; DynamicMemoryEnabled=$false
    AutomaticStartAction='Nothing'; AutomaticCheckpointsEnabled=$false; CheckpointType='Standard'
}
$firmware = [pscustomobject]@{ SecureBoot='Off' }
function Get-VM($Id) { if ($Id -ne $vmId) { throw 'Unexpected VM ID' }; $laneVm }
function Assert-PlainPath($Path) { if ($Path -ne $store) { throw 'Unexpected store' } }
function Get-VMProcessor($VM) { [pscustomobject]@{ Count=2; ExposeVirtualizationExtensions=$true } }
function Get-VMDvdDrive($VM) { $disc }
function Get-VMNetworkAdapter($VM) {}
function Get-VMHardDiskDrive($VM) {}
function Get-VMComPort($VM) {}
function Get-VMFirmware($VM) { if ($VM -ne $laneVm) { throw 'Unexpected firmware target' }; $firmware }

foreach ($state in 'Off','Running') {
    $laneVm.State = $state
    $view = Get-LabView $state $disc.Path
    if ($view.VM -ne $laneVm -or $view.DVD -ne $disc -or $view.State -ne $state) {
        throw "Valid $state lane did not return its VM and DVD"
    }

    foreach ($case in @(
        @{ Target=$laneVm; Property='AutomaticStartAction'; Value='Start' }
        @{ Target=$laneVm; Property='AutomaticCheckpointsEnabled'; Value=$true }
        @{ Target=$laneVm; Property='CheckpointType'; Value='Production' }
        @{ Target=$firmware; Property='SecureBoot'; Value='On' }
    )) {
        $target = $case.Target
        $property = $case.Property
        $original = $target.$property
        $target.$property = $case.Value
        $script:finalState = 'ERROR'
        try {
            $null = Get-LabView $state $disc.Path
        } catch {
            if ($script:finalState -ne 'ATTENTION') { throw }
            continue
        } finally {
            $target.$property = $original
        }
        throw "$state lane accepted $property drift"
    }
}
'Lane preflight: valid Off/Running views accepted; four isolated drifts rejected with ATTENTION in both states.'
