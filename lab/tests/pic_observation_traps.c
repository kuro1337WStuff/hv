/* Link-only boundary for the host formatter test. No guest entry, stack
 * switch or IDT operation is permitted. Keep firmware and host traps in
 * separate translation units so the compiler checks the normal firmware paths. */
#include <stdint.h>
#include <stdlib.h>
volatile unsigned linux_vmrun_count;
void RunGprGuest(uintptr_t pa, uint64_t regs[14]) { (void)pa; (void)regs; abort(); }
void L1PrepareIdt(void) { abort(); }
__declspec(noreturn) void L1Enter(uintptr_t top) { (void)top; abort(); }
