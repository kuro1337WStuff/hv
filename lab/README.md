# Supervised Hyper-V regression queue

For scheduling across both dedicated lanes, see [task-board dispatch, holds and recovery](#re-read-a-task-board-after-every-result).

From the project root on the Windows lab host (Python 3.11+, PowerShell 5.1, Hyper-V), run:

```powershell
python lab/hv_queue.py run
```

The default order is `l2`, `cpuid`, `npt-write`, `npt-remap`, `npt-cross`. To rebuild and run just two profiles: `python lab/hv_queue.py run --rebuild npt-write npt-remap`. Without `--rebuild`, the runner uses the already-built ISOs in `build/`. It freezes the ordered profiles and SHA-256 digests for this batch before asking Windows to launch the visible Hyper-V worker. Building and staging use the caller's existing privileges. No change to UAC, no service and no per-test approval. An operator must still authorize elevation when Windows requires it.

### Windows-visible core and SMT inventory (host only)

From the project root, use an ordinary local x64 Windows shell with x64 Python 3.11+. The first command runs synthetic binary fixtures only; the second inventories the current host. Neither needs Hyper-V, elevation, affinity changes, a driver, or firmware access.

```powershell
python -m unittest -v lab.tests.win_core_inventory_test
python lab/win_core_inventory.py
```

Read `os_visible.cores` as one active physical core per row. Its `logical_processors` are the members of that core (SMT siblings when more than one). `G:N` means Windows processor group `G`, bit `N` of its 64-bit affinity mask; `core-G:N` uses the lowest member as a deterministic label, not a persistent hardware/APIC ID. Identical topology produces identical labels regardless of record order. `independent_active_processors` comes from [`GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)`](https://learn.microsoft.com/windows/win32/api/winbase/nf-winbase-getactiveprocessorcount). Exit zero means valid inventory and agreeing counts, **not** a target pass. Malformed data or a failed query exits nonzero with a diagnostic; if the two snapshots disagree, the JSON preserves both counts and exits nonzero. Recollect before drawing conclusions.

`target.observed_shape_matches` compares only the observed sibling sets to 16 cores with two threads each (32 total). SMT disabled can legitimately show 16 single-member rows. `cpu_model_registry` is the separate local Windows CPU 0 brand string, or `null` if unavailable. Neither a matching name nor matching counts qualifies a board, shows disabled cores, tests another SKU, or demonstrates that a hypervisor launched any core.

ABI: [`GetLogicalProcessorInformationEx(RelationProcessorCore)`](https://learn.microsoft.com/windows/win32/api/sysinfoapi/nf-sysinfoapi-getlogicalprocessorinformationex) returns one variable-sized [EX record](https://learn.microsoft.com/windows/win32/api/winnt/ns-winnt-system_logical_processor_information_ex) per active core across all groups. On x64 each [`PROCESSOR_RELATIONSHIP`](https://learn.microsoft.com/windows/win32/api/winnt/ns-winnt-processor_relationship) for a core has `GroupCount == 1`; the first [`GROUP_AFFINITY`](https://learn.microsoft.com/windows/win32/api/winnt/ns-winnt-group_affinity) begins at byte 32 of the EX record. The next record starts at the current record's declared `Size`, not a fixed stride.

To map unified L3 sharing on the same local host, run `python lab/win_core_inventory.py --l3`. It adds the Windows release and `major.minor.build` under `windows` alongside `cpu_model_registry`. Each `unified_l3.caches` row gives the reported size in **bytes**, the `G:N` members, and the core labels that intersect them. The extra `RelationCache` query walks each record by `Size`. On x64 the [`CACHE_RELATIONSHIP`](https://learn.microsoft.com/windows/win32/api/winnt/ns-winnt-cache_relationship) mask starts at EX byte 40: a zero count in the legacy Windows 10 reserved word selects one `GroupMask`; a nonzero `GroupCount` selects that many 16-byte group affinities.

`unified_l3.coverage` is `exact` only when every measured LP belongs to exactly one unified L3, no cache names an unknown LP, and siblings stay together. `absent` means no unified L3 was reported; `inconsistent` retains the memberships and lists missing, unknown, or overlapping LPs and split sibling cores. Both exit nonzero, as does disagreement with the independent active count. A malformed record or failed query instead exits nonzero with a diagnostic, not an empty topology. `target.core_shape_with_exact_l3_coverage` checks the original 16×2 core shape plus those coverage and count checks, not a specific cache size or CPU identity. Cache rows are not CCD IDs, scheduler evidence, benchmarks, or qualification of this or another SKU.

### Host-only legacy PIC discovery and IRQ0 regression

Use the root of the **combined source tree** in an ordinary x64 MSVC Developer Command Prompt. Build and run both controls there; each compile and each program must exit zero. These commands need no firmware build, queue session, VM or port I/O.

```bat
cl /nologo /std:c11 /W4 /WX /TC lab\tests\pic8259_test.c /Fo"%TEMP%\pic8259_test.obj" /Fe"%TEMP%\pic8259_test.exe" && "%TEMP%\pic8259_test.exe"
cl /nologo /std:c11 /W4 /WX /TC lab\tests\pit_pic_test.c /Fo"%TEMP%\pit_pic_test.obj" /Fe"%TEMP%\pit_pic_test.exe" && "%TEMP%\pit_pic_test.exe"
```

- **Discovery/init (`pic8259_test`).** The pinned [`probe_8259A()` and `init_8259A(0)` in Linux v6.18.60](https://github.com/gregkh/linux/blob/v6.18.60/arch/x86/kernel/i8259.c) use scalar bytes (hex here): `a1←ff`, `21←fb`, then read `fb` at `21`. Initialize master `20←11`, `21←30,04,01` and slave `a0←11`, `a1←38,02,01`. Each chip consumes only its own words. ICW1 clears its mask; only after ICW4 does its data port become an IMR again. Both chips must finish for pair readiness; cached masks can then be restored and read back. In the interleaved control the slave finishes first and accepts `38` as a mask while the master still expects ICW4. Wrong words, mid-sequence restarts and unsupported writes must leave the **whole pair** byte-for-byte unchanged; refused reads leave the output sentinel untouched. This tests fixed vector bases, cascade wiring and 8086 normal-EOI *setup*, not command-port status, polling, EOI service, arbitration or cascade/SCI delivery.
- **IRQ0 service (`pit_pic_test`).** `pit_pic.h` shares the master's ports and vector, not the pair's programming state. Check the consumer's vector against wire value `30`, not another model constant. A one-shot expiring while masked must remain pending; unmask with `21←fe`, take it once, then mask with `21←ff` before acknowledging with `20←60`. An edge latched during service survives the EOI. The consumer must refuse early takes, premature/duplicate EOIs, `61` and non-specific `20` EOIs, other IRQ masks and slave accesses without changing state. These controls distinguish an IRQ0 service regression from a PIC setup regression; neither host process delivers an interrupt to a vCPU.

[065](../journal/runs/065-l1-pit-irq0.md) observed two IRQ0 entries and EOIs and one wake on the earlier guest fixture, **not** a guest ICW sequence on this tree. [068](https://github.com/kuro1337WStuff/hv/blob/3dc7e8b23103bfddcf568c708104f88660d5920a/journal/trials/068.md) remains FAIL/ATTENTION: the final refusal receipts for both chip states, cookie and entry count were not visible. [072](https://github.com/kuro1337WStuff/hv/blob/3dc7e8b23103bfddcf568c708104f88660d5920a/journal/trials/072.md) isolated label width, not row exhaustion, using host-supplied values. None of these host controls supplies the missing guest receipts or shows Linux selected a PIC.

### Shared input pulses for PIT channels 0 and 2

[`PitShared`](../firmware/pit_shared.h) accepts the same absolute count of completed PIT pulses for both channels. Reads and writes never advance time. The [host test](tests/pit_shared_test.c) interleaves channel-2 calibration reads and gate changes with channel-0 clockevents; it does not execute Linux, measure TSC frequency or inject interrupts. From the same Developer Command Prompt:

```bat
cl /nologo /O2 /std:c11 /W4 /WX /TC lab\tests\pit_shared_test.c /Fo"%TEMP%\pit_shared_test.obj" /Fe"%TEMP%\pit_shared_test.exe" && "%TEMP%\pit_shared_test.exe"
```

### Host-only PIT fallback calibration (097)

[`pit_fallback_test.c`](tests/pit_fallback_test.c) replays the legacy-PIC-present branch of [`pit_calibrate_tsc()` in Linux v6.18.60](https://github.com/gregkh/linux/blob/v6.18.60/arch/x86/kernel/tsc.c#L414-L491), not the outer retry/reference selection. From this worktree's root in an x64 MSVC Developer Command Prompt:

```bat
cl /nologo /std:c11 /W4 /WX /TC lab\tests\pit_fallback_test.c /Fo"%TEMP%\pit_fallback_test.obj" /Fe"%TEMP%\pit_fallback_test.exe" && "%TEMP%\pit_fallback_test.exe"
```

At a fixed 2.5 GHz **virtual** rate, 10 µs polls observe 1,000 and 5,000 low samples for the 11,931 and 59,659 reloads. The sample at 10 or 50 ms is still low; the next port read is high and supplies no cycle sample. Both estimates should be 2,500,000 virtual kHz. A missing poll (999 lows) and a gap strictly over ten times the minimum should be Linux rejections. A frozen same-time sequence exhausts the harness budget with OUT2 still low: Linux's loop has **not** returned a failure or a rate. These are synthetic schedules, not measurements of a host or observations from an OS.

### Host-only guest UEFI memory-map handoff (125)

From the worktree root in an x64 MSVC Developer Command Prompt:

```bat
cl /nologo /std:c11 /W4 /WX /TC lab\tests\guest_uefi_map_test.c /Fo"%TEMP%\guest_uefi_map_test.obj" /Fe"%TEMP%\guest_uefi_map_test.exe" && "%TEMP%\guest_uefi_map_test.exe"
```

The [fixture](../firmware/guest_uefi_map.h) follows UEFI 2.10 [§7.2.3 GetMemoryMap](https://uefi.org/specs/UEFI/2.10/07_Services_Boot_Services.html#efi-boot-services-getmemorymap) and [§7.4.6 ExitBootServices](https://uefi.org/specs/UEFI/2.10/07_Services_Boot_Services.html#efi-boot-services-exitbootservices). The [test](tests/guest_uefi_map_test.c) checks the required byte count without a partial copy. Allocating guest page `0x101000` as loader data splits four conventional pages into one allocated and three conventional pages; the map grows from 96 to 144 bytes. The old key must fail with `EFI_INVALID_PARAMETER`. Retrying with the key from the new map must succeed once. After success, “unavailable” is a harness observation, **not** an EFI status.

Only these five synthetic guest pages, one fixed allocation, one assumed valid loader, and the key handoff are modeled. The 48-byte descriptor stride and two key values belong to this fixture, not all firmware. A failed exit can partially shut down real boot services; the retry uses only `GetMemoryMap` and `ExitBootServices`, not console, protocols or devices. No system table, events, runtime services, outer firmware map, VM, or Windows boot is exercised.

### One launch for a supervised session

From the checkout, run **once** and approve the Windows elevation prompt:

```powershell
powershell.exe -NoProfile -File .\lab\Start-HvLabSession.ps1
```

Leave the visible **LAB SESSION READY** window open. The ordinary build terminal can now submit any number of finite batches without another UAC prompt or a pasted worker command:

```powershell
python lab/hv_queue.py run --session --rebuild vcpu-mxcsr
```

For all five standalone profiles in dependency order, use `python lab/hv_queue.py run --session --suite standalone --rebuild`. The batch runs handoff (including its COM1 capture), L2 entry, CPUID resume, write protection and backing-frame remap. It stops at the first missing or unexpected result. Use the printed run ID with `watch` or `stop`. Omit `--rebuild` to use existing images. Explicit profiles and `--suite` are alternatives; the default queue is unchanged.

`--session` never falls back to elevation. The foreground process accepts a run ID and bounded JSON data on loopback, not commands or scripts. Anyone running as your account can submit lab batches while it is open. In ordinary checkouts it pins the `HV-SVM-L1` VM ID; the existing media digest, offline-network, checkpoint and console checks still apply. Guest images and the declarative `hv-profiles.json` can change between batches. Review new screenshots before adding PASS digests. The privileged worker, console reader and fixed COM1 collector are hash-checked and copied at launch; changes to that code require a fresh session. Build tools, the repo and the submitting terminal get no administrator token.

**Revoke:** press **Q** in the session window or close it. An active batch can also be stopped using the run-specific `stop` command. A cooperative STOP ends the session; it does not undo an issued Hyper-V operation. Closing the session installs no task, service, or logon trigger. Per-batch events and PNGs arrive in `build/runs/<id>/`; the protected worker copies and recovery artifacts are under the checkout-specific `HvLabSession-*` directory in native Program Files. Do not share its session descriptor.

For a manual handoff instead, `run --stage-only` builds/stages without starting a worker and prints a command for an existing administrator console. Staging is not a guest PASS.

### Two dedicated DVD-only lanes

The checkout containing the **reviewed runner** determines its VM. There is no lane argument, environment override, VM-name parameter or pool that assigns an available VM:

| Dedicated runner checkout directory name | Only target |
| --- | --- |
| `hv-lane-a` | `HV-SVM-L1-A` |
| `hv-lane-b` | `HV-SVM-L1-B` |

The original lab flow elsewhere still targets `HV-SVM-L1`; never use the original VM provisioner to create lanes. Do not point either lane at the primary VM, its disk, COM pipe, or checkpoints.

1. **Deploy trusted host code first**, as an ordinary user. Make the two sibling dedicated worktrees from the **same reviewed runner commit** (for example, `git worktree add --detach ..\hv-lane-a REVIEWED_COMMIT`, then repeat for `hv-lane-b` with the same commit). Check both revisions and host scripts before elevation. Use real local directories, not junctions, links, or aliases. Keep the runner scripts under operator control; never check a topic branch out in a runner or run feature-branch host scripts elevated. Create `build\runs` and `lab\media` in each runner as the ordinary user before starting its session.
2. **Provision once per lane.** In a 64-bit Windows PowerShell 5.1 administrator console, enter the trusted `hv-lane-a` worktree and run `& "$PSHOME\powershell.exe" -NoProfile -File .\lab\New-HvLane.ps1`; repeat from `hv-lane-b`. The provisioner accepts no arguments and requires at least 8 GiB available physical memory and 16 GiB free on the storage volume. Its VM store is `HvLabMachine-<VM name>` under native Program Files, not in either checkout. It refuses an existing VM or store. Each new Generation 2 VM stays Off with fixed 2 GiB RAM, two nested vCPUs, one empty boot DVD, no VHD, no NIC or COM connection, Secure Boot off, standard (not automatic) checkpoints, and no automatic start. A provisioning error can leave partial state for manual inspection.
3. **Build in the feature worktree**, without elevation (e.g. `firmware\build.cmd PROFILE`). Copy **only** its selected firmware ISO(s) to the chosen runner's `build\` and the reviewed, data-only `lab\hv-profiles.json` to that runner's `lab\`. Keep the manifest's ISO stems intact; compare SHA-256 on both sides of the transfer. Do not copy host scripts, Python, executables, modules, build tools, directories of artifacts, or links. Do not accept a topic-provided PASS hash as proof: leave new screens unapproved until independently reviewed.
4. **Run in that runner**, not the feature tree. Launch `powershell.exe -NoProfile -File .\lab\Start-HvLabSession.ps1`, approve UAC, and check the visible VM name and ID. Submit `python lab/hv_queue.py run --session PROFILE` in the same checkout, **without `--rebuild`**. Use explicit DVD firmware profiles; these VMs have no host COM connection for `l1-handoff`. For parallel work, open a separate supervised session and ordinary terminal in the other dedicated checkout. Do not update inputs while a batch using them is active.
5. **Watch or stop the exact lane and run.** In another ordinary terminal in the submitting runner's root, use `python lab/hv_queue.py watch RUN_ID` or `python lab/hv_queue.py stop RUN_ID` with the ID printed by that launcher. Q in the matching session window revokes that lane; detaching a `watch` does not stop the run. Keep the lane name, VM ID and run ID with its ISO digest, events and screenshots; the other checkout has its own `build\runs\`. STOP is cooperative, not a revert. Never automatically remove or restore VMs, stores, checkpoints, or run evidence after success, failure or STOP. Inspect them before taking any manual recovery action.

Before each checkpoint, power change, DVD mount or boot-order change, the worker reads the dedicated VM's current settings. Automatic start must be `Nothing`, automatic checkpoints disabled, checkpoint type `Standard`, and Secure Boot `Off`. Drift stops the queue with `ATTENTION` before that action; the worker does not repair the VM. To exercise this guard without Hyper-V or elevation, run `powershell.exe -NoProfile -File .\lab\tests\lane_preflight_test.ps1` from the checkout root.

**Inspect the current dedicated stage without contacting the broker.** From a reviewed supervisor checkout in ordinary 64-bit Windows PowerShell 5.1, run `.\lab\Get-HvStageIdentity.ps1 -LaneRoot C:\path\to\hv-lane-b` (or `hv-lane-a`). A successful result identifies a single stage and the reviewed worker digest. The broker keeps a write handle on that stage's `ready.lease`; after it closes, the file remains but is not evidence of readiness. A matching historical copy is never a substitute for the selected worker. An older session with no witness is unverified; preserve it rather than inventing or deleting a marker.

For a new dedicated session, the broker finishes writing its descriptor in the protected stage, acquires the witness, then renames the completed descriptor to `session.json`. That rename is the publication point. Until then, the observer refuses a live stage with `session.pending`; it checks only file attributes, not the pending or published credentials. The ordinary launcher waits for a changed descriptor with its child's PID, not a matching PID left over from an earlier launch. If publication fails, the broker closes its own witness before removing its pending descriptor. A crash or blocked cleanup can leave descriptor files behind: treat them as private, not as evidence of a running session. Do not delete stages to recover readiness.

This is a point-in-time **observation**, not permission to deploy, mount media, bypass a lane hold, or assume the same session will handle a later request. It does not read session credentials or authenticate a PID. It trusts the broker's protected directory and refuses ambiguous or inaccessible markers, more than 128 candidate stages, and a mismatch in either the staged worker or the runner checkout. Use the reviewed observer that understands pending publication; older launchers and observers do not gain this protection retroactively.

Run `powershell.exe -NoProfile -NonInteractive -File lab\tests\stage_identity_test.ps1` from the checkout root. In addition to the original trial 079 controls, it exercises the broker's publication, the caller's readiness decision and teardown with matching workers and real file conflicts in temporary directories. It does not start an elevated broker, validate a live protected root, contact a listener, operate a VM or release either lane.

### Test profiles

`l1-handoff` tests L1 without an L2 guest. Run `python lab/hv_queue.py run --session --rebuild l1-handoff`. Use a fresh session that supports serial capture, or omit `--session` to use a one-batch worker. The VM must already expose COM1 at `\\.\pipe\HV-SVM-L1-COM1`; close any serial viewer first. The worker requests evidence after boot and saves `l1-handoff.serial` in the run directory. Acceptance requires the ordered handoff, memory ownership, expected allocation failure and halt markers **and** the matching framebuffer terminal. A screenshot alone is not a serial test.

`l1-l2` launches one fixed 16-bit guest **after** `ExitBootServices`, from the standalone L1's private stack. Run `python lab/hv_queue.py run --session --rebuild l1-l2`. Only the guest code page is NPT-mapped; it executes `MOV AX,0x4C32; HLT`. The framebuffer must report the observed HLT exit (`0x78`), RIP (`0x1003`) and RAX (`0x4C32`). Unlike `l1-handoff`, this profile does not require a COM1 handshake. Unlike the original `l2` profile, it cannot allocate pages or print using UEFI boot services after handoff.

`l1-cpuid` tests a second entry into the same L2 after `ExitBootServices`. Run `python lab/hv_queue.py run --session --rebuild l1-cpuid`. L1 handles only the fixture's synthetic CPUID request (`EAX=0x400000FE`, `ECX=7`). L2 compares all four response registers after resume and branches to different halts for success and failure. The framebuffer must show the CPUID exit (`0x72`), the resume address, two entries, and the success HLT (`0x78`) with `RAX=0x50415353`.

`l1-npt-write` tests the same L2 store twice after `ExitBootServices`. Run `python lab/hv_queue.py run --session --rebuild l1-npt-write`. With GPA `0x2000` writable, the store must change the backing byte from `0x5A` to `0xA5` and reach `HLT`. L1 then resets the byte, clears the leaf write bit and requests a local TLB flush on re-entry. The second store must exit at the store instruction with a final-write protection fault for GPA `0x2000`, leaving `0x5A` intact. The profile stops at the fault; it does not emulate the store.

`l1-npt-remap` tests a backing-frame change after `ExitBootServices`. Run `python lab/hv_queue.py run --session --rebuild l1-npt-remap`. On the same vCPU, the same load from GPA `0x2000` must return `0xA11A` from frame A, then `0xB22B` from frame B after remapping and requesting a full local translation flush. Both entries must reach the same `HLT`; both backing words must remain intact. The mapping stays read-only throughout.

`l1-package` runs four dependent checks after firmware handoff in one boot. Use `python lab/hv_queue.py run --session --rebuild l1-package`. A fresh VMCB and guest mappings separate each fixture; the first mismatch prevents the next fixture from running. Require all four, not just the title:

1. A 64-bit guest changes its integer and legacy FP state at `VMMCALL`. After resume, guest SSE and x87 calculations must return 1 and 12; L1's independent register bank must survive both exits. No AVX/XSAVE claim.
2. An unsupported CPUID request must stop at the CPUID instruction on the first entry. A separate `UD2` guest must take one injected fault, return past `UD2`, and reach a success HLT.
3. An L2 store at `0x2FFF` must succeed; the next at `0x3000` must fault without modifying the adjacent private backing. L1 checks the entire four-level NPT against the allowed frame list before and after entry.
4. Check deferred and priority-masked interrupts, virtual timer expiry following HLT, and byte-at-a-time output from L2 to a virtual UART. The captured guest marker must accompany the independent post-wake guest halt; there is no host port passthrough. This fixture's interrupt acknowledgement is not a PC interrupt-controller implementation.

`l1-linux-admission` stages the pinned Linux payload and checks the first attempted kernel **instruction fetch**, not a boot. Prepare `build/linux-inputs/` once with `python lab/linux/prepare.py` (requires the checksum-pinned local Alpine 3.24.4 virt ISO used by lab provisioning). The packer rechecks the three input hashes. Run `python lab/hv_queue.py run --session --rebuild l1-linux-admission`. Reloading or seating Linux directly in Hyper-V would bypass the code under test.

Before `ExitBootServices`, L1 reads the fixed guest files from its own boot volume into private staging. After handoff it builds the Linux 2.15 `boot_params`, e820, GDT and identity map in owned RAM. L1's separate NPT must map that RAM and no private pages, leave the reserved hole absent, and deny execution. The only acceptable guest exit is a present final instruction-fetch NPF at kernel entry `0x1000200`, with `RSI=0x10000`. Do not lift NX to turn this admission check into an OS runner.

The admission image first measures fault-address precision using three fresh VMCBs and nonidentity guest mappings. Two instruction offsets share a page, then the first offset is used on a different page. L1 independently walks the guest tables; raw `EXITINFO2` must consistently be either the exact walked byte GPA or exactly its 4 KiB page base in all three arms. A different offset on the right page is not accepted. The Linux arm must match the independently established mode, retain its exact entry RIP and satisfy the original fault-cause, register, full-NPT and guard checks. This is a nested configuration measurement, not a universal AMD reporting rule.

`l1-linux-inputs` tests the post-handoff integrity gate with the same pinned media. Run `python lab/hv_queue.py run --session --rebuild l1-linux-inputs`. L1 flips one bit in its private staging buffer for the kernel, initramfs and command line in turn, restoring that byte before touching the next input. Each trial calls the real SHA-256 admission path and must identify the modified file (IDs 1, 2, 3) without issuing `VMRUN`. The restored set (ID 0) must reach the three control fetches and the Linux-entry fetch, all NX, for exactly four attempts. Neither the ISO nor any host file is modified by L1. This checks staged data against pins compiled into this firmware; it does not attest the firmware itself.

For a host-only layout check, use an x64 Visual Studio Developer Command Prompt at the repository root:

```cmd
cl /nologo /std:c11 /O2 /W4 /WX lab\tests\linux_layout_test.c /Fobuild\linux_layout_test.obj /Febuild\linux_layout_test.exe
build\linux_layout_test.exe build\linux-inputs\vmlinuz-virt build\linux-inputs\initramfs.cpio build\linux-inputs\cmdline.bin
```

`npt-cross` uses its own two-processor L1 payload. Rebuild with `firmware\build.cmd npt-cross`, then run `python lab/hv_queue.py run npt-cross`. The builder caches hash-pinned Limine v8 UEFI loader/header and Zig 0.13 tools under `build/tools`; first build needs network access. Other profiles remain UEFI applications. The cross test is read-only with respect to guest disks: it exits boot services via Limine, measures both local APIC IDs, runs four fixed L2 loads and halts on its evidence screen. Do not install this lab image as a host bootloader.

`npt-broadcast` is opt-in. It uses the two-processor L1 harness to request one synchronous Hyper-V flush of nested GPA translations for the selected NPT root across all processors. This is not a native `INVLPGB` test. Run `python lab/hv_queue.py run --rebuild npt-broadcast` to build and boot it. Only the [reviewed PASS screen](../journal/images/npt-broadcast-017.png) is accepted; the worker stops for inspection on any other display.

`forensic-write` is opt-in: `python lab/hv_queue.py run --rebuild forensic-write`. The same 16-bit store runs against a writable mapping and then a read-only mapping. The report uses the VMCB fault fields, decodes the destination and immediate byte from the fixed guest code at the saved RIP, and samples the owned backing byte before and after the protected attempt. It does not resume or emulate the faulting store.

`l1-cpu-contract`: run `python lab/hv_queue.py run --session --rebuild l1-cpu-contract` for the standalone, single-vCPU CPUID and state-switch fixture. L2 must see an explicitly virtual processor, use SSE2 across a CPUID exit, and finish at a denied MSR read. L1 checks its own state independently and must not resume the denied operation. The ISO contains the fixed guest only; it neither boots Linux nor changes run 045's NX policy.

`l1-serial-8250`: run `python lab/hv_queue.py run --session --rebuild l1-serial-8250`. A fixed L2 initializes a non-FIFO COM1, polls its line status, transmits `L2:8250` followed by LF, and halts. L1 checks the captured bytes separately from guest memory, then refuses one COM2 write. All port I/O terminates in the VM; no host serial device, Linux image, or serial IRQ is involved.

The opt-in standalone device contracts use `python lab/hv_queue.py run --session --rebuild PROFILE`. `l1-pit-irq0` checks two PIT channel-0 expirations on a controlled virtual timeline, actual L2 IRQ0 entry/return, a HLT wake and a master PIC specific EOI; it does not calibrate the TSC. `l1-console-tx` checks two phases of serial output, including CR/LF, and stops at overflow rather than truncating. `l1-power-s5` distinguishes a fixed guest's virtual power request from printed text and HLT; it never powers off the Windows machine. These are interface fixtures, not Linux boot results.

`l1-entry-guard` changes actual entry RSI and CR3 in separate arms and refuses each before VMRUN. Restored pinned media remains non-executable: one denied fetch, not a Linux boot. `l1-cpu-floor` tests virtual CPUID answers, CX8, CMOV and legacy SSE2 around repeated exits; the final MSR read is refused. `l1-page-pair` distinguishes a guest-linear fetch fault from a nested GPA fault using nonidentity mappings and unchanged private guards. Each is an opt-in standalone profile with the same build and queue commands above.

`l1-page-transition`: build with `firmware\build.cmd l1-page-transition`, or run `python lab/hv_queue.py run --session --rebuild l1-page-transition`. On one stopped BSP-owned vCPU, L1 seals one 4 KiB page at `0x8000`: writable/NX during fixed guest setup, then read-only/NX in both the guest and nested tables. With L2 stopped, L1 checks both complete table trees, all 4,096 candidate bytes (six fixed instruction bytes followed by zeros), the setup code and a private guard. Only the matching page with no writable nested alias becomes read-only/executable; the next entry requests a full local TLB flush (`1`).

All three setup HLTs must stop at `0x1011`. Arm A must report zero writers and bad bytes, gate `0`, then HLT (`0x78`) at `0x8005` with RAX `0x5452414E`. A change to the last byte in B must report one bad byte, no writers and gate `1`; a writable nested alias at `0x1FF000` in C must report one writer, no bad bytes and gate `2`. Neither rejected page executes. Require `A/B/C RUNS` = `2/1/1` and `ENTRIES` = `4`. This fixed fixture requires nested paging, NX and host NXE; it admits no external code, arbitrary entry point or Linux image, and does not test multi-vCPU invalidation or concurrent writers.

`l1-range-audit`: with the pinned inputs in `build/linux-inputs`, build `firmware\build.cmd l1-range-audit`, or run `python lab/hv_queue.py run --session --rebuild l1-range-audit`. Both the packer and L1 check all three compiled SHA-256 pins. L1 makes the copy itself from the unmodified loaded image; it does not simulate earlier Linux writes, set a guest entry point, enable SVM, or grant execute permission. On this stopped BSP, L1 checks the complete four-level NPT in its reservation, including every absent slot, canonical frame ownership, and writable aliases to either protected span. Both spans stay RO/NX through their entire last frame; only the file-backed `0x400` bytes of that frame are evidence about the image. The preceding byte and `0xc00` bytes after the range are separate copy guards, not authenticated kernel bytes.

Preboot oracle (hex, not a recorded result): `L1 RANGE AUDIT PASS`, `COPY AUDITED: STILL NX`, `HV:HANDOFF`; `FILE OFFSET=4000`, `SRC GPA=1000000`, `DST GPA=29c3000`, `DST END=35cf400` (exclusive), `SRC RO NX=DST RO NX=c0d` pages, `COPY BYTES=c0c400`, `LAST BYTES=400`, `GOOD GATE=0`. Flipping only the last copied byte gives `BYTE DIFF=1`, `BYTE GATE=1`. A writable/NX alias of the last frame gives `ALIAS WRITE=1`, `ALIAS GATE=2`. Destinations crossing RAM end and wrapping address space give `BOUNDS GATE=WRAP GATE=3` before traversal. After restoring the byte and leaf, require `REST GATE=WRITERS=ENTRIES=MARKER=0`. This is a snapshot of a never-run root, not a Linux boot, TLB flush, concurrent writer exclusion, or an executable allowlist.

`l1-cr4-guard` arms the hardware CR4 write intercept before either fixed guest enters. A read must reach HLT. An attempt to set OSXSAVE must stop on MOV CR4 with the original CR4 and untouched progress byte; it is not forwarded or resumed. This is a dedicated denial fixture, not a general control-register emulator.

Offline admission checks do not boot the VM. With an archived `l1-entry-guard` ISO, use `build\tools\packenv\Scripts\python.exe lab\linux\verify_media.py PATH_TO_FROZEN_ISO SHA256_FROM_MOUNT_EVENT` to read its EFI boot catalog and independently hash the three FAT members. `python lab/linux/inspect_exec_layout.py build/linux-inputs/vmlinuz-virt` shows why the pinned ELF load flags alone are insufficient for executable mappings. The host `exit_budget_test.c` covers bounded return handling; this is not a watchdog able to interrupt a guest that never exits.

`vcpu-gpr` is opt-in: `python lab/hv_queue.py run --rebuild vcpu-gpr`. A fixed 64-bit L2 guest checks its integer registers after each of two CPUID intercepts. Expected path: CPUID, CPUID, success HLT. This test does not cover interrupt delivery, extended state, or vCPU migration.

`vcpu-xmm` is opt-in: from the repository root, run `python lab/hv_queue.py run --rebuild vcpu-xmm` to build and run `build/hv-vcpu-xmm.iso`. To build without starting a VM, run `firmware\build.cmd vcpu-xmm`. The profile checks 128-bit XMM register contents across nested guest exits and resumes. It does not test MXCSR, x87/MMX, AVX upper lanes, or general XSAVE state.

`vcpu-mxcsr` is opt-in. From the repository root, run `python lab/hv_queue.py run --rebuild vcpu-mxcsr` to build and run `build/hv-vcpu-mxcsr.iso`, or `firmware\build.cmd vcpu-mxcsr` to build it without starting a VM. This profile checks whether the L2 guest retains its SSE control/status register (MXCSR) across nested exits and resumes. It does not test XMM register contents, floating-point arithmetic correctness, or full XSAVE state.

`vcpu-fpstate` combines the integer-register and legacy floating-point switch in one 64-bit guest. Run `python lab/hv_queue.py run --session --rebuild vcpu-fpstate` in the foreground lab session. The same guest changes its x87 stack and rounding controls, integer registers and XMM values before a VMMCALL, then uses the restored state after resume. The host checks its own state after each exit. This is a legacy FXSAVE/FXRSTOR path, not AVX/XSAVE or vCPU migration.

`irq-wake` is opt-in: `python lab/hv_queue.py run --rebuild irq-wake`. L1 intercepts the fixed guest's HLT and queues one maskable virtual interrupt. The handler records its return RIP and count; the resumed guest separately records forward progress. This exercises virtual HLT emulation without a host timer, device IRQ, or physical sleep.

`irq-shadow` is opt-in: `python lab/hv_queue.py run --rebuild irq-shadow`. The guest makes a checkpoint while interrupts are disabled. L1 verifies the original virtual IRQ is still pending there and resumes without queuing it again. The handler records which instruction had just completed and its return address; both must match the end of the `STI` shadow.

`irq-priority` is opt-in: `python lab/hv_queue.py run --rebuild irq-priority`. Before guest entry, L1 queues vector `0x41` once at virtual IRQ priority 4 with `V_TPR=4`. With interrupts enabled, no interrupt shadow, and `V_IGN_TPR=0`, equal priority must block delivery: at the guest's `VMMCALL` checkpoint the IRQ must still be pending and the handler count must be zero. L1 then lowers only `V_TPR` to 3 and resumes. The same IRQ must be delivered exactly once, without being queued again.

`exception-ud` is opt-in: `python lab/hv_queue.py run --rebuild exception-ud` builds and boots `build/hv-exception-ud.iso`. The fixed 64-bit L2 guest executes `UD2`, which raises the invalid-opcode fault (`#UD`, vector 6). Unlike a trap, this fault identifies the `UD2` instruction itself, not the instruction after it. Inspect the guest evidence screen; an unreviewed screen does not advance the queue.

**Watch:** leave the launcher open for live transitions, or use `python lab/hv_queue.py watch RUN_ID` in another terminal. The launcher prints the actual ID and commands at startup. Before every boot, `NEXT` names the profile and gives you five seconds to veto it; no response means continue. VMConnect can also display `HV-SVM-L1`; view only while the queue owns it.

**Stop:** press **Q** in the worker window, **Ctrl+C** in the launcher, or run `python lab/hv_queue.py stop RUN_ID` from another terminal. Stop is cooperative: an already-issued build or Hyper-V operation may finish. The runner does not undo that operation, restore snapshots automatically or power off the VM merely because you stopped watching. Interrupting a separate `watch` window detaches the observer; use `stop` to stop the queue. Start a fresh batch after inspection; a stopped run is not resumed implicitly.

**Results:** `build/runs/RUN_ID/` contains the frozen queue (`queue.json`), timestamped events (`events.jsonl`) and captured console evidence. An exact match to a reviewed guest success screen is required to advance. A reviewed skip is not a pass; an unrecognized screen times out for operator review. A booted VM, an exit code from the launcher, or text printed by a build is not guest evidence. New guest output requires independent review before changing the expected result. Exit codes: `0` complete, `2` stopped, `3` attention, `1` runner error.

Only known firmware profiles in the isolated project VM are eligible. Each batch preserves the starting state with a checkpoint; inspect checkpoint storage as it accumulates. No host boot changes, networking changes, background source edits, automatic commits, or retries of failures. Review evidence before adding it to the [journal](../journal/README.md). Raw batch artifacts stay local.

### W01: pinned guest firmware reset to SEC (bench A)

`l1-w01` is an opt-in standalone AMD SVM/NPT profile, not a Windows installer. Build the split OVMF image with `bash lab/w01/build.sh` in the pinned CI container, or download `w01-split-flash` from the corresponding successful project workflow into `build/w01`. Use the CODE and VARS SHA-256 values **from that build’s log**, not values first calculated from the download.

1. Run `python firmware/w01_media.py prepare --inputs build/w01 --code-sha256 CODE_DIGEST --vars-sha256 VARS_DIGEST`. Compare the reported SEC GPA to `SecMain`’s `EntryPoint` in the same build’s `SECFV.Fv.map`.
2. Run `firmware\build.cmd l1-w01`. Before moving A, verify the EFI boot catalog selects the ESP containing the built `EFI/BOOT/BOOTX64.EFI` and the two exact flash files. Record the source revision, complete ISO hash, both guest flash hashes, and the independently derived SEC entry in the reservation.
3. A valid observation must join the reset instruction fetch at `0xfffffff0` to a fetch at that SEC entry **in the same cold L2 run**. On a stop, record the first missing transition and the raw exit fields. Neither a host parser result nor the firmware address printed as an expected value is execution evidence. The profile has no automatic PASS screen; inspect the retained console. No Windows, storage or PEI/DXE claim follows from W01 alone.

### W02: PEI permanent memory followed by DXE (bench A)

`l1-w02` starts from cold reset, not a saved SEC register image. Its guest OVMF contains two lab-only markers. The PEI marker identifies an allocated page; the DXE marker originates inside DXE Core, not DXE IPL. Both must come from their respective pinned module sites in one attempt. The page's first and last words must match the guest's writes, and its backing must belong to that guest. A guessed port value, a previous run's screen, or the host's firmware map cannot establish W02.

Build the `w02-split-flash` artifact with `lab/w02/build.sh` in the pinned CI container. This instrumented single-BSP firmware publishes only 32–128 MiB as permanent memory and omits DXE drivers and storage. The original EDK II PEI allocator, DXE IPL loader and DXE Core are the components under test. Extract to `build/w02`; use the CODE and VARS SHA-256 values from the corresponding hosted build **log**, not just the download:

```bat
python firmware\w02_media.py prepare --inputs build\w02 --code-sha256 CODE_DIGEST --vars-sha256 VARS_DIGEST
firmware\build.cmd l1-w02
```

`build/w02-preboot-map.json` records the section digests and marker RVAs in that flash image. From an x64 MSVC developer prompt, run the host control against those same bytes:

```bat
cl /nologo /TC /std:c11 /O1 /Ibuild\tools\edk2\MdePkg\Include\X64 /Ibuild\tools\edk2\MdePkg\Include /Fobuild\w02_evidence_test.obj /Febuild\w02_evidence_test.exe lab\tests\w02_evidence_test.c
build\w02_evidence_test.exe build\w02\W02CODE.FD
```

Keep W01 and W02 flash and generated pins in their separate build directories. Build with `firmware\build.cmd l1-w02` after admitting the W02 firmware artifacts. Verify the boot-selected ESP, EFI, CODE and VARS hashes before staging the ISO. Use the existing bench A broker and the trial's prior-screen/checkpoint guard. There is no automatic W02 PASS screen. Retain the actual console even on a stop; distinguish an unsupported guest exit or missing marker from a broker or media failure. Do not advance to Windows Boot Manager on the basis of this test.

### Re-read a task board after every result

From the supervisor checkout, run `python lab/hv_plan.py status "board.md"`, then `python lab/hv_plan.py run "board.md" --max-cards 2 --minutes 15`. The two sibling runner sessions above must already be open. The supervisor has no build, elevation or deployment step. Each completed worker triggers a fresh read; independent A/B cards may overlap, but never two on the same lane. The count and time limit apply to **admissions**; previously admitted finite batches may finish. A new or edited binding needs a new invocation. It exits when nothing eligible remains.

Use top-level Markdown checkboxes (`- [ ] R01 My frozen regression`). In one `<!-- hv-plan` comment, put a JSON object on the following lines and close with `-->` on a separate line. Keys are board IDs; every automatic `Rnn` card needs `lane` (`a` or `b`), `profile`, lowercase SHA-256 `iso` (frozen media), and `screen` (reviewed RGB565 framebuffer). Optional `after` lists prerequisite IDs; `note` states a manual blocker. Omit all four pins for a manual task. Board order breaks ties. A checked manual prerequisite or matching `COMPLETE` receipt can release a dependent; a checked regression without a receipt cannot. The supervisor never edits checkboxes or runs instructions from Markdown.

To fence a lane **without a board run ID**, use `python lab/hv_plan.py hold "board.md" --lane a --reason "Untracked session; owner has not confirmed shutdown"`. The supervisor will neither reserve nor submit work on A; independent work on B can still finish. `status` and the final `run` report list lane holds and their reasons even if there is no card on that lane. Removing a card, restarting the supervisor, or clearing a global stop does not remove a lane hold.

Only after independently verifying the lane is clear, use `python lab/hv_plan.py unhold "board.md" --lane a --reason "Operator verified VM off and external session closed"`. Both `hold` and `unhold` need the same exclusive board lock as `run`: if the supervisor owns it, request `stop` and wait for the supervisor to exit first. These commands affect admissions from this board only; they do not stop an external session, clear a run receipt, or grant a PASS.

`stop "board.md"` or Ctrl+C fences the whole board and forwards STOP to its active batches. An unexplained exit, `ATTENTION`, or uncertain `ACTIVE` reservation holds the recorded lane across restarts. Inspect the retained run ID, console and real session before recovery. After the worker is known stopped and the lane independently cleared, `release "board.md" --run-id ID --reason "evidence and decision"` removes only that run's hold; it neither changes the verdict nor replays the ID, and does not clear an external hold. `resume "board.md"` clears only the global stop. Prefix each action with `python lab/hv_plan.py`.

State is in `.hv-plan/<board stem>/` beside the board. `external-holds.json` is a JSON array of operator decisions in order (`lane`, `action` = `hold` or `unhold`, UTC `time`, and `reason`); `receipts.json` records board attempts separately. Keep both local, on one machine; never edit, sync or delete them to clear a hold. The lock is per board: use one canonical board for the two dedicated lanes. Raw receipts do not replace reviewed journal entries.

## Offline WinPE storage-driver evidence

Use a fresh private extraction, not the host Windows directory, a mounted image or a merged pair of indexes. Record the `boot.wim` hash, boot/Setup index, architecture, locale, build and product type from the archive. For that **same** index extract every direct `Windows/INF/*.inf` and `Windows/System32/drivers/*.sys` with paths preserved. Independently list those two flat sets from the archive into `index-paths.txt`: UTF-8, one index-relative filename per line, e.g. `Windows/INF/mshdc.inf`. No leading slash, index prefix, header or quotes. Check the extracted bytes against archive hashes separately. A list made from the extraction itself is not evidence of completeness. An entire extracted index also works with the same independent list.

For the en-US, AMD64, `10.0.19041` WinNT (product type 1) Setup index, run:

```text
python lab/winpe_storage.py INDEX_ROOT --inventory index-paths.txt --build 19041 --product-type 1
python -m unittest lab.tests.winpe_storage_test
```

The scanner compares both name sets, then follows `[Manufacturer]` → selected AMD64 models → DDInstall → matching `.Services` → the one `AddService` with `SPSVCINST_ASSOCSERVICE` → local kernel service → `%12%\name.sys` with an AMD64 PE32+ header. `id_field=1` is the model's first ID; `2` onward are its additional compatible IDs. Check the independent `mshdc.inf` AHCI control (`PCI\CC_010601` → `storahci`), the selected section names, entry counts, `model_scope_gaps` and each candidate's `chain_gap`. `literal_mentions_not_models` is a separate file-level cross-check; an ID in prose or another vendor's `DEV_1048` is not a model match.

`excluded_ids` preserves DDInstall exclusions that may disqualify a particular controller even when the class has a local service/binary chain. An explicitly excluded model ID never counts as a positive. A class-level result without the actual controller's complete IDs is not a binding decision.

For [modern virtio-SCSI](https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html), device type 8 plus `0x1040` gives `PCI\VEN_1AF4&DEV_1048`. `PCI\VEN_1AF4` is a [vendor-only compatible ID](https://learn.microsoft.com/windows-hardware/drivers/install/identifiers-for-pci-devices), not SCSI-specific. Revision, subsystem and class/configuration-specific matches require more PCI fields than this design supplies. A SCSI class model is conditional; `1AF4:1004` denotes transitional SCSI, and block/network transport IDs do not substitute. Only published INFs are binding evidence here: a staged-only `DriverStore/FileRepository` INF, `.sys` basename or wildcard is insufficient. A separate package inventory can corroborate what is staged, not what is published.

This is an offline declaration search, not SetupAPI selection. The parser targets en-US NT 10.0 AMD64; unsupported/ambiguous decorations, suite masks, string syntax, cross-INF service resolution and paths such as DriverStore DIRID 13 do not establish a chain. It does not replay DDInstall directives, authenticate packages, inspect registry activation, rank competing drivers or predict load/boot/I/O. A complete negative is only **no exact ID in the reviewed models**, not proof that no compatible driver exists. Exit `0` means the JSON report was produced (possibly inconclusive); `2` means input/authority failure, no conclusion. Keep all archive data, inventories and extractions outside Git.

## Check the optical installation payload without booting it

Run these **host-only** steps in a Python 3.10+ virtual environment. Do not mount the image. First verify the ISO size and full SHA-256 against the journal. An invented image or the right bytes in a different ISO is not the pinned installation medium.

1. `python -B lab/install_file_reference.py IMAGE --seven-zip PATH_TO_7Z` streams the outer `sources/install.esd` by name. Retain the file size, full file digest, and digest at byte 65520 for 64 bytes. This does not use the proposed sector address or unpack Windows to disk.
2. `python -m pip install --only-binary=:all: --no-deps --require-hashes -r lab/install-udf-requirements.txt` installs the optional UDF parser; use `--no-index --find-links WHEELHOUSE` with the pinned wheel offline. `python -B lab/install_udf_extent.py IMAGE` derives the file size and absolute sector containing byte 65520 from UDF metadata. Require the size to agree with step 1.
3. `python -B lab/install_content_witness.py IMAGE --media-size MEDIA_BYTES --file-size FILE_BYTES --file-offset 65520 --length 64 --first-lba ABSOLUTE_SECTOR --expected-sha256 RANGE_DIGEST` opens the raw image read-only. The expected digest must come from step 1, not from the address under test. The range uses the last 16 bytes of one 2048-byte sector and the first 48 of its successor. A matching digest establishes the expected host bytes, not a Windows guest read.

### Replay the two-sector optical command on the host

`lab/optical_media.h` routes optical commands on a host fixture, not an AHCI controller. The replay sends `28 00 00 03 c5 15 00 00 02 00` through it to the existing `READ(10)` handler, never to a device. Build `lab/tests/optical_read10_replay.c` with a C11 host compiler. For example:

```
cc -std=c11 -O2 lab/tests/optical_read10_replay.c -o optical-replay
python -B lab/replay_optical_read10.py IMAGE TRIAL_134_WITNESS_JSON ./optical-replay
```

Use the [published trial 134 receipt](https://github.com/kuro1337WStuff/hv/blob/1e12ac6ad4229c237f6252f57bd6416d9cdee50c/journal/reports/install-content-134.witness.json). The wrapper checks the ISO identity and compares the returned payload with that independent reference. Run the wrapper, not the reader executable directly: its binary stdout belongs to the wrapper, not a terminal or journal. CI uses authored bytes; it needs no installation media.

### Check optical geometry without an image

Build `lab/tests/optical_capacity_test.c` as C11. Run without arguments for the authored-medium checks. Run `optical-capacity --media-bytes 3839819776` for the size in trial 134's receipt. It takes a byte count, not a filename. The two big-endian words in the reply are **last addressable block** and **bytes per block**; the block count is one greater than the last address. Empty media are not reportable. A last address of `0xffffffff` or greater is reported as the `0xffffffff` sentinel; capacity discovery beyond it requires READ CAPACITY(16), which is not implemented.

### Discover the drive independently of its disc

Compile and run `lab/tests/optical_discovery_test.c` as C11. It uses authored memory only. A null target is absent; an attached target with zero blocks is an empty drive, so it can identify itself but cannot serve sector data. Standard INQUIRY returns up to 36 identity bytes, limited by its allocation length. A caller buffer smaller than that reply is rejected, not silently overrun. TEST UNIT READY can succeed with zero transferred bytes; check the acceptance flag separately from the byte count.

This is the plain six-byte discovery profile used by the pinned firmware, not a controller or full SCSI implementation. It does not implement vital product data pages, target status or sense records, or show that a guest loaded an optical driver.
