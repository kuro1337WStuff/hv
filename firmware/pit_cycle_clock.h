#ifndef HV_PIT_CYCLE_CLOCK_H
#define HV_PIT_CYCLE_CLOCK_H

#include "pit_shared.h"

/* Host oracle: elapsed_ns is supplied by the caller, relative to one zero
 * epoch. PIT pulses and virtual cycles are independent floors of that time;
 * neither reads, PIT2 GATE nor PIC service supplies or suspends time. Use
 * pit_shared_read/write and the existing IRQ service on pit, but advance it
 * only through this clock. No native TSC, CPU exposure, VM I/O or delivery. */
typedef struct {
    PitShared pit;
    uint64_t elapsed_ns, cycles;
    uint32_t hz; /* Fixed virtual cycles/second until reinitialization. */
} PitCycleClock;

/* Every nonzero uint32_t rate is supported. Zero refuses without mutation. */
static inline int pit_cycle_clock_init(PitCycleClock *c, uint32_t hz)
{
    if (!hz) return 0;
    pit_shared_init(&c->pit);
    c->elapsed_ns = c->cycles = 0;
    c->hz = hz;
    return 1;
}

/* floor(ns * hz / 10^9) since the epoch. The fractional numerator fits
 * (< 10^9 * 2^32). Always supply the low 64 bits; return whether the full
 * count fits, so bounded and wrapping clocks use identical rounding. */
static inline int pit_cycle_clock_count(uint64_t ns, uint32_t hz, uint64_t *cycles)
{
    const uint64_t billion = UINT64_C(1000000000);
    uint64_t seconds = ns / billion, fraction = ((ns % billion) * hz) / billion;
    *cycles = seconds * hz + fraction;
    return !hz || seconds <= (UINT64_MAX - fraction) / hz;
}

static inline int pit_cycle_clock_advance(PitCycleClock *c, uint64_t elapsed_ns)
{
    uint64_t ticks;
    if (elapsed_ns < c->elapsed_ns) return 0;
    if (elapsed_ns == c->elapsed_ns) return 1;
    pit_pic_ns_to_ticks(elapsed_ns, &ticks);
    if (!pit_shared_advance(&c->pit, ticks)) return 0;

    /* This isolated oracle models a wrapping 64-bit cycle counter. */
    (void)pit_cycle_clock_count(elapsed_ns, c->hz, &c->cycles);
    c->elapsed_ns = elapsed_ns;
    return 1;
}

#endif
