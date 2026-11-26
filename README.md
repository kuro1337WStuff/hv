# AMD SVM hypervisor lab

An x86-64 lab for **AMD Secure Virtual Machine (SVM) and Nested Page Tables (NPT)**. The goal is a type-1 hypervisor that boots directly from removable media and runs a single-vCPU Linux guest. Today, Windows Hyper-V on a **Ryzen 9 9950X** hosts the experiment. The guests that passed are short, fixed instruction sequences, not ordinary operating systems.

| Evidence | Result and limit |
|---|---|
| **`main` firmware through [041][runtime]** | A standalone L1 took control after UEFI boot services ended, ran fixed L2 guests, and checked their registers, memory and exits. |
| **Host checks and design reviews through [119](journal/trials/119.md)** | [118–119: fresh vCPU state and terminal device shutdown](journal/reports/cold-state-and-shutdown-119.svg). Earlier checks cover [boot evidence](journal/reports/guest-readiness-107.svg), [entry readiness](journal/reports/entry-readiness-111.svg), and [interrupt priority and Linux boot inputs](journal/reports/linux-platform-117.svg). [Earlier feature trials][features] ran fixed guests; no ordinary OS has booted. |
| **Not qualified** | No ordinary Linux or Windows guest boot; no physical boot from removable media. The physical targets are **9950X first, then 9950X3D**; each requires separate qualification. There is no five-year AMD compatibility promise; Intel VMX/EPT is out of scope. |

## What the nested lab proves

![Trial 039: Hyper-V on the 9950X hosts L1, which maps a page for a fixed L2 guest. With writes allowed the backing byte changes from 5A to A5. After resetting the byte and revoking write permission, the same store faults and the byte remains 5A.](docs/images/amd-svm-lab.svg)

[Trial 039][protect] checks the forbidden store **and** the byte it must not change. [Trial 040][remap] asks a different question: can the same guest address read from a new backing frame after a translation flush? A halt or a screenshot without the expected values would not answer either question.

## Capability map

The middle column is [integrated, nested evidence][ledger]. In the right column **Nested** means a controlled VM trial on a feature revision; **Host** means a model or offline inspection with no guest execution. Follow the links for the exact fixture and its limits.

| Subsystem | `main` · nested fixtures | Additional evidence, by scope |
|---|---|---|
| Boot & loader | [UEFI handoff, private stack and bounded allocator][handoff]; [first L2 entry][entry] | **Nested:** [staged input identity][inputs], [pre-entry CR3 and RSI checks][guard]. **Host:** [EFI media readback][media]. |
| CPU & exits | [CPUID reply and resume][cpuid]; [unsupported-leaf stop][runtime]; [invalid-opcode delivery and return][ud] | **Nested:** [refused CR4 write raises #GP][gp]; [fixed EFER read][efer]. |
| Register state | [Integer registers, XMM0–15 and x87 stack/control][runtime]; [MXCSR rounding and status][mxcsr] | These are legacy FP/SSE checks, not AVX/XSAVE or vCPU migration. |
| Address translation | [Write revocation][protect]; [frame remap][remap]; [private-page boundary][runtime]; [two-processor local flushes][cross]; [Hyper-V GPA flush][flush] | **Nested:** [page-fault attribution][faults], [owned page from writable to executable][seal], [fetch-fault address][fetch]. **Host:** [four-level NPT layout][layout]. |
| Interrupts & time | [Wake after HLT][wake]; [defer past STI][sti]; [priority masking][priority] | **Nested:** [one-shot PIT IRQ0][pit]. **Host:** [dual PIC setup][pic], [fallback][calibration] and [fast PIT calibration](journal/trials/113.md), [IRQ9 controller acceptance][sci] (not CPU delivery). |
| Console & power | [Fixed guest serial transmit][runtime] | **Nested:** [polled 8250 output][uart], [UART IRQ4][irq4], [virtual ACPI S5 request][s5]. **Host:** [COM discovery][com] and [16450 identification](journal/trials/112.md), [PM1a events][pm1], [published ACPI fields](journal/trials/104.md), [serial and power I/O stop at S5](journal/trials/105.md). |

**Linux boot inputs:** [102](journal/trials/102.md) identified the linked entry page; [110](journal/trials/110.md) found three places where the kernel declares potential instruction changes on it. The disk hash does not establish that the page stays immutable. [103](journal/trials/103.md) and [109](journal/trials/109.md) traced `/init` to BusyBox and its library on disk, not to a running PID 1. Linux remains non-executable pending CPU-state ownership and a safe page-transition policy.

## Qualification roadmap

![Two independent decisions follow repeatable nested Linux evidence: an optional offline Windows 10 L2 installer and guest trial, or an operator-led removable-media trial. Neither lane has started.](docs/images/qualification-gates.svg)

1. **First, pinned Linux under Hyper-V.** Require serial output from PID 1, advancing timer ticks and an independently witnessed orderly shutdown on **two fresh boots** of the same pinned inputs. The [two-boot evidence check](journal/trials/106.md) does not itself supply either boot. A resumed snapshot is not a second boot.
2. **Windows 10, VM first; deferred.** [111](journal/trials/111.md) found the first mismatch before any ISO read: the fixed L2 starts at `0x1000`, not at the PC's high firmware reset vector. [107](journal/trials/107.md) traced the installer offline; [108](journal/trials/108.md) selected a file on synthetic media. Neither supplies guest firmware or a storage controller. After Linux qualifies and a compatible platform exists, test WinPE startup, disk discovery, installed boot and shutdown separately on a disposable virtual disk. Preserve failed disks and logs before rollback.
3. **Separate physical decision.** Stable, repeated nested evidence is a prerequisite, not permission to deploy. An operator would need independent recovery media, a verified recovery procedure and **no primary disk attached** before considering removable media on a 9950X, then a separate qualification on a 9950X3D. A Windows VM result would not qualify either machine.

**Correctness before speed.** The [feature report](journal/reports/feature-phase.svg) comes from [eight retained trial records](journal/reports/feature-phase.json); [trial 101](journal/trials/101.md) records its scope. Use the [reporting guide](lab/measurement/README.md) to regenerate it or add a workload. Start comparing speed only after the feature baseline and repeatable workloads are stable: match platform, workload, clock and controls across independent cold boots and require correctness on both builds. No matched performance runs exist in this report.

[ledger]: journal/test-report.md
[features]: journal/status.md#feature-evidence-before-the-separate-trial-ledger
[handoff]: journal/test-report.md#036--2026-11-25-local--standalone-l1-handoff
[entry]: journal/test-report.md#037--2026-11-25-local--one-l2-entry-from-the-standalone-l1
[cpuid]: journal/test-report.md#038--2026-11-25-local--standalone-l1-cpuid-resume
[runtime]: journal/test-report.md#041--2026-11-25--standalone-runtime-package-and-two-processor-control
[mxcsr]: journal/test-report.md#030--2026-11-25-local--mxcsr-rounding-and-status-preservation
[layout]: journal/test-report.md#004006--2026-11-24--fixed-npt-layout
[protect]: journal/test-report.md#039--2026-11-25-local--standalone-l1-npt-write-protection
[remap]: journal/test-report.md#040--2026-11-25-local--standalone-backing-frame-remap-and-baseline-batch
[cross]: journal/test-report.md#015--2026-11-24-local--two-measured-l1-processor-ids-and-shared-npt-remap
[flush]: journal/test-report.md#017--2026-11-24-local--one-all-processor-nested-gpa-flush
[wake]: journal/test-report.md#022--2026-11-24-local--one-virtual-interrupt-wakes-l2
[sti]: journal/test-report.md#025--2026-11-24-local--pending-vector-delivered-after-the-sti-shadow
[priority]: journal/test-report.md#026--2026-11-24-local--virtual-interrupt-priority-threshold
[ud]: journal/test-report.md#027--2026-11-24-local--invalid-opcode-delivery-and-return
[inputs]: https://github.com/kuro1337WStuff/hv/blob/c50789a0dbe11690abed52f00a92d900775f7021/journal/test-report.md#045--staged-linux-input-identity-before-guest-entry
[guard]: https://github.com/kuro1337WStuff/hv/blob/c50789a0dbe11690abed52f00a92d900775f7021/journal/test-report.md#051--pre-entry-state-guard-for-the-pinned-nx-fetch
[faults]: https://github.com/kuro1337WStuff/hv/blob/c50789a0dbe11690abed52f00a92d900775f7021/journal/test-report.md#053--paired-guest-and-nested-page-faults
[seal]: https://github.com/kuro1337WStuff/hv/blob/429a6814bfbe4f0ad21acba345b5a2bdd7e62865/journal/runs/060-l1-page-transition.md
[fetch]: https://github.com/kuro1337WStuff/hv/blob/c50789a0dbe11690abed52f00a92d900775f7021/journal/test-report.md#044--2026-11-25--controlled-nested-fetch-address-precision
[gp]: https://github.com/kuro1337WStuff/hv/blob/c50789a0dbe11690abed52f00a92d900775f7021/journal/test-report.md#056--deliver-a-general-protection-fault-after-a-refused-cr4-write
[uart]: https://github.com/kuro1337WStuff/hv/blob/c50789a0dbe11690abed52f00a92d900775f7021/journal/test-report.md#047--polled-8250-compatible-serial-output
[irq4]: https://github.com/kuro1337WStuff/hv/blob/0b7c28a4cb7f39d0e09073415dfcf2efa823a47b/journal/runs/062-l1-uart-irq4.md
[pit]: https://github.com/kuro1337WStuff/hv/blob/c50789a0dbe11690abed52f00a92d900775f7021/journal/test-report.md#048--pit-one-shot-legacy-irq0-and-a-specific-eoi
[pic]: journal/trials/082.md
[s5]: https://github.com/kuro1337WStuff/hv/blob/c50789a0dbe11690abed52f00a92d900775f7021/journal/test-report.md#050--virtual-s5-request-versus-guest-text-and-hlt
[media]: journal/trials/078.md
[efer]: https://github.com/kuro1337WStuff/hv/blob/0fe588858882b35f0df82ad0a2700f0550b4e5a8/journal/runs/064-l1-efer-read.md
[calibration]: journal/trials/097.md
[sci]: journal/trials/098.md
[com]: journal/trials/099.md
[pm1]: journal/trials/100.md

## Quickstart: stage one fixed guest

From a fresh Windows checkout, use Git, Python 3.11+, Windows PowerShell 5.1, and Visual Studio Build Tools with the MSVC x64 C/C++ tools and assembler. The first build needs internet access for EDK II headers and the Python ISO-packaging dependencies. From the repository root:

```powershell
python lab/hv_queue.py run --stage-only --rebuild cpuid
```

This builds `build/hv-cpuid-resume.iso` and stages a digest-named copy. **Stop here for a build-only check:** do not execute the printed worker command. No VM starts; staging is not a guest pass. `cpuid` is the small UEFI-hosted profile, distinct from the post-handoff `l1-cpuid` fixture.

<details>
<summary>Optional: run the fixed guest in your own isolated Hyper-V lab</summary>

- **Host:** AMD SVM enabled in firmware, Hyper-V and its PowerShell module. [Microsoft's AMD nesting prerequisites](https://learn.microsoft.com/en-us/windows-server/virtualization/hyper-v/enable-nested-virtualization#prerequisites) specify **Windows 11 or later, or Windows Server 2022 or later**, and VM configuration version **9.3+**. Windows 10 above is a proposed **L2 guest**, not the Hyper-V host. Microsoft [does not support third-party hypervisors nested under Hyper-V](https://learn.microsoft.com/en-us/windows-server/virtualization/hyper-v/nested-virtualization#non-microsoft-virtualization-on-hyper-v-virtualization); this is a lab result, not a support claim.
- **Isolation:** Use a fresh, dedicated lab on a host you control. Never reuse the project's held VMs. If `HV-SVM-L1` or its store already exists, leave it untouched and use a separate lab host.
- **VM:** Create a Generation 2 VM named `HV-SVM-L1` under `lab/machines/HV-SVM-L1/` in the checkout. Use two vCPUs with virtualization extensions exposed, 2 GiB static RAM, no network adapter, standard checkpoints and exactly one DVD drive first in boot order. Disable Secure Boot **in the VM only**. Leave the VM **off with an empty DVD drive**. The [provisioner](lab/New-HvLab.ps1) illustrates these settings; do not run it for this quickstart, as it requires and boots a separate Alpine ISO.

After staging, from the same checkout:

```powershell
python lab/hv_queue.py run cpuid
```

The launcher requests elevation for the visible VM worker, which checkpoints the VM before boot. `PASS` means the captured guest console matched the reviewed success screen; `COMPLETE` means the batch finished. `ATTENTION` stops the queue for inspection; it is **not** a guest pass. Use the printed run ID to find events and console capture in `build/runs/<run-id>/`. Press **Q** in the worker window to request a stop. A stop does not automatically rewind a checkpoint.

</details>

## Where to go next

- [Lab guide](lab/README.md) — profile commands, stop controls, and result handling.
- [Development journal](journal/README.md) — milestones and reviewed console screenshots.
- [Test ledger](journal/test-report.md) — commands, measured values, failed attempts, and limits of each result.
- [Current status](journal/status.md) — integrated baseline, feature trials, open issues and next decisions.
- [Project workflow](journal/project-workflow.md) — repository map and how trial evidence reaches `main`.
