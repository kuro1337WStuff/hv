#Requires -Version 5.1
# Host-only: load just the worker's filesystem guards, not its privileged entry point.
# No enrollment, public API call, Hyper-V command or protected/live file access.
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$lab = Split-Path -Parent $PSScriptRoot
$tokens=$null; $errors=$null
$worker = [Management.Automation.Language.Parser]::ParseFile((Join-Path $lab 'Invoke-HvQueue.ps1'),[ref]$tokens,[ref]$errors)
if ($errors.Count) { throw 'Could not parse the worker.' }
foreach ($name in 'Assert-PlainPath','Initialize-SessionMedia') {
    $def = @($worker.EndBlock.Statements | Where-Object { $_ -is [Management.Automation.Language.FunctionDefinitionAst] -and $_.Name -ceq $name })
    if ($def.Count -ne 1) { throw "Missing worker guard $name." }; . ([scriptblock]::Create($def[0].Extent.Text))
}
Initialize-SessionMedia
. (Join-Path $lab 'HvW04Release.ps1')

function Denied([scriptblock]$Action, [string]$Message) {
    try { $null = & $Action } catch {
        if ($Message -ceq 'missing') {
            if ($_.Exception.GetBaseException() -isnot [IO.FileNotFoundException]) { throw }
        } elseif (!$_.Exception.Message.Contains($Message)) { throw }
        return
    }
    throw "Expected refusal: $Message"
}

$root = Join-Path ([IO.Path]::GetTempPath()) ('w04-release-'+[guid]::NewGuid().ToString('N'))
$ascii = [Text.Encoding]::ASCII
try {
    $r = @{Store=(Join-Path $root 'preservation'); SessionRoot=(Join-Path $root 'session'); Operations=(Join-Path $root 'operations'); Phase=-1; CheckpointId=$null; Pins=@{};
           WorkerHash=('1'*64); ModuleHash=('2'*64); VmId=[guid]::NewGuid(); SavedId=[guid]::NewGuid(); Ids=@()}
    $r.Package = Join-Path $r.Store '146-R2'; $r.Selector = Join-Path $r.SessionRoot '147-W04.approval'
    foreach ($p in $r.Package,$r.SessionRoot,(Join-Path $r.Store 'requests'),(Join-Path $r.Operations 'evidence\147')) { $null=[IO.Directory]::CreateDirectory($p) }
    $id = 8000000123L
    Denied { Read-W04Selector $r.Selector } 'missing'
    foreach ($invalid in '7204015719',"$id`n",('0'+$id)) {
        [IO.File]::WriteAllText($r.Selector,$invalid,$ascii); Denied { Read-W04Selector $r.Selector } 'Invalid W04 enrollment selector'
    }
    [IO.File]::WriteAllText($r.Selector,[string]$id,$ascii); $r.Enrolled = Read-W04Selector $r.Selector

    $policy = [pscustomobject]@{milestone='windows-10-l2'; gates=@{W04='Windows kernel starts'}; benches=@{A=@{held=$false;release="146-R2C Off $w04Off"};B=@{held=$true}};
        admissions=[pscustomobject]@{'224'=[pscustomobject]@{gate='W04';observation=$w04Observation}}}
    Assert-W04Policy $policy
    $policy.benches.A.held = $true; Denied { Assert-W04Policy $policy } 'No exact bench A'; $policy.benches.A.held=$false
    $policy.admissions.'224'.observation = 'W04 PASS'; Denied { Assert-W04Policy $policy } 'No exact bench A'; $policy.admissions.'224'.observation=$w04Observation
    $policyPath = Join-Path $r.Operations 'windows_l2.json'; [IO.File]::WriteAllText($policyPath,($policy | ConvertTo-Json -Depth 6),$ascii)
    $r.PolicyHash=(Read-W04File $policyPath).Hash; $r.Pins.Add($policyPath,$r.PolicyHash)
    $body = Get-W04Commitment $r
    $now = [datetimeoffset]::Parse('2026-12-02T01:00:00Z'); $issuedText='2026-12-02T00:00:00Z'
    $comment = @{id=$id; user=@{login='kuro1337WStuff'};author_association='OWNER';issue_url='https://api.github.com/repos/kuro1337WStuff/hv/issues/13';
        html_url="https://github.com/kuro1337WStuff/hv/issues/13#issuecomment-$id";body=$body;created_at=$issuedText;updated_at=$issuedText}
    $r.Issued = Get-W04GrantTime $comment $id $body $now
    foreach ($badBody in $body.Replace('bench=A','bench=B'),$body.Replace('reservation=7204015719','reservation=1'),$body.Replace($r.WorkerHash,('3'*64)),
              $body.Replace($w04Off,('5'*64)),$body.Replace($r.PolicyHash,('6'*64)),$body.Replace($w04Run,'different-run'),
              $body.Replace($r.ModuleHash,('4'*64)),$body.Replace([string]$w04StopTicks,'0'),$body.Replace('one checkpoint,mount,firmware,boot','power off only')) {
        $comment.body = $badBody; Denied { Get-W04GrantTime $comment $id $body $now } 'No distinct, unedited'; $comment.body=$body
    }
    $comment.author_association='MEMBER'; Denied { Get-W04GrantTime $comment $id $body $now } 'No distinct, unedited'; $comment.author_association='OWNER'
    $comment.updated_at='2026-12-02T00:01:00Z'; Denied { Get-W04GrantTime $comment $id $body $now } 'No distinct, unedited'; $comment.updated_at=$issuedText
    Denied { Get-W04GrantTime $comment $id $body $r.Issued.AddHours(24) } 'not current'
    Denied { Get-W04GrantTime $comment $id $body $r.Issued.AddTicks(-1) } 'not current'

    $stop = Join-Path $r.Operations 'STOP'; $stamp = [datetime]::new($w04StopTicks,[DateTimeKind]::Utc)
    Denied { Assert-W04Freeze $stop } 'missing'
    [IO.File]::WriteAllBytes($stop,[byte[]]@()); [IO.File]::SetCreationTimeUtc($stop,$stamp); [IO.File]::SetLastWriteTimeUtc($stop,$stamp)
    Assert-W04Local $r
    [IO.File]::SetLastWriteTimeUtc($stop,$stamp.AddTicks(1)); Denied { Assert-W04Local $r } 'campaign STOP'; [IO.File]::SetLastWriteTimeUtc($stop,$stamp)
    [IO.File]::SetCreationTimeUtc($stop,$stamp.AddTicks(1)); Denied { Assert-W04Local $r } 'campaign STOP'; [IO.File]::SetCreationTimeUtc($stop,$stamp)
    [IO.File]::WriteAllText($stop,'x',$ascii); [IO.File]::SetLastWriteTimeUtc($stop,$stamp); Denied { Assert-W04Local $r } 'campaign STOP'
    [IO.File]::WriteAllBytes($stop,[byte[]]@()); [IO.File]::SetLastWriteTimeUtc($stop,$stamp); Assert-W04Local $r
    $revoked = Join-Path $r.SessionRoot '147-W04.REVOKED'; [IO.File]::WriteAllText($revoked,''); Denied { Assert-W04Local $r } 'blocked by existing'; [IO.File]::Delete($revoked)
    [IO.File]::AppendAllText($policyPath,' '); Denied { Assert-W04Local $r } 'decision inputs changed'
    [IO.File]::WriteAllText($policyPath,($policy | ConvertTo-Json -Depth 6),$ascii); Assert-W04Local $r
    [IO.File]::WriteAllText($r.Selector,[string]($id+1),$ascii); Denied { Assert-W04Local $r } 'enrollment changed'
    [IO.File]::Delete($r.Selector); Denied { Assert-W04Local $r } 'missing'
    [IO.File]::WriteAllText($r.Selector,[string]$id,$ascii); $r.Enrolled=Read-W04Selector $r.Selector; Assert-W04Local $r

    # A partial marker of any phase (including an empty file) consumes the fixed store.
    $partial = Join-Path $r.Store '147-W04.firmware.json'; [IO.File]::WriteAllText($partial,''); Denied { Assert-W04Unused $r } 'blocked by existing'; [IO.File]::Delete($partial)
    $other = $r.Clone(); Assert-W04Unused $r; Write-W04Receipt $r intent
    $intent = Join-Path $r.Store '147-W04.intent.json'; $digest=(Read-W04File $intent).Hash
    Denied { Assert-W04Unused $other } 'blocked by existing'; Denied { Write-W04Receipt $other intent } 'blocked by existing'
    Denied { Write-W04Receipt $r mount } 'out of order'; Denied { Write-W04Receipt $r checkpoint } 'No fresh'
    $checkpoint=[guid]::NewGuid(); Write-W04Receipt $r checkpoint $checkpoint
    # Even a stale process with a matching in-memory cursor cannot replace a published receipt.
    $other.Phase=0
    try { Write-W04Receipt $other checkpoint ([guid]::NewGuid()); throw 'Overwrote a published checkpoint.' }
    catch { if ($_.Exception.GetBaseException() -isnot [IO.IOException]) { throw } }
    foreach ($phase in 'mount','firmware','boot') { Write-W04Receipt $r $phase }
    $boot=(Read-W04File (Join-Path $r.Store '147-W04.boot.json') -Text).Text | ConvertFrom-Json
    if ($boot.phase -cne 'boot' -or $boot.checkpoint -cne $checkpoint.ToString() -or $boot.approval_id -ne $id -or $boot.release_sha256 -cne $w04Off -or (Read-W04File $intent).Hash -cne $digest) { throw 'Broken durable receipt binding.' }
    'W04 host controls: grant scope, expiry, local revocation, STOP generation and exclusive phase receipts.'
} finally {
    if ([IO.Directory]::Exists($root)) { [IO.Directory]::Delete($root,$true) }
}
