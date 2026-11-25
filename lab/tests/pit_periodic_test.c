/* Host only, from the repo root:
 * cc -std=c11 -Wall -Wextra -Werror lab/tests/pit_periodic_test.c -o pit_periodic_test && ./pit_periodic_test
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../firmware/pit_periodic.h"

#define CHECK(e) do { if (!(e)) { fprintf(stderr, "%d: %s\n", __LINE__, #e); exit(EXIT_FAILURE); } } while (0)
#define REFUSE(p, e) do { unsigned char before[sizeof(p)]; memcpy(before, &(p), sizeof(p)); \
    CHECK(!(e)); CHECK(!memcmp(before, &(p), sizeof(p))); } while (0)

static void program(PitPeriodic *p, uint16_t n)
{
    CHECK(pit_periodic_write(p, 0x43, 1, 0x34));
    CHECK(pit_periodic_write(p, 0x40, 1, (uint8_t)n));
    CHECK(pit_periodic_write(p, 0x40, 1, (uint8_t)(n >> 8)));
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
    REFUSE(p, pit_periodic_write(&p, 0x43, 1, 0x38)); /* mode 4 stays separate */
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

int main(void)
{
    linux_periodic();
    live_reload();
    long_jump();
    divisor_limits();
    refused_writes_and_restart();
    puts("PIT channel 0 periodic mode 2: PASS");
    return 0;
}
