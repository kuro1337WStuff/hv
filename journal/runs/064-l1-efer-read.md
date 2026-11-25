# 064 · Fixed guest read-only EFER view

**Before boot:** Read virtual EFER (`0500h`) twice on opposite sides of two distinct CPUID barriers; verify VMCB EFER remains `1500h`. Five positive entries including terminal HLT. In the matched arm substitute `DEADBEEFh` only at the second RDMSR; stop on the fourth entry with read exit `7Ch`, `INFO1=0`, second result poisoned and no success cookie. This is a read-view discriminator, not permission to expose more CPU state, inject a new exception, or forward host MSRs.

Positive: five restores, success cookie `45464552h`, HLT at base + `50h`. Refusal: four restores, second-result poison `FFFFFFFFFFFFFFFFh`, untouched cookie `FFFFFFFFh`, second RDMSR at base + `40h`. CR4 remains `220h`.
