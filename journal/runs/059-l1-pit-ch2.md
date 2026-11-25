# 059 · PIT channel 2 under virtual pulses

**Verdict:** firmware PASS by review of the [live framebuffer](../images/l1-pit-ch2-059.png); queue ATTENTION on its first capture, no retry. Nested fixed guest on the 9950X; not a real clock calibration.

| Predeclared observation | Arm A | Arm B |
|---|---|---|
| Guest samples, hex | `21012D002E000100` | `2101E800E9000100` |
| Pulse count, decimal | 11932 | 59660 |
| Terminal, hex | HLT `78` at `1002` | IOIO `7B` at `1003`; refused `B6` |
| Entries | 13 | 13; no re-entry after refusal |

All fields matched the captured screen. Build `firmware\build.cmd l1-pit-ch2` at `9c50e5f22d`; runner `56db7a855d`, `python lab/hv_queue.py run --session l1-pit-ch2`. VM `HV-SVM-L1-A` / `af1b9ae6-ff90-4d62-9aab-43cb3794bf97`; batch `8bc17981e5ad4da9b997addc841a698d`, checkpoint `HvQueue before 8bc17981e5ad4da9b997addc841a698d`. Boot issued 2026-11-25 18:33:23.381 UTC, concurrently with 060 on another VM. Mounted ISO SHA-256 `9c86d585cea3faf30680a95b40f32651192f281ec70c317dc1da64e22c48d805`. Reviewed RGB565 SHA-256 `83fc9e1557820ea030978c3e18529ca33200a2efd40a56d172b0ffeb34cc4e38`.
