# AMD SVM hypervisor lab

This project builds small x86-64 hypervisor images to test AMD Secure Virtual Machine (SVM) and Nested Page Tables (NPT). Hyper-V is the outer hypervisor (L0); the image under test runs in `HV-SVM-L1` (L1) and launches a fixed guest (L2). The guest is small enough to identify the exact instruction, register value, or memory access that proves each result.

**Verified environment:** nested Hyper-V on a Ryzen 9 9950X. The journal records L2 entry and resume, NPT write protection and remapping, cross-vCPU invalidation, integer-register preservation, and virtual interrupt delivery. The Ryzen 9 9950X3D is the intended bare-metal target, not a verified platform. These are lab images, not a host bootloader or a general-purpose hypervisor. Other AMD processors are not qualified by these runs; Intel VMX is out of scope.

## Build and run one guest

Start with `cpuid`: the guest executes `CPUID`, L1 handles the exit, and L2 must resume and reach its success halt. Booting an ISO alone does not pass the test.

- **Host:** Windows with Hyper-V and its PowerShell module, AMD SVM enabled in firmware, and support for AMD nested virtualization (VM configuration version 9.3 or newer).
- **Build tools:** Git, Python 3.11+, Windows PowerShell 5.1, and Visual Studio Build Tools with the MSVC x64 C/C++ tools and assembler. The first build needs internet access for EDK II headers and the Python ISO-packaging dependencies.
- **VM:** Use only the dedicated Generation 2 VM named `HV-SVM-L1`, stored under `lab/machines/HV-SVM-L1/` in this checkout. For a fresh setup, configure two vCPUs with virtualization extensions exposed, 2 GiB static RAM, no network adapter, standard checkpoints, and a single DVD drive first in boot order. Disable Secure Boot **in the VM only**. Leave a new VM **off with an empty DVD drive**. The queue refuses unrelated media. The [lab provisioning script](lab/New-HvLab.ps1) shows the settings; it also requires a checksum-pinned Alpine ISO and starts that image, neither of which is needed for this test.

From the repository root in PowerShell:

```powershell
python lab/hv_queue.py run --rebuild cpuid
```

This builds `build/hv-cpuid-resume.iso`, stages a digest-named copy, and requests elevation for the VM worker. The worker checkpoints the VM before boot. Expect `PASS` for `cpuid` followed by `COMPLETE`. `ATTENTION` means stop and inspect the captured screen, not success. The launcher prints the run ID; its event log and console capture are in `build/runs/<run-id>/`. Press **Q** in the worker window to request a stop.

## Where to go next

- [Lab guide](lab/README.md) — profile commands, stop controls, and result handling.
- [Development journal](journal/README.md) — milestones and reviewed console screenshots.
- [Test ledger](journal/test-report.md) — commands, measured values, failed attempts, and limits of each result.
- [Contributing](CONTRIBUTING.md) — what belongs in a commit and how to roll one back.
