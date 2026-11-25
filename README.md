# AMD SVM hypervisor lab

This lab builds a small x86-64 hypervisor using AMD Secure Virtual Machine (SVM) and Nested Page Tables (NPT). Its fixed guests make each claim testable: the same store must succeed on a writable page and fault on a read-only page **without changing the backing byte**.

**Tested:** nested virtualization on Windows Hyper-V with a Ryzen 9 9950X. **Target, not tested:** a removable-media hypervisor running a single-vCPU Linux guest on a physical Ryzen 9 9950X3D. No ordinary Linux boot or bare-metal qualification is claimed. These images are not replacements for a host bootloader.

![On the 9950X, Hyper-V hosts the standalone L1 hypervisor; a fixed L2 guest exits on CPUID and L1 resumes it. The 9950X3D is a separate, unqualified bare-metal target.](docs/images/amd-svm-lab.svg)

## Capability roadmap

The middle column describes the [firmware on `main` through run 041](journal/README.md). The right column describes what the first ordinary guest must demonstrate, **not** what runs today.

| Area | Observed in the nested lab | Next evidence needed |
| --- | --- | --- |
| Boot chain | After leaving UEFI boot services, L1 used its own stack and allocator, reported an expected allocation failure, and [launched a fixed L2](journal/test-report.md#037--2026-11-25-local--one-l2-entry-from-the-standalone-l1). | Load a pinned kernel and initramfs; reach a userspace serial marker and shut down cleanly on repeat cold boots. |
| SVM state and exits | [L2 checked all four CPUID reply registers after resume](journal/test-report.md#038--2026-11-25-local--standalone-l1-cpuid-resume). Separate fixtures preserved integer and legacy floating-point state, stopped on an unsupported request, and delivered an invalid-opcode fault. | Account for every CPU feature advertised to the guest, including the state L1 must save and restore. |
| Nested Page Tables (NPT) | L1 [revoked write access](journal/test-report.md#039--2026-11-25-local--standalone-l1-npt-write-protection) and [changed the backing frame](journal/test-report.md#040--2026-11-25-local--standalone-backing-frame-remap-and-baseline-batch) for one guest-physical address. A separate two-processor control tested invalidation with both guests stopped. | Apply those ownership and permission checks to the kernel's whole memory map, keeping L1's private pages outside guest RAM. |
| Guest I/O and interrupts | A [fixed guest](journal/test-report.md#041--2026-11-25--standalone-runtime-package-and-two-processor-control) woke from halt on a virtual timer interrupt and sent bytes to a virtual serial port. | Show the kernel's own serial, timer and interrupt drivers working together through userspace and shutdown. |
| Diagnostics | The [handoff test](journal/test-report.md#036--2026-11-25-local--standalone-l1-handoff) compared L1's serial output with its framebuffer; the guest I/O test required guest-originated bytes **and** an independent success halt. | Tie each ordinary boot's guest output and terminal state to the exact image and configuration. |

**Feature experiments are not `main` runtime.** The [feature trial index](journal/status.md#feature-evidence-before-the-separate-trial-ledger) distinguishes later nested UART interrupt and memory tests from unbooted images. The [PIT timer builds (080)](journal/trials/080.md) and [PIC interrupt-controller checks (082)](journal/trials/082.md) were host-only; neither proves delivery to a guest on those builds.

**Qualification and performance come after correctness.** The compatibility goal is AMD processors from the last five years that provide the required SVM and NPT features. A model's release date is not a qualification result. Physical testing starts with the 9950X, then separately verifies the 9950X3D. To measure overhead, count exits separately from timing them and compare repeated boots of the same workload against a named native baseline. Nested correctness runs are not benchmarks. Intel VMX/EPT is out of scope.

## Build and run one guest

Start with `cpuid`: after L1 handles the exit, L2 checks the reply registers and reaches its success halt. This is the small UEFI-hosted profile, not the post-handoff `l1-cpuid` test.

- **Host:** Windows with Hyper-V and its PowerShell module, AMD SVM enabled in firmware, and support for AMD nested virtualization (VM configuration version 9.3 or newer).
- **Build tools:** Git, Python 3.11+, Windows PowerShell 5.1, and Visual Studio Build Tools with the MSVC x64 C/C++ tools and assembler. The first build needs internet access for EDK II headers and the Python ISO-packaging dependencies.
- **Isolation:** Use a fresh checkout and a fresh, dedicated lab on a host you control. **Never reuse the project's held VMs.** If `HV-SVM-L1` or its store already exists, leave it untouched; use a separate lab host rather than repointing the runner.
- **VM:** In that lab, create a Generation 2 VM named `HV-SVM-L1`, stored under `lab/machines/HV-SVM-L1/` in the checkout. Set two vCPUs with virtualization extensions exposed, 2 GiB static RAM, no network adapter, standard checkpoints, and exactly one DVD drive first in boot order. Disable Secure Boot **in the VM only**. Leave the new VM **off with an empty DVD drive**. The [provisioning script](lab/New-HvLab.ps1) illustrates the settings; do not run it for this quickstart, as it requires and boots a separate Alpine ISO.

From the repository root in PowerShell:

```powershell
python lab/hv_queue.py run --rebuild cpuid
```

The launcher builds `build/hv-cpuid-resume.iso`, stages a digest-named copy, and requests elevation for the VM worker. The worker checkpoints the VM before boot. `PASS` means the captured guest console matched the reviewed success screen; `COMPLETE` means the batch finished. `ATTENTION` means the queue stopped for inspection, **not** that the guest passed. Find the events and console capture in `build/runs/<run-id>/` using the printed run ID. Press **Q** in the worker window to request a stop.

## Where to go next

- [Lab guide](lab/README.md) — profile commands, stop controls, and result handling.
- [Development journal](journal/README.md) — milestones and reviewed console screenshots.
- [Test ledger](journal/test-report.md) — commands, measured values, failed attempts, and limits of each result.
- [Current status](journal/status.md) — integrated baseline, feature trials, open issues and next decisions.
- [Project workflow](journal/project-workflow.md) — repository map and how trial evidence reaches `main`.
