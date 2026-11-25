# 062 · Guest IRQ4 delivery and acknowledgement

**Before boot, lane A:** With the THRE cause pending, OUT2 high and the guest interruptible, require one real guest handler for IRQ4, one IIR acknowledgement and one specific EOI (`64h` to master port `20h`). Require a return to the guest continuation and no second delivery at a later eligible checkpoint. Change only the OUT2 gate for the matched arm: cause stays pending without a line or guest handler/EOI, and foreground guest execution still completes. The modeled line alone does not count. No host UART, physical IRQ or Linux boot.

Expected numeric receipts: OUT2 set → handler/delivery/EOI `1/1/1`, ISR/mainline IIR `2/1`, eight entries. OUT2 clear → `0/0/0`, ISR slot zero/mainline IIR `2`, six entries. Both write completion cookie `34515249h`. The same pending THRE cause is visible in the masked arm; reading it at the later foreground poll clears it.
