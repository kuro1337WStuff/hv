/* Host only: cc -std=c11 -Wall -Wextra -Werror lab/tests/pit_pic_test.c -o pit_pic_test */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../firmware/pit_pic.h"

/* The guest IDT gate and the queued interrupt both use this alias; they
 * could agree on the wrong chip's vector. Pin the IRQ0 wire value here. */
_Static_assert(PIT_PIC_VECTOR == 0x30, "IRQ0 must use master vector 0x30");

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "line %d: %s\n", __LINE__, #expr); exit(EXIT_FAILURE); \
} } while (0)

/* Snapshot bytes rather than comparing copied structs (padding need not copy).
 * Evaluate the request exactly once, also when compiled with NDEBUG. */
#define REFUSE_UNCHANGED(p, request) do { \
    unsigned char before[sizeof(p)]; \
    memcpy(before, &(p), sizeof(p)); \
    CHECK(!(request)); \
    CHECK(memcmp(before, &(p), sizeof(p)) == 0); \
} while (0)

static void arm(PitPic *p, uint16_t reload)
{
    CHECK(pit_pic_write(p, 0x43, 1, 0x38));
    CHECK(pit_pic_write(p, 0x40, 1, (uint8_t)reload));
    CHECK(pit_pic_write(p, 0x40, 1, (uint8_t)(reload >> 8)));
}

static void clock_bounds(void)
{
    PitPic p;
    uint64_t ticks;
    CHECK(pit_pic_ns_to_ticks(0, &ticks) && ticks == 0);
    CHECK(pit_pic_ns_to_ticks(838, &ticks) && ticks == 0);
    CHECK(pit_pic_ns_to_ticks(839, &ticks) && ticks == 1);
    CHECK(pit_pic_ns_to_ticks(UINT64_C(1000000000), &ticks) && ticks == 1193182);
    CHECK(pit_pic_ns_to_ticks(UINT64_MAX, &ticks) && ticks == UINT64_C(22010322987356910));
    CHECK(!pit_pic_ns_to_ticks(1, NULL));

    pit_pic_init(&p);
    CHECK(pit_pic_advance(&p, UINT64_MAX - 257));
    arm(&p, 2);
    CHECK(pit_pic_write(&p, 0x40, 1, 1));
    /* An overflowing replacement must preserve the old arm and staged LSB. */
    REFUSE_UNCHANGED(p, pit_pic_write(&p, 0x40, 1, 2));
    CHECK(p.armed && p.deadline == UINT64_MAX - 255);
    CHECK(pit_pic_write(&p, 0x40, 1, 1)); /* 0x101, deadline exactly UINT64_MAX */
    CHECK(p.armed && p.deadline == UINT64_MAX);
    CHECK(pit_pic_advance(&p, UINT64_MAX - 1));
    CHECK(!p.pending);
    REFUSE_UNCHANGED(p, pit_pic_advance(&p, 0));
    REFUSE_UNCHANGED(p, pit_pic_wake(&p, UINT64_MAX - 1));
    CHECK(pit_pic_wake(&p, UINT64_MAX));
    CHECK(!p.armed && p.pending && p.now == UINT64_MAX);
}

static void access_contract(void)
{
    static const uint8_t bad_commands[] = {0x00, 0x30, 0x34, 0x39, 0x78, 0xb8, 0xc0};
    static const uint16_t bad_reads[] = {0x20, 0x40, 0x41, 0x42, 0x43, 0xa1};
    static const uint8_t bad_widths[] = {0, 2, 4, 8};
    PitPic p;
    uint8_t value = 0xa5;
    pit_pic_init(&p);
    REFUSE_UNCHANGED(p, pit_pic_write(&p, 0x40, 1, 1)); /* no mode word */
    CHECK(pit_pic_read(&p, 0x21, 1, &value) && value == 0xff);
    arm(&p, 32);
    for (size_t i = 0; i < sizeof(bad_commands); ++i)
        REFUSE_UNCHANGED(p, pit_pic_write(&p, 0x43, 1, bad_commands[i]));
    for (size_t i = 0; i < sizeof(bad_widths); ++i) {
        REFUSE_UNCHANGED(p, pit_pic_write(&p, 0x40, bad_widths[i], 1));
        REFUSE_UNCHANGED(p, pit_pic_write(&p, 0x43, bad_widths[i], 0x38));
        REFUSE_UNCHANGED(p, pit_pic_write(&p, 0x21, bad_widths[i], 0xfe));
        REFUSE_UNCHANGED(p, pit_pic_read(&p, 0x21, bad_widths[i], &value));
        CHECK(value == 0xff);
    }
    for (size_t i = 0; i < sizeof(bad_reads) / sizeof(bad_reads[0]); ++i) {
        REFUSE_UNCHANGED(p, pit_pic_read(&p, bad_reads[i], 1, &value));
        CHECK(value == 0xff);
    }
    REFUSE_UNCHANGED(p, pit_pic_read(&p, 0x21, 1, NULL));
    REFUSE_UNCHANGED(p, pit_pic_write(&p, 0x21, 1, 0xfc)); /* other IRQ unmasked */
    REFUSE_UNCHANGED(p, pit_pic_write(&p, 0xa1, 1, 0xff)); /* slave not modeled */
    REFUSE_UNCHANGED(p, pit_pic_write(&p, 0x41, 1, 0));
}

static void cancel_and_rearm(void)
{
    PitPic p;
    pit_pic_init(&p);
    REFUSE_UNCHANGED(p, pit_pic_wake(&p, 100));
    arm(&p, 5);
    CHECK(pit_pic_advance(&p, 4));
    pit_pic_cancel(&p);
    CHECK(pit_pic_advance(&p, 5) && !p.armed && !p.pending);
    REFUSE_UNCHANGED(p, pit_pic_write(&p, 0x40, 1, 7));
    arm(&p, 0); /* largest divisor, not immediate expiry */
    CHECK(p.deadline == 65541);
    CHECK(pit_pic_write(&p, 0x40, 1, 8)); /* replacement LSB only */
    CHECK(p.armed && p.deadline == 65541);
    CHECK(pit_pic_advance(&p, 65540) && !p.pending);
    CHECK(pit_pic_advance(&p, 65541) && p.pending && !p.armed);
    CHECK(pit_pic_write(&p, 0x40, 1, 0)); /* MSB commits at current time */
    CHECK(p.armed && p.pending && p.deadline == 65549);
    CHECK(pit_pic_write(&p, 0x40, 1, 99));
    CHECK(pit_pic_write(&p, 0x43, 1, 0x38)); /* abort old countdown and staged LSB */
    CHECK(!p.armed && p.pending);
    CHECK(pit_pic_write(&p, 0x40, 1, 7));
    CHECK(!p.armed);
    CHECK(pit_pic_write(&p, 0x40, 1, 0));
    CHECK(p.deadline == 65548);
    pit_pic_cancel(&p);
    CHECK(!p.armed && p.pending); /* stopping PIT does not clear PIC */
}

static void interrupt_order(void)
{
    PitPic p;
    uint8_t imr;
    pit_pic_init(&p);
    arm(&p, 32);
    CHECK(pit_pic_advance(&p, 31) && p.armed && !p.pending);
    REFUSE_UNCHANGED(p, pit_pic_take(&p));
    REFUSE_UNCHANGED(p, pit_pic_wake(&p, 31)); /* frozen source: no synthesized tick */
    CHECK(pit_pic_advance(&p, 32) && p.pending && !p.armed);
    REFUSE_UNCHANGED(p, pit_pic_take(&p)); /* masked edge retained */
    REFUSE_UNCHANGED(p, pit_pic_write(&p, 0x20, 1, 0x60)); /* not yet in service */
    CHECK(pit_pic_write(&p, 0x21, 1, 0xfe));
    CHECK(pit_pic_take(&p) && p.in_service && !p.pending);
    REFUSE_UNCHANGED(p, pit_pic_take(&p));
    REFUSE_UNCHANGED(p, pit_pic_write(&p, 0x20, 1, 0x60)); /* IRQ0 not masked */
    CHECK(pit_pic_read(&p, 0x21, 1, &imr) && imr == 0xfe);
    CHECK(pit_pic_write(&p, 0x21, 1, (uint8_t)(imr | 1)));
    arm(&p, 64);
    REFUSE_UNCHANGED(p, pit_pic_wake(&p, 95));
    CHECK(pit_pic_wake(&p, 100) && p.now == 96 && p.pending && p.in_service);
    arm(&p, 1);
    CHECK(pit_pic_advance(&p, 110)); /* a second edge into an already set IRR collapses */
    CHECK(pit_pic_write(&p, 0x21, 1, 0xfe));
    REFUSE_UNCHANGED(p, pit_pic_take(&p)); /* in service alone inhibits reentry */
    CHECK(pit_pic_write(&p, 0x21, 1, 0xff));
    REFUSE_UNCHANGED(p, pit_pic_write(&p, 0x20, 1, 0x61)); /* wrong specific EOI */
    REFUSE_UNCHANGED(p, pit_pic_write(&p, 0x20, 1, 0x20)); /* non-specific EOI */
    REFUSE_UNCHANGED(p, pit_pic_write(&p, 0x20, 2, 0x60));
    CHECK(pit_pic_write(&p, 0x20, 1, 0x60) && !p.in_service && p.pending);
    REFUSE_UNCHANGED(p, pit_pic_write(&p, 0x20, 1, 0x60)); /* duplicate, next tick intact */
    REFUSE_UNCHANGED(p, pit_pic_take(&p)); /* still masked */
    CHECK(pit_pic_write(&p, 0x21, 1, 0xfe));
    CHECK(pit_pic_take(&p) && !p.pending && p.in_service);
    CHECK(pit_pic_write(&p, 0x21, 1, 0xff));
    CHECK(pit_pic_write(&p, 0x20, 1, 0x60));
    CHECK(pit_pic_write(&p, 0x21, 1, 0xfe));
    CHECK(pit_pic_advance(&p, 110) && pit_pic_advance(&p, 200));
    REFUSE_UNCHANGED(p, pit_pic_take(&p)); /* no duplicate/phantom IRQ after EOI */
    REFUSE_UNCHANGED(p, pit_pic_wake(&p, 250));
}

int main(void)
{
    clock_bounds();
    access_contract();
    cancel_and_rearm();
    interrupt_order();
    puts("PASS: virtual PIT deadlines, access refusal, cancellation, IRQ0 latch and specific EOI");
    return 0;
}
