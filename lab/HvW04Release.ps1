#Requires -Version 5.1
# Loaded only by the staged worker. Its Assert-PlainPath and HvSessionImage.OpenRead
# reject aliases/reparse points; the latter checks the single-link file on the open handle.
$w04Run = '1f0eceeb4492541ab6a0b8bc6b49bd17'
$w04Off = '5db0c4fc0b377f8db40eddabe91e2f26d954bc36ea896ec511319f4fab7ba3f3'
$w04StopTicks = 639312592452835964L
$w04Observation = 'Witness same-invocation HandleProtocol return to RVA 0x31245 with restored RSP and full 64-bit EFI_STATUS, or record the first bounded stop and what was not witnessed; not a W04 kernel PASS.'
$w04Phases = 'intent','checkpoint','mount','firmware','boot'

function Assert-W04Protection([string]$Path, [string]$Root) {
    Assert-PlainPath $Path
    if ($Path -ine $Root -and !$Path.StartsWith($Root+'\',[StringComparison]::OrdinalIgnoreCase)) { throw 'Outside protected W04 tree.' }
    $trusted = 'S-1-5-18','S-1-5-32-544'
    $readOnly = [int][Security.AccessControl.FileSystemRights]'ReadAndExecute,Synchronize'
    for ($p = $Path; ; $p = [IO.Path]::GetDirectoryName($p)) {
        $acl = Get-Acl -LiteralPath $p -ErrorAction Stop
        $owner = $acl.GetOwner([Security.Principal.SecurityIdentifier]).Value
        $rules = @($acl.GetAccessRules($true,$true,[Security.Principal.SecurityIdentifier]))
        $ownerRestricted = $false
        $fileInherits = $false
        if (!$rules.Count) { throw 'Missing protected W04 ACL.' }
        foreach ($rule in $rules) {
            $sid = $rule.IdentityReference.Value
            if ($rule.AccessControlType -ne 'Allow' -or ($sid -notin $trusted -and (([int]$rule.FileSystemRights) -band (-bnot $readOnly)))) { throw 'Writable or unexpected W04 ACL.' }
            if ($sid -eq 'S-1-3-4' -and !($rule.PropagationFlags -band [Security.AccessControl.PropagationFlags]::InheritOnly)) { $ownerRestricted = $true }
            if ($sid -in $trusted -and ($rule.InheritanceFlags -band [Security.AccessControl.InheritanceFlags]::ObjectInherit)) { $fileInherits = $true }
        }
        if ($owner -notin $trusted -and !$ownerRestricted) { throw 'Unrestricted W04 owner.' }
        if ($p -ieq $Root) {
            # Without an inheritable file ACE a new receipt can receive the token default DACL.
            if (!$acl.AreAccessRulesProtected -or !$fileInherits -or $owner -notin $trusted) { throw 'Unprotected W04 root.' }; break
        }
    }
}

function Read-W04File([string]$Path, [switch]$Text) {
    Assert-PlainPath $Path
    $f = [HvSessionImage]::OpenRead($Path)
    try {
        if ($f.Length -gt 131072) { throw 'Oversized W04 input.' }
        $result = @{Hash=(Get-FileHash -InputStream $f -Algorithm SHA256).Hash.ToLowerInvariant(); Length=$f.Length;
                    Created=[IO.File]::GetCreationTimeUtc($Path).Ticks; Written=[IO.File]::GetLastWriteTimeUtc($Path).Ticks}
        if ($Text) {
            $f.Position = 0
            $reader = [IO.StreamReader]::new($f,[Text.UTF8Encoding]::new($false,$true),$true,4096,$true)
            try { $result.Text = $reader.ReadToEnd() } finally { $reader.Dispose() }
        }
        return $result
    } finally { $f.Dispose() }
}

function Read-W04Proof($Release, [string]$Root, [string]$Path, [string]$Pin) {
    Assert-W04Protection $Path $Root
    $record = Read-W04File $Path -Text
    if ($Pin -cnotmatch '\A[0-9a-f]{64}\z' -or $record.Hash -cne $Pin) { throw 'W04 release evidence digest mismatch.' }
    $Release.Pins.Add($Path,$Pin)
    return (ConvertFrom-Json -InputObject $record.Text -ErrorAction Stop)
}

function Assert-W04Absent([string]$Parent, [string]$Leaf) {
    # Inspect the directory entry, including a dangling link; only absence permits proceeding.
    Assert-PlainPath $Parent
    if ([IO.Directory]::GetFileSystemEntries($Parent,$Leaf).Length) { throw "W04 blocked by existing $Leaf." }
}

function Assert-W04Unused($Release) {
    foreach ($ph in $w04Phases) { Assert-W04Absent $Release.Store "147-W04.$ph.json" }
}

function Assert-W04Freeze([string]$Path) {
    $entry = Read-W04File $Path
    if ($entry.Length -ne 0 -or $entry.Created -ne $w04StopTicks -or $entry.Written -ne $w04StopTicks) { throw 'W04 campaign STOP was changed or reasserted.' }
}

function Assert-W04Policy($Policy) {
    $a = $Policy.admissions.'224'
    if ($Policy.milestone -cne 'windows-10-l2' -or $Policy.gates.W04 -isnot [string] -or !$Policy.gates.W04 -or
        $Policy.benches.A.held -isnot [bool] -or $Policy.benches.A.held -or $Policy.benches.B.held -isnot [bool] -or !$Policy.benches.B.held -or
        $Policy.benches.A.release -isnot [string] -or !$Policy.benches.A.release.Contains($w04Off) -or
        $Policy.admissions -isnot [pscustomobject] -or @($Policy.admissions.PSObject.Properties).Count -ne 1 -or
        $a -isnot [pscustomobject] -or @($a.PSObject.Properties).Count -ne 2 -or $a.gate -isnot [string] -or $a.gate -cne 'W04' -or
        $a.observation -isnot [string] -or $a.observation -cne $w04Observation) { throw 'No exact bench A W04 admission for #224.' }
}

function Read-W04Selector([string]$Path) {
    $record = Read-W04File $Path -Text
    # ASCII digits only, including on disk (no BOM, line ending or surrounding whitespace).
    if ($record.Text -cnotmatch '\A[1-9][0-9]{0,18}\z' -or $record.Length -ne $record.Text.Length -or [long]$record.Text -le 7204015719L) { throw 'Invalid W04 enrollment selector.' }
    return $record
}

function Get-W04Commitment($Release) {
    return "AUTHORIZE 147-W04: bench=A issue=224 gate=W04 one checkpoint,mount,firmware,boot; retain root STOP generation=$w04StopTicks; no retry, reference, B or kernel PASS. run=$w04Run reservation=7204015719 release=$w04Off worker=$($Release.WorkerHash) module=$($Release.ModuleHash) policy=$($Release.PolicyHash)"
}

function Assert-W04Time([datetimeoffset]$Issued, [datetimeoffset]$Now) {
    if ($Now -lt $Issued -or $Now -ge $Issued.AddHours(24)) { throw 'W04 authorization is not current.' }
}

function Get-W04GrantTime($Comment, [long]$Id, [string]$Body, [datetimeoffset]$Now) {
    if ($Id -le 7204015719L -or [string]$Comment.id -cne [string]$Id -or $Comment.user.login -cne 'kuro1337WStuff' -or
        $Comment.author_association -cne 'OWNER' -or $Comment.issue_url -cne 'https://api.github.com/repos/kuro1337WStuff/hv/issues/13' -or
        $Comment.html_url -cne "https://github.com/kuro1337WStuff/hv/issues/13#issuecomment-$Id" -or $Comment.body -cne $Body -or
        $Comment.created_at -isnot [string] -or $Comment.created_at -cne $Comment.updated_at) { throw 'No distinct, unedited W04 owner grant on #13.' }
    $issued = [datetimeoffset]::ParseExact($Comment.created_at,"yyyy-MM-dd'T'HH:mm:ss'Z'",[Globalization.CultureInfo]::InvariantCulture,[Globalization.DateTimeStyles]::AssumeUniversal)
    Assert-W04Time $issued $Now
    return $issued
}

function Assert-W04Local($Release) {
    Assert-W04Freeze (Join-Path $Release.Operations 'STOP')
    foreach ($dir in $Release.Store,$Release.SessionRoot,$Release.Package) { Assert-W04Absent $dir 'STOP'; Assert-W04Absent $dir 'REVOKED' }
    foreach ($dir in $Release.Store,$Release.SessionRoot,$Release.Operations) {
        foreach ($leaf in 'W04.STOP','W04.REVOKED','147-W04.STOP','147-W04.REVOKED') { Assert-W04Absent $dir $leaf }
    }
    foreach ($leaf in '146-R2C.STOP','146-R2C.REVOKED','146-R2.saved.json','146-R2.off.json') { Assert-W04Absent $Release.Package $leaf }
    foreach ($leaf in 'live146-R1.STOP','live146-R2.STOP','live146-preserved.json','live146-R1.intent.json','live146-R1.saved.json','live146-R1.off.json') { Assert-W04Absent $Release.SessionRoot $leaf }
    $requests = Join-Path $Release.Store 'requests'
    Assert-PlainPath $requests
    foreach ($trial in '146','147') {
        if ([IO.Directory]::GetFileSystemEntries($requests,$trial).Length) { Assert-W04Absent (Join-Path $requests $trial) 'REVOKED' }
    }
    Assert-W04Absent $Release.Operations 'REVOKED'
    $evidence = Join-Path $Release.Operations 'evidence\147'
    foreach ($leaf in 'STOP','REVOKED','submission.json') { Assert-W04Absent $evidence $leaf }
    $selector = Read-W04Selector $Release.Selector
    if ($selector.Hash -cne $Release.Enrolled.Hash -or $selector.Created -ne $Release.Enrolled.Created -or $selector.Written -ne $Release.Enrolled.Written) { throw 'W04 enrollment changed.' }
    foreach ($path in $Release.Pins.Keys) {
        if ((Read-W04File $path).Hash -cne $Release.Pins[$path]) { throw 'W04 decision inputs changed.' }
    }
}

function Assert-W04Authority([hashtable]$Release) {
    Assert-W04Protection $Release.Selector $Release.SessionRoot
    Assert-W04Protection $Release.Store $Release.Store
    Assert-W04Local $Release
    Assert-W04Time $Release.Issued ([datetimeoffset]::UtcNow)
}

function Open-W04Release([string]$SessionRoot, [string]$Operations, [string]$WorkerPath) {
    # Bind the operations sibling to the very same checkout-derived Program Files root as the broker.
    foreach ($path in $Operations,$WorkerPath,$SessionRoot) {
        if ($path -notmatch '\A[A-Za-z]:\\' -or [IO.Path]::GetFullPath($path) -ine $path) { throw 'Noncanonical W04 path.' }; Assert-PlainPath $path
    }
    $lane = Join-Path ([IO.Path]::GetDirectoryName($Operations)) 'hv-lane-a'
    $sha = [Security.Cryptography.SHA256]::Create()
    try { $key = [BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($lane.ToLowerInvariant()))).Replace('-','').Substring(0,20).ToLowerInvariant() } finally { $sha.Dispose() }
    $pf = [Environment]::GetFolderPath('ProgramFiles'); $stage = [IO.Path]::GetDirectoryName($WorkerPath)
    if ([IO.Path]::GetFileName($Operations) -ine 'hv-operations' -or $SessionRoot -ine (Join-Path $pf "HvLabSession-$key") -or
        [IO.Path]::GetDirectoryName($stage) -ine $SessionRoot -or [IO.Path]::GetFileName($stage) -cnotmatch '\A[0-9a-f]{32}\z' -or
        [IO.Path]::GetFileName($WorkerPath) -ine 'Invoke-HvQueue.ps1') { throw 'Unexpected W04 receiver or operations location.' }
    $store = Join-Path $pf 'HvLabPreservation-A'; $package = Join-Path $store '146-R2'
    $r = @{Store=$store; Package=$package; SessionRoot=$SessionRoot; Operations=$Operations; Selector=(Join-Path $SessionRoot '147-W04.approval'); Pins=@{}; Phase=-1; CheckpointId=$null}
    # The fixed preservation DACL is inherited. An individual default owner could
    # otherwise reclaim WRITE_DAC on its newly created receipts after elevation ends.
    if ([Security.Principal.WindowsIdentity]::GetCurrent().Owner.Value -notin 'S-1-5-18','S-1-5-32-544') { throw 'Unsafe default owner for immutable W04 receipts.' }
    Assert-W04Protection $r.Selector $SessionRoot
    Assert-W04Protection $store $store
    Assert-W04Unused $r
    foreach ($old in [IO.Directory]::GetDirectories($SessionRoot)) {
        if ($old -ieq $stage -or [IO.Path]::GetFileName($old) -notmatch '\A[0-9a-f]{32}\z') { continue }
        Assert-PlainPath $old
        if ([IO.Directory]::GetFileSystemEntries($old,'runs').Length) { Assert-W04Absent (Join-Path $old 'runs') $w04Run }
    }
    $r.Enrolled = Read-W04Selector $r.Selector
    $off = Read-W04Proof $r $store (Join-Path $package '146-R2C.off.json') $w04Off
    $intent = Read-W04Proof $r $store (Join-Path $package '146-R2.intent.json') $off.r2_intent
    $completion = Read-W04Proof $r $store (Join-Path $package '146-R2C.intent.json') $off.intent_sha256
    # R2 intent has no parent field. Its authenticated pre-preservation inspection does.
    $inspection = Read-W04Proof $r $SessionRoot (Join-Path $SessionRoot 'live146-inspection.json') $intent.inspection_sha256
    if ($off.stage -cne '146-R2C' -or $off.phase -cne 'off' -or $off.state -cne 'Off' -or $intent.phase -cne 'intent' -or
        $completion.phase -cne 'intent' -or $completion.approval_id -ne $off.approval_id -or $completion.checkpoint -cne $off.checkpoint -or
        $off.vm -cne $inspection.vm -or $off.run -cne $inspection.run -or $intent.vm -cne $off.vm -or $intent.source -cne $off.run) { throw 'W04 release chain mismatch.' }
    $r.VmId = [guid]::ParseExact($off.vm,'D'); $r.SavedId = [guid]::ParseExact($off.checkpoint,'D'); $r.SavedParent = [guid]::ParseExact($inspection.parent,'D')
    $ids = [Collections.Generic.HashSet[guid]]::new()
    $r.Ids = [string[]]@(foreach ($v in $off.ids) {
        $g = [guid]::ParseExact($v,'D'); if ($g -eq [guid]::Empty -or !$ids.Add($g)) { throw 'Invalid W04 release inventory.' }; $g.ToString('D')
    })
    if ($ids.Count -ne 18 -or $r.VmId -eq [guid]::Empty -or !$ids.Contains($r.SavedParent) -or !$ids.Contains($r.SavedId) -or $r.SavedParent -eq $r.SavedId) { throw 'Incomplete W04 release inventory.' }
    $module = Join-Path $stage 'HvW04Release.ps1'; $policyPath = Join-Path $Operations 'windows_l2.json'
    foreach ($file in $WorkerPath,$module) { Assert-W04Protection $file $SessionRoot; $r.Pins.Add($file,(Read-W04File $file).Hash) }
    $policy = Read-W04File $policyPath -Text; Assert-W04Policy (ConvertFrom-Json -InputObject $policy.Text -ErrorAction Stop)
    $r.WorkerHash=$r.Pins[$WorkerPath]; $r.ModuleHash=$r.Pins[$module]; $r.PolicyHash=$policy.Hash; $r.Pins.Add($policyPath,$policy.Hash)
    Assert-W04Local $r
    # Only this exact selected public comment is read, once. No auth, discovery, redirects or mutation.
    $id = [long]$r.Enrolled.Text
    $comment = Invoke-RestMethod -UseBasicParsing -Method Get -MaximumRedirection 0 -TimeoutSec 10 -ErrorAction Stop -Uri "https://api.github.com/repos/kuro1337WStuff/hv/issues/comments/$id" -Headers @{'User-Agent'='HvLab-147-W04';'Cache-Control'='no-cache'}
    $r.Issued = Get-W04GrantTime $comment $id (Get-W04Commitment $r) ([datetimeoffset]::UtcNow)
    Assert-W04Authority $r; Assert-W04Unused $r
    return $r
}

function Write-W04Receipt([hashtable]$Release, [ValidateSet('intent','checkpoint','mount','firmware','boot')][string]$Phase, [guid]$CheckpointId = [guid]::Empty) {
    $next = [Array]::IndexOf($w04Phases,$Phase)
    if ($next -lt 0 -or $next -ne $Release.Phase+1) { throw 'W04 receipt out of order.' }
    if ($next -eq 0) { Assert-W04Unused $Release }
    if ($Phase -ceq 'checkpoint') {
        if ($CheckpointId -eq [guid]::Empty -or $Release.Ids -contains $CheckpointId.ToString('D')) { throw 'No fresh W04 checkpoint identity.' }
    } elseif ($CheckpointId -ne [guid]::Empty -and $CheckpointId.ToString('D') -cne $Release.CheckpointId) { throw 'Inconsistent W04 checkpoint in receipt.' }
    if ($Phase -ceq 'checkpoint') { $Release.CheckpointId = $CheckpointId.ToString('D') }
    $receipt = [ordered]@{trial=147;run=$w04Run;reservation=7204015719L;phase=$Phase;utc=[datetime]::UtcNow.ToString('o');approval_id=[long]$Release.Enrolled.Text;issued=$Release.Issued.ToString('o');release_sha256=$w04Off;worker_sha256=$Release.WorkerHash;module_sha256=$Release.ModuleHash;policy_sha256=$Release.PolicyHash;stop_ticks=$w04StopTicks;vm=$Release.VmId.ToString();saved=$Release.SavedId.ToString();checkpoint=$Release.CheckpointId}
    $bytes = [Text.Encoding]::UTF8.GetBytes(($receipt | ConvertTo-Json -Compress))
    # Existence, even of a zero-length file after failure, spends the run. Never repair or truncate it.
    $file = [IO.File]::Open((Join-Path $Release.Store "147-W04.$Phase.json"),'CreateNew','Write','None')
    try { $file.Write($bytes,0,$bytes.Length); $file.Flush($true) } finally { $file.Dispose() }
    $Release.Phase = $next
}
