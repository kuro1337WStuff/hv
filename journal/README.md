# Hypervisor development journal

Development host: Ryzen 9 9950X. Firmware runs use the `HV-SVM-L1` Hyper-V VM; host-only checks are labeled separately. Neither qualifies the 9950X3D deployment target on bare metal.

## 036 · 2026-11-25 · L1 reports an allocation failure after UEFI handoff

**Passed in `HV-SVM-L1`.** After `ExitBootServices`, L1 switched to its private stack and verified a page from its own arena. It rejected a nine-page request without advancing the allocator: the next one-page allocation returned exactly the second arena page. The worker received all four expected lines on COM1, and the VM framebuffer independently displayed the terminal below.

![L1 HANDOFF PASS with matching handoff, ownership, expected OOM and halt markers](images/l1-handoff-036.png)

The queue stopped for first-time screen review. The [run record](test-report.md#036--2026-11-25-local--standalone-l1-handoff) identifies the tested image and serial bytes. No L2 guest or bare-metal boot was part of this test.

## 034–035 · 2026-11-25 · First direct L1 memory handoff image

**Image built; VM acceptance pending.** The new profile tests whether L1 can allocate memory, reject an oversized request, and report the outcome over COM1 after leaving UEFI boot services. The first build failed to link; the corrected image staged, but Windows canceled the worker elevation before any VM operation. There is no guest screenshot or serial result for these attempts. The [ledger](test-report.md#035--2026-11-25-local--standalone-l1-handoff-staged-not-booted) records the image digest and the exact criteria for the first boot.

## 032–033 · 2026-11-25 · One vCPU switches integer and floating-point state together

**Run 033 passed in `HV-SVM-L1`.** L2 changed its integer registers, XMM registers, x87 stack and rounding controls, then exited through `VMMCALL`. On resume it used the two preserved x87 values, `5` and `6`, to round `5 + 6 + 0.25` up to `12`. A separate SSE conversion rounded `0.25` up to `1` using MXCSR. The host recovered its own values after each exit. Restoring the guest's *initial* state would not have passed.

![Console showing distinct host and guest state at VMMCALL and HLT, and both resumed rounding results](images/vcpu-fpstate-033.png)

Reproduce with `python lab/hv_queue.py run --session --rebuild vcpu-fpstate`. The [ledger](test-report.md#033--2026-11-25-local--integrated-legacy-vcpu-context) records the image, exit addresses, and limits of this fixed guest. Run 032 stopped at assembly and did not change the VM.

## 029–030 · 2026-11-25 · Guest rounding mode survives a VM exit

**Run 030 passed in `HV-SVM-L1`.** L2 rounded `1.5` down to integer `1`, changed its mode to round up, and executed `VMMCALL`. After resume, the same conversion produced `2`. Each conversion set only the precision flag. L1 read its own live MXCSR after each exit and found its original rounding mode and status flags, not the guest's.

![VM console showing both integer results, MXCSR samples for L1 and L2, and the success halt](images/vcpu-mxcsr-030.png)

Reproduce with `python lab/hv_queue.py run --rebuild vcpu-mxcsr`. The [test record](test-report.md#030--2026-11-25-local--mxcsr-rounding-and-status-preservation) includes both exits; run 029 stopped during compilation without changing the VM.

## 028 · 2026-11-25 · Host and guest retain separate XMM registers

**Run 028 passed in `HV-SVM-L1`.** L2 checked both halves of XMM0–XMM15 on entry, wrote a different pattern, and exited through `VMMCALL`. On resume it found its own updated values, not the host's values or its initial pattern. It then wrote a third pattern and reached its success HLT. L1 sampled its live registers after each return: all 32 lanes matched its own pattern for that entry.

![VM console showing the VMMCALL and HLT exits with distinct guest and restored host values for XMM15.high](images/vcpu-xmm-028.png)

Reproduce with `python lab/hv_queue.py run --rebuild vcpu-xmm`. The [test record](test-report.md#028--2026-11-25-local--legacy-xmm-register-preservation) includes the measured lane values and the scope of the result.

## 027 · 2026-11-24 · The guest handles an invalid opcode and returns

**Run 027 passed in `HV-SVM-L1`.** L2 executed `UD2` at `0x100C`. L1 intercepted `#UD` and delivered vector 6 without advancing the guest RIP. The guest handler ran once and found `0x100C` on its exception stack, not the address after the fault. Only the handler moved that saved address past the two-byte `UD2`. After `IRETQ`, L2 checked its restored stack and reached the success halt at `0x104F`.

![VM console showing matching intercepted and stacked fault addresses, a single handler entry, and the final guest halt](images/exception-ud-027.png)

Reproduce with `python lab/hv_queue.py run --rebuild exception-ud`. See the [test record](test-report.md#027--2026-11-24-local--invalid-opcode-delivery-and-return) for the injection fields and image digest.

## 026 · 2026-11-24 · Equal interrupt priority stays pending

**Run 026 passed in `HV-SVM-L1`.** At `V_TPR=4`, a queued priority-4 interrupt remained pending even though guest interrupts were enabled and there was no interrupt shadow. The handler count was zero. L1 lowered `V_TPR` to 3 without queuing another interrupt. The handler ran once and saved return RIP `0x100F`, the first instruction after the checkpoint. The guest returned from the handler, recorded progress and halted.

![VM console showing the equal-priority checkpoint and one interrupt delivered after lowering V_TPR](images/irq-priority-026.png)

Reproduce with `python lab/hv_queue.py run --rebuild irq-priority`. The [ledger](test-report.md#026--2026-11-24-local--virtual-interrupt-priority-threshold) distinguishes the firmware result from the queue's first-screen review stop.

## 023–025 · 2026-11-24 · A pending IRQ waits for the STI shadow

**Run 025 passed in `HV-SVM-L1`.** L1 queued vector `0x41` once. At a guest checkpoint with interrupts disabled, the IRQ was still pending and the handler count was zero. L1 resumed L2 without requeuing it. The instruction after `STI` incremented a phase counter. The interrupt handler saw the increment and recorded `0x1018` as its return address: exactly the next instruction.

![VM console showing the IRQ pending with IF clear, one handler entry after the shadow, and a cleared pending bit](images/irq-shadow-025.png)

After `IRETQ`, the guest reached phase 3 and halted with its PASS marker. Run `python lab/hv_queue.py run --rebuild irq-shadow` to reproduce. The [ledger](test-report.md#025--2026-11-24-local--pending-vector-delivered-after-the-sti-shadow) records the image, registers, and two earlier build errors that did not touch the VM.

## 021–022 · 2026-11-24 · One interrupt wakes a virtual CPU

**Run 022 passed in `HV-SVM-L1`.** L2 reached HLT at `0x1002` with interrupts enabled, no handler entries, and no progress past HLT. L1 queued vector `0x41`. The guest handler ran once and recorded `0x1003` on its interrupt stack: the instruction immediately after HLT. After `IRETQ`, the guest checked its stack pointer, recorded forward progress and reached the success HLT at `0x1039`.

![VM console showing one handler entry, the stacked post-HLT RIP, and resumed execution](images/irq-wake-022.png)

The [ledger](test-report.md#022--2026-11-24-local--one-virtual-interrupt-wakes-l2) preserves the first fixture failure and corrected run separately. To reproduce: `python lab/hv_queue.py run --rebuild irq-wake`. This is an emulated guest halt and virtual interrupt, not a physical CPU sleep or a timer test.

## 019–020 · 2026-11-24 · A 64-bit vCPU keeps its integer registers across exits

**Run 020 passed in `HV-SVM-L1`.** After each of two CPUID intercepts, L2 checked the four emulated replies and all twelve other integer registers, including RSP. Different values in the second round prevent a stale first-round frame from passing. Only after both checks did the guest set `RAX=0x50415353` and reach the success HLT.

![VM console showing two CPUID exits followed by the guest's success HLT and PASS marker](images/vcpu-gpr-020.png)

Run: `python lab/hv_queue.py run --rebuild vcpu-gpr`. The [ledger](test-report.md#020--2026-11-24-local--two-64-bit-register-round-trips) records the earlier assembler error separately from the live run. This is one vCPU on the nested 9950X lab; SIMD/FPU state, interrupt delivery and physical hardware were not tested.

## 018 · 2026-11-24 · Distinguishing an attempted write from a committed write

**PASS in `HV-SVM-L1`.** With writes allowed, L2 stored `0xA5` and reached HLT. With the same page read-only, L1 reported a write protection fault. The instruction at the saved RIP decoded as `MOV byte [0x2000], 0xA5`; its destination matched the fault address reported by the processor. A separate read of the backing byte showed `0x5A` both before and after the attempt.

![VM console showing the writable control, decoded protected store, and unchanged backing byte](images/forensic-write-018.png)

Run: `python lab/hv_queue.py run --rebuild forensic-write`. The queue stopped for first-time screen review. [Run 018](test-report.md#018--2026-11-24-local--forensic-write-protection) records the exact image and exit fields. This verifies one fixed 16-bit instruction under nested Hyper-V, not general-purpose instruction decoding.

## 017 · 2026-11-24 · One nested GPA flush for both processors

**PASS in `HV-SVM-L1`.** Both L2s read `0xA11A` at GPA `0x2000` and halted. With both stopped, L1 remapped the page. Processor 0 issued a single Hyper-V all-processor flush for the shared NPT root and received status `0`. On re-entry, neither processor requested a local flush. Both read `0xB22B`.

![Console showing the two L1 IDs, reads before and after the remap, and hypercall status zero](images/npt-broadcast-017.png)

Run: `python lab/hv_queue.py run --rebuild npt-broadcast`. Hyper-V did **not** advertise native AMD `INVLPGB` to the L1 bootstrap processor. This tests the Hyper-V interface, not a physical broadcast instruction or interruption of a running guest. The queue stopped for first-time screen review; [run 017](test-report.md#017--2026-11-24-local--one-all-processor-nested-gpa-flush) records the image and verdict.

## 015–016 · 2026-11-24 · Two L1 processors see the new frame

The earlier UEFI image could not start a second processor. The new `npt-cross` image uses Limine's multiprocessor handoff instead. Code running on each L1 processor reported its own APIC ID before either entered L2: **0 on the bootstrap processor and 1 on the application processor**.

Both processors read `0xA11A` from GPA `0x2000`. With both guests stopped, L1 replaced the backing frame. Each processor requested its own local flush and read `0xB22B` from the same address. All four reads reached the expected HLT. The [test report](test-report.md) distinguishes the guest result from the queue's first-time screen review.

![Two L1 processor IDs and four guest reads across the remap](images/npt-cross-015.png)

After reviewing that screen, we booted the halt-only image and then booted `npt-cross` again. Both profiles passed and the supervised queue reported COMPLETE.

## 011–013 · 2026-11-24 · Watch a regression batch and veto its next boot

The [supervised runner](../lab/README.md) booted four known-good images in sequence, matched each live guest console, then stopped on the cross-vCPU image's known SKIP. A second batch verified the operator control: after `npt-remap` passed, a stop request during the `l2` countdown ended the batch without mounting or starting `l2`. The remap guest was left on its success screen.

The [test report](test-report.md) distinguishes a stopped queue from a failing guest and links the independently captured [attention screen](images/queue-012-skip.png). Cross-vCPU invalidation remains unverified.

## 010 · 2026-11-24 · Cross-vCPU invalidation blocked before guest entry

The new test image booted in `HV-SVM-L1`, configured with two virtual processors and nested virtualization enabled. Its firmware could not locate UEFI MP Services, the interface the test uses to run a callback on a second processor. The result was **SKIP**, not a failed invalidation or a pass: neither processor executed the test guest.

![Run 010 console showing the missing MP Services protocol and skipped test](images/npt-cross-010.png)

The [test report](test-report.md) records the image hash, replay commands, earlier build failure and the evidence needed to close this gap. Run 008 remains the last verified remap and covers one vCPU only.

## 008 · 2026-11-24 · Same guest address, different backing frame

L2 read `0xA11A` from guest physical address `0x2000`. L1 then pointed the same read-only mapping at a second frame, requested a full TLB flush, and restarted the guest at the same load. L2 read `0xB22B`. Neither frame's contents changed between reads, and both runs reached the same HLT at `0x1003`.

![Run 008 console showing the two different guest register values at the same halt](images/npt-remap-008.png)

The [test report](test-report.md) records the image hash and replay commands. This verifies frame replacement on one nested vCPU, not invalidation across CPUs.

## 007 · 2026-11-24 · The same guest store succeeds, then faults

Run 004 checked the page tables in memory. Run 007 checked what happened when L2 actually used them. With the target page writable, the guest changed its byte from `0x5A` to `0xA5` and reached HLT. L1 restored `0x5A`, made that same page read-only, flushed the translation and restarted at the same instruction. This time the store produced a nested write protection fault at GPA `0x2000`; the byte stayed `0x5A`.

![Writable and read-only outcomes from the same L2 store](images/npt-write-007.png)

The [test report](test-report.md) records the exact ISO, commands and exit fields. Next test: replace the backing frame without changing the guest physical address and verify which frame the guest reads.

## 004–006 · 2026-11-24 · Check the nested page tables before entering L2

Both guests execute from one page at guest physical address `0x1000`. We extracted the table builder used by the firmware so a host program can inspect its output without entering a VM. The host test used different physical addresses for the tables and code, checked every slot, and found exactly one read-only leaf. Rebuilt halt and CPUID guests then reproduced their earlier results in Hyper-V.

The [test report](test-report.md) ties each run to the source commit, binary hash and [host output](npt-layout-004.txt), [halt screenshot](images/npt-005.png) or [resume screenshot](images/npt-006.png). **Still open:** neither guest attempted a store. A writable control and an observed nested protection fault on the read-only page are needed before claiming write enforcement.

## 003 · 2026-11-24 · Resume after CPUID

**Question:** Can L1 emulate an instruction and resume the same vCPU with all four result registers intact?

**Run:** `firmware\build.cmd cpuid`. With the lab VM off, mount and start the image using `lab\Boot-HvProbe.ps1 -CpuidResume` in elevated PowerShell.

**Result:** CPUID exit `0x72` at `0x100C`, leaf `0x400000FE`, subleaf `7`. The reported next RIP was `0x100E`. L2 checked EAX, EBX, ECX and EDX, wrote `0x50415353` (“PASS”), then halted at `0x1037` with exit `0x78`. Exactly two guest entries. The no-nRIP fallback was not exercised.

![L1 console showing the two exits, resume address and guest-generated PASS marker](images/cpuid-resume-console.png)

## 002 · 2026-11-24 · First L2 guest

**Question:** Can the UEFI application temporarily enable SVM, enter a guest and regain control on HLT?

**Run:** `firmware\build.cmd l2`; `lab\Boot-HvProbe.ps1 -L2Smoke` with the VM off. The fixed guest executed `MOV AX,4C32h; HLT` from a single read-only nested page.

**Result:** Guest `RAX=0x4C32`, `RIP=0x1003`, `EXITCODE=0x78`. The marker shows that the instruction before HLT actually ran.

![First nested VM exit with the expected halt code, marker and instruction address](images/l2-smoke-console.png)

## 001 · 2026-11-24 · Boot our own UEFI image

**Question:** Does our own x86-64 EFI application boot and see AMD SVM in the lab VM?

**Run:** `firmware\build.cmd`; `lab\Boot-HvProbe.ps1` from elevated PowerShell.

**Result:** UEFI console reported `AuthenticAMD`, `CPUID.80000001h:ECX=0x004003F7` and SVM revision `1`. This was a read-only capability probe; no L2 guest was launched.

![UEFI application reporting the CPU vendor and advertised SVM feature](images/uefi-probe-console.png)

---

**Adding an entry:** Put the next numbered entry at the top. Record the date, machine or VM, question, build/run command, observed result and any untested condition. Failures belong here too. For a screenshot, save a reviewed PNG under `journal/images/` and embed it with `![what the image proves](images/filename.png)`. Keep credentials, personal desktop content, VM disks and raw logs out of the repository.
