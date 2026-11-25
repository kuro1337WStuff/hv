#Requires -Version 5.1
# From the checkout root: powershell.exe -NoProfile -File lab\tests\stage_identity_test.ps1
$ErrorActionPreference = 'Stop'
if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT) { throw 'Requires Windows file sharing.' }

$lab = [IO.Path]::GetDirectoryName($PSScriptRoot)
function Get-TestDefinitions($FileName) {
    $tokens = $errors = $null
    $ast = [Management.Automation.Language.Parser]::ParseFile(
        [IO.Path]::Combine($lab, $FileName), [ref]$tokens, [ref]$errors)
    if ($errors.Count) { throw ($errors -join "`n") }
    $ast.EndBlock.Statements | Where-Object { $_ -is [Management.Automation.Language.FunctionDefinitionAst] }
}
# Neither the broker nor the command-line observer entry point is executed.
$launcher = @(Get-TestDefinitions 'Start-HvLabSession.ps1')
$producer = @($launcher | Where-Object { $_.Name -eq 'Open-StageReadiness' })
if ($producer.Count -ne 1) { throw 'Expected one Open-StageReadiness definition.' }
. ([scriptblock]::Create($producer[0].Extent.Text))
foreach ($name in 'Read-SessionDescriptor','Test-SessionPublication','Publish-SessionReadiness','Close-SessionPublication') {
    $definition = @($launcher | Where-Object { $_.Name -eq $name })
    if ($definition.Count -ne 1) { throw "Expected one $name definition." }
    . ([scriptblock]::Create($definition[0].Extent.Text))
}
$observer = @(Get-TestDefinitions 'Get-HvStageIdentity.ps1')
if (@($observer | Where-Object { $_.Name -eq 'Get-CurrentStageIdentity' }).Count -ne 1) {
    throw 'Expected one Get-CurrentStageIdentity definition.'
}
foreach ($definition in $observer) { . ([scriptblock]::Create($definition.Extent.Text)) }

function New-TestStage($Root, $SourceWorker) {
    $stage = [IO.Directory]::CreateDirectory([IO.Path]::Combine($Root, [guid]::NewGuid().ToString('N')))
    # The broker stages the worker text as UTF-8 with BOM.
    [IO.File]::WriteAllText([IO.Path]::Combine($stage.FullName, 'Invoke-HvQueue.ps1'),
        [IO.File]::ReadAllText($SourceWorker), [Text.UTF8Encoding]::new($true))
    $stage.FullName
}
function Assert-Refused($Label, $Root, $Runner) {
    try { $null = Get-CurrentStageIdentity $Root $Runner }
    catch { "PASS: $Label refused"; return }
    throw "FAIL: $Label was not refused"
}

# A child holds the actual producer stream while the parent observes it. Named events
# order the two observations without sleeps, pipe buffering, or unbounded waits.
function Test-LiveProducer($Stage, $Root, $Runner, $OtherRunner) {
    $prefix = 'Local\HvStageIdentityTest-' + [IO.Path]::GetFileName($Stage)
    $ready = $release = $child = $null
    $started = $false
    try {
        $ready = [Threading.EventWaitHandle]::new($false, 'AutoReset', $prefix + '-ready')
        $release = [Threading.EventWaitHandle]::new($false, 'AutoReset', $prefix + '-release')
        $childSource = '$ErrorActionPreference = ''Stop''' + "`n" + $producer[0].Extent.Text + "`n" + @'
$ready = [Threading.EventWaitHandle]::OpenExisting($env:HV_TEST_EVENT + '-ready')
$release = [Threading.EventWaitHandle]::OpenExisting($env:HV_TEST_EVENT + '-release')
$lease = Open-StageReadiness $env:HV_TEST_STAGE
try {
    if ($lease -isnot [IO.FileStream] -or !$lease.CanWrite -or $lease.CanRead -or $lease.Length -ne 0) {
        throw 'Expected a write-only zero-byte readiness stream.'
    }
    [void]$ready.Set()
    if (!$release.WaitOne(60000)) { throw 'Parent did not release producer.' }
} finally { $lease.Dispose(); $release.Dispose(); $ready.Dispose() }
'@
        $info = [Diagnostics.ProcessStartInfo]::new()
        $info.FileName = [IO.Path]::Combine([Environment]::SystemDirectory, 'WindowsPowerShell\v1.0\powershell.exe')
        $info.UseShellExecute = $false
        $info.CreateNoWindow = $true
        $info.EnvironmentVariables['HV_TEST_EVENT'] = $prefix
        $info.EnvironmentVariables['HV_TEST_STAGE'] = $Stage
        $info.Arguments = '-NoProfile -NonInteractive -EncodedCommand ' +
            [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($childSource))
        $child = [Diagnostics.Process]::new()
        $child.StartInfo = $info
        $started = $child.Start()
        if (!$started -or !$ready.WaitOne(15000)) { throw 'Child producer did not signal readiness.' }

        $proof = @(Get-CurrentStageIdentity $Root $Runner)
        if ($proof.Count -ne 1 -or $proof[0].Stage -cne [IO.Path]::GetFileName($Stage) -or
            $proof[0].WorkerSha256 -cne 'b1ec9193b5d93ad851c19a5a1eff545adfb8fd08f47393afcfa3f71c89fea6e2') {
            throw 'FAIL: proof did not identify exactly the live stage and reviewed worker'
        }
        'PASS: one live matching worker beside a historical mismatch identified'
        try {
            $extra = [IO.File]::Open([IO.Path]::Combine($Stage, 'ready.lease'), 'Open', 'Write', 'ReadWrite')
            $extra.Dispose()
            throw 'FAIL: producer admitted another writer'
        } catch [IO.IOException] {
            if (($_.Exception.GetBaseException().HResult -band 0xffff) -ne 32) { throw }
        }
        'PASS: live producer denies a second writer'
        Assert-Refused 'wrong checkout worker for live matching stage' $Root $OtherRunner
        $second = Open-StageReadiness (New-TestStage $Root $Runner)
        try { Assert-Refused 'two ready matching stages' $Root $Runner } finally { $second.Dispose() }

        [void]$release.Set()
        if (!$child.WaitForExit(15000) -or $child.ExitCode -ne 0) { throw 'Child producer did not exit cleanly.' }
    } finally {
        try {
            if ($started -and !$child.HasExited) {
                $child.Kill()
                if (!$child.WaitForExit(15000)) { throw 'Test producer did not terminate.' }
            }
        } finally {
            if ($child) { $child.Dispose() }
            if ($release) { $release.Dispose() }
            if ($ready) { $ready.Dispose() }
        }
    }
}

function Test-SessionStartup($Root, $Runner) {
    $utf8 = [Text.UTF8Encoding]::new($false,$true)
    $stage = New-TestStage $Root $Runner
    $descriptor = [IO.Path]::Combine($Root,'session.json')
    # These inert fixture descriptors use the real test process's PID. Start
    # with a stale match: neither a failed publication nor PID reuse alone
    # should satisfy the very predicate the ordinary launcher polls.
    $data = @{pid=$PID;port=1;token=('1' * 32);handoffSerial=0} | ConvertTo-Json -Compress
    $old = @{pid=$PID;port=1;token=('0' * 32);handoffSerial=0} | ConvertTo-Json -Compress
    [IO.File]::WriteAllText($descriptor,$old,$utf8)
    $previous = Read-SessionDescriptor $descriptor
    $marker = [IO.Path]::Combine($stage,'ready.lease')
    $pending = [IO.Path]::Combine($stage,'session.pending')
    [IO.File]::WriteAllBytes($marker,[byte[]]::new(0))
    try {
        $unexpected = Publish-SessionReadiness $stage $descriptor $data $true
        Close-SessionPublication $unexpected
        throw 'FAIL: startup accepted an occupied witness'
    } catch [IO.IOException] {
        if (($_.Exception.GetBaseException().HResult -band 0xffff) -notin 80,183) { throw }
    }
    if ((Read-SessionDescriptor $descriptor) -cne $previous -or
        (Test-SessionPublication $descriptor $PID $previous) -or [IO.File]::Exists($pending) -or
        ![IO.File]::Exists($marker)) { throw 'FAIL: witness failure altered history or announced readiness' }
    Assert-Refused 'failed startup with matching historical witness and recycled PID' $Root $Runner

    $stage = New-TestStage $Root $Runner
    $pending = [IO.Path]::Combine($stage,'session.pending')
    $marker = [IO.Path]::Combine($stage,'ready.lease')
    # Fail the final descriptor replacement, after the real writer opens.
    # Keep the old descriptor readable to the ordinary readiness predicate.
    $block = [IO.File]::Open($descriptor,'Open','Read','Read')
    try {
        try {
            $unexpected = Publish-SessionReadiness $stage $descriptor $data $true
            Close-SessionPublication $unexpected
            throw 'FAIL: startup replaced a deny-delete descriptor'
        } catch [IO.IOException] {
            if (($_.Exception.GetBaseException().HResult -band 0xffff) -ne 32) { throw }
        }
        if ((Read-SessionDescriptor $descriptor) -cne $previous -or
            (Test-SessionPublication $descriptor $PID $previous) -or [IO.File]::Exists($pending) -or
            ![IO.File]::Exists($marker)) { throw 'FAIL: descriptor failure leaked or published state' }
        Assert-Refused 'descriptor replacement failed; retained witness has no writer' $Root $Runner
    } finally { $block.Dispose() }

    $stage = New-TestStage $Root $Runner
    $pending = [IO.Path]::Combine($stage,'session.pending')
    # A live writer cannot qualify while the descriptor has not moved. The
    # exclusive stream also makes its contents unavailable to the observer.
    $block = [IO.File]::Open($pending,'CreateNew','Write','None')
    try {
        $lease = Open-StageReadiness $stage
        try { Assert-Refused 'live matching stage with commit still pending' $Root $Runner } finally { $lease.Dispose() }
    } finally { $block.Dispose() }

    $stage = New-TestStage $Root $Runner
    $pending = [IO.Path]::Combine($stage,'session.pending')
    [IO.File]::WriteAllText($pending,'preserve',$utf8)
    try {
        $unexpected = Publish-SessionReadiness $stage $descriptor $data $true
        Close-SessionPublication $unexpected
        throw 'FAIL: startup reused an unowned pending descriptor'
    } catch [IO.IOException] {
        if (($_.Exception.GetBaseException().HResult -band 0xffff) -notin 80,183) { throw }
    }
    if ([IO.File]::ReadAllText($pending) -cne 'preserve' -or (Read-SessionDescriptor $descriptor) -cne $previous -or
        [IO.File]::Exists([IO.Path]::Combine($stage,'ready.lease'))) {
        throw 'FAIL: failed descriptor creation changed unowned files'
    }
    'PASS: failed startup preserves unowned descriptor files'

    $stage = New-TestStage $Root $Runner
    $publication = Publish-SessionReadiness $stage $descriptor $data $true
    try {
        $proof = Get-CurrentStageIdentity $Root $Runner
        if (!(Test-SessionPublication $descriptor $PID $previous) -or
            (Test-SessionPublication $descriptor 0 $previous) -or
            (Read-SessionDescriptor $descriptor) -cne $data -or
            $proof.Stage -cne [IO.Path]::GetFileName($stage)) { throw 'FAIL: startup did not publish both signals' }
        'PASS: successful startup replaces stale descriptor and identifies only its live stage'
    } finally { Close-SessionPublication $publication }
    if ([IO.File]::Exists($descriptor) -or (Test-SessionPublication $descriptor $PID $previous)) {
        throw 'FAIL: closed session still announced ready'
    }
    Assert-Refused 'published session closed; retained stage cannot replay readiness' $Root $Runner

    # The original VM still publishes its descriptor without a lane witness.
    $stage = New-TestStage $Root $Runner
    $publication = Publish-SessionReadiness $stage $descriptor $data $false
    try {
        if (!(Test-SessionPublication $descriptor $PID $null) -or
            [IO.File]::Exists([IO.Path]::Combine($stage,'ready.lease'))) { throw 'FAIL: non-lane startup changed' }
        'PASS: fresh non-lane descriptor published without a witness'
    } finally { Close-SessionPublication $publication }
}

$temp = [IO.Path]::Combine([IO.Path]::GetTempPath(), 'hv-stage-identity-' + [guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path $temp
try {
    $current = [IO.Directory]::CreateDirectory([IO.Path]::Combine($temp, 'current', 'lab'))
    $other = [IO.Directory]::CreateDirectory([IO.Path]::Combine($temp, 'other', 'lab'))
    $runner = [IO.Path]::Combine($current.FullName, 'Invoke-HvQueue.ps1')
    $otherRunner = [IO.Path]::Combine($other.FullName, 'Invoke-HvQueue.ps1')
    [IO.File]::Copy([IO.Path]::Combine($lab, 'Invoke-HvQueue.ps1'), $runner)
    [IO.File]::Copy($runner, $otherRunner)
    [IO.File]::AppendAllText($otherRunner, "`n# Different checkout fixture.`n")

    $root = [IO.Path]::Combine($temp, 'session')
    $stage = New-TestStage $root $runner
    Assert-Refused 'uninstrumented checkout (worker only, no lease)' $root $runner
    $history = Open-StageReadiness (New-TestStage $root $otherRunner)
    $history.Dispose()
    Test-LiveProducer $stage $root $runner $otherRunner

    $marker = [IO.Path]::Combine($stage, 'ready.lease')
    $stagedWorker = [IO.Path]::Combine($stage, 'Invoke-HvQueue.ps1')
    if ([IO.FileInfo]::new($marker).Length -ne 0 -or
        [IO.File]::ReadAllText($stagedWorker) -cne [IO.File]::ReadAllText($runner)) {
        throw 'FAIL: producer exit did not preserve the empty lease and matching worker'
    }
    Assert-Refused 'producer exited; retained lease and matching worker cannot replay proof' $root $runner
    try {
        $reopened = Open-StageReadiness $stage
        if ($null -ne $reopened) { $reopened.Dispose() }
        throw 'FAIL: producer did not refuse an existing lease'
    } catch [IO.IOException] {
        if (($_.Exception.GetBaseException().HResult -band 0xffff) -notin 80,183) { throw }
    }
    'PASS: historical lease cannot be reacquired by the producer'

    $reader = [IO.File]::Open($marker, 'Open', 'Read', 'Read')
    try { Assert-Refused 'read-only holder of historical matching lease' $root $runner } finally { $reader.Dispose() }
    $exclusive = [IO.File]::Open($marker, 'Open', 'Read', 'None')
    try {
        Assert-Refused 'exclusive reader of historical matching lease' $root $runner
        $live = Open-StageReadiness (New-TestStage $root $runner)
        try { Assert-Refused 'one ready stage beside an inaccessible lease' $root $runner } finally { $live.Dispose() }
    } finally { $exclusive.Dispose() }

    $wrong = Open-StageReadiness (New-TestStage $root $otherRunner)
    try {
        Assert-Refused 'live wrong worker beside historical matching workers' $root $runner
        Assert-Refused 'live worker and checkout matching each other but not the reviewed digest' $root $otherRunner
    } finally { $wrong.Dispose() }
    Test-SessionStartup ([IO.Path]::Combine($temp,'startup')) $runner
} finally { [IO.Directory]::Delete($temp, $true) }
