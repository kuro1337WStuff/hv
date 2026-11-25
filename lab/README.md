# Supervised Hyper-V regression queue

From the project root on the Windows lab host (Python 3.11+, PowerShell 5.1, Hyper-V), run:

```powershell
python lab/hv_queue.py run
```

The default order is `l2`, `cpuid`, `npt-write`, `npt-remap`, `npt-cross`. To rebuild and run just two profiles: `python lab/hv_queue.py run --rebuild npt-write npt-remap`. Without `--rebuild`, the runner uses the already-built ISOs in `build/`. It freezes the ordered profiles and SHA-256 digests for this batch before asking Windows to launch the visible Hyper-V worker. Building and staging use the caller's existing privileges. No change to UAC, no service and no per-test approval. An operator must still authorize elevation when Windows requires it.

`npt-cross` uses its own two-processor L1 payload. Rebuild with `firmware\build.cmd npt-cross`, then run `python lab/hv_queue.py run npt-cross`. The builder caches hash-pinned Limine v8 UEFI loader/header and Zig 0.13 tools under `build/tools`; first build needs network access. Other profiles remain UEFI applications. The cross test is read-only with respect to guest disks: it exits boot services via Limine, measures both local APIC IDs, runs four fixed L2 loads and halts on its evidence screen. Do not install this lab image as a host bootloader.

`npt-broadcast` is opt-in. It uses the two-processor L1 harness to request one synchronous Hyper-V flush of nested GPA translations for the selected NPT root across all processors. This is not a native `INVLPGB` test. Run `python lab/hv_queue.py run --rebuild npt-broadcast` to build and boot it. Only the [reviewed PASS screen](../journal/images/npt-broadcast-017.png) is accepted; the worker stops for inspection on any other display.

`forensic-write` is opt-in: `python lab/hv_queue.py run --rebuild forensic-write`. The same 16-bit store runs against a writable mapping and then a read-only mapping. The report uses the VMCB fault fields, decodes the destination and immediate byte from the fixed guest code at the saved RIP, and samples the owned backing byte before and after the protected attempt. It does not resume or emulate the faulting store.

`vcpu-gpr` is opt-in: `python lab/hv_queue.py run --rebuild vcpu-gpr`. A fixed 64-bit L2 guest checks its integer registers after each of two CPUID intercepts. Expected path: CPUID, CPUID, success HLT. This test does not cover interrupt delivery, extended state, or vCPU migration.

`irq-wake` is opt-in: `python lab/hv_queue.py run --rebuild irq-wake`. L1 intercepts the fixed guest's HLT and queues one maskable virtual interrupt. The handler records its return RIP and count; the resumed guest separately records forward progress. This exercises virtual HLT emulation without a host timer, device IRQ, or physical sleep.

**Watch:** leave the launcher open for live transitions, or use `python lab/hv_queue.py watch RUN_ID` in another terminal. The launcher prints the actual ID and commands at startup. Before every boot, `NEXT` names the profile and gives you five seconds to veto it; no response means continue. VMConnect can also display `HV-SVM-L1`; view only while the queue owns it.

**Stop:** press **Q** in the worker window, **Ctrl+C** in the launcher, or run `python lab/hv_queue.py stop RUN_ID` from another terminal. Stop is cooperative: an already-issued build or Hyper-V operation may finish. The runner does not undo that operation, restore snapshots automatically or power off the VM merely because you stopped watching. Interrupting a separate `watch` window detaches the observer; use `stop` to stop the queue. Start a fresh batch after inspection; a stopped run is not resumed implicitly.

**Results:** `build/runs/RUN_ID/` contains the frozen queue (`queue.json`), timestamped events (`events.jsonl`) and captured console evidence. An exact match to a reviewed guest success screen is required to advance. A reviewed skip is not a pass; an unrecognized screen times out for operator review. A booted VM, an exit code from the launcher, or text printed by a build is not guest evidence. New guest output requires independent review before changing the expected result. Exit codes: `0` complete, `2` stopped, `3` attention, `1` runner error.

Only known firmware profiles in the isolated project VM are eligible. Each batch preserves the starting state with a checkpoint; inspect checkpoint storage as it accumulates. No host boot changes, networking changes, background source edits, automatic commits, or retries of failures. Review evidence before adding it to the [journal](../journal/README.md). Raw batch artifacts stay local.
