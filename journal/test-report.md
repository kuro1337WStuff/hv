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
| Future NPT permission check | Separate fixed-guest negative test | An attempted write to the read-only guest page must produce an AMD nested-page fault for the expected guest physical address and write access; the backing bytes must remain unchanged. Neither the host walk nor a successful HLT establishes this. |

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
