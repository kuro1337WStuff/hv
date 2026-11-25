# 064 · Fixed guest read-only EFER view

**Before boot:** Read virtual EFER (`0500h`) twice on opposite sides of two distinct CPUID barriers; verify VMCB EFER remains `1500h`. Five positive entries including terminal HLT. In the matched arm substitute `DEADBEEFh` only at the second RDMSR; stop on the fourth entry with read exit `7Ch`, `INFO1=0`, second result poisoned and no success cookie. This is a read-view discriminator, not permission to expose more CPU state, inject a new exception, or forward host MSRs.

Positive: five restores, success cookie `45464552h`, HLT at base + `50h`. Refusal: four restores, second-result poison `FFFFFFFFFFFFFFFFh`, untouched cookie `FFFFFFFFh`, second RDMSR at base + `40h`. CR4 remains `220h`.

**Observed: firmware PASS**, [064 live framebuffer](../images/l1-efer-read-064.png). The accepted arm returned `500h` both times with backing `1500h`, five entries/restores and HLT `78h` at `1050h`. The denied read stopped at `1040h`, exit `7Ch`, INFO1 zero, index `DEADBEEFh`; second word and cookie retained their all-ones poison. Four entries/restores, no resume. No host-register forwarding or new advertised feature. First-capture queue ATTENTION, no retry.

`firmware\build.cmd l1-efer-read` at `71b752acb9`; `python lab/hv_queue.py run --session l1-efer-read` on `HV-SVM-L1-A` / `af1b9ae6-ff90-4d62-9aab-43cb3794bf97`, runner `56db7a855d`, batch `dcd60bd8c4fa42e2a35f71383d8a2489`. Prior 062 revalidated, checkpoint `HvQueue before dcd60bd8c4fa42e2a35f71383d8a2489`. Boot 2026-11-25 19:04:03.974 UTC. ISO SHA-256 `071d7b4e70b423fbea9a28c3325fb79fd4d52f3e09ae26258cb98387efdcbdcc`; RGB565 `b5937ea3096bec5863697e447268916fa0680dfa0f8f9f82a8da7f13d1db38be`.
