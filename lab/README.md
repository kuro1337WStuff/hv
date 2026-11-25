# Supervised Hyper-V regression queue

From the project root on the Windows lab host (Python 3.11+, PowerShell 5.1, Hyper-V), run:

```powershell
python lab/hv_queue.py run
```

The default order is `l2`, `cpuid`, `npt-write`, `npt-remap`, `npt-cross`. To rebuild and run just two profiles: `python lab/hv_queue.py run --rebuild npt-write npt-remap`. Without `--rebuild`, the runner uses the already-built ISOs in `build/`. It freezes the ordered profiles and SHA-256 digests for this batch before asking Windows to launch the visible Hyper-V worker. Building and staging use the caller's existing privileges. No change to UAC, no service and no per-test approval. An operator must still authorize elevation when Windows requires it.

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
