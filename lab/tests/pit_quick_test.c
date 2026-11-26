/* Host-only replay of the legacy-PIC arm of Linux stable v6.18.60
 * (f45ed76d6791), arch/x86/kernel/tsc.c: pit_verify_msb(), pit_expect_msb(),
 * quick_pit_calibrate(). No native clocks, port I/O or Linux execution. */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include "../../firmware/pit_cycle_clock.h"

#define CHECK(e) do { if (!(e)) { fprintf(stderr, "%d: %s\n", __LINE__, #e); exit(EXIT_FAILURE); } } while (0)
#define BILLION UINT64_C(1000000000)
enum { PULSE = 512, HZ = PULSE * PIT_PIC_HZ, MAX_I = 50 * PIT_PIC_HZ / 1000 / 256 };

/* Schedule in virtual cycles relative to 10 s. Pair 1 always ends at the
 * load pulse. Later pairs use step, except for one optional change: use gap
 * at that pair, then step_after thereafter. Budget counts complete pairs. */
typedef struct { unsigned budget, step, change_pair, gap, step_after; } Schedule;
typedef struct {
    PitCycleClock clock;
    Schedule schedule;
    uint64_t offset;
    unsigned pairs, cycle_reads;
    uint8_t low, high;
} Fixture;
typedef struct { unsigned matches; uint64_t last, error; } Band;
typedef struct { Band first, current; unsigned i; uint64_t delta, khz; } Trace;
enum Outcome { BUDGET, FIRST_SHORT, BAND_SHORT, FAST_FAIL, FINAL_MISS, ITER_LIMIT, SUCCESS };

static uint8_t byte_in(Fixture *f, uint16_t port)
{
    uint8_t v;
    CHECK(pit_shared_read(&f->clock.pit, port, 1, &v));
    return v;
}

static uint64_t cycles(Fixture *f)
{
    ++f->cycle_reads;
    return f->clock.cycles;
}

/* -1 means no observation: the harness stopped, not a Linux mismatch.
 * Read the live LSB at the old cursor; advance explicitly before the MSB.
 * Since HZ < 1 GHz, rounding the requested cycle instant UP to ns gives
 * exactly that virtual cycle after the clock's independent floor. */
static int verify_msb(Fixture *f, uint8_t expected)
{
    const Schedule *s = &f->schedule;
    unsigned n;
    if (f->pairs == s->budget) return -1;
    n = ++f->pairs;
    f->low = byte_in(f, 0x42);
    if (n == 1) f->offset = PULSE;
    else if (n == s->change_pair) f->offset += s->gap;
    else f->offset += s->change_pair && n > s->change_pair ? s->step_after : s->step;
    CHECK(pit_cycle_clock_advance(&f->clock, 10 * BILLION + (f->offset * BILLION + HZ - 1) / HZ));
    f->high = byte_in(f, 0x42);
    return f->high == expected;
}

static int expect_msb(Fixture *f, uint8_t expected, Band *b)
{
    uint64_t previous = 0;
    const Band empty = {0};
    *b = empty;
    for (; b->matches < 50000; ++b->matches) {
        int seen = verify_msb(f, expected);
        if (seen < 0) return -1;
        if (!seen) break;
        previous = b->last;
        b->last = cycles(f);
    }
    /* Linux uses the penultimate match here, not the last, even when the
     * loop ended on the internal cap rather than a mismatching pair. */
    b->error = cycles(f) - previous;
    return b->matches > 5;
}

static enum Outcome quick(Fixture *f, Trace *r)
{
    unsigned i;
    int seen;
    CHECK(pit_cycle_clock_init(&f->clock, HZ));
    CHECK(pit_cycle_clock_advance(&f->clock, 10 * BILLION));
    CHECK(pit_shared_write(&f->clock.pit, 0x61, 1, 2)); /* fixture: speaker on */
    CHECK(pit_shared_write(&f->clock.pit, 0x61, 1, (uint8_t)((byte_in(f, 0x61) & ~2) | 1)));
    CHECK(pit_shared_write(&f->clock.pit, 0x43, 1, 0xb0));
    CHECK(pit_shared_write(&f->clock.pit, 0x42, 1, 0xff));
    CHECK(pit_shared_write(&f->clock.pit, 0x42, 1, 0xff));
    if (verify_msb(f, 0) < 0) return BUDGET; /* ignore match/mismatch */
    CHECK(f->low == 0 && f->high == 0xff); /* this pair straddles LOAD */
    seen = expect_msb(f, 0xff, &r->first);
    if (seen < 0) return BUDGET;
    if (!seen) return FIRST_SHORT;
    for (i = 1; i <= MAX_I; ++i) {
        uint64_t errors; /* x86-64 Linux unsigned long, including on LLP64 hosts */
        r->i = i;
        seen = expect_msb(f, (uint8_t)(0xff - i), &r->current);
        if (seen < 0) return BUDGET;
        if (!seen) return BAND_SHORT;
        r->delta = r->current.last - r->first.last;
        errors = r->first.error + r->current.error;
        if (i == 1 && errors >= ((r->delta * MAX_I) >> 11)) return FAST_FAIL;
        if (errors >= (r->delta >> 11)) continue;
        seen = verify_msb(f, (uint8_t)(0xfe - i));
        if (seen < 0) return BUDGET;
        if (!seen) return FINAL_MISS;
        r->khz = (r->delta * PIT_PIC_HZ) / (i * 256 * 1000);
        return SUCCESS;
    }
    return ITER_LIMIT;
}

static Trace run(const char *name, Schedule s, enum Outcome expected,
                 unsigned pairs, unsigned reads, unsigned iteration)
{
    static const char *const outcomes[] = {"external-budget", "linux-zero:first-short",
        "linux-zero:band-short", "linux-zero:fast-fail", "linux-zero:final-miss",
        "linux-zero:iteration-limit", "success"};
    Fixture f = {0};
    Trace r = {0};
    enum Outcome result;
    f.schedule = s;
    result = quick(&f, &r);
    printf("%s: %s pairs=%u cycle_reads=%u i=%u matches=%u/%u last=%" PRIu64 "/%" PRIu64
           " d1/d2=%" PRIu64 "/%" PRIu64 " delta=%" PRIu64 " kHz=%" PRIu64 "\n",
           name, outcomes[result], f.pairs, f.cycle_reads, r.i, r.first.matches, r.current.matches,
           r.first.last, r.current.last, r.first.error, r.current.error, r.delta, r.khz);
    CHECK(result == expected && f.pairs == pairs && f.cycle_reads == reads && r.i == iteration);
    if (result != SUCCESS) CHECK(!r.khz);
    return r;
}

int main(void)
{
    /* One load pulse, then 256 pulses per MSB. On the ordinary grid FF
     * matches at pulses 2..256, FE at 258..512 (the mismatch consumed
     * 257), etc. At i=32 the shifted precision bound is exactly 2048:
     * equality must continue. At i=33 it is 2112; final pair is 8706. */
    Schedule s = {10000, PULSE, 0, 0, 0};
    Trace r = run("pulse-grid", s, SUCCESS, 8706, 8704, 33);
    CHECK(r.first.matches == 255 && r.current.matches == 255);
    CHECK(r.first.last == UINT64_C(6109222912) && r.current.last == UINT64_C(6113548288));
    CHECK(r.first.error == 1024 && r.current.error == 1024 && r.delta == 4325376);
    CHECK(r.khz == 610909); /* prescribed 610,909,184 Hz, truncated to kHz */

    /* Place pair 2 at pulse 252 (5 matches) or 251 (6 matches).
     * Only the latter may proceed to the FE band. */
    s.change_pair = 2; s.step_after = PULSE; s.gap = 251 * PULSE;
    r = run("five", s, FIRST_SHORT, 7, 6, 0);
    CHECK(r.first.matches == 5);
    s.gap = 250 * PULSE;
    r = run("six", s, SUCCESS, 8457, 8455, 33);
    CHECK(r.first.matches == 6 && r.khz == 610909);

    /* FF's mismatch skips FE entirely. A mismatching pair is consumed;
     * the next expect must not reuse the last match or assume FE occurred. */
    s.change_pair = 257; s.gap = 257 * PULSE;
    r = run("skip-fe", s, BAND_SHORT, 258, 257, 1);
    CHECK(r.first.matches == 255 && !r.current.matches && !r.current.last);

    /* Delay only the candidate's final verification past the next band. */
    s.change_pair = 8706; s.gap = 256 * PULSE;
    r = run("skip-final", s, FINAL_MISS, 8706, 8704, 33);
    CHECK(r.delta == 4325376 && r.current.error == 1024);

    /* First FE mismatch normally samples pulse 513, previous match 511.
     * Delay it by 25 1/8 pulses. The sum is then exactly 14912 cycles,
     * the extrapolated bound for 233 iterations. One cycle less must NOT
     * fast-fail, even though the ordinary precision bound is only 64. */
    s.change_pair = 513; s.gap = 26 * PULSE + PULSE / 8;
    r = run("fast-equal", s, FAST_FAIL, 513, 512, 1);
    CHECK(r.delta == 131072 && r.first.error == 1024 && r.current.error == 13888);
    --s.gap;
    r = run("fast-one-below", s, SUCCESS, 8681, 8679, 33);
    CHECK(r.delta == 4325376 + 63 && r.khz == 610918);

    /* Freeze immediately after LOAD. A short external budget is NOT a
     * Linux return. With enough budget Linux consumes 50000 FF matches,
     * takes its extra cycle sample, and returns zero on the first FE read. */
    s.change_pair = 2; s.gap = s.step_after = 0; s.budget = 14;
    r = run("frozen-short", s, BUDGET, 14, 13, 0);
    CHECK(r.first.matches == 13 && r.first.last == UINT64_C(6109092352));
    s.budget = 50002;
    r = run("frozen-linux-cap", s, BAND_SHORT, 50002, 50002, 1);
    CHECK(r.first.matches == 50000 && !r.first.error && !r.current.matches);
    CHECK(r.current.error == UINT64_C(6109092352)); /* previous sample was zero */

    /* Seven pulses per pair through the FE mismatch (pair 75 at pulse
     * 519), eight thereafter. Initial error is 28 pulses, small enough
     * not to fast-fail. Later sums of 30 pulses stay above the precision
     * bound even in band 233 (last match at pulse 59903). */
    s.step = 7 * PULSE; s.change_pair = 76; s.gap = s.step_after = 8 * PULSE;
    r = run("all-bands", s, ITER_LIMIT, 7499, 7498, 233);
    CHECK(r.first.error == 14 * PULSE && r.current.error == 16 * PULSE);
    CHECK(r.delta == 30540800);
    return 0;
}
