# Hypervisor development journal

Development host: Ryzen 9 9950X. Firmware runs use the `HV-SVM-L1` Hyper-V VM; host-only checks are labeled separately. Neither qualifies the 9950X3D deployment target on bare metal.

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
