/* Host C11. Linux v6.18.60 (f45ed76d6791), arch/x86/kernel/tsc.c:
 * quick_pit_calibrate()/pit_verify_msb() and pit_calibrate_tsc(). Only
 * their PIT byte observations are exercised, not their TSC measurements. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../firmware/pit_shared.h"

#define CHECK(e) do { if (!(e)) { fprintf(stderr, "%d: %s\n", __LINE__, #e); exit(EXIT_FAILURE); } } while (0)
#define UNCHANGED(s, e) do { unsigned char before[sizeof(s)]; memcpy(before, &(s), sizeof(s)); \
    CHECK(e); CHECK(!memcmp(before, &(s), sizeof(s))); } while (0)

static void wr(PitShared *s, uint16_t port, uint8_t byte)
{
    CHECK(pit_shared_write(s, port, 1, byte));
}

static uint8_t rd(PitShared *s, uint16_t port)
{
    uint8_t byte;
    CHECK(pit_shared_read(s, port, 1, &byte));
    return byte;
}

static void gate_on_speaker_off(PitShared *s)
{
    wr(s, 0x61, (rd(s, 0x61) & ~2) | 1);
}

static void eoi(PitShared *s)
{
    CHECK(rd(s, 0x21) == 0xfe);
    wr(s, 0x21, 0xff);
    wr(s, 0x20, 0x60);
    wr(s, 0x21, 0xfe);
}

static void quick_and_periodic(void)
{
    PitShared s;
    pit_shared_init(&s);
    wr(&s, 0x21, 0xfe);
    wr(&s, 0x61, 2);
    gate_on_speaker_off(&s);
    wr(&s, 0x43, 0x34);
    wr(&s, 0x40, 0xa9);
    wr(&s, 0x43, 0xb0);
    wr(&s, 0x42, 0xff);
    CHECK(pit_shared_advance(&s, 10));
    wr(&s, 0x40, 0x04); /* CH0 divisor 1193, completed at 10 */
    CHECK(pit_shared_advance(&s, 12));
    wr(&s, 0x42, 0xff); /* CH2 divisor 65535, completed at 12 */
    CHECK(rd(&s, 0x42) == 0 && rd(&s, 0x42) == 0); /* discarded pair */
    CHECK(pit_shared_advance(&s, 13));
    CHECK(rd(&s, 0x42) == 0xff && rd(&s, 0x42) == 0xff);
    CHECK(pit_shared_advance(&s, 268));
    CHECK(rd(&s, 0x42) == 0 && rd(&s, 0x42) == 0xff);
    CHECK(rd(&s, 0x42) == 0);
    CHECK(pit_shared_advance(&s, 269));
    CHECK(rd(&s, 0x42) == 0xfe); /* unlatched MSB, sampled a pulse later */
    wr(&s, 0x61, 2); /* stop CH2 at FEFF; leave speaker latch on */
    CHECK(pit_shared_advance(&s, 1203));
    CHECK(rd(&s, 0x61) == 2 && !pit_pic_take(&s.ch0.pic));
    CHECK(pit_shared_advance(&s, 1204));
    CHECK(rd(&s, 0x42) == 0xff && rd(&s, 0x42) == 0xfe);
    CHECK(pit_pic_take(&s.ch0.pic)); /* CH2 GATE did not gate CH0 */
    eoi(&s);
    gate_on_speaker_off(&s);
    CHECK(pit_shared_advance(&s, 1205));
    CHECK(rd(&s, 0x42) == 0xfe && rd(&s, 0x42) == 0xfe);
    CHECK(rd(&s, 0x61) == 1 && !pit_pic_take(&s.ch0.pic));
}

static void fallback_and_strobe(void)
{
    PitShared s;
    pit_shared_init(&s);
    wr(&s, 0x21, 0xfe);
    gate_on_speaker_off(&s);
    wr(&s, 0x43, 0xb0);
    wr(&s, 0x42, 0x9b); /* 10 ms CAL_LATCH = 11931 (2e9bh) */
    wr(&s, 0x43, 0x38);
    wr(&s, 0x40, 15);
    wr(&s, 0x42, 0x2e);
    wr(&s, 0x40, 0);
    CHECK(pit_shared_advance(&s, 16));
    CHECK(rd(&s, 0x61) == 1 && !pit_pic_take(&s.ch0.pic));
    CHECK(pit_shared_advance(&s, 17));
    wr(&s, 0x43, 0x30); /* pinned CH0 disable cannot retract IRQ0 */
    wr(&s, 0x40, 0);
    wr(&s, 0x40, 0);
    wr(&s, 0x43, 0x30);
    CHECK(pit_pic_take(&s.ch0.pic));
    eoi(&s);
    CHECK(pit_shared_advance(&s, 11931));
    CHECK(rd(&s, 0x61) == 1);
    CHECK(pit_shared_advance(&s, 11932));
    CHECK(rd(&s, 0x61) == 0x21 && !pit_pic_take(&s.ch0.pic));
    wr(&s, 0x61, 3);
    CHECK(rd(&s, 0x61) == 0x23);
    gate_on_speaker_off(&s); /* Linux RMW echoes the high OUT bit */
    wr(&s, 0x43, 0xb0);
    wr(&s, 0x42, 0x0b); /* 50 ms CAL2_LATCH = 59659 (e90bh) */
    wr(&s, 0x42, 0xe9);
    CHECK(rd(&s, 0x61) == 1);
    CHECK(pit_shared_advance(&s, 71591));
    CHECK(rd(&s, 0x61) == 1);
    CHECK(pit_shared_advance(&s, 71592));
    CHECK(rd(&s, 0x61) == 0x21 && !pit_pic_take(&s.ch0.pic));
}

static void large_jump_and_refusals(void)
{
    static const struct { uint16_t port; unsigned width; uint8_t byte; } bad[] = {
        {0x40, 2, 0}, {0x42, 2, 0}, {0x43, 2, 0xb0}, {0x61, 2, 3},
        {0x43, 1, 0x74}, {0x43, 1, 0xc0}, {0x43, 1, 0x36}, {0x43, 1, 0xb4},
        {0x41, 1, 0}, {0x80, 1, 0}, {0x61, 1, 4}, {0x21, 257, 0xfe}, {0x20, 1, 0x61}
    };
    static const uint16_t unreadable[] = {0x40, 0x41, 0x43, 0x20, 0x80};
    PitShared s;
    uint8_t dest = 0xa5;
    size_t i;
    pit_shared_init(&s);
    wr(&s, 0x43, 0x34);
    wr(&s, 0x40, 2);
    wr(&s, 0x40, 0);
    gate_on_speaker_off(&s);
    wr(&s, 0x43, 0xb0);
    wr(&s, 0x42, 0x34);
    wr(&s, 0x42, 0x12);
    CHECK(pit_shared_advance(&s, UINT64_MAX)); /* many periods, masked */
    CHECK(rd(&s, 0x61) == 0x21 && rd(&s, 0x42) == 0x36);
    wr(&s, 0x40, 1); /* leave a CH0 high-byte write and CH2 high-byte read outstanding */
    UNCHANGED(s, pit_shared_advance(&s, UINT64_MAX));
    UNCHANGED(s, !pit_shared_advance(&s, UINT64_MAX - 1));
    UNCHANGED(s, !pit_shared_write(&s, 0x40, 1, 0)); /* divisor 1 */
    for (i = 0; i < sizeof(bad)/sizeof(bad[0]); ++i)
        UNCHANGED(s, !pit_shared_write(&s, bad[i].port, bad[i].width, bad[i].byte));
    for (i = 0; i < sizeof(unreadable)/sizeof(unreadable[0]); ++i) {
        UNCHANGED(s, !pit_shared_read(&s, unreadable[i], 1, &dest));
        CHECK(dest == 0xa5);
    }
    UNCHANGED(s, !pit_shared_read(&s, 0x42, 2, &dest));
    UNCHANGED(s, !pit_shared_read(&s, 0x21, 257, &dest));
    UNCHANGED(s, !pit_shared_read(&s, 0x42, 1, NULL));
    CHECK(dest == 0xa5 && rd(&s, 0x42) == 0x12); /* full count = 1236h */
    CHECK(!pit_pic_take(&s.ch0.pic));
    wr(&s, 0x21, 0xfe);
    CHECK(pit_pic_take(&s.ch0.pic));
    eoi(&s);
    CHECK(!pit_pic_take(&s.ch0.pic)); /* no FIFO of missed periods */
}

static void reject_before_either_advance(void)
{
    PitShared s;
    pit_shared_init(&s);
    wr(&s, 0x43, 0x30);
    wr(&s, 0x40, 2);
    wr(&s, 0x40, 0);
    /* Deliberately break lockstep: a speculative CH0 advance to 3 would
     * fire IRQ0 before CH2 refuses. The reverse order must be safe too. */
    CHECK(pit2_advance(&s.ch2, 4));
    UNCHANGED(s, !pit_shared_advance(&s, 3));
    pit_shared_init(&s);
    CHECK(pit_periodic_irq0_advance(&s.ch0, 4));
    UNCHANGED(s, !pit_shared_advance(&s, 3));
}

int main(void)
{
    quick_and_periodic();
    fallback_and_strobe();
    large_jump_and_refusals();
    reject_before_either_advance();
    puts("PASS: shared PIT pulses, CH0 IRQ0 and CH2 calibration ports");
    return 0;
}
