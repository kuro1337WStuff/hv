# AMD SVM / NPT test ledger

## Historical baseline · 2026-11-24

The [journal](README.md) records these console observations on a Ryzen 9 9950X under Hyper-V. They apply to the images run then, not to a later revision. The baseline entries do not record source revisions or image hashes.

| Run ID | Build profile | Observed result | Console evidence |
| --- | --- | --- | --- |
| 001 | `firmware\build.cmd` | UEFI reported `AuthenticAMD`, `CPUID.80000001h:ECX=0x004003F7` and SVM revision 1. No L2 entry. | [001 · UEFI probe](images/uefi-probe-console.png) |
| 002 | `firmware\build.cmd l2` | HLT exit `0x78` at `RIP=0x1003`; `RAX=0x4C32` matched the fixed guest's marker. | [002 · L2 halt](images/l2-smoke-console.png) |
| 003 | `firmware\build.cmd cpuid` | CPUID exit `0x72` at `0x100C`, next RIP `0x100E`; guest checked four result registers and halted at `0x1037` with marker `0x50415353` and exit `0x78`. Two entries; no-nRIP fallback untested. | [003 · CPUID resume](images/cpuid-resume-console.png) |

## Acceptance criteria

| Check | Profile | Acceptance criterion |
| --- | --- | --- |
| Host-side NPT layout | Host only, no VMRUN | The four-level walk maps the fixed guest page at GPA `0x1000` to its designated backing page, read-only; no other leaf is present. This checks table contents, not hardware enforcement. |
| Nested regression | `l2` and `cpuid`, each rebuilt and booted in the AMD Hyper-V lab | Record each image separately. `l2` must reproduce the halt, RIP and marker from 002; `cpuid` must reproduce the two exits and guest-generated PASS from 003. Record the resume path actually observed. |
| NPT write protection | `npt-write`, paired control and protected store | The writable store must reach HLT and change the target byte. After restoring the byte and revoking write permission, the same store must cause a final-data write protection NPF at the expected GPA and instruction, leaving the byte unchanged. |
| Forensic write report | `forensic-write`, same fixed store with permissions changed | Writable control: no protection event, HLT at `0x1005`, byte `0xA5`. Protected arm: final-data write protection NPF at RIP `0x1000`, GPA `0x2000`; decoding the saved instruction must independently yield `MOV byte [0x2000],0xA5`; backing byte must be `0x5A` before and after. |
| All-processor nested GPA invalidation | `npt-broadcast`, opt-in on AMD Hyper-V | Both measured processor IDs must first read A through the same NPT root. With both L2s stopped, change the leaf, require success from one BSP `HvCallFlushGuestPhysicalAddressSpace` on that root, then require both L2s to read B at the original GPA and HLT site with no entry flush. Missing capability is SKIP, not PASS. No claim about which instruction L0 used. |
| 64-bit integer state | `vcpu-gpr`, one L2 vCPU | Two ordered CPUID exits at distinct known instructions. On each resume L2 must check four zero-extended replies, all other GPRs (including high halves), and its round-specific stack pointer. Third exit must be HLT at the success label with a guest-generated PASS marker; a failure HLT is not interchangeable. |
| Virtual HLT wake | `irq-wake`, fixed 64-bit L2 | Intercept the waiting HLT while IF is set and handler count and progress are zero. Queue one vector `0x41`. Require one handler entry, the post-HLT return RIP, restored stack pointer, guest progress, and the success marker at a separate terminal HLT. Do not credit a skipped HLT as a delivered interrupt. |
| Deferred virtual IRQ | `irq-shadow`, fixed 64-bit L2 | Queue one vector before entry with IF clear. At the masked checkpoint require IF=0, pending=1, phase=1, count=0. Resume without reasserting the IRQ; the handler must observe phase=2 and save the RIP immediately following the shadow instruction. Require one entry, pending=0, and post-IRETQ phase=3 at the success HLT. |

**Target qualification:** No bare-metal Ryzen 9 9950X3D run is recorded here. Host-side checks and nested runs on the 9950X do not qualify that target.

## New run records · append only

Use the next unused run ID for every attempt. Keep FAIL and SKIP records, including the reason; give a retry a new ID and link the earlier record. Each record names the date, source revision, exact binary or ISO digest, host and profile, command, expected and observed result, verdict and evidence. Use a new reviewed artifact per run. Publish only project-relative links, not VM media or raw logs.

If an attempt stops before producing a binary or capturing evidence, record `not produced` or `not captured` with the reason; do not borrow a hash or artifact from another run.

### 004–006 · 2026-11-24 · Fixed NPT layout

Source: [`ff5f1867c8`](https://github.com/kuro1337WStuff/hv/commit/ff5f1867c85f85cd7feddbeebaa889007521f0b2). All runs used the 9950X development PC. The two firmware tests booted separate rebuilt images in `HV-SVM-L1`, not on bare metal. Run the commands from the repository root; the boot scripts require an off lab VM and elevated PowerShell.

| ID | Build / run | Expected → observed | Verdict / artifact |
| --- | --- | --- | --- |
| 004 | Host MSVC x64; exact compile and run commands in the transcript | Four levels, one read-only code frame at GPA `0x1000`, all other slots zero → independent walk matched all entries with simulated physical addresses above 4 GiB; exit status 0. | PASS · [host transcript](npt-layout-004.txt) |
| 005 | `firmware\build.cmd l2`; `.\lab\Boot-HvProbe.ps1 -L2Smoke` | Baseline 002 → HLT exit `0x78`, RIP `0x1003`, RAX `0x4C32`. | PASS · [halt console](images/npt-005.png) |
| 006 | `firmware\build.cmd cpuid`; `.\lab\Boot-HvProbe.ps1 -CpuidResume` | Baseline 003 → CPUID `0x72` at `0x100C`, nRIP `0x100E`, two entries; guest checked all four register values, halted with `0x78` at `0x1037`, RAX `0x50415353`. | PASS · [resume console](images/npt-006.png) |

| Run | Tested artifact | SHA-256 |
| --- | --- | --- |
| 004 | `fixed_npt_test.exe` | `c5dbdc0c52b0d71828629e6988217d2c3abfa3a48456d27fb33f9bd8deec4e81` |
| 005 | `hv-l2-smoke.iso` | `c51d227156cdef4489782cbe7307ebc8b06d1c414cba2108fce842dc627c4296` |
| 006 | `hv-cpuid-resume.iso` | `6976361fe10966f151b1da4b6d8b3c28b4614455853340bb48733f85b3e1e31c` |

### 007 · 2026-11-24 · Writable control and read-only NPT store

**PASS · nested SVM, Hyper-V on Ryzen 9 9950X.** Source [`da53379ed1`](https://github.com/kuro1337WStuff/hv/commit/da53379ed1d1e923e43202a6bdb0d2d4325b869a); tested `hv-npt-write.iso` SHA-256 `8385a3903385b1559beb65c447ba1c6095af0eddd9e9107dcd21b370f05478b0`. Build `firmware\build.cmd npt-write`; with the isolated VM off, run `.\lab\Boot-HvProbe.ps1 -NptWrite` in elevated PowerShell.

| Arm | Expected | Observed |
| --- | --- | --- |
| Writable data leaf | Store `0xA5` at GPA `0x2000`, then exit on HLT | Byte `0xA5`, exit `0x78`, RIP `0x1005` |
| Same frame, read-only | Final-data write protection fault at the store; target remains `0x5A` | Exit `0x400`, RIP `0x1000`, `EXITINFO1=0x100000007`, `EXITINFO2=0x2000`, byte `0x5A` |

L1 restored the initial byte, cleared the data leaf's write bit and requested a full TLB flush before the second entry. The code page remained read-only in both arms. [Console capture](images/npt-write-007.png). This tests a permission change on one vCPU, not frame replacement, remote invalidation or bare metal.

### 008 · 2026-11-24 · Replace the backing frame at a fixed GPA

**PASS · nested SVM, Hyper-V on Ryzen 9 9950X.** Source [`152f15cc21`](https://github.com/kuro1337WStuff/hv/commit/152f15cc21ae0b937cad1503cf4a9d941781aaad); tested `hv-npt-remap.iso` SHA-256 `5188b63f9b155176e9971dcb3588f1410bcb77102541227eb85bcbf6c856d248`. Build `firmware\build.cmd npt-remap`; with the isolated VM off, run `.\lab\Boot-HvProbe.ps1 -NptRemap` in elevated PowerShell.

| Read from GPA `0x2000` | Expected guest RAX | Observed guest RAX | Observed exit / RIP |
| --- | --- | --- | --- |
| Before remap: frame A | `0xA11A` | `0xA11A` | HLT `0x78` / `0x1003` |
| After remap: frame B | `0xB22B` | `0xB22B` | HLT `0x78` / `0x1003` |

The guest instruction, ASID, nested root and read-only permissions stayed fixed. Both markers were initialized before entry; guest RAX was cleared before each load. L1 requested the conservative full flush (`TLB_CONTROL=1`) on entry. [VM console](images/npt-remap-008.png). This does not test a selective flush, cross-vCPU invalidation or the physical 9950X3D.

### 009 · 2026-11-24 · Cross-vCPU image build

**FAIL · build only.** The first `firmware\build.cmd npt-cross` attempt failed at link time: the freestanding image referenced `_disable`, `_enable` and `memset`. No bootable ISO or VM observation was produced. The next build used inline flag intrinsics and explicitly initialized callback fields instead of depending on runtime library symbols.

### 010 · 2026-11-24 · Cross-vCPU full-flush rendezvous

**SKIP · no L2 entries.** Source [`b650d8684a`](https://github.com/kuro1337WStuff/hv/commit/b650d8684aa5ca167dc0edf223e3403c258ed96d); `hv-npt-cross.iso` SHA-256 `bcf573780db9c60da387d9ec434dbe0d1b55a84388a7be4aeb321486f5a9f72d`. Built with `firmware\build.cmd npt-cross`; boot with `.\lab\Boot-HvProbe.ps1 -NptCross` in elevated PowerShell with the lab VM off.

**Expected:** two distinct firmware processor IDs, each with its own VMCB and host-save page, read `0xA11A` from GPA `0x2000` before the BSP changes the shared read-only leaf. Each processor must then request its own `TLB_CONTROL=1` flush on entry and return `0xB22B`. Every acknowledgement requires the requested CPU identity, HLT exit `0x78`, RIP `0x1003`, and the correct register value. This is a quiesced rendezvous, not an IPI interrupting a running guest or an `INVLPGB` broadcast test.

**Observed:** Hyper-V reported two configured vCPUs with nested extensions enabled on the 9950X lab host. The mounted ISO digest matched. The [UEFI console](images/npt-cross-010.png) reported `LocateProtocol status: 0x800000000000000E` (`EFI_NOT_FOUND`) for MP Services. The image stopped before either warm-up read, remap or acknowledgement. No stale translation was observed, and none was ruled out. Qualifying this case requires a supported way to dispatch and identify the second processor in L1; increasing the VM's configured CPU count alone does not supply that interface.

### 011–013 · 2026-11-24 local · Supervised queue controls

Lab: 9950X / `HV-SVM-L1`, nested SVM, two virtual processors. These are harness checks using existing ISOs, not new firmware builds. The finalized [runner](../lab/README.md) is revision `bafa241753`. Run 012 preceded the five-second between-profile countdown; run 013 exercised it. Raw JSONL, frozen image hashes and frames are preserved locally in `build/runs/<batch-id>/`.

| ID | Batch ID | Scenario and observed result |
| --- | --- | --- |
| 011 | `9090d3dfb59b43acad8545a563923102` | `run l2` plus separate `watch` and `stop` clients. Both observers saw STOPPED (exit 2) during preparation. No elevation or VM change. |
| 012 | `934f4ed6b3774ea5a4395d5c7f3e2c69` | Default queue. `l2`, `cpuid`, `npt-write`, `npt-remap` each matched their PASS console (ISOs from runs 005–008). `npt-cross` used the run 010 ISO and matched its [SKIP console](images/queue-012-skip.png); terminal ATTENTION, exit 3. No retry. |
| 013 | `0711822374d14aba96de7d5368137a4b` | `run npt-remap l2`. Remap passed, then the separate `stop` client vetoed `l2` during NEXT. Terminal STOPPED, exit 2; the only BOOT was `npt-remap`. No subsequent mount or boot. |

### 014 · 2026-11-24 local · Operator replay of original queue

Batch `281294cc0f27481681aae4c630138f16`: `l2`, `cpuid`, `npt-write` and `npt-remap` passed. The original `npt-cross` ISO from 010 again produced SKIP, not an L2 result. The queue stopped at ATTENTION. No processor IDs were inferred from this batch.

### 015 · 2026-11-24 local · Two measured L1 processor IDs and shared-NPT remap

**Guest PASS; queue ATTENTION pending screen review.** Batch `d0782ede6ed14e21bcc40940f047cb6e` booted [`a2da063bb9`](https://github.com/kuro1337WStuff/hv/commit/a2da063bb9c9c6244fea99513da7f3d7a33f610f), `hv-npt-cross.iso` SHA-256 `9d7a5434bd3a5253155dbd4318e362cf8c30edd0a090162a2c0e1eb99f3aab24`, in the isolated 9950X Hyper-V VM. Build: `firmware\build.cmd npt-cross`. Run: `python lab/hv_queue.py run npt-cross`.

Before guest entry, the bootstrap processor and application processor independently executed CPUID leaf 1 and returned initial APIC IDs `0` and `1`, respectively; each matched its Limine routing ID. Expected: both CPUs read marker A, then both read B after replacing the shared read-only leaf at GPA `0x2000`. Both epochs used ASID 1 and a local full flush at each guest entry. Neither frame's contents changed.

| L1 APIC ID | Observed guest AX, frame A | Observed guest AX, frame B | Exit / RIP on both reads |
| --- | --- | --- | --- |
| 0 (BSP) | `0xA11A` | `0xB22B` | HLT `0x78` / `0x1003` |
| 1 (AP) | `0xA11A` | `0xB22B` | HLT `0x78` / `0x1003` |

[Reviewed console with IDs and all four samples](images/npt-cross-015.png). The queue correctly treated the previously unseen display as unknown until review. Architecture reference: AMD64 Architecture Programmer’s Manual, Vol. 2, §§15.16–15.16.1 (combined manual PDF pp. 1013–1014), and §15.25.1 (p. 1034). The result demonstrates a coordinated **quiesced** remap with local flushes on two virtual processors, not a broadcast shootdown, an interrupt of a running L2, or bare-metal qualification.

### 016 · 2026-11-24 local · Supervised replay after screen review

**PASS / COMPLETE.** `python lab/hv_queue.py run l2 npt-cross`, batch `c59455d8d942473caf588dd7c18f2daf`. The intervening `l2` image (run 005 digest) reached its distinct PASS screen before the worker mounted and booted the run 015 cross ISO again. The fresh cross console matched the independently reviewed run 015 screen, and the queue exited 0. Per-run frames and `events.jsonl` remain under the local batch directory; the VM was left at cross-vCPU PASS.

### 017 · 2026-11-24 local · One all-processor nested GPA flush

**Guest PASS; queue ATTENTION (new screen).** Ryzen 9 9950X, `HV-SVM-L1`, two nested vCPUs. Source `a327bf22ed`; `python lab/hv_queue.py run --rebuild npt-broadcast`; batch `6db8de80390d485586cc6550f649cded`. ISO SHA-256 `160300AAEB03C976ACC5F05177297820E0F08C4EDD78B5B53D3F90CCC0EAA336`. The worker checkpointed the prior `npt-cross` PASS, booted the new profile and captured its first unrecognized terminal after 60 seconds. It did not advance.

The [captured console](images/npt-broadcast-017.png) meets the predeclared criterion. L1 APIC IDs 0 and 1 each read `AX=0xA11A` at GPA `0x2000` before the remap. The BSP reported `CPUID.4000000A:EAX=0x4A0000` (AMD enlightened NPT bit 22 set). After the leaf changed, the BSP issued one `HvCallFlushGuestPhysicalAddressSpace` (`0xAF`) for the shared `nCR3`; status `0` means success. Both vCPUs re-entered with `TLB_CONTROL=0` and returned `AX=0xB22B`. All four samples exited at HLT (`0x78`, RIP `0x1003`).

[Hyper-V nested address spaces](https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/nested-virtualization#second-level-address-translation) define the AMD target; [the hypercall contract](https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/hypercalls/hvcallflushguestphysicaladdressspace) defines completion on all processors before return. The BSP reported `CPUID.80000008:EBX[3]=0`; native `INVLPGB`/`TLBSYNC` was not tested. This run establishes the guest-visible result, not TLB residency, L0's implementation, interruption of an active L2, or 9950X3D bare-metal behavior.

### 018 · 2026-11-24 local · Forensic write protection

**Firmware PASS; queue ATTENTION (first screen review).** Ryzen 9 9950X, `HV-SVM-L1`. Source `ea55a6e9c2`; `python lab/hv_queue.py run --rebuild forensic-write`; batch `26f2f1ade697485cb4951a1310c55371`. ISO SHA-256 `380FE7D6B3C972428A70748E7DB559A618B5330B09CCC72F7A2E26494D974F24`. The queue checkpointed the preceding `npt-broadcast` PASS, booted the forensic image, then saved the [live console](images/forensic-write-018.png) and stopped for review; it did not report COMPLETE.

| Observation | Writable control | Read-only attempt |
| --- | --- | --- |
| Exit and saved RIP | HLT `0x78` at `0x1005` (after store) | NPF `0x400` at `0x1000` (on store) |
| Backing byte | `0xA5` after store | `0x5A` before and after |

`EXITINFO1=0x100000007`: present, write, final translation; not an instruction fetch or a guest page-table walk. `EXITINFO2=0x2000`. Independently decoding the bytes at the saved RIP in the immutable fixture yielded destination `0x2000` and immediate `0xA5`. The fault was neither resumed nor emulated. The attempted value came from the instruction, not the NPF metadata. Reference: AMD64 Architecture Programmer’s Manual, Vol. 2, §15.25.7, pp. 575–576. No bare-metal run.

### 019 · 2026-11-24 local · Register-test build stopped before VM staging

**BUILD ERROR; no guest result.** `python lab/hv_queue.py run --rebuild vcpu-gpr`, batch `b72141a573554e87bebd673b91191b80`. ML64 rejected the branch to the byte-typed failure HLT label (`A2024`). No ISO was staged, no VM was changed and there was no console capture. The correction makes the target a code label; the failure HLT and round/register checks remain in the test.

### 020 · 2026-11-24 local · Two 64-bit register round trips

**Firmware PASS; queue ATTENTION for first-time screen review.** Ryzen 9 9950X, `HV-SVM-L1`; `python lab/hv_queue.py run --rebuild vcpu-gpr`; batch `a85336bc51fd445288dd2cb5aa33b22e`. Source `f3507d2e46`; ISO SHA-256 `C86508F837D37F99D1AA6B3172F7A96F565BB9438EF105F1EC6A7CBD5176CA8E`. The queue checkpointed the prior forensic-write PASS, booted the new profile and saved the [live console](images/vcpu-gpr-020.png). It did not report COMPLETE.

| Entry | Observed exit | Guest RIP |
| --- | --- | --- |
| 1 | CPUID (`0x72`) | `0x1091` |
| 2 | CPUID (`0x72`) | `0x12A7` |
| 3 | Success HLT (`0x78`), `RAX=0x50415353` | `0x141F` |

L1 compared all 16 integer-register values against the input pattern on each CPUID exit. L2 checked the four zero-extended replies and every other GPR after each resume, including RSP (`0x7FF0` then `0x7FC0`). The PASS marker was produced by L2, not supplied by the exit handler. The observed instruction addresses also match the two CPUID opcodes and success HLT in the built EFI payload. AMD64 APM Vol. 2, §15.5 and Appendix B describe the VMRUN/VMCB state boundary. No claim about extended state, stack-memory integrity, interrupts, migration or bare-metal behavior.

### 021 · 2026-11-24 local · IRQ fixture rejected before guest entry

**Firmware FAIL; no interrupt result.** On the nested Ryzen 9 9950X lab, `python lab/hv_queue.py run --rebuild irq-wake`, batch `28120f2866b74f85a614d84648264b02`, booted ISO SHA-256 `F3CE5FFBAD48F6A02158381C7CEADDA38DED28EE5F001CE9419847BE06C8433C`. The [live console](images/irq-wake-021.png) reported `FAIL: invalid fixed IRQ guest layout`; no L2 entry occurred. The queue stopped at ATTENTION, preserving the VM. The assembler listing placed the wait HLT and following instruction at offsets `2` and `3`; the optimized C listing went unconditionally to the layout error. The correction exports the offsets as assembler data instead of asking C to establish adjacency between separate external objects.

### 022 · 2026-11-24 local · One virtual interrupt wakes L2

**Firmware PASS; queue ATTENTION (first review of success screen).** Ryzen 9 9950X, `HV-SVM-L1`; source `78cf309c0a`; `firmware\build.cmd irq-wake` then `python lab/hv_queue.py run irq-wake`. Batch `de69637a7a4f4eb38b5da41f0e38ff9a`; ISO SHA-256 `44185BA59CB176120801B7BA44673E654EF795096F86195D4056702DB11049F5`. Before the retry, the operator authorized power-off of the isolated VM after a live identity, media and failure-screen match. The previous checkpoint and run 021 capture were retained.

The [console](images/irq-wake-022.png) shows the first intercepted HLT at `0x1002`: IF=1, interrupt shadow=0, count=0, progress=0. L1 advanced past the fixed HLT and set `V_IRQ` with priority 4 and vector `0x41` for the second VMRUN. Only the guest IDT handler increments the count and reads the return address from the CPU-built stack frame. Count was `1`, stacked RIP `0x1003`, continuation progress `1`, final RIP `0x1039`, and final RAX `0x50415353`. PASS also required a cleared `V_IRQ`, restored RSP `0x7FF0`, IF set, and the final HLT intercept. Clearing `V_IRQ` alone is not proof that the handler ran: AMD APM Vol. 2 §15.21.4 states the processor clears it before accessing the IDT.

The queue did not auto-advance or report COMPLETE. This covers one eligible maskable interrupt and return on one virtual CPU; it does not cover deferred delivery with IF clear, APIC timer/EOI emulation, or bare metal.

### 023 · 2026-11-24 local · Masked interrupt build stopped before staging

**BUILD ERROR; no guest result.** `python lab/hv_queue.py run --rebuild irq-shadow`, batch `29cd80b782c743a7945c2d69ddb081a0`. MSVC rejected an unterminated header guard (`C1070`). No image was staged and the VM was unchanged. The corrected build keeps the same guest acceptance criteria.

### 024 · 2026-11-24 local · Masked interrupt image failed to link

**BUILD ERROR; no guest result.** Same command, batch `b42a199d3f744c1ab708e3be6c2b5e08`. Zero-initializing the expanded trace emitted a `memset` reference, unavailable in this freestanding EFI build (`LNK2019`). No VM changes. The runner writes every trace field it reads, so the unused initialization was removed rather than adding a C runtime.

### 025 · 2026-11-24 local · Pending vector delivered after the STI shadow

**Firmware PASS; queue ATTENTION for first-time screen review.** Ryzen 9 9950X, nested `HV-SVM-L1`; source `32283ef88d`; `python lab/hv_queue.py run --rebuild irq-shadow`; batch `310148b1bd304056a6f86c739a83962a`. ISO SHA-256 `B3A4F6BEF38B1E9FC405A41BD93D6517B2462B3FD58920830C3385B221576DDA`. The worker checkpointed the preceding `irq-wake` PASS and captured the [live console](images/irq-shadow-025.png). The queue did not report COMPLETE.

| Observation | Result |
| --- | --- |
| Masked checkpoint, `VMMCALL` at `0x100C` | IF=0, V_IRQ=1, phase=1, handler count=0 |
| Handler for vector `0x41` | Observed phase=2, stacked return RIP=`0x1018` immediately after the shadowed `INC` |
| Success HLT at `0x1059` | IF=1, V_IRQ=0, handler count=1, phase=3, RSP=`0x7FF0`, RAX=`0x50415353` |

L1 did not reassert the IRQ or set IF at the checkpoint. Guest `STI` set IF, the next instruction incremented the phase, and only then did the handler run. Checking both phase and stacked RIP distinguishes delivery at the shadow boundary from merely seeing the handler eventually. Reference: AMD64 APM Vol. 2, §§15.21.4–5, p. 557. This is one maskable vector on one nested vCPU; no timer, priority arbitration or physical target was tested.

### 026 · 2026-11-24 local · Virtual interrupt priority threshold

**Firmware PASS; queue ATTENTION for first-time screen review.** Ryzen 9 9950X, nested `HV-SVM-L1`; source `4279c63f1a`. `python lab/hv_queue.py run --rebuild irq-priority`, batch `424915930b264f5c8ab2d98cf282af95`, ISO SHA-256 `FC81939C9723BA484B8D7328D8F8A22850D46C95824282D4ABE70AEA3815EA8D`. The worker checkpointed the prior `irq-shadow` PASS before booting. The [captured console](images/irq-priority-026.png) was reviewed after the queue stopped; the batch did not report COMPLETE.

| Checkpoint | Expected | Observed |
| --- | --- | --- |
| Equal threshold at `VMMCALL` | With IF set, no shadow and no priority bypass, priority 4 cannot exceed `V_TPR=4` | RIP `0x100C`; vector `0x41`, priority 4, `V_TPR=4`, `V_IRQ=1`, `V_IGN_TPR=0`, IF=1, shadow=0, phase=1, count=0 |
| Resume with `V_TPR=3` | Same request delivered before the first continuation instruction | Handler saw phase 1, count 1, stacked RIP `0x100F` (immediately after `VMMCALL`) |
| Guest returns and halts | Pending bit clears; guest advances and restores its stack | `V_TPR=3`, `V_IRQ=0`, IF=1, shadow=0, phase=2, RSP `0x7FF0`, RIP `0x1050`, HLT exit `0x78`, RAX `0x50415353` |

The only interrupt-control change on resume was the priority threshold. AMD64 APM Vol. 2, §§15.21.2 and 15.21.4 (pp. 556–557): without `V_IGN_TPR`, the pending interrupt's priority must be **greater than** `V_TPR`. This tests one pending vector on one nested vCPU, not arbitration between vectors, guest CR8 writes, APIC EOI, a timer, or bare metal.
