# 065 · IRQ0 regression after sharing PIC wire constants

**Before boot:** rebuild `l1-pit-irq0` at PIC model commit `0894d877c7`, not from the prior archived ISO. Require the unchanged 048 terminal (reviewed RGB565 `79734331bc020f614d514f1fdb94b7f5b772e5f39d7c41e909363d8e3a2cce9c`): two IRQ0 entries/EOIs, one wake, cookie `04871001h`, refusal of `61h`, and 19 guest entries total. The new initialization model is tested separately on the host; this regression does not exercise guest ICW discovery or claim that Linux selected a PIC.

**Observed: PASS → COMPLETE**, [065 live framebuffer](../images/l1-pit-irq0-065.png). The unchanged classifier recognized the rebuilt profile without a new hash. IRQ vector `30h`, stacked RIPs `1037h`/`10A7h`, two IRQs, two EOIs, one wake, cookie `04871001h`, refused byte `61h`, `13h` (19) entries. This is the old IRQ0 behavior on the new source, not a nested test of the new init model.

`firmware\build.cmd l1-pit-irq0`, `python lab/hv_queue.py run --session l1-pit-irq0`; lane `HV-SVM-L1-B` / `dc0c1b56-fe53-4dc6-a583-8290f7030f0a`, runner `56db7a855d`. Batch `58bf4f2600dc4c6a89a23465f7b1d222`, prior 063 revalidated live; checkpoint `HvQueue before 58bf4f2600dc4c6a89a23465f7b1d222`. Mounted ISO SHA-256 `74e954b1324fa85d738291bf49c061e279e4d7ea00ea4e40800c864fe2a38045`.
