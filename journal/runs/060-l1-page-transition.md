# 060 · Seal and execute one owned page

**Verdict:** firmware PASS by review of the [live framebuffer](../images/l1-page-transition-060.png); queue ATTENTION on its first capture, no retry. This is a fixed guest on one nested vCPU, not a Linux boot or a cross-core permission proof.

| Predeclared observation | Clean page | Changed byte | Writable alias |
|---|---:|---:|---:|
| Setup HLT RIP, hex | `1011` | `1011` | `1011` |
| Writers / bad bytes / gate | 0 / 0 / 0 | 0 / 1 / 1 | 1 / 0 / 2 |
| Entries | 2 | 1 | 1 |

All fields matched. Only the clean page ran: TLB control 1, HLT exit `78h` at `8005h`, return marker `5452414Eh`. Neither denied arm re-entered. Four entries total.

Build `firmware\build.cmd l1-page-transition` at `ab7058f957`; runner `56db7a855d`, `python lab/hv_queue.py run --session l1-page-transition`. VM `HV-SVM-L1-B` / `dc0c1b56-fe53-4dc6-a583-8290f7030f0a`; batch `920365291aae44e1b590efbd3b8e6259`, checkpoint `HvQueue before 920365291aae44e1b590efbd3b8e6259`. Boot issued 2026-11-25 18:33:23.374 UTC, concurrently with 059 on another VM. Mounted ISO SHA-256 `c7d939796ea7d6cd806e44b8cbee798de19a660bdf47ba9210e96d3d1741409c`. Reviewed RGB565 SHA-256 `b8ab9f27b975afe4b88c0ee0e6c627f60a379383e174c0646000a63370f8c083`.
