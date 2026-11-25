#ifndef HV_PIT_PERIODIC_H
#define HV_PIT_PERIODIC_H

/** @file Host-only PIT channel 0, binary LSB/MSB writes, GATE fixed high.
 * Intel 8254, 231164-005, Figs. 15, 17, 19: pulse 1 loads N. Mode 2
 * falls on N, rises on N+1, repeats every N. Mode 4 falls on N+1 and
 * rises on N+2, once. Mode 0 rises on N+1 and holds high.
 * https://www.scs.stanford.edu/10wi-cs140/pintos/specs/8254.pdf
 *
 * Initialize a caller-owned PitPeriodic. Advance to each write's absolute
 * pulse before applying the write. Linux v6.18.60 drivers/clocksource/i8253.c
 * programs 34h/A9h/04h for HZ=1000, then 38h and one count per shot.
 * Disable writes 30h/00h/00h/30h: the final command cancels the mode-0
 * count, not an IRQ already latched. Only OUT and its rising edges are
 * modeled: no counter reads, other channels, wall clock or guest injection.
 */
#include <stdint.h>

enum { PIT_PERIODIC_OFF, PIT_PERIODIC_LSB, PIT_PERIODIC_MSB };
enum { PIT_CH0_TERMINAL = 0x30, PIT_CH0_RATE = 0x34, PIT_CH0_STROBE = 0x38 };
typedef struct {
    uint64_t tick;
    uint32_t count;  /* remaining pulses to rise; zero if not counting */
    uint32_t reload; /* mode 2: latest complete divisor, also for first load */
    uint8_t phase, low, mode, out_low, pending;
} PitPeriodic;

/* Unprogrammed fixture at pulse zero, OUT high, no pending rising edge. */
static inline void pit_periodic_init(PitPeriodic *p)
{
    const PitPeriodic initial = {0};
    *p = initial;
}

static inline int pit_periodic_out(const PitPeriodic *p)
{
    return !p->out_low;
}

/* Control words replace the counting sequence immediately, even between
 * count bytes. Changing mode is an IRQ source only if the pin actually
 * rises. Refused writes (including mode-2 divisor 1) change no state. */
static inline int pit_periodic_write(PitPeriodic *p, uint16_t port, unsigned width, uint8_t byte)
{
    if (width != 1) return 0;
    if (port == 0x43) {
        if (byte != PIT_CH0_RATE && byte != PIT_CH0_STROBE && byte != PIT_CH0_TERMINAL) return 0;
        if (p->out_low && byte != PIT_CH0_TERMINAL) p->pending = 1;
        p->mode = byte;
        p->out_low = byte == PIT_CH0_TERMINAL;
        p->count = p->reload = 0;
        p->phase = PIT_PERIODIC_LSB;
    } else if (port == 0x40 && p->phase == PIT_PERIODIC_LSB) {
        if (p->mode == PIT_CH0_TERMINAL) {
            p->count = 0; /* mode 0 alone stops and drives OUT low on the LSB */
            p->out_low = 1;
        }
        p->low = byte;
        p->phase = PIT_PERIODIC_MSB;
    } else if (port == 0x40 && p->phase == PIT_PERIODIC_MSB) {
        uint32_t n = ((uint32_t)byte << 8) | p->low;
        if (!n) n = 65536U;
        if (p->mode == PIT_CH0_RATE) {
            if (n == 1) return 0;
            p->reload = n; /* wait for the next periodic reload */
        } else {
            /* Mode 4 retriggers on the next pulse. If OUT is low now, the
             * old strobe still ends on that pulse, not on this data write. */
            p->count = n + (p->mode == PIT_CH0_STROBE ? 2U : 1U);
        }
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
    if (!pulses) return 1;
    if (p->mode != PIT_CH0_RATE) {
        if (!p->count) return 1; /* waiting for a count, or already expired */
        if (p->mode == PIT_CH0_STROBE && p->out_low) p->pending = 1;
        if (pulses >= p->count) {
            p->count = 0;
            p->out_low = 0;
            p->pending = 1;
        } else {
            p->count -= (uint32_t)pulses;
            p->out_low = p->mode == PIT_CH0_TERMINAL || p->count == 1;
        }
        return 1;
    }
    if (!p->reload) return 1;
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
    p->out_low = p->count == 1;
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
