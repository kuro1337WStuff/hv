# 061 · UART THRE and the modeled IRQ4 line

**Verdict:** firmware PASS by review of the [live framebuffer](../images/l1-uart-thre-061.png); queue ATTENTION on its first capture. This is a register/line test, **not** interrupt delivery to a guest handler.

The pre-boot contract required the fixed 25-I/O sequence before the success HLT, with guest receipts `00000F000155AA` (probe), `02010100600201` (causes) and `54485245h` (cookie). All three values appear on the captured screen. L1 independently recorded the expected `00614000h` line samples, one `54h` byte and one busy poll. The refused IER byte `82h` produced `REFUSED=1`, `UNCHANGED=1`, denial cookie zero. The screen does not independently display all 25 exits; the firmware checks them before declaring success.

Build `firmware\build.cmd l1-uart-thre`, source `5fca64727b`, runner `56db7a855d`. `python lab/hv_queue.py run --session l1-uart-thre` on `HV-SVM-L1-A` / `af1b9ae6-ff90-4d62-9aab-43cb3794bf97`: prior 059 live framebuffer classified PASS, checkpoint `HvQueue before d98785db9d4242a79e1f22c757984e74`, batch `d98785db9d4242a79e1f22c757984e74`. Boot 2026-11-25 18:35:20 UTC. Mounted ISO SHA-256 `eb235223013a87484d38e3b2c459821895faa7c80175bdd90205123dff96171c`. RGB565 SHA-256 `5c3cc7170c1f9825694e26c70aeb40aedecc8b84b3a47450a7507534eda3b08c`.
