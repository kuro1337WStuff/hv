/* Host only, from the repo root:
 * cc -std=c11 -Wall -Wextra -Werror lab/tests/pit_periodic_test.c -o pit_periodic_test && ./pit_periodic_test
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../firmware/pit_periodic_irq0.h"

#define CHECK(e) do { if (!(e)) { fprintf(stderr, "%d: %s\n", __LINE__, #e); exit(EXIT_FAILURE); } } while (0)
#define REFUSE(p, e) do { unsigned char before[sizeof(p)]; memcpy(before, &(p), sizeof(p)); \
    CHECK(!(e)); CHECK(!memcmp(before, &(p), sizeof(p))); } while (0)

static void write_count(PitPeriodic *p, uint16_t n)
{
    CHECK(pit_periodic_write(p, 0x40, 1, (uint8_t)n));
    CHECK(pit_periodic_write(p, 0x40, 1, (uint8_t)(n >> 8)));
}

static void program(PitPeriodic *p, uint16_t n)
{
    CHECK(pit_periodic_write(p, 0x43, 1, 0x34));
    write_count(p, n);
}

/* Observe the pin AND consume the edge latch. A jump can end with OUT low
 * despite having crossed earlier rising edges. A second take must be empty. */
static void sample(PitPeriodic *p, uint64_t t, int level, int edge)
{
    CHECK(pit_periodic_advance(p, t));
    int out = pit_periodic_out(p);
    int pending = pit_periodic_take(p);
    if (out != level || pending != edge) {
        fprintf(stderr, "pulse %" PRIu64 ": OUT=%d IRQ=%d; expected %d/%d\n",
                t, out, pending, level, edge);
        exit(EXIT_FAILURE);
    }
    CHECK(!pit_periodic_take(p));
}

static void linux_periodic(void)
{
    PitPeriodic p;
    pit_periodic_init(&p);
    CHECK(pit_periodic_write(&p, 0x43, 1, 0x34));
    CHECK(pit_periodic_write(&p, 0x40, 1, 0xa9));
    sample(&p, 2000, 1, 0); /* partial count: no initial load yet */
    CHECK(pit_periodic_write(&p, 0x40, 1, 0x04));
    /* Two complete periods, origin at the completed write. */
    for (unsigned i = 0; i <= 2387; ++i)
        sample(&p, 2000U + i, i != 1193 && i != 2386, i == 1194 || i == 2387);
    sample(&p, 4387, 1, 0); /* same sample after consuming the second rise */
}

static void live_reload(void)
{
    PitPeriodic p;
    pit_periodic_init(&p);
    program(&p, 5);
    sample(&p, 6, 1, 1);
    CHECK(pit_periodic_write(&p, 0x40, 1, 3)); /* LSB across an old boundary */
    sample(&p, 10, 0, 0);
    sample(&p, 11, 1, 1);
    sample(&p, 12, 1, 0);
    CHECK(pit_periodic_write(&p, 0x40, 1, 0)); /* complete 3 mid-period */
    sample(&p, 14, 1, 0);
    sample(&p, 15, 0, 0);
    sample(&p, 16, 1, 1); /* still the old boundary, now load 3 */
    sample(&p, 17, 1, 0);
    sample(&p, 18, 0, 0);
    sample(&p, 19, 1, 1);
}

static void long_jump(void)
{
    PitPeriodic p;
    pit_periodic_init(&p);
    program(&p, 5);
    sample(&p, 7, 1, 1);
    CHECK(pit_periodic_write(&p, 0x40, 1, 4));
    CHECK(pit_periodic_write(&p, 0x40, 1, 0));
    /* Old period rises at 11, then 15, 19, ... (3 mod 4). Cross all but
     * the last rise, stopping precisely on the final one-pulse low strobe. */
    CHECK(pit_periodic_advance(&p, UINT64_MAX - 1));
    REFUSE(p, pit_periodic_advance(&p, 0)); /* also preserves the pending bit */
    sample(&p, UINT64_MAX - 1, 0, 1);
    sample(&p, UINT64_MAX - 1, 0, 0);
    sample(&p, UINT64_MAX, 1, 1);
    sample(&p, UINT64_MAX, 1, 0);
}

static void divisor_limits(void)
{
    PitPeriodic p;
    pit_periodic_init(&p);
    sample(&p, UINT64_MAX - 65537, 1, 0);
    program(&p, 0); /* 65536 plus the initial load pulse */
    sample(&p, UINT64_MAX - 2, 1, 0);
    sample(&p, UINT64_MAX - 1, 0, 0);
    sample(&p, UINT64_MAX, 1, 1);

    pit_periodic_init(&p);
    sample(&p, UINT64_MAX - 2, 1, 0);
    program(&p, 2); /* legal even when first rise is beyond the time domain */
    sample(&p, UINT64_MAX - 1, 1, 0);
    sample(&p, UINT64_MAX, 0, 0);
}

static void refused_writes_and_restart(void)
{
    PitPeriodic p;
    pit_periodic_init(&p);
    REFUSE(p, pit_periodic_write(&p, 0x40, 1, 3)); /* before command */
    program(&p, 4);
    sample(&p, 1, 1, 0);
    CHECK(pit_periodic_write(&p, 0x40, 1, 1));
    REFUSE(p, pit_periodic_write(&p, 0x40, 1, 0)); /* illegal count 1 */
    REFUSE(p, pit_periodic_write(&p, 0x40, 2, 0));
    REFUSE(p, pit_periodic_write(&p, 0x42, 1, 0));
    REFUSE(p, pit_periodic_write(&p, 0x43, 1, 0x36)); /* unsupported mode 3 */
    sample(&p, 4, 0, 0);
    CHECK(pit_periodic_write(&p, 0x40, 1, 1)); /* retry MSB: 0x0101 */
    sample(&p, 5, 1, 1);
    sample(&p, 261, 0, 0);
    CHECK(pit_periodic_write(&p, 0x40, 1, 0xab));
    CHECK(pit_periodic_write(&p, 0x43, 1, 0x34)); /* abandons partial; OUT rises */
    sample(&p, 1000, 1, 1); /* idle clock doesn't clear the edge */
    CHECK(pit_periodic_write(&p, 0x40, 1, 2));
    CHECK(pit_periodic_write(&p, 0x40, 1, 0));
    sample(&p, 1002, 0, 0);
    sample(&p, 1003, 1, 1);
}

static void irq0_service(void)
{
    PitPeriodicIrq0 s;
    _Static_assert(PIT_PIC_VECTOR == 0x30, "IRQ0 acceptance vector");
    pit_periodic_irq0_init(&s);
    CHECK(pit_periodic_irq0_write(&s, 0x43, 1, 0x34));
    CHECK(pit_periodic_irq0_write(&s, 0x40, 1, 4));
    CHECK(pit_periodic_irq0_write(&s, 0x40, 1, 0));
    CHECK(pit_periodic_irq0_advance(&s, 4));
    CHECK(!pit_periodic_out(&s.pit) && !s.pic.pending);
    CHECK(pit_periodic_irq0_advance(&s, 5));
    CHECK(pit_periodic_out(&s.pit) && !s.pit.pending && s.pic.pending);
    CHECK(pit_periodic_irq0_advance(&s, 21)); /* four more masked rises */
    CHECK(pit_periodic_irq0_advance(&s, 21)); /* no edge; retain PIC IRR */
    CHECK(s.pic.pending == 1 && !s.pic.in_service && !s.pic.armed && s.pic.now == 0);
    REFUSE(s, pit_pic_take(&s.pic)); /* masked */
    CHECK(pit_periodic_irq0_write(&s, 0x21, 1, 0xfe));
    CHECK(pit_pic_take(&s.pic) && s.pic.in_service && !s.pic.pending);
    CHECK(pit_periodic_irq0_advance(&s, 21) && !s.pic.pending); /* high is not an edge */

    CHECK(pit_periodic_irq0_advance(&s, 32)); /* crossed rises; ends LOW */
    CHECK(!pit_periodic_out(&s.pit) && !s.pit.pending && s.pic.pending == 1);
    REFUSE(s, pit_pic_take(&s.pic)); /* unmasked but in service */
    REFUSE(s, pit_periodic_irq0_advance(&s, 20));
    REFUSE(s, pit_periodic_irq0_write(&s, 0x43, 1, 0x39)); /* unsupported BCD */
    REFUSE(s, pit_periodic_irq0_write(&s, 0x20, 1, 0x60)); /* mask first */
    CHECK(pit_periodic_irq0_write(&s, 0x21, 1, 0xff));
    REFUSE(s, pit_periodic_irq0_write(&s, 0x20, 1, 0x61)); /* wrong IRQ */
    CHECK(pit_periodic_irq0_write(&s, 0x20, 1, 0x60));
    REFUSE(s, pit_periodic_irq0_write(&s, 0x20, 1, 0x60)); /* duplicate */
    REFUSE(s, pit_pic_take(&s.pic));
    CHECK(pit_periodic_irq0_write(&s, 0x21, 1, 0xfe));
    CHECK(pit_pic_take(&s.pic) && s.pic.in_service && !s.pic.pending);
    CHECK(pit_periodic_irq0_write(&s, 0x21, 1, 0xff));
    CHECK(pit_periodic_irq0_write(&s, 0x20, 1, 0x60));
    CHECK(pit_periodic_irq0_write(&s, 0x21, 1, 0xfe));
    CHECK(pit_periodic_irq0_advance(&s, 32));
    REFUSE(s, pit_pic_take(&s.pic)); /* no queued ticks or phantom after EOI */

    CHECK(pit_periodic_irq0_write(&s, 0x43, 1, 0x34)); /* low -> high at cursor */
    CHECK(pit_pic_take(&s.pic) && !s.pit.pending);
    CHECK(pit_periodic_irq0_write(&s, 0x43, 1, 0x34) && !s.pic.pending);
}

/* Software-triggered strobe: CR writes aren't control writes. Even a
 * complete rewrite during the low pulse cannot raise OUT at the cursor. */
static void strobe_rewrites(void)
{
    PitPeriodic p;
    pit_periodic_init(&p);
    CHECK(pit_periodic_write(&p, 0x43, 1, 0x38));
    write_count(&p, 32);
    sample(&p, 30, 1, 0);
    CHECK(pit_periodic_write(&p, 0x40, 1, 64)); /* LSB across terminal count */
    sample(&p, 32, 1, 0);
    sample(&p, 33, 0, 0);
    sample(&p, 34, 1, 1);
    sample(&p, 35, 1, 0);
    CHECK(pit_periodic_write(&p, 0x40, 1, 0));
    sample(&p, 40, 1, 0);
    write_count(&p, 15); /* replace 64 while counting */
    sample(&p, 55, 1, 0);
    sample(&p, 56, 0, 0);
    write_count(&p, 1); /* chip minimum, below Linux's minimum delta */
    sample(&p, 56, 0, 0);
    sample(&p, 57, 1, 1); /* old strobe ends on the new load pulse */
    sample(&p, 58, 0, 0);
    write_count(&p, 1);
    sample(&p, 60, 0, 1); /* skipped recovery; new strobe is already low */
    sample(&p, 60, 0, 0);
    CHECK(pit_periodic_write(&p, 0x43, 1, 0x38)); /* unlike count: immediate rise */
    sample(&p, 60, 1, 1);
    sample(&p, 102, 1, 0); /* neither old recovery nor superseded 64 survives */
    write_count(&p, 15);
    sample(&p, 117, 1, 0);
    write_count(&p, 0x7fff); /* now replace with a longer Linux delta */
    sample(&p, 118, 1, 0);
    sample(&p, 119, 1, 0);
    sample(&p, 117 + 0x7fff, 1, 0);
    sample(&p, 118 + 0x7fff, 0, 0);
    sample(&p, 119 + 0x7fff, 1, 1);
    sample(&p, UINT64_MAX, 1, 0);
}

static void strobe_cursor_limits(void)
{
    PitPeriodic p;
    pit_periodic_init(&p);
    CHECK(pit_periodic_write(&p, 0x43, 1, 0x38));
    write_count(&p, 0);
    /* A jump can skip the entire strobe; do not narrow elapsed to a count. */
    CHECK(pit_periodic_advance(&p, UINT64_MAX - 65538));
    REFUSE(p, pit_periodic_advance(&p, 0));
    sample(&p, UINT64_MAX - 65538, 1, 1);
    write_count(&p, 0); /* zero encodes 65536 */
    sample(&p, UINT64_MAX - 2, 1, 0);
    sample(&p, UINT64_MAX - 1, 0, 0);
    sample(&p, UINT64_MAX, 1, 1);
    write_count(&p, 15); /* accepted, but beyond the representable horizon */
    sample(&p, UINT64_MAX, 1, 0);
}

/* With no final 30h, 00h/00h is a real mode-0 count. Distinguish its
 * terminal rise from mode 4, and from a shutdown-only byte recognizer. */
static void terminal_count(void)
{
    PitPeriodic p;
    pit_periodic_init(&p);
    CHECK(pit_periodic_write(&p, 0x43, 1, 0x30));
    sample(&p, 10, 0, 0);
    write_count(&p, 0);
    sample(&p, 10 + 65536, 0, 0);
    sample(&p, 11 + 65536, 1, 1);
    sample(&p, 70000, 1, 0);
    CHECK(pit_periodic_write(&p, 0x40, 1, 3));
    sample(&p, 70000, 0, 0); /* first byte immediately lowers OUT */
    CHECK(pit_periodic_write(&p, 0x40, 1, 0));
    sample(&p, 70003, 0, 0);
    CHECK(pit_periodic_write(&p, 0x40, 1, 1)); /* and stops a running count */
    sample(&p, 70010, 0, 0);
    CHECK(pit_periodic_write(&p, 0x40, 1, 0));
    sample(&p, 70011, 0, 0);
    sample(&p, 70012, 1, 1);

    /* Empty latch distinguishes an erroneous low-to-low programming IRQ
     * from the retention of an already pending PIC bit in linux_switch. */
    CHECK(pit_periodic_write(&p, 0x43, 1, 0x38));
    write_count(&p, 1);
    sample(&p, 70014, 0, 0);
    CHECK(pit_periodic_write(&p, 0x43, 1, 0x30));
    sample(&p, 70014, 0, 0);
    sample(&p, 70015, 0, 0);
}

static void irq_sample(PitPeriodicIrq0 *s, uint64_t t, int level, int request)
{
    CHECK(pit_periodic_irq0_advance(s, t));
    CHECK(s->pit.tick == t && pit_periodic_out(&s->pit) == level);
    CHECK(!s->pit.pending && s->pic.pending == request);
    CHECK(!s->pic.armed && s->pic.now == 0); /* not the legacy logical clock */
}

/* Pinned v6.18.60 port sequence. Exercise both initial OUT levels of each
 * mode switch, on a single absolute cursor, with a separately held PIC IRR. */
static void linux_switch(int switch_low, int stop_low)
{
    PitPeriodicIrq0 s;
    uint64_t t = switch_low ? 1193U : 1192U;
    pit_periodic_irq0_init(&s);
    CHECK(pit_periodic_irq0_write(&s, 0x43, 1, 0x34));
    CHECK(pit_periodic_irq0_write(&s, 0x40, 1, 0xa9));
    CHECK(pit_periodic_irq0_write(&s, 0x40, 1, 0x04));
    irq_sample(&s, t, !switch_low, 0);
    CHECK(pit_periodic_irq0_write(&s, 0x40, 1, 0xaa)); /* abandon periodic LSB */
    CHECK(pit_periodic_irq0_write(&s, 0x43, 1, 0x38));
    irq_sample(&s, t, 1, switch_low);
    CHECK(pit_periodic_irq0_write(&s, 0x21, 1, 0xfe));
    CHECK(pit_pic_take(&s.pic) == switch_low);
    CHECK(pit_periodic_irq0_write(&s, 0x21, 1, 0xff));
    if (switch_low) CHECK(pit_periodic_irq0_write(&s, 0x20, 1, 0x60));

    CHECK(pit_periodic_irq0_write(&s, 0x40, 1, 15));
    t += 2400; /* no more periodic rises, no shot from a lone LSB */
    irq_sample(&s, t, 1, 0);
    CHECK(pit_periodic_irq0_write(&s, 0x40, 1, 0));
    irq_sample(&s, t + 15, 1, 0);
    irq_sample(&s, t + 16, 0, 0);
    irq_sample(&s, t + 17, 1, 1);
    t += 17;
    CHECK(pit_periodic_irq0_write(&s, 0x40, 1, 32));
    CHECK(pit_periodic_irq0_write(&s, 0x40, 1, 0));
    t += stop_low ? 33U : 32U;
    irq_sample(&s, t, !stop_low, 1);
    CHECK(pit_periodic_irq0_write(&s, 0x43, 1, 0x30)); /* neither level rises */
    irq_sample(&s, t, 0, 1);
    CHECK(pit_periodic_irq0_write(&s, 0x40, 1, 0));
    CHECK(pit_periodic_irq0_write(&s, 0x40, 1, 0));
    CHECK(pit_periodic_irq0_write(&s, 0x43, 1, 0x30));
    CHECK(pit_periodic_irq0_write(&s, 0x21, 1, 0xfe));
    CHECK(pit_pic_take(&s.pic) && !s.pic.pending && s.pic.in_service);
    irq_sample(&s, t + 2, 0, 0); /* canceled mode-4 edge, even if already low */
    irq_sample(&s, UINT64_MAX, 0, 0); /* final 30h canceled mode-0 terminal count */
    irq_sample(&s, UINT64_MAX, 0, 0);
    CHECK(pit_periodic_irq0_write(&s, 0x21, 1, 0xff));
    CHECK(pit_periodic_irq0_write(&s, 0x20, 1, 0x60));
    CHECK(pit_periodic_irq0_write(&s, 0x21, 1, 0xfe));
    REFUSE(s, pit_pic_take(&s.pic)); /* no third event to service */
}

int main(void)
{
    irq0_service();
    linux_periodic();
    live_reload();
    long_jump();
    divisor_limits();
    refused_writes_and_restart();
    puts("PIT channel 0 periodic mode 2: PASS");
    strobe_rewrites();
    strobe_cursor_limits();
    terminal_count();
    linux_switch(0, 1);
    linux_switch(1, 0);
    puts("PIT channel 0 periodic / strobe / disable on one cursor: PASS");
    return 0;
}
