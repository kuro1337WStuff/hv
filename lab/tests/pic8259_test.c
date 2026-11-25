/* Host only: cc -std=c11 -Wall -Wextra -Werror lab/tests/pic8259_test.c -o pic8259_test */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../firmware/pic8259.h"

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "line %d: %s\n", __LINE__, #expr); exit(EXIT_FAILURE); \
} } while (0)

/* Refusal is transactional for the whole pair, including a partially set up
 * peer. Compare the object representation, not private phase/field values. */
static void refuse_write(Pic8259 *p, uint16_t port, unsigned width, uint8_t byte)
{
    unsigned char before[sizeof(*p)];
    memcpy(before, p, sizeof(before));
    CHECK(!pic8259_write(p, port, width, byte));
    CHECK(memcmp(before, p, sizeof(before)) == 0);
}

static void refuse_read(const Pic8259 *p, uint16_t port, unsigned width)
{
    uint8_t byte = 0xa5;
    CHECK(!pic8259_read(p, port, width, &byte));
    CHECK(byte == 0xa5);
}

static void expect_imr(const Pic8259 *p, uint16_t data, uint8_t expected)
{
    uint8_t byte;
    CHECK(pic8259_read(p, data, 1, &byte) && byte == expected);
}

/* Port numbers and ICW values are the wire contract, independent of the
 * model's constants. First follow the sequential normal-EOI driver path. */
static void pinned_probe_and_init(void)
{
    Pic8259 p;
    pic8259_init(&p);
    CHECK(!pic8259_ready(&p));
    expect_imr(&p, 0x21, 0xff);
    expect_imr(&p, 0xa1, 0xff);
    CHECK(pic8259_write(&p, 0xa1, 1, 0xff));
    CHECK(pic8259_write(&p, 0x21, 1, 0xfb));
    expect_imr(&p, 0x21, 0xfb);
    CHECK(pic8259_write(&p, 0x21, 1, 0xff));
    CHECK(pic8259_write(&p, 0x20, 1, 0x11));
    CHECK(pic8259_write(&p, 0x21, 1, 0x30));
    CHECK(pic8259_write(&p, 0x21, 1, 0x04));
    CHECK(pic8259_write(&p, 0x21, 1, 0x01));
    CHECK(!pic8259_ready(&p));
    CHECK(pic8259_write(&p, 0xa0, 1, 0x11));
    CHECK(pic8259_write(&p, 0xa1, 1, 0x38));
    CHECK(pic8259_write(&p, 0xa1, 1, 0x02));
    CHECK(pic8259_write(&p, 0xa1, 1, 0x01));
    CHECK(pic8259_ready(&p));
    expect_imr(&p, 0x21, 0);
    expect_imr(&p, 0xa1, 0);
    CHECK(pic8259_write(&p, 0x21, 1, 0xff));
    CHECK(pic8259_write(&p, 0xa1, 1, 0xff));
    expect_imr(&p, 0x21, 0xff);
    expect_imr(&p, 0xa1, 0xff);
}

static void interleaved_and_refused(void)
{
    static const struct { uint16_t cmd, data; uint8_t icw[3]; } chip[] = {
        {0x20, 0x21, {0x30, 0x04, 0x01}}, {0xa0, 0xa1, {0x38, 0x02, 0x01}}
    };
    static const unsigned wrong_width[] = {0, 2, 4, 8, 257};
    Pic8259 p;
    pic8259_init(&p);
    /* Start slave first and advance it a word ahead of the master. */
    CHECK(pic8259_write(&p, 0xa0, 1, 0x11));
    CHECK(pic8259_write(&p, 0xa1, 1, 0x38));
    CHECK(pic8259_write(&p, 0x21, 1, 0x11)); /* still a mask, not ICW1 */
    expect_imr(&p, 0x21, 0x11);
    for (size_t w = 0; w < sizeof(wrong_width)/sizeof(wrong_width[0]); ++w) {
        refuse_write(&p, 0x20, wrong_width[w], 0x11);
        refuse_write(&p, 0xa1, wrong_width[w], 0x02);
        refuse_read(&p, 0x21, wrong_width[w]);
    }
    CHECK(!pic8259_read(&p, 0x21, 1, NULL));
    refuse_read(&p, 0x121, 1);
    refuse_write(&p, 0x1a1, 1, 0x02);
    CHECK(pic8259_write(&p, 0x20, 1, 0x11));
    for (unsigned step = 0; step < 3; ++step) {
        for (unsigned i = 0; i < 2; ++i) {
            unsigned word = step + i;
            if (word == 3) continue; /* slave ICW4 already complete */
            refuse_read(&p, chip[i].data, 1);
            refuse_write(&p, chip[i].cmd, 1, 0x11); /* no partial restart */
            for (unsigned b = 0; b < 256; ++b)
                if (b != chip[i].icw[word]) refuse_write(&p, chip[i].data, 1, (uint8_t)b);
            CHECK(!pic8259_ready(&p));
            CHECK(pic8259_write(&p, chip[i].data, 1, chip[i].icw[word]));
        }
        if (step == 1) { /* slave done; master still expects ICW4 */
            CHECK(!pic8259_ready(&p));
            CHECK(pic8259_write(&p, 0xa1, 1, 0x38)); /* ICW-looking mask */
            expect_imr(&p, 0xa1, 0x38);
        }
    }
    CHECK(pic8259_ready(&p));
    for (unsigned i = 0; i < 2; ++i) {
        refuse_read(&p, chip[i].cmd, 1);
        for (unsigned b = 0; b < 256; ++b)
            if (b != 0x11) refuse_write(&p, chip[i].cmd, 1, (uint8_t)b); /* incl. EOI, OCW3 */
    }
    expect_imr(&p, 0x21, 0);
    expect_imr(&p, 0xa1, 0x38);
    CHECK(pic8259_write(&p, 0x20, 1, 0x11)); /* re-init drops pair readiness */
    CHECK(!pic8259_ready(&p));
    refuse_read(&p, 0x21, 1);
    expect_imr(&p, 0xa1, 0x38);
}

int main(void)
{
    pinned_probe_and_init();
    interleaved_and_refused();
    puts("PASS: PIC pair probe, normal-mode setup, independent phases, mask readback and refusal");
    return 0;
}
