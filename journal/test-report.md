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

### 027 · 2026-11-24 local · Invalid-opcode delivery and return

**Firmware PASS; queue ATTENTION for first-time screen review.** Ryzen 9 9950X, nested `HV-SVM-L1`; source `7bb2e4aaa3`. `python lab/hv_queue.py run --rebuild exception-ud`, batch `61ad1e1ce42746c49dfb0be37d3ef5a5`, ISO SHA-256 `952D72208651AA53FAD56C92ACED79E877AD4A7A3623CFFA0885E29764FDBACE`. The worker checkpointed the preceding `irq-priority` PASS and saved the [console capture](images/exception-ud-027.png). This first batch stopped for review and did not report COMPLETE.

| Observation | Expected | Observed |
| --- | --- | --- |
| First exit on the guest's `UD2` | Exception intercept for vector 6, RIP still at the opcode | `EXITCODE=0x46`, RIP `0x100C` |
| Re-entry without changing RIP | Deliver `#UD` with no error code; handler sees the original fault address once | Submitted `EVENTINJ=0x80000306`; handler count 1, stacked RIP `0x100C` |
| Return past the fixed two-byte opcode | Guest continuation runs with the original stack and reaches its success HLT | Phase 3, RSP `0x7FF0`, HLT exit `0x78` at `0x104F`, RAX `0x50415353` |

All exception intercepts remained enabled on re-entry. The guest validated its exception frame and the `0F 0B` bytes before advancing the saved RIP to `0x100E`. Skipping the fault in L1 would leave the handler count at zero; returning to the fault unchanged would produce another `#UD` exit instead of HLT. Reference: AMD64 APM Vol. 2, §§8.2.7 and 15.20. This covers one no-error-code fault on a single nested vCPU, not page faults, nested exceptions, or bare-metal delivery.

### 028 · 2026-11-25 local · Legacy XMM register preservation

**Firmware PASS; queue ATTENTION for first-time screen review.** Ryzen 9 9950X, nested `HV-SVM-L1`; source `88ce1bc39a`. `python lab/hv_queue.py run --rebuild vcpu-xmm`, batch `999f4edb3b0042febe33f8b2c09f8409`, ISO SHA-256 `B04E550134C016D04E8553D2CD02254557D3AB898AB8CDCB649CA8B8D0AB4842`. The launcher timed out after READY, before any recorded VM operation. The elevated `lab/Invoke-HvQueue.ps1` worker used the same frozen run directory, checkpointed the prior `exception-ud` PASS and booted the staged image once. It stopped for review of the [console capture](images/vcpu-xmm-028.png); it did not report COMPLETE.

| L2 exit | Exit code / RIP | Restored live L1 XMM15.high | Captured L2 XMM15.high |
| --- | --- | --- | --- |
| VMMCALL | `0x81` / `0x1110` | `0x4853001FB1C7001F` | `0x4753011FA6E9011F` |
| Success HLT after resume | `0x78` / `0x1228` | `0x4853011FB1C7011F` | `0x4753021FA6E9021F` |

The displayed lane is one example. PASS required **all 32 64-bit lanes** to match in each entry's guest snapshot, captured guest exit frame, host save frame, and live host sample after restore. Values varied by owner, register, lane and entry, so reloading an initial guest frame on resume could not pass. L2 also checked its own incoming registers before replacing them. Its success HLT required the expected RIP, RSP and RAX marker.

This is an explicit legacy XMM0–15 switch on one nested vCPU, not a full extended-state implementation. It does not cover MXCSR, x87, upper AVX lanes, vCPU migration or bare metal. References: AMD64 APM Vol. 2, §§11.4.1, 15.5–15.6.

### 029 · 2026-11-25 local · MXCSR build stopped before VM staging

**BUILD ERROR; no guest result.** `python lab/hv_queue.py run --rebuild vcpu-mxcsr`, batch `a862179ffe624de099f17cbce3146cee`. MSVC `/WX` rejected a signed/unsigned comparison (`C4389`) between the saved 32-bit guest MXCSR and the expected exit value. No image was staged and the VM was not changed. The correction gives the expected literals unsigned types; the required register values are unchanged.

### 030 · 2026-11-25 local · MXCSR rounding and status preservation

**Firmware PASS; queue ATTENTION for first-time screen review.** Ryzen 9 9950X, nested `HV-SVM-L1`; firmware source `d70cac84e8`. `python lab/hv_queue.py run --rebuild vcpu-mxcsr`, batch `316e65031d88475b93555d45a7fc6eaa`, ISO SHA-256 `990A9D737822DE1C38829789027874F59A5E8A8D43C98F09853AC15503A3C970`. The worker checkpointed the prior `vcpu-xmm` PASS before booting. The [console capture](images/vcpu-mxcsr-030.png) was reviewed after the batch stopped; the queue did not report COMPLETE.

| Exit / guest RIP | L1 seed, saved and restored live | L2 entry | Integer result for 1.5 | L2 after conversion | L2 at exit |
| --- | --- | --- | --- | --- | --- |
| VMMCALL `0x81` / `0x1034` | `0x1F81` | `0x3F80` (down) | 1 | `0x3FA0` | `0x5F80` |
| HLT `0x78` / `0x1069` | `0x7F84` | `0x5F80` (up) | 2 | `0x5FA0` | `0x5FA0` |

L2 explicitly cleared the precision flag when selecting round-up before `VMMCALL`. Its next entry had to recover that captured value, not the initial round-down value. `CVTSS2SI` set just the precision flag (`0x20`) in both conversions. L1 used different status flags and rounding controls on each entry; `STMXCSR` sampled the restored live register separately from its save slot. Final guest RAX was `0x50415353`. Reference: AMD64 APM Vol. 1 §4.2.2. This covers masked exceptions and the tested MXCSR values on one nested vCPU, not unmasked SIMD exceptions, x87 state, or bare metal.

### 031 · 2026-11-25 local · Delegated VM batch completes from a standard terminal

**PASS / COMPLETE.** On the 9950X nested lab, one foreground administrator session accepted `python lab/hv_queue.py run --session vcpu-mxcsr` from a non-administrator terminal. Batch `ced6858a810c46f68a78adab61140783` checkpointed the prior guest, mounted the existing run 030 ISO (`990A9D737822DE1C38829789027874F59A5E8A8D43C98F09853AC15503A3C970`), booted `HV-SVM-L1`, and returned a fresh PNG matching the [reviewed guest screen](images/vcpu-mxcsr-030.png). The submitting process received a COMPLETE result without elevating. Separately, an invalid session credential was rejected, and stopping a batch during preparation returned STOPPED before VM management. This exercises the control workflow, not new processor behavior.

### 032 · 2026-11-25 local · Integrated vCPU fixture stopped at assembly

**BUILD ERROR; no VM result.** `python lab/hv_queue.py run --session --rebuild vcpu-fpstate`, batch `1f8b53330af843dc93e3489bd438d682`. ML64 could not locate `firmware\fpstate_guest.inc` (`A1000`). No ISO was staged and no VM operation was issued. The corrected build gives ML64 the firmware include directory explicitly and includes the guest fixture by filename.

### 033 · 2026-11-25 local · Integrated legacy vCPU context

**Firmware PASS; queue ATTENTION for first-time screen review.** Nested `HV-SVM-L1` on the Ryzen 9 9950X. `python lab/hv_queue.py run --session --rebuild vcpu-fpstate`; source `792342dc46`; batch `c8f535348db247919395e4cf4eaa5c42`; ISO SHA-256 `272E8951F71B14D6940E7EDA5F17D7A73D8AFC4DF19759A50A536CBB07869FEF`. The delegated worker checkpointed the prior `vcpu-mxcsr` PASS. It captured the [new screen](images/vcpu-fpstate-033.png) and stopped for review rather than reporting COMPLETE.

| Guest exit / RIP | Guest RAX | Restored L1 FCW / MXCSR | Captured L2 FCW / MXCSR |
| --- | --- | --- | --- |
| VMMCALL `0x81` / `0x1028` | `0x46504332` | `0x037F` / `0x1F81` | `0x0B7F` / `0x5F80` |
| HLT `0x78` / `0x1082` | `0x50415353` | `0x077F` / `0x3F84` | `0x0F7F` / `0x7F80` |

The guest bank was initialized once. `RunLegacyVcpu` switched the 14 software-managed GPRs and an aligned 64-bit FXSAVE image together; RAX, RSP and RIP remained VMCB-managed. The fixture compared guest entry snapshots, exit banks, restored L1 state and the actual caller before/after the harness. It checked both halves of XMM0–15, the occupied 80-bit stack values, TOP and the physical tag mask, and both rounding controls. After resume, SSE produced `1` and x87 produced `12` (`0x0C`). Only then did L2 install its final state and halt.

AMD64 APM Vol. 2 §§2.5.12 and 11.4.4 describe the format. The profile requires L1 `EFER.FFXSR=0` so CPL0 saves include XMM. This is not qualification of AVX/XSAVE, every x87 environment field, vCPU migration, or the physical 9950X3D.

### 034 · 2026-11-25 local · Standalone L1 image failed to link

**Host build only; no VM change.** `firmware\build.cmd l1-handoff` assembled and compiled but failed at link with `LNK2019` on `_disable`. The freestanding target has no runtime supplying that function. Moved the failure-path `CLI` into the profile's MASM module rather than linking a host runtime. No ISO or boot result from this attempt.

### 035 · 2026-11-25 local · Standalone L1 handoff staged, not booted

**NOT RUN in `HV-SVM-L1`.** `python lab/hv_queue.py run --rebuild l1-handoff`, batch `8e90e81c28d64fe0b856831d50a9ab09`. The corrected image built and staged: ISO SHA-256 `0787BB201F8F9B873C0E36789421166FE1980E0C36366410CBAB1578F6119141`. Windows reported that worker elevation was canceled. No worker started, no checkpoint or VM change was issued, and no COM1 transcript or guest screen was captured. The profile has no reviewed PASS digest.

**Acceptance contract for the next boot:** successful `ExitBootServices` with the current map key, private stack within its 4-page reservation, correct readback at both ends of the first arena page, then rejection of a 9-page request against an 8-page arena without moving the cursor. The next 1-page allocation must return exactly the second page. After a COM1 request, require `HV:HANDOFF`, `HV:OWNED`, `HV:EXPECTED_OOM`, `HV:HALT` in order, plus the independently captured `L1 HANDOFF PASS` screen. The profile retains inherited address mappings; it does not launch L2. See UEFI 2.11 §§7.2.3 and 7.4.6 for the map and exit contract.

**Host evidence only:** the session-client loopback check accepted the exact four-line transcript and rejected missing and truncated transcripts. That verifies evidence transfer, not firmware execution.

### 036 · 2026-11-25 local · Standalone L1 handoff

**Firmware PASS; queue ATTENTION (first-time framebuffer review).** `python lab/hv_queue.py run --session l1-handoff` booted the staged run 035 image in `HV-SVM-L1` on the Ryzen 9 9950X. Firmware source `d828ef04dc`; batch `9b242bb1b7a74aefbac03b0a21e8dc87`; ISO SHA-256 `0787BB201F8F9B873C0E36789421166FE1980E0C36366410CBAB1578F6119141`. The worker checkpointed the prior `vcpu-fpstate` PASS before booting.

The COM1 capture contained exactly `HV:HANDOFF`, `HV:OWNED`, `HV:EXPECTED_OOM`, `HV:HALT`, in that order, each terminated by CRLF. Captured bytes SHA-256: `b21bb1e384b4d30b9e603a9921f8acf6b4a777d5bae44321295b78c8a49a9f40`. The [1024×768 framebuffer](images/l1-handoff-036.png) showed `L1 HANDOFF PASS` with the same four markers. The unreviewed framebuffer timed out as designed; this was not a queue COMPLETE or a second boot.

The pass path requires successful `ExitBootServices`, a stack address inside the four reserved stack pages, and readback at both ends of the first arena page. Rejecting a nine-page request did not consume any of the seven remaining pages: the next successful allocation was page two. This verifies the bounded bootstrap arena with inherited address mappings, not a general physical allocator, a forced stale map-key retry, an L2 launch, or operation on the physical 9950X3D.

### 037 · 2026-11-25 local · One L2 entry from the standalone L1

**Firmware PASS; queue ATTENTION for first-time screen review.** `python lab/hv_queue.py run --session --rebuild l1-l2` on the nested Ryzen 9 9950X lab. Source `54d03398fd`; batch `d40ba15cd9a24bb68b9dfb921e071046`; ISO SHA-256 `AFC1E7E51E2DE58AADB596D289FF7F761FA46A8B9813AE57DE033447B3345682`. The worker checkpointed the prior `l1-handoff` PASS and captured the [new framebuffer](images/l1-l2-037.png); it did not report queue COMPLETE.

The L1 bootstrap processor entered on its own stack after `ExitBootServices`. Its 12-page arena held one VMCB, one host-save page, four NPT pages, one L2 code page, three I/O permission-map pages and two MSR permission-map pages. The separate four-page L1 stack was not mapped into L2. The only guest mapping was the read-only page at GPA `0x1000`, beginning with `B8 32 4C F4` (`MOV AX,0x4C32; HLT`).

The framebuffer reported `L1 L2 PASS`: `EXITCODE=0x78`, `RIP=0x1003`, `RAX=0x4C32`. All three match the predeclared acceptance criteria. The VMCB started with an invalid exit code and zero RAX: a boot or an immediate exit before the MOV would not satisfy the test. AMD64 APM Vol. 2 §§15.5.1, 15.6 and Appendix B describe the entry, exit and saved guest fields.

Scope: one fixed real-mode guest, one `VMRUN`, no guest stack access, device I/O, resume, or physical deployment. Evidence is the returned framebuffer, not a guest serial message. The original UEFI `l2` profile was separately rebuilt against the shared VMCB setup but not booted in this run.

### 038 · 2026-11-25 local · Standalone L1 CPUID resume

**Firmware PASS; queue ATTENTION for first-time screen review.** `python lab/hv_queue.py run --session --rebuild l1-cpuid` on the Ryzen 9 9950X nested Hyper-V lab. Source `3e7c65f571`; batch `95b14e7287ef42c78c218c550afecd16`; ISO SHA-256 `D8F7DBD2DF86F8CEE6D93CC13F03D60B2ED228CD999DAEE836D29444DC6F14C7`. The worker checkpointed the run 037 `l1-l2` PASS and captured the [new framebuffer](images/l1-cpuid-038.png). It did not report queue COMPLETE.

| Entry | Observed exit | Saved RIP | Guest result |
| --- | --- | --- | --- |
| 1 | CPUID `0x72` | `0x100C` | Leaf `0x400000FE`, subleaf `7`; reported next RIP `0x100E` |
| 2 | HLT `0x78` | `0x1037` | RAX `0x50415353` at the success halt |

L1 supplied EAX=`0x11223344`, EBX=`0x55667788`, ECX=`0x99AABBCC`, EDX=`0xDDEEFF00` without changing the guest mapping or ASID. The guest compared all four 32-bit registers after resume. A mismatch would set `0x4641494C` and halt at a different address; L1 requires the success address **and** marker. The framebuffer reported two entries. AMD64 APM Vol. 2 §15.7.1 describes the exit state used here.

Scope: one fixed request on one post-handoff vCPU using the reported next RIP. No general CPUID policy, no test of the no-nRIP fallback, and no physical 9950X3D boot. The original UEFI `cpuid` profile rebuilt against the shared handler but was not booted in this run.

### 039 · 2026-11-25 local · Standalone L1 NPT write protection

**Firmware PASS; queue ATTENTION for first-time screen review.** `python lab/hv_queue.py run --session --rebuild l1-npt-write` on `HV-SVM-L1`, nested on the Ryzen 9 9950X. Source `749c9a9d02`; batch `10c216862b294eeb9e774350b32513ff`; ISO SHA-256 `66D6FF026DF81A91043DECA71EDC7998754E8B300B37FEE16993B5BA0230E3C1`. The worker checkpointed the prior `l1-cpuid` PASS and captured the [new framebuffer](images/l1-npt-write-039.png). It did not report queue COMPLETE.

| Mapping at GPA `0x2000` | Observed exit | Saved RIP | Backing byte before → after |
| --- | --- | --- | --- |
| Writable | HLT `0x78` | `0x1005` (after the store) | `0x5A` → `0xA5` |
| Read-only | NPF `0x400` | `0x1000` (at the store) | `0x5A` → `0x5A` |

Both entries ran the same `MOV byte ptr [0x2000],0xA5` on the same vCPU after `ExitBootServices`. Between entries L1 reset the owned data byte, cleared only the NPT leaf's write bit, reset the guest RIP, and requested the full local flush (`TLB_CONTROL=1`). The ASID, nested root, code and data frames did not change.

The protected exit reported `EXITINFO1=0x0000000100000007` and `EXITINFO2=0x2000`. Bits 0, 1 and 32 identify a protection violation on a write during final guest-physical translation; bit 33 was clear (not a guest page-table walk). The backing byte was read from L1's owned frame, not through the fault address. L1 stopped without resuming or emulating the store. See AMD64 APM Vol. 2 §§15.16 and 15.25.7. No cross-vCPU or bare-metal result is claimed.

### 040 · 2026-11-25 local · Standalone backing-frame remap and baseline batch

**Remap firmware PASS; batch ATTENTION on the fifth profile for first-time framebuffer review.** Command: `python lab/hv_queue.py run --session --suite standalone --rebuild`. Nested `HV-SVM-L1` on the Ryzen 9 9950X; firmware source `1e158b606d`, suite revision `d487be34f4`, batch `1581c486491042e4add188872e1e9efb`. The worker checkpointed the prior `l1-npt-write` PASS. All five profiles booted in order without a per-test launch; the first four produced queue PASS, not merely successful builds.

| Profile | Evidence | ISO SHA-256 |
|---|---|---|
| `l1-handoff` | Ordered COM1 transcript and reviewed terminal | `AA9D19D828E532D0DA479AC7832E52F7025D29C6E1D5E867129E34FFA00F49E8` |
| `l1-l2` | Reviewed guest HLT/marker screen | `4B3B5C37EFB8B9491C39D462E6686DC206673EC4E424ACB9B114853BE9CBC94B` |
| `l1-cpuid` | Reviewed two-entry CPUID resume screen | `F449D1C8471BA561FF638D9F91D8BD829B65B5A1D9262A70375DC9209578C158` |
| `l1-npt-write` | Reviewed writable/read-only pair | `1726FDE4308EDB19FD6B4EF04E930F297E919AFEB5A1B8D2864AC485DCB18D47` |
| `l1-npt-remap` | [New screen reviewed after capture](images/l1-npt-remap-040.png) | `D3F42C4E644AA71F428788913C5D4E194F379E2B4AF8229515D9B549D9D557E2` |

After `ExitBootServices`, L1 seeded two distinct owned pages once. `MOV AX,[0x2000]; HLT` ran twice, starting with full RAX zero each time. Frame A returned `RAX=0xA11A`; frame B returned `RAX=0xB22B`. Both exits were `0x78` at RIP `0x1003`. Final L1 backing reads were `0xA11A` and `0xB22B`. Only the leaf address bits changed on replacement; the read-only permissions, guest code, ASID and NPT root did not. Both entries requested a full local flush (`TLB_CONTROL=1`); see AMD64 APM Vol. 2 §15.16.

This verifies same-vCPU behavior **with** invalidation. It does not establish that omitting a flush would fail on this run, that a remote vCPU saw the remap, or that the physical 9950X3D is qualified. The UEFI `npt-remap` control rebuilt against the same fixed fixture but was not booted here.

### 041 · 2026-11-25 · Standalone runtime package and two-processor control

`HV-SVM-L1`, Hyper-V on Ryzen 9 9950X. Built `firmware\build.cmd l1-package` from the changes in `79982549af`, then ran `python lab/hv_queue.py run --session npt-cross l1-package` using the existing two-processor control. Batch `1236321e451f46b180605eb9bb146ebe` checkpointed the prior remap PASS. The two-CPU profile passed automatically. The package booted at 16:49:32 UTC; after 60 seconds the worker captured an unknown screen and stopped with `ATTENTION`, exit 3. The [live framebuffer](images/l1-package-041.png) meets the predeclared four-part contract. **Firmware PASS by review, not queue COMPLETE.**

| Check | Required and observed |
|---|---|
| Legacy vCPU state | `VMMCALL` exit `0x81` at `0x1028`, AX `0x46504332`; L1 resumes at `0x102B`. Second exit `0x78` at `0x1082`, AX `0x50415353`. Both entries compared L1 and L2 GPR/x87/MXCSR/XMM banks; resumed SSE and x87 calculations returned `1` and `12`. No AVX/XSAVE claim. |
| Unsupported request | CPUID exit `0x72` at `0x100C`, still on the faulting instruction with `EAX=0x400000FF`. One entry, no emulation, no second entry. |
| Guest exception | `#UD` exit `0x46` at `0x100C`. L1 left the fault RIP unchanged for injection; the guest handler advanced its own stacked RIP past `UD2`. Handler count `1`, continuation phase `3`, terminal HLT RIP `0x104F` (not the IRETQ destination). |
| Private-memory boundary | Write to `0x2FFF` committed `0xA5`; write to `0x3000` produced `0x400` at RIP `0x1005`, INFO1 `0x100000006` (nonpresent final write), guard `0xF1358B76D94A20CE` unchanged. Root and all 2,048 table entries matched the allowlist before and after. |
| Interrupts and guest TX | IF `0` with request pending; STI-shadow return `0x100C`. Equal TPR `4` held the next request; lowering to `3` yielded `0x1013`. Intercepted HLT at `0x1013`, then TSC one-shot due; interrupt return `0x1014`. Three IRQs, three guest EOIs, one post-wake increment. Ten byte OUTs yielded `L2:UART:TX`. Independent terminal HLT `0x78` at `0x1060`, AX `0x50415353`. No host serial passthrough or APIC emulation. |
| [Separate two-CPU control](images/npt-cross-041.png) | Locally measured APIC IDs `0` and `1`, A/A (`0xA11A`), then B/B (`0xB22B`) after quiesced remap and local flushes. Four HLTs at `0x1003`. Uses the Limine payload, not SMP support in `l1-package`. |

ISO SHA-256: `l1-package` `AEDF74D3E4C8E9E416A0C6B78C05BE3003C0FFECA4CF1BB63CFD8403795DECFD`; `npt-cross` `9D7A5434BD3A5253155DBD4318E362CF8C30EDD0A090162A2C0E1EB99F3AAB24`. Build environment: Visual Studio installation `18.6.11822.322`, EDK II `fbe0805b2091393406952e84724188f8c1941837`, pycdlib `1.22.0`, pyfatfs `1.1.0`. No physical boot, Linux userspace, calibrated timer frequency or multi-tenant isolation is established by this test.

### 042 · 2026-11-25 · Pinned diskless Linux inputs and host handoff verification

**Host PASS only.** Recipe `4fa5cdfd3f`, layout `f8c6295a72`. On Windows x64, `python lab/linux/prepare.py` read the checksum-pinned local Alpine 3.24.4 virt ISO. A second invocation targeting `build/linux-inputs-repeat` produced the identical `SHA256SUMS`. `tar.exe -tvf build/linux-inputs/initramfs.cpio` showed the executable `/init`, BusyBox, musl, and guest console device node. This did not mount media, boot an image, or run PID 1.

| Input | SHA-256 |
|---|---|
| Alpine ISO | `157d353f8e54252cad2d9812a0d56bb7fa2231fc154a110e1d180d7480ef8b4f` |
| Linux `vmlinuz-virt` | `1e8ac7aebfab3f0fcb443eae94ad320efbbda51be91b928c32ca1740b44ebe9d` |
| `config-6.18.60-0-virt` | `29636aa39722a1e48b440f3b48bd3f579f88f7f498552789a735791235e8050b` |
| `initramfs.cpio` | `99db7e0bb0742619d85deaab031ff7a539cd8e46cb6b7dd8ed8dd01196e3baa4` |
| NUL-terminated `cmdline.bin` | `5161eac9cd95353e2d47917e60e46bef0630b64f4bc2645f4cffd2b17f1bf958` |

Ran the [MSVC host verifier](../lab/README.md) with those three input files. It materialized real payload bytes after the `0x4000`-byte setup section in a 128 MiB buffer: kernel `0x1000000+0xC0C400`, initialization window `0x260A000` bytes, initrd `0x7E97000+0x168964`, command line `0x11000+123`. Entry `0x1000200`, RSI `0x10000`, CR3 `0x13000`, GDTR `0x12000:0x1F`, CS/DS `0x10/0x18`, RSP `0x18000`. Verified all initial guest-paging entries, the untouched/unmapped reserved hole, e820, and rejection of truncated or overlapping layouts. These guest page tables are **not** proof of NPT isolation.

The userspace oracle is ordered `HV:P5:USER pid=1 kernel=6.18.60-0-virt`, `HV:P5:TIMER` with measured forward progress across a two-second sleep, and `HV:P5:SHUTDOWN`, followed by a distinct L1-observed poweroff operation. None of those events occurred in this host test.

### 043 · 2026-11-25 · Linux first-fetch admission failed

**Firmware FAIL; queue ATTENTION, exit 3.** Source `140ac045dd`; `firmware\build.cmd l1-linux-admission`, then `python lab/hv_queue.py run --session l1-linux-admission`. Nested Ryzen 9 9950X. Batch `184bfa9f9e6943579cf08854e34f9813`, ISO SHA-256 `610041BD3F138A32733BB6FBEBC762EA7F004095524C527DB54423127153FF6F`. The worker recognized the preceding `l1-package` PASS and saved `HvQueue before 184bfa9f9e6943579cf08854e34f9813`. No rollback or retry was performed.

L1 read the three pinned inputs from the frozen ISO before `ExitBootServices`. After handoff it populated owned guest RAM, built and checked a 67-page NPT with NX on every present leaf, left the e820 hole absent, and issued one VMRUN. The fail-closed oracle required a final protection NPF on an instruction fetch, exact fault GPA and saved RIP `0x1000200`, intact entry state and private guard. The guest trap flag plus intercepted `#DB` provided a secondary stop, not an alternative pass.

| Observation | Measured on the [failed terminal](images/l1-linux-admission-043.png) |
|---|---|
| VMEXIT | `0x400` (nested page fault) |
| EXITINFO1 | `0x100000015`: present, user, instruction fetch, final GPA; not a guest-table walk |
| EXITINFO2 versus RIP | `0x1000000` versus `0x1000200` — differs by `0x200` |
| Entry values | RSI `0x10000`, CR3 `0x13000`, initrd `0x7E97000` |
| Private guard | `0x50354E5847554152` unchanged |

The whole-tree and private guard checks succeeded before the code returned `ADMIT FETCH NPF`. Neither a wrong mapping nor a Hyper-V defect is established by the byte-offset difference. AMD64 APM Vol. 2 (24593 rev. 3.46), §15.25.7 p. 576 describes EXITINFO2 as the faulting GPA and defines the observed flag bits; that passage does not establish byte-offset behavior for this nested fetch. Keep the original assertion and this result. The next diagnostic is a **distinct** fixed NX-fetch experiment that varies the entry offset and page independently, recording raw EXITINFO2, saved RIP and an independent guest page walk. Do not turn this capture into PASS or retry over the evidence. No userspace boots, shutdown events, native hardware tests or release sign-off.

Media cross-check after the stop: reopened the digest-named ISO, verified its whole-file SHA-256 against the mount event, then hashed `VMLINUZ.BIN`, `INITRD.BIN` and `CMDLINE.BIN` from the embedded FAT volume. All three matched run 042's input digests. This verifies **this artifact** independently of the packer; the current EFI reader checks lengths and EOF, not cryptographic identity in the guest's staged memory. The framebuffer alone cannot authenticate a future kernel image.

### 044 · 2026-11-25 · Controlled nested fetch-address precision

**Question:** Does this virtual AMD platform report the exact byte GPA of a denied instruction fetch, its 4 KiB page base, or neither? AMD64 APM Vol. 2 §15.25.7 identifies the raw faulting GPA field; saved RIP is a separate architectural field. A Linux identity mapping alone cannot distinguish guest linear and guest physical addresses.

**Independent controls:** A and B start at different nonzero byte offsets in the same nonidentity guest mapping. C keeps A's offset but changes the guest physical page. Each arm has a fresh VMCB, a local flush, intercepted `#DB`, an independent bounded walk, and a pre/post NPT allowlist and private guard check. All three must report the same mode. No masking of raw `EXITINFO2`: another page, another offset on the right page, mixed modes, a changed RIP, or a non-final/non-fetch exit stops the test. Only then is the real Linux mapping constructed and tested against that mode. The 043 artifact and failure stand unchanged.

**Host verifier PASS:** compiled `lab/tests/fetch_walk_test.c` with MSVC `/std:c11 /O2 /W4 /WX` and ran `build\fetch_walk_test.exe`. Covered aliases, malformed guest tables, guest NX at all levels, 2 MiB and 4 KiB guest leaves and exact-versus-base discrimination, including rejection of an unrelated byte on the correct page. Firmware source `f3245dc608`. Build: `firmware\build.cmd l1-linux-admission`. VM command: `python lab/hv_queue.py run --session l1-linux-admission`.

**Firmware PASS by review; queue ATTENTION (first capture), not COMPLETE.** Ryzen 9 9950X, `HV-SVM-L1`, Hyper-V. Batch `5ac1618c175b4a0fa2f99d15565f7135` cold-booted ISO SHA-256 `B9212B7A580E7542C5225738E62F703812E9056DFADB6C828604AAA942FBADF9`. [Full framebuffer](images/l1-linux-admission-044.png). Each exit was `0x400` with INFO1 `0x100000015`: present final-GPA instruction fetch, not a guest table walk. No guest was resumed.

| Arm | Saved RIP | Independently walked byte GPA | Raw EXITINFO2 |
|---|---:|---:|---:|
| A | `0x81123` | `0x321123` | `0x321000` |
| B | `0x81678` | `0x321678` | `0x321000` |
| C | `0x82123` | `0x634123` | `0x634000` |
| Linux entry | `0x1000200` | `0x1000200` | `0x1000000` |

The raw field tracked the 4 KiB **guest physical page**, not the guest linear address or byte offset. The Linux arm agreed with all three controls, with RSI `0x10000`, CR3 `0x13000`, initrd `0x7E97000` and guard `0x50354E5847554152`. This explains the exact-byte mismatch in run 043 on this nested configuration. It does not identify whether L0 or the CPU chose the granularity, say anything about write-fault precision, or qualify a native AMD platform. Linux remained non-executable; zero boots to PID 1.

### 045 · Staged Linux input identity before guest entry

**Contract:** after `ExitBootServices`, compare SHA-256 of the actual private staging bytes with the three pins compiled into L1. Mutate one byte without changing the input length; call the real admission path; restore the byte before testing the next input. Require rejection of the specifically altered artifact before any `VMRUN`. With all bytes restored, the same gate must accept and reproduce run 044's four NX fetch exits. Count attempts at the instruction site, not at the request dispatcher. This test does not grant Linux execute permission.

**Host verification:** compiled `lab/tests/sha256_test.c` with MSVC `/std:c11 /O2 /W4 /WX`; `build\sha256_test.exe` exited 0 on five known-answer vectors: empty, `abc`, 55 bytes, 56 bytes and a 256-byte binary message. `firmware\build.cmd l1-linux-admission` and `firmware\build.cmd l1-linux-inputs` both built from source `4c4ee9bbda`. The normal profile includes the hash gate; only the trial includes the deliberate byte changes and VMRUN counter. The staged pins are the three input digests from run 042, not values supplied by the boot volume.

**2026-11-25 · Firmware PASS by framebuffer review.** Nested `HV-SVM-L1` on the 9950X, batch `8913ff0eb7544e6dba6f2e41a6cba424`, `python lab/hv_queue.py run --session l1-linux-inputs`. The broker checkpointed the run-044 PASS and booted ISO `3866B0BC8521F6C975FE4C893BE5906B2004D90E8AF1D10D0663F353B8E578E5` at 17:17:35 UTC. As this was a new profile, the queue stopped at `ATTENTION` (exit 3) and preserved the [first capture](images/l1-linux-inputs-045.png); it did not report `COMPLETE` or retry.

| In-memory change, same input length | Observed rejected input ID | Measured VMRUN attempts |
|---|---:|---:|
| Kernel, low bit of last byte flipped | 1 (kernel) | 0 |
| Initramfs, low bit of last byte flipped | 2 (initramfs) | 0 |
| Command line, low bit of first byte flipped | 3 (command line) | 0 |
| Original bytes restored | 0 (none) | 4 |

In the restored arm, all four exits were `0x400` with `EXITINFO1=0x100000015`. For the Linux entry, saved RIP and independent walk were `0x1000200`; raw `EXITINFO2` was `0x1000000`. Guard `0x50354E5847554152` remained intact. The counter was incremented immediately before the assembly `VMRUN`, so a rejection with no entries is distinct from a guest that ran and then faulted. The measured gate authenticates these staged bytes relative to the trusted firmware's pins, not the firmware or boot chain. No executable Linux boot or physical qualification.

After review, `Get-HvConsoleVerdict` classified the archived 045 framebuffer as `PASS` and the prior 044 framebuffer as `UNKNOWN` for `l1-linux-inputs`. This was an offline check of those captures, not a second boot.

### 046 · Single-vCPU contract and terminal MSR refusal

**Pre-boot criteria:** on the standalone AMD/NPT runtime, a fixed 64-bit guest must observe `AuthenticAMD`, one logical processor, an explicit hypervisor bit and lab vendor, the legacy feature mask `EDX=0x07000001`, no XSAVE/AVX advertisement, and zero for unsupported leaf 7. The guest must use SSE2 state from before a CPUID exit to calculate 42 after resume. L1 must independently observe four CPUID exits followed by a read-MSR intercept for `0xDEADBEEF` at the fixed instruction, with restored L1 state each time. No emulated MSR read, RIP advance, or re-entry at the terminal exit. No Linux payload or claim of a complete Linux CPU model.

**2026-11-25 · Firmware PASS by framebuffer review.** `firmware\build.cmd l1-cpu-contract` at `e27542112e`; `python lab/hv_queue.py run --session l1-cpu-contract`. The broker checkpointed the reviewed 045 terminal and booted ISO `7CB444575A4C98988C97406533AE27F098C2F375DEA73524889D52BC25F2B23D` in `HV-SVM-L1` at 17:25:34 UTC on the 9950X. Batch `7f7381905f8d4144899307b6fc8a8d2b` stopped at `ATTENTION` (exit 3) on the first unregistered terminal. No retry. [Fresh console capture](images/l1-cpu-contract-046.png).

- **Guest:** four verified CPUID replies (leaves `0`, `1`, `0x40000000`, `7`, all subleaf 0). Observed logical count 1, ECX `0x80000000`, EDX `0x07000001`. At the fourth exit XMM0 contained `0x15` (21); after resume L2 doubled it with `PADDD` and recorded `0x2A` (42) and its success cookie `0x43505531`.
- **L1:** five entries, five checked restorations of caller and seeded legacy register state. The fifth exit was `0x7C`, `EXITINFO1=0` (read), RIP `0x10EE`, RCX `0xDEADBEEF`. The dispatcher ended there without reading that MSR, advancing RIP, or entering a sixth time.
- **Limit:** these are CPUID advertisements and state-switch observations for one fixed guest. They neither enforce AVX instruction unavailability for arbitrary code nor establish a complete Linux CPU model, physical compatibility, or a userspace boot. No extended-state or APIC topology qualification.

### 047 · Polled 8250-compatible serial output

**Pre-boot criteria:** the fixed L2 must use COM1's divisor-latch aliases without transmitting divisor bytes, select 8N1 and divisor 1, leave interrupts disabled, see non-FIFO status, and observe one busy-to-ready transition per byte. After the last byte require line status `0x60` (THRE and TEMT), guest cookie `0x82504701`, and a success HLT. Independently require L1 to capture exactly `4C 32 3A 38 32 35 30 0A` in order, not text printed by L1. A separate fixed arm must stop on its first byte OUT to `0x2F8` (COM2), without changing the capture, forwarding it, or resuming. No Linux boot or physical serial I/O.

**2026-11-25 · Firmware PASS by framebuffer review.** Built `firmware\build.cmd l1-serial-8250` at `f7e0431157`; ran `python lab/hv_queue.py run --session l1-serial-8250` in the nested 9950X lab. Batch `1aa334e6324c43039c23009df0f84f56` checkpointed the reviewed 046 state and cold-booted ISO `1FD786103955F1108FC7370DEEF12DC941B6E310DBF8D14F5B0F9E392465C133` at 17:34:13 UTC. The first unregistered [terminal capture](images/l1-serial-8250-047.png) stopped the queue at `ATTENTION` (exit 3), not `COMPLETE`. No retry.

| Witness | Observed |
|---|---|
| Guest readbacks / poll | Cookie `0x82504701`, LSR `0x60`, eight busy samples. Guest checked DLAB aliases, scratch, modem control, non-FIFO status and AL-only read semantics before writing the cookie. |
| L1 virtual COM1 | Captured `4C 32 3A 38 32 35 30 0A` from the transmit register; count 8, busy reads 8, divisor 1; LCR/IER/MCR/SCR = `03/00/03/5A`. No extra byte from divisor programming. |
| Independent positive stop | 44 entries (`0x2C`); exit `0x78` (HLT), RIP `0x1172`, RAX `0x50415353`. |
| Refused COM2 arm | Exit `0x7B`, `EXITINFO1=0x02F80210`, RIP `0x11A5`, next IP `0x11A6`, DX `0x2F8`, AL `0xA5`. Refusal 1; no RIP advance or re-entry; final count and captured bytes unchanged. |

Scope: fixed guest and one-way, polled COM1 subset. Read-driven busy intervals are deterministic test behavior, not baud timing. No UART IRQ, receive stream, physical wire, Linux earlycon/tty driver, or PID 1 was run.

### 048 · PIT one-shot, legacy IRQ0 and a specific EOI

**Pre-boot contract:** use two distinct PIT channel-0 one-shots on a controlled tick timeline (32 then 64 PIT ticks), never host port I/O. Latch the first edge while IRQ0 is masked, deliver vector `0x30` only after unmask, and save the hardware-stacked return IP in L2. Arm the second deadline and reach HLT before it; resume at HLT+1 only when due. Require two ISR executions, two guest acknowledgements via master PIC command `OUT 0x20,0x60`, second post-wake progress and terminal guest cookie `0x04871001`. A separate arm's out-of-contract EOI `0x61` must not retire IRQ0 or resume. Observe negative/boundary cases on host. Not a kernel boot, real-time calibration, LAPIC timer, full 8259 model or general hardware compatibility result.

**2026-11-25 · Firmware PASS, reviewed [048 capture](images/l1-pit-irq0-048.png).** Built with `firmware\build.cmd l1-pit-irq0`, verified `lab/tests/pit_pic_test.c` under MSVC `/O2 /W4 /WX`, then `python lab/hv_queue.py run --session l1-pit-irq0`. Batch `635d3b98d9de4aafaca281ceb9b598ef`, ISO `6CC2D9E1DDF181B11ABD594BD23A044D6C1AB64FA35A629AB04F5B4E30A2FC8F`, boot 17:51:47 UTC. The broker checkpointed the prior 047 terminal and stopped at first-capture `ATTENTION`, exit 3. No VM retry.

At virtual tick 31 no expiry; at 32 IMR=`FF`, IRR=`01`. Hardware delivered vector `30h` after unmask, stacking `1037h`. The second HLT was at `10A6h`; no wake at tick 95, then tick 96 stacked `10A7h` and resumed. Two EOIs, two L2 ISR entries, one post-wake marker, cookie `04871001`, successful HLT at `1120h`. An independent arm refused byte `61h` on port `20h` at `1184h`. Both arms together used 19 VM exits. Logical expirations are `arm + N`, not a physical 8254 output waveform or a measured guest TSC frequency. The pinned kernel still needs a timebase, discovery/initialization and calibration not exercised here.

### 049 · Two-phase console and bounded capture

**Pre-boot contract:** L2 transmits `EARLY:console!\r\n` then `LATE:terminal!\r\n`. Full arm: match all 32 bytes and L2's completion cookie `B060CAFE`, drained line, independent success HLT. Short arm: capacity 24; refuse byte 25 (`6D`) at the OUT before RIP advance; preserve exactly the first 24 bytes and no terminal guest cookie. No actual console handover by Linux.

**2026-11-25 · Firmware PASS, reviewed [049 capture](images/l1-console-tx-049.png).** Built `firmware\build.cmd l1-console-tx` and ran `python lab/hv_queue.py run --session l1-console-tx`. Batch `3a30e97c38b043368eb58a28ae9710c1`, ISO `1A26E6B6B703E505DA8097D8B28BD6A0AFE1BC0D35F0DB8A21B74D07495DB6CB`, boot 17:53:49 UTC. Prior 048 checkpointed; first new terminal `ATTENTION` (3). No VM retry. Full 32 bytes in four wire-order words: `4541524C593A636F 6E736F6C65210D0A 4C4154453A746572 6D696E616C210D0A`. Guest cookie `B060CAFE`, final hash `765B6B87`, independent HLT RIP `1006h` after 98 entries. The short arm stopped on entry 74 at OUT `1004h`, AL=`6Dh`, first 24 bytes unchanged, overflow=1, no guest cookie. This extends 047's init/register checks with a bounded transcript, not an interrupt-driven tty test.

### 050 · Virtual S5 request versus guest text and HLT

**Pre-boot contract:** identical guest serial marker in both fixed arms. Accept a documented 16-bit PM1a preparation and S5 enable write on guest-only port `0x604`, and stop on the latter without re-entry. The comparison arm with no valid enable request must not reach the shutdown state, even if it prints the same marker and halts. Record terminal exit/RIP, data and latch from the decoder separately from the marker. No host power, firmware or ACPI namespace execution.

**2026-11-25 · [050 capture](images/l1-power-s5-050.png), firmware PASS by review.** Build profile `l1-power-s5`, batch `308ca8df03ea41efaa1f43e3e8e63977`, ISO `E16D7190184B71BC63D2830585BCFFCEFF2DBBB39EE0E031D599F563E55A844F`, cold boot 17:56:24 UTC. Same six bytes `4C323A53350A` and guest cookie `53354701` in both arms. No enable: PM1a `1401h`, 22 entries, HLT exit `78h` at `1073h`, waiting state 1. S5 enable: `3401h`, 21 entries, IOIO exit `7Bh` stopped on the OUT at `1066h`, stopped state 2. Neither HLT nor guest text was accepted as shutdown. First capture ATTENTION, no retry. Prior 049 PASS checkpointed by broker. Host `power_tables_test` accepted the guest AML and tables and refused corrupt and dangling references; this guest did not discover them through Linux.

### 051 · Pre-entry state guard for the pinned NX fetch

**Pre-boot contract:** after the three independent 044 fault-precision controls, substitute an invalid RSI and then an out-of-RAM CR3 into the actual Linux entry state. Each must report the corresponding guard error and zero new assembly-site VMRUN attempts. Restore the real state; the same gate must permit exactly one attempt, followed by the original final-fetch NPF, unchanged entry RIP, independent walk and private guard. This qualification keeps Linux pages NX; no code from the kernel may execute.

**2026-11-25 · [051 capture](images/l1-entry-guard-051.png), firmware PASS by review.** `firmware\build.cmd l1-entry-guard`; `python lab/hv_queue.py run --session l1-entry-guard`. ISO `AA9DA8340ACB3542473E4196C4C64862B52B1651EF2AFEA491F3D6D677E8E961`, batch `067e69eca606420086669749bcbc4f8a`, boot 17:58:34 UTC, 050 checkpointed. RSI refusal 8 and CR3 refusal 3, both with zero new entries. Valid-path CR0/CR4/EFER=`80010031/20/1D00`, RSP=`18000`, GDTR base=`12000`. One attempted NX entry: NPF `400`, INFO1 `100000015`, raw GPA `1000000`, saved RIP and walked GPA `1000200`, RSI `10000`, CR3 `13000`, initrd `7E97000`, private guard unchanged. Three precision controls completed first; “zero” is per refused arm. No Linux instruction executed. Host `entry_guard_test` and `linux_entry_test` passed. First frame ATTENTION, no retry.

Independent readback (`lab/linux/verify_media.py`) from the frozen 33,607,680-byte 051 ISO, not the packer's input directory: the EFI boot catalog selected the inspected FAT image. Kernel (12,649,472 bytes), initramfs (1,476,964), and command line (123) matched the three published 042 hashes. Substituting an all-zero ISO digest failed before parsing.

### 052 · Single-vCPU feature floor across exits

**Pre-boot contract (hex):** seven intercepted CPUIDs at 1010 through 1070 in steps of 10; inputs `0,1,1,80000000,80000001,40000000,7`, secondary leaf-1 request with ECX=A5A5A5A5. FMS `20F32`, bits `07008161`, extended `21008161`; CMPXCHG8B outcomes `9:9`, ZF `1:0`, CMOV `22:33`, NOP witness `6E6F706C`. XMM0 input 15, guest SSE2 result 2A, cookie 464C5231. Eight entries and eight caller restores. Stop on read MSR at RIP 1080, EXIT=7C/INFO1=0, RCX=DEADBEEF, RAX=2A. No ninth entry, native leaf passthrough or Linux boot.

**2026-11-25 · [052 capture](images/l1-cpu-floor-052.png), firmware PASS by review.** Profile `l1-cpu-floor`, batch `893b7e2d1dc948fdb3fb6bca117ac2a7`, ISO `3F53FB1DAB67E719B930BE5C324D6599C1873E38D5E2935B4E736F4DF9DF5E88`, boot 18:00:39 UTC, prior 051 checkpointed. Seven request pairs including both leaf-1 subleaf values; independent guest reports `00020F32:07008161`, extended `21008161`, CX8 `9:9` with different ZF, CMOV `22:33`, NOP `6E6F706C`, SSE2 `2A`, cookie `464C5231`. Eight entries/eight caller restores, read MSR exit `7C`/INFO1 `0` at `1080`, RCX `DEADBEEF`, RAX `2A`. First-capture ATTENTION, no retry. No syscall/privilege-transition, guest TSC calibration or Linux compatibility claim.

### 053 · Paired guest and nested page faults

**Pre-boot contract (hex):** five fresh one-entry arms. A: owned RX fetch halts 78/1812E/50353038. G: guest NX returns intercepted #PF 4E/RIP 18123/error 11/raw GVA 18123; separate leaf walk GPA 8123. N: nested NX returns NPF 400/RIP 19123/final fetch/raw GPA page 9000; walk 9123. W: accepted store halts at 18137 with byte B6. R: denied write at 18134 returns final write-protection NPF/raw GPA A000 with byte 5A unchanged. Ignore only the NPF U/S bit, not the fault stage or address. Private guard and out-of-map witness remain unchanged; five total assembly-site entries. Fixed layout, not executable Linux RAM.

**2026-11-25 · [053 capture](images/l1-page-pair-053.png), firmware PASS by review.** Profile `l1-page-pair`, source `47f99e5582`, batch `1cf395a53953466f8e02d72d2ff1000d`, ISO `A773C63DC7DDDEE5C16EB15E65E88E79F7344E47DB896F9E21A61FCF8B056068`, boot 18:02:48 UTC. Five arms/five VMRUNs. Allowed code halted at `1812E` with marker `50353038`. Guest NX: intercepted #PF `4E`, error `11`, raw guest linear `18123`, walked GPA `8123`. Nested NX: `400`, info `100000015`, raw `9000`, walk `9123`. Permitted write produced `B6`, HLT `18137`; the denied write stopped at `18134`, `400/100000007/A000`, byte remained `5A`. All addresses hex. First-capture ATTENTION, 052 checkpointed, no retry.

**Witness wording:** the private guard is outside the NPT. The separate data witness is *mapped* at GPA `A000h`; its byte is allowed to change only in W and its marker only in A. The pre-boot shorthand “out-of-map witness” is not evidence of a second private canary. The implementation checks the full data and guard pages in every arm.

**Reproduction notes, 048–053.** Each command is `firmware\build.cmd PROFILE` followed by `python lab/hv_queue.py run --session PROFILE`. Runs used the single fixed L2 inside `HV-SVM-L1` on the 9950X. Each batch first checkpointed its preceding terminal under `HvQueue before <batch-id>`. The per-run group committed the fixture implementation separately from its screen. 048–050 were built from the working tree based on `cbc9c26db6`; corresponding fixture commits are `cdb5eb6d1c` (timer), `060b8551fc` (TX), `667df46498` (S5), with their common integration in `ac6f534bbc`. Guard source `404bee377e` plus final integration `47f99e5582` covers 051. `47f99e5582` contains the sources for 052/053. These are source references, not claims that timestamped EFI/ISO rebuilds are byte-identical. The per-run ISO hashes bind the actual binaries.

For each of the six new profiles, the offline classifier accepted its actual frame and returned `UNKNOWN` for the immediately preceding different profile. Its reviewed pixel digest is in `lab/hv-profiles.json`. These are not six queue `COMPLETE`s: the queue correctly retained each previously unknown frame. Independent host checks also rejected a changed first or last byte of the pinned relocated entry page, the wrong guest frame and a modeled writable alias. The relocated-page check does not enumerate live NPT aliases or flush a TLB.

### 054 · Regression of staged identity through the new handoff guard

**Before boot:** rebuild `l1-linux-inputs` after the shared admission gate change. Retain 045's original fingerprint, not 051's. Each same-length kernel/initramfs/cmdline substitution must be refused before VMRUN; restored inputs must produce the original four NX-only entries including the three calibrated fetch controls. A fresh PASS from the unchanged 045 classifier, frozen ISO and independent readback are required. No boot of executable Linux.

**2026-11-25 · Queue PASS → COMPLETE; [fresh 054 capture](images/l1-linux-inputs-054.png).** Ran `python lab/hv_queue.py run --session --rebuild l1-linux-inputs` on `HV-SVM-L1` with shared admission code from `47f99e5582`. Batch `bdd5b606eca9439080e460e26fe7cff1`, ISO `36818EBC9EFB78D8A9A44C65380681FF6C00FF6829D49A22965F1D399EA403C3`, boot 18:05:10 UTC. The broker saved `HvQueue before bdd5b606eca9439080e460e26fe7cff1` from reviewed 053. Each corrupted input gave its own ID (1, 2, 3) and zero entries; restored ID 0, four NX attempts and unchanged private guard. The original 045 pixel digest matched live; offline the fresh 054 frame gave PASS and 053 gave UNKNOWN. Read back the mount-bound 33,607,680-byte ISO: all three FAT hashes/sizes matched 042. This is an affected-control regression, not another OS cold boot.

### 055 · Intercept a CR4 write before guest state changes

**Before boot:** two one-entry fixed guests, CR4-write intercept armed for both, initial CR4 `20h`. Read control stores `20h` in its witness, writes marker `B6h`, and halts at `1015h` with RAX `20h`. Refusal arm reads the same value, computes `40020h`, then attempts `MOV CR4,RAX` at `100Eh`. Require intercept exit `14h`, saved CR4 still `20h`, marker still `5Ah`, one VMRUN, unchanged L1 CR4. Do not resume, write a host register, run XSETBV, expose extra features or count this as guest #GP delivery.

**2026-11-25 · Firmware PASS, [055 capture](images/l1-cr4-guard-055.png).** Source `d574de2efd`, built `firmware\build.cmd l1-cr4-guard`; disassembled the 22-byte guest and verified the write and HLT instruction sites before issuing `python lab/hv_queue.py run --session l1-cr4-guard`. Batch `b1d26061eb6a4656b91befc68b7eb0d9`, ISO `264C1A50464A3B59CDA8EC24ABFF2F09DA836BF85A1B8B36723F2B7C5AF03D56`, boot 18:08:58 UTC. Prior 054 saved as `HvQueue before b1d26061eb6a4656b91befc68b7eb0d9`. Read: HLT `78h` at `1015h`, RAX/seen/CR4=`20h`, marker `B6h`. Write: `14h` at `100Eh`, RAX=`40020h` but seen/CR4 remained `20h`, marker `5Ah`. One entry per arm. First capture ATTENTION, no retry. This terminal refusal is not guest exception injection, general CR4 emulation or isolation of the syscall MSR bank.

### 056 · Deliver a general-protection fault after a refused CR4 write

**Pre-boot contract:** no-write arm: one entry, zero handlers, untouched error poison, cookie `47505254h`, HLT. Matched write: intercept `14h` before effect, leave RIP at MOV CR4, inject vector 13 with valid zero error code. Exactly one guest handler must observe error zero, stacked faulting IP, stack `7FC0h` with correct CS, old SS:RSP. Only the handler selects the fixed continuation. Require IRETQ, original RSP `7FF0h`, cookie, independent HLT and two entries. Both guest CR4 readbacks `20h`; L1 CR4 unchanged. No execution of XSETBV or host CR4 write.

**2026-11-25 · [056 capture](images/l1-cr4-gp-056.png), firmware PASS by review.** Source `688abd8d55`; build `firmware\build.cmd l1-cr4-gp`, run `python lab/hv_queue.py run --session l1-cr4-gp`. Batch `d3881d80c13e4ace8069f4cf2a823038`, ISO `01C9CF566BA464B3C0A1E0CAA0181BBEEAC129C0D4B306193CF9E328722F93F3`, boot 18:16:25 UTC, prior 055 checkpointed. No-write: all-ones error poison, handler count zero, one entry. Write: `14h` at `100Fh`, handler count one, error zero, stacked RIP `100Fh`, stack `7FC0h`. Both reached `HLT 1020h` with CR4 `20h` and the continuation cookie. Write arm took two entries. Queue ATTENTION on its first frame; no VM retry.

### 057 · Publish guest-owned ACPI tables without granting execute

**Pre-boot contract:** pinned Linux NX staging, three old fetch controls. Materialize six E820 regions; RSDP at `90000h` on read-only/NX guest backing, FACS at `91000h` in writable/NX NVS. Substitute an RSDP in the PC hole and, separately, mark NVS as free RAM: distinct rejections and zero *new* VMRUNs. Restore both: one final-fetch NPF on the original pinned entry, expected raw GPA/walk and intact private guard. ACPI graph must still validate. No kernel discovery or AML execution is inferred.

**2026-11-25 · [057 FAIL](images/l1-acpi-map-057.png), not an ACPI pass.** `12fc8072d9`, `firmware\build.cmd l1-acpi-map` then `python lab/hv_queue.py run --session l1-acpi-map`. Batch `42eb432cb1ec453dadb48e1087cc3431`, frozen ISO `FB72771955B22FD6BA83FE2F51F043DAF216DD5A4A0D3F12A7C52093D83A0C3E`, boot 18:18:24 UTC; `HvQueue before 42eb432cb1ec453dadb48e1087cc3431` retained the reviewed 056 terminal. The three fetch controls and `BAD ROOT 13h` were visible. The next label, `ROOT ENTRIES `, has 13 characters, while `pkg_observe` allowed only 12. Its `OBSERVATION LIMIT` stop prevented the remaining observations. Root entry delta, NVS refusal and restored pinned fetch **were not observed**. First-capture ATTENTION. Failed pixels were not added to the pass manifest. The independent host publisher test passed; it cannot replace this VM result.

### 058 · ACPI observation-width correction

Reserved, **not run**. The host formatter accepts the previously rejected 13-character label; the firmware must still satisfy the unchanged 057 guest oracle through the final guarded fetch. Candidate ISO `c99f522905d8c038dd16fbf647a339d71278b97e053db3d92dc304c6b53f1cb6`. Recovery of the original VM stopped after the failed-state checkpoint request and before restore; do not claim 056 was restored or 058 passed.
