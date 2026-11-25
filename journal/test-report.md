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
