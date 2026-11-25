/* Host-only replay of the legacy-PIC arm of Linux stable v6.18.60
 * (f45ed76d6791), arch/x86/kernel/tsc.c:414-491, pit_calibrate_tsc().
 * No host clock, port I/O, Linux execution, or clocksource selection. */
#include <stdio.h>
#include <stdlib.h>
#include "../../firmware/pit_cycle_clock.h"

#define CHECK(e) do { if (!(e)) { fprintf(stderr, "%d: %s\n", __LINE__, #e); exit(EXIT_FAILURE); } } while (0)
#define EPOCH_NS UINT64_C(1000000000)
#define VIRTUAL_HZ UINT32_C(2500000000)

typedef struct { unsigned ms; uint16_t latch; unsigned loopmin; } Calibration;
static const Calibration cal = {10, 11931, 1000}, cal2 = {50, 59659, 5000};

enum Outcome { BUDGET_LOW, LINUX_REJECT, ACCEPT };
typedef struct {
    enum Outcome outcome;
    unsigned lows;
    uint64_t t1, t2, min_gap, max_gap, poll_ns, khz;
} Trace;

static uint8_t port61(PitCycleClock *c)
{
    uint8_t value;
    CHECK(pit_shared_read(&c->pit, 0x61, 1, &value));
    return value;
}

/* All setup and the initial cycle sample share a nonzero epoch. First poll
 * at epoch + first_ns, subsequent polls every step_ns; each low port read
 * and its cycle sample share an instant. Polling itself never advances time.
 * The external budget counts port polls, including the terminating high. */
static Trace replay(PitCycleClock *c, Calibration p, unsigned first_ns,
                    unsigned step_ns, unsigned budget)
{
    Trace r = {0};
    uint64_t ns = EPOCH_NS + first_ns;
    unsigned i;
    CHECK(pit_cycle_clock_init(c, VIRTUAL_HZ));
    CHECK(pit_cycle_clock_advance(c, EPOCH_NS));
    CHECK(pit_shared_write(&c->pit, 0x61, 1, 2)); /* gate off, speaker on */
    CHECK(pit_shared_write(&c->pit, 0x61, 1, (uint8_t)((port61(c) & ~2) | 1)));
    CHECK(pit_shared_write(&c->pit, 0x43, 1, 0xb0));
    CHECK(pit_shared_write(&c->pit, 0x42, 1, (uint8_t)p.latch));
    CHECK(pit_shared_write(&c->pit, 0x42, 1, (uint8_t)(p.latch >> 8)));
    r.t1 = r.t2 = c->cycles;
    r.min_gap = UINT64_MAX; /* x86-64 Linux unsigned long, even on LLP64 */
    CHECK(budget > 0);
    for (i = 0; i < budget; ++i, ns += step_ns) {
        uint8_t value;
        uint64_t delta;
        CHECK(pit_cycle_clock_advance(c, ns));
        value = port61(c);
        CHECK((value & 3) == 1);
        r.poll_ns = ns - EPOCH_NS;
        if (value & 0x20) {
            if (r.lows < p.loopmin || r.max_gap > 10 * r.min_gap) {
                r.outcome = LINUX_REJECT; /* Linux returns ULONG_MAX here. */
            } else {
                r.outcome = ACCEPT;
                r.khz = (r.t2 - r.t1) / p.ms;
            }
            return r;
        }
        delta = c->cycles - r.t2;
        r.t2 = c->cycles;
        /* Linux compares unsigned long gaps but assigns unsigned int.
         * Every trace here has < 2^32 cycles per gap and < INT_MAX lows. */
        CHECK(delta <= UINT32_MAX);
        if (delta < r.min_gap) r.min_gap = (uint32_t)delta;
        if (delta > r.max_gap) r.max_gap = (uint32_t)delta;
        ++r.lows;
    }
    r.outcome = BUDGET_LOW; /* No terminating high: Linux hasn't returned. */
    return r;
}

static void check_trace(Trace r, enum Outcome outcome, unsigned lows,
                        uint64_t high_ns, uint64_t last_cycles,
                        uint64_t min_gap, uint64_t max_gap, uint64_t khz)
{
    CHECK(r.outcome == outcome && r.lows == lows && r.poll_ns == high_ns);
    CHECK(r.t1 == VIRTUAL_HZ && r.t2 - r.t1 == last_cycles);
    CHECK(r.min_gap == min_gap && r.max_gap == max_gap && r.khz == khz);
}

int main(void)
{
    PitCycleClock c;
    Trace frozen;
    /* Hand oracle: after 10/50 ms, floor(t * 1193182 Hz) is 11931/59659.
     * Mode 0 needs one load pulse PLUS N pulses. Both samples are low;
     * the next 10 us poll is high. At 2.5 cycles/ns the last low, not that
     * high, contributes 25M/125M cycles. Equality with loopmin passes. */
    check_trace(replay(&c, cal, 10000, 10000, 1001), ACCEPT,
                1000, 10010000, 25000000, 25000, 25000, 2500000);
    check_trace(replay(&c, cal2, 10000, 10000, 5001), ACCEPT,
                5000, 50010000, 125000000, 25000, 25000, 2500000);
    /* Miss only the first 10 us poll: 999 lows, gap ratio just 2. */
    check_trace(replay(&c, cal, 20000, 10000, 1001), LINUX_REJECT,
                999, 10010000, 25000000, 25000, 50000, 0);
    /* 5 us grid after one long gap: equality at 10*min is admissible;
     * one additional ns gives 125002 cycles and must fail. */
    check_trace(replay(&c, cal, 50000, 5000, 2001), ACCEPT,
                1991, 10005000, 25000000, 12500, 125000, 2500000);
    check_trace(replay(&c, cal, 50001, 5000, 2001), LINUX_REJECT,
                1991, 10005001, 25000002, 12500, 125002, 0);
    /* Offset grid: last low at 9999950 ns gives 24999875/10, truncated. */
    check_trace(replay(&c, cal, 7950, 8000, 1251), ACCEPT,
                1250, 10007950, 24999875, 19875, 20000, 2499987);
    /* The clock actually stalls after 10 us. OUT2 remains device-low;
     * repeating port reads does not supply pulses. A strictly earlier
     * timestamp from this nonzero cursor must be refused, not clamped. */
    frozen = replay(&c, cal, 10000, 0, 1200);
    check_trace(frozen, BUDGET_LOW, 1200, 10000, 25000, 0, 25000, 0);
    CHECK(!pit_cycle_clock_advance(&c, EPOCH_NS + 9999));
    CHECK(c.elapsed_ns == EPOCH_NS + 10000 && c.cycles == frozen.t2 && port61(&c) == 1);
    puts("PIT fallback: 10/50 ms accepted at 2500000 virtual kHz; rejection and frozen-budget controls passed");
    return 0;
}
