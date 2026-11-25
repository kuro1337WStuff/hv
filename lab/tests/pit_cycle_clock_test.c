/* Host only. From the worktree root in a VS developer shell:
 * cl /nologo /std:c11 /W4 /WX lab\tests\pit_cycle_clock_test.c && pit_cycle_clock_test.exe */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../firmware/pit_cycle_clock.h"

#define CHECK(e) do { if (!(e)) { fprintf(stderr, "%d: %s\n", __LINE__, #e); exit(EXIT_FAILURE); } } while (0)
#define UNCHANGED(c, e) do { unsigned char saved[sizeof *(c)]; memcpy(saved, (c), sizeof saved); \
    CHECK(e); CHECK(!memcmp(saved, (c), sizeof saved)); } while (0)
#define VIRTUAL_HZ UINT32_C(2500000000)

static void wr(PitCycleClock *c, uint16_t port, uint8_t byte)
{
    CHECK(pit_shared_write(&c->pit, port, 1, byte));
}

static uint8_t rd(PitCycleClock *c, uint16_t port)
{
    uint8_t byte;
    CHECK(pit_shared_read(&c->pit, port, 1, &byte));
    return byte;
}

static void program_pair(PitCycleClock *c)
{
    CHECK(pit_cycle_clock_init(c, VIRTUAL_HZ));
    wr(c, 0x43, 0x34); /* CH0 periodic, divisor 3, initially masked. */
    wr(c, 0x40, 3);
    wr(c, 0x40, 0);
    wr(c, 0x43, 0xb0); /* CH2 loads 1234h with GATE off, then holds. */
    wr(c, 0x42, 0x34);
    wr(c, 0x42, 0x12);
}

static void observe_pair(PitCycleClock *c)
{
    CHECK(c->elapsed_ns == 10001 && c->cycles == 25002);
    CHECK(c->pit.ch0.pit.tick == 11 && c->pit.ch2.tick == 11);
    CHECK(pit_periodic_out(&c->pit.ch0.pit));
    CHECK(rd(c, 0x21) == 0xff && c->pit.ch0.pic.pending && !pit_pic_take(&c->pit.ch0.pic));
    CHECK(rd(c, 0x61) == 0 && rd(c, 0x42) == 0x34);
    UNCHANGED(c, pit_cycle_clock_advance(c, 10001));
    UNCHANGED(c, !pit_cycle_clock_advance(c, 10000)); /* Still PIT tick 11. */
    CHECK(rd(c, 0x42) == 0x12); /* High byte and masked edge survived. */

    wr(c, 0x61, 1);
    CHECK(pit_cycle_clock_advance(c, 13000)); /* Gate open but IRQ still masked. */
    CHECK(c->cycles == 32500 && c->pit.ch2.tick == 15);
    CHECK(rd(c, 0x42) == 0x30 && rd(c, 0x42) == 0x12);
    CHECK(!pit_periodic_out(&c->pit.ch0.pit) && !pit_pic_take(&c->pit.ch0.pic));
    wr(c, 0x21, 0xfe);
    CHECK(pit_pic_take(&c->pit.ch0.pic));
    CHECK(pit_cycle_clock_advance(c, 13001) && c->cycles == 32502);
    CHECK(!c->pit.ch0.pic.pending && c->pit.ch0.pic.in_service);
}

static void partitions_and_controls(void)
{
    PitCycleClock bulk, split;
    uint64_t ns;
    program_pair(&bulk);
    program_pair(&split);
    CHECK(pit_cycle_clock_advance(&bulk, 10001));
    CHECK(pit_cycle_clock_advance(&split, 0) && !split.cycles && !split.pit.ch2.tick);
    CHECK(pit_cycle_clock_advance(&split, 1) && split.cycles == 2 && !split.pit.ch2.tick);
    CHECK(pit_cycle_clock_advance(&split, 838) && split.cycles == 2095 && !split.pit.ch2.tick);
    CHECK(pit_cycle_clock_advance(&split, 839) && split.cycles == 2097 && split.pit.ch2.tick == 1);
    for (ns = 840; ns <= 10001; ++ns) CHECK(pit_cycle_clock_advance(&split, ns));
    observe_pair(&bulk);
    observe_pair(&split);
}

static void measure_ch2_msb_interval(void)
{
    PitCycleClock c;
    uint64_t ns, cycles_fe = 0;
    uint8_t prev = 0;
    CHECK(pit_cycle_clock_init(&c, VIRTUAL_HZ));
    wr(&c, 0x61, 1);
    wr(&c, 0x43, 0xb0);
    wr(&c, 0x42, 0xff);
    wr(&c, 0x42, 0xff);
    /* Poll live LSB/MSB pairs on an independent 31 ns grid. Only a change
     * of the observed count byte starts/stops the cycle measurement. */
    for (ns = 0; ns < 450000; ns += 31) {
        uint8_t low, high;
        CHECK(pit_cycle_clock_advance(&c, ns));
        low = rd(&c, 0x42);
        high = rd(&c, 0x42);
        if (prev == 0xff && high == 0xfe) {
            CHECK(ns == 215419 && low == 0xff);
            cycles_fe = c.cycles;
        } else if (prev == 0xfe && high == 0xfd) {
            CHECK(ns == 429970 && low == 0xff && cycles_fe == 538547);
            /* Ideal: 256 PIT pulses * 2,500,000,000 / 1,193,182 =
             * 536380.87... virtual cycles. Pulses 257 and 513 are first
             * seen at the grid times above; floor(5*ns/2) at EACH end
             * gives 1074925 - 538547 = 536378, not floor(ideal). */
            CHECK(c.cycles - cycles_fe == 536378);
            printf("CH2 FE->FD: %" PRIu64 " virtual cycles / 256 PIT pulses at %" PRIu32 " virtual Hz\n",
                   c.cycles - cycles_fe, c.hz);
            return;
        }
        prev = high;
    }
    fputs("Missing CH2 MSB transitions\n", stderr);
    exit(EXIT_FAILURE);
}

static void rate_and_epoch_bounds(void)
{
    PitCycleClock c;
    uint64_t wrap_tick;
    CHECK(pit_cycle_clock_init(&c, 1));
    CHECK(pit_cycle_clock_advance(&c, 999999999) && !c.cycles);
    CHECK(pit_cycle_clock_advance(&c, 1000000000) && c.cycles == 1);
    CHECK(pit_cycle_clock_advance(&c, UINT64_MAX));
    CHECK(c.cycles == UINT64_C(18446744073) && c.pit.ch2.tick == UINT64_C(22010322987356910));
    UNCHANGED(&c, !pit_cycle_clock_init(&c, 0));

    CHECK(pit_cycle_clock_init(&c, UINT32_MAX));
    /* (2^32+1) whole seconds * (2^32-1) Hz = 2^64-1 cycles.
     * The next ns adds four complete cycles, wrapping without a PIT pulse. */
    CHECK(pit_cycle_clock_advance(&c, UINT64_C(4294967297000000000)) && c.cycles == UINT64_MAX);
    wrap_tick = c.pit.ch2.tick;
    CHECK(pit_cycle_clock_advance(&c, UINT64_C(4294967297000000001)) && c.cycles == 3);
    CHECK(c.pit.ch0.pit.tick == wrap_tick && c.pit.ch2.tick == wrap_tick && !c.pit.ch0.pic.pending);
    CHECK(pit_cycle_clock_advance(&c, UINT64_MAX));
    CHECK(c.cycles == UINT64_C(5441186200979387051));
    CHECK(c.pit.ch0.pit.tick == UINT64_C(22010322987356910) && c.pit.ch2.tick == c.pit.ch0.pit.tick);
    UNCHANGED(&c, pit_cycle_clock_advance(&c, UINT64_MAX));
    UNCHANGED(&c, !pit_cycle_clock_advance(&c, UINT64_MAX - 1));
}

int main(void)
{
    partitions_and_controls();
    measure_ch2_msb_interval();
    rate_and_epoch_bounds();
    puts("PASS: virtual cycle clock, shared PIT, observed CH2 interval");
    return 0;
}
