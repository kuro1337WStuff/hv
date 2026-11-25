/** @file Fixed-input host tests; no real I/O or time source. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../firmware/pit_channel2.h"

#define CHECK(e) do { if (!(e)) { fprintf(stderr, "%d: %s\n", __LINE__, #e); exit(EXIT_FAILURE); } } while (0)
#define REFUSE(p, e) do { unsigned char before[sizeof(p)]; memcpy(before, &(p), sizeof(p)); \
    CHECK(!(e)); CHECK(!memcmp(before, &(p), sizeof(p))); } while (0)

static uint8_t input(PitChannel2 *p, uint16_t port)
{
    uint8_t byte;
    CHECK(pit2_read(p, port, 1, &byte));
    return byte;
}

static void program(PitChannel2 *p, uint16_t reload)
{
    CHECK(pit2_write(p, 0x43, 1, 0xb0));
    CHECK(pit2_write(p, 0x42, 1, (uint8_t)reload));
    CHECK(pit2_write(p, 0x42, 1, (uint8_t)(reload >> 8)));
}

static void quick_read(void)
{
    PitChannel2 p;
    pit2_init(&p);
    CHECK(pit2_write(&p, 0x61, 1, 2)); /* initially speaker enabled */
    CHECK(input(&p, 0x61) == 2);
    CHECK(pit2_write(&p, 0x61, 1, (input(&p, 0x61) & ~2) | 1));
    program(&p, 0xffff);
    CHECK(input(&p, 0x42) == 0 && input(&p, 0x42) == 0); /* discarded pair */
    CHECK(pit2_advance(&p, 1));
    CHECK(input(&p, 0x42) == 0xff && input(&p, 0x42) == 0xff);
    CHECK(pit2_advance(&p, 256));
    CHECK(input(&p, 0x42) == 0 && input(&p, 0x42) == 0xff);
    CHECK(input(&p, 0x42) == 0);
    CHECK(input(&p, 0x61) == 1);
    CHECK(pit2_advance(&p, 257));
    CHECK(input(&p, 0x42) == 0xfe); /* live MSB, not latched at LSB read */
    CHECK(input(&p, 0x42) == 0xff && input(&p, 0x42) == 0xfe);
}

static void fallback_rearm(void)
{
    PitChannel2 p;
    unsigned i;
    const uint16_t reload[] = {11931, 59659}; /* actual CAL_LATCH, CAL2_LATCH */
    uint64_t start = 0;
    pit2_init(&p);
    for (i = 0; i < 2; ++i) {
        CHECK(pit2_write(&p, 0x61, 1, (input(&p, 0x61) & ~2) | 1));
        program(&p, reload[i]);
        CHECK(input(&p, 0x61) == 1);
        CHECK(pit2_advance(&p, start + reload[i]));
        CHECK(input(&p, 0x61) == 1);
        start += reload[i] + 1U;
        CHECK(pit2_advance(&p, start));
        CHECK(input(&p, 0x61) == 0x21);
        CHECK(pit2_advance(&p, ++start));
        CHECK(input(&p, 0x42) == 0xff && input(&p, 0x61) == 0x21);
        /* Leave the MSB outstanding: next mode command resets read order. */
    }
}

static void gate_cancel(void)
{
    PitChannel2 p;
    pit2_init(&p);
    program(&p, 0x1234);
    CHECK(pit2_advance(&p, 900)); /* load with GATE low, no decrement */
    CHECK(input(&p, 0x42) == 0x34 && input(&p, 0x42) == 0x12);
    CHECK(pit2_write(&p, 0x61, 1, 1));
    CHECK(pit2_advance(&p, 902));
    CHECK(pit2_write(&p, 0x61, 1, 0));
    CHECK(pit2_advance(&p, 10000));
    CHECK(input(&p, 0x42) == 0x32 && input(&p, 0x42) == 0x12);
    CHECK(pit2_write(&p, 0x61, 1, 1));
    CHECK(pit2_advance(&p, 14657)); /* one before old terminal */
    CHECK(input(&p, 0x61) == 1);
    CHECK(pit2_write(&p, 0x43, 1, 0xb0));
    CHECK(pit2_write(&p, 0x42, 1, 0x34));
    CHECK(pit2_advance(&p, 20000));
    CHECK(input(&p, 0x61) == 1); /* no stale or partial-count completion */
    program(&p, 0); /* fresh command abandons staged 34h; zero means 65536 */
    CHECK(pit2_advance(&p, 85536));
    CHECK(input(&p, 0x61) == 1);
    CHECK(pit2_advance(&p, 85537));
    CHECK(input(&p, 0x61) == 0x21);
}

static void refused_access(void)
{
    PitChannel2 p;
    uint8_t dest = 0xcc;
    pit2_init(&p);
    REFUSE(p, pit2_write(&p, 0x42, 1, 0xab));
    CHECK(pit2_write(&p, 0x43, 1, 0xb0));
    CHECK(pit2_write(&p, 0x42, 1, 0xcd));
    REFUSE(p, pit2_read(&p, 0x42, 1, &dest));
    REFUSE(p, pit2_write(&p, 0x43, 1, 0xb4));
    REFUSE(p, pit2_write(&p, 0x42, 2, 0xab));
    REFUSE(p, pit2_write(&p, 0x40, 1, 0xab));
    REFUSE(p, pit2_write(&p, 0x61, 1, 0x80));
    CHECK(pit2_write(&p, 0x42, 1, 0xab));
    CHECK(pit2_advance(&p, UINT64_MAX)); /* load, then frozen by GATE */
    CHECK(input(&p, 0x42) == 0xcd);
    REFUSE(p, pit2_read(&p, 0x42, 2, &dest));
    REFUSE(p, pit2_read(&p, 0x43, 1, &dest));
    REFUSE(p, pit2_read(&p, 0x42, 1, NULL));
    REFUSE(p, pit2_advance(&p, 0));
    CHECK(dest == 0xcc && input(&p, 0x42) == 0xab);
}

int main(void)
{
    quick_read();
    fallback_rearm();
    gate_cancel();
    refused_access();
    puts("PIT channel 2 calibration subset: PASS");
    return 0;
}
