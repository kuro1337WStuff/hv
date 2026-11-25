#ifndef HV_PIT_PERIODIC_H
#define HV_PIT_PERIODIC_H

/** @file Host-only PIT channel 0, binary mode 2, GATE fixed high.
 * Device reference: Intel 8254 Programmable Interval Timer, 231164-005,
 * Mode Definitions, Mode 2 (Fig. 17): a completed count N loads on pulse 1;
 * OUT falls on pulse N, rises on N+1, then rises every N pulses.
 * https://www.scs.stanford.edu/10wi-cs140/pintos/specs/8254.pdf
 *
 * Initialize a caller-owned PitPeriodic. Advance to each write's absolute
 * pulse before applying the write. Linux pit_set_periodic() at f45ed76d6791
 * writes 34h to 43h, then A9h and 04h to 40h (PIT_LATCH=1193 at HZ=1000).
 * After advancing, sample OUT without side effects; take consumes the IRQ0
 * edge latch. No port reads, wall clock, PIC acceptance or guest injection.
 */
#include <stdint.h>

enum { PIT_PERIODIC_OFF, PIT_PERIODIC_LSB, PIT_PERIODIC_MSB };
typedef struct {
    uint64_t tick;
    uint32_t count;  /* pulses until OUT rises; zero before the first load */
    uint32_t reload; /* latest complete count; zero until both bytes arrive */
    uint8_t phase, low, pending;
} PitPeriodic;

/* Unprogrammed fixture at pulse zero, OUT high, no pending rising edge. */
static inline void pit_periodic_init(PitPeriodic *p)
{
    const PitPeriodic initial = {0};
    *p = initial;
}

static inline int pit_periodic_out(const PitPeriodic *p)
{
    return p->count != 1;
}

/* Scalar byte writes at the cursor. A mode command abandons the old period
 * and any partial write. It drives OUT high, latching a rise only if OUT
 * was low. Refusals (including divisor 1) leave all state unchanged. */
static inline int pit_periodic_write(PitPeriodic *p, uint16_t port, unsigned width, uint8_t byte)
{
    if (width != 1) return 0;
    if (port == 0x43 && byte == 0x34) {
        if (!pit_periodic_out(p)) p->pending = 1;
        p->count = p->reload = 0;
        p->phase = PIT_PERIODIC_LSB;
    } else if (port == 0x40 && p->phase == PIT_PERIODIC_LSB) {
        p->low = byte;
        p->phase = PIT_PERIODIC_MSB;
    } else if (port == 0x40 && p->phase == PIT_PERIODIC_MSB) {
        uint32_t n = ((uint32_t)byte << 8) | p->low;
        if (n == 1) return 0;
        p->reload = n ? n : 65536U; /* don't disturb the counting element */
        p->phase = PIT_PERIODIC_LSB;
    } else return 0;
    return 1;
}

/* Absolute completed input pulses, including UINT64_MAX. Work is bounded:
 * finish the current period, then skip whole periods of the latest reload.
 * Only elapsed pulses are subtracted; there is no absolute next deadline. */
static inline int pit_periodic_advance(PitPeriodic *p, uint64_t tick)
{
    uint64_t pulses;
    if (tick < p->tick) return 0;
    pulses = tick - p->tick;
    p->tick = tick;
    if (!pulses || !p->reload) return 1;
    if (!p->count) {
        p->count = p->reload;
        --pulses; /* initial load does not also decrement */
    }
    if (pulses >= p->count) {
        pulses -= p->count;
        p->pending = 1;
        p->count = p->reload;
        if (pulses >= p->count) pulses %= p->count;
    }
    p->count -= (uint32_t)pulses;
    return 1;
}

/* One or more unconsumed OUT rises return 1 once, not a queue of ticks. */
static inline int pit_periodic_take(PitPeriodic *p)
{
    int pending = p->pending;
    p->pending = 0;
    return pending;
}
#endif
