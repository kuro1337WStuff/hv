/* Host only: cl /nologo /std:c11 /W4 /WX /TC lab\tests\legacy_elcr_test.c */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../firmware/pic8259.h"
#include "../../firmware/power_event.h"
#include "../../firmware/power_tables.h"

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "line %d: %s\n", __LINE__, #expr); exit(EXIT_FAILURE); \
} } while (0)

static uint8_t in_byte(const Pic8259 *p, uint16_t port)
{
    uint8_t v = 0;
    CHECK(pic8259_read(p, port, 1, &v));
    return v;
}

static unsigned trigger_mask(const Pic8259 *p)
{
    unsigned low = in_byte(p, 0x4d0);
    return low | ((unsigned)in_byte(p, 0x4d1) << 8);
}

static void refuse(Pic8259 *p, uint16_t port, unsigned width)
{
    unsigned char saved[sizeof(*p)];
    uint8_t out = 0xa5;
    memcpy(saved, p, sizeof(saved));
    CHECK(!pic8259_read(p, port, width, &out) && out == 0xa5);
    CHECK(!pic8259_write(p, port, width, 0));
    CHECK(memcmp(saved, p, sizeof(saved)) == 0);
}

static void port_contract(void)
{
    static const unsigned bad_width[] = {0, 2, 4, 8, 257};
    Pic8259 p;
    pic8259_init(&p);
    CHECK(trigger_mask(&p) == 0);
    CHECK(pic8259_write(&p, 0x4d1, 1, 0x42));
    for (unsigned v = 0; v <= 0xff; ++v) {
        CHECK(pic8259_write(&p, 0x4d0, 1, (uint8_t)v));
        CHECK(trigger_mask(&p) == (0x4200u | (v & 0xf8u)));
    }
    for (unsigned v = 0; v <= 0xff; ++v) {
        CHECK(pic8259_write(&p, 0x4d1, 1, (uint8_t)v));
        CHECK(trigger_mask(&p) == (0xf8u | ((v & 0xdeu) << 8)));
    }
    for (size_t i = 0; i < sizeof(bad_width)/sizeof(bad_width[0]); ++i) {
        refuse(&p, 0x4d0, bad_width[i]);
        refuse(&p, 0x4d1, bad_width[i]);
    }
    refuse(&p, 0x4cf, 1); refuse(&p, 0x4d2, 1);
    refuse(&p, 0xd0, 1);  refuse(&p, 0x14d1, 1);
    CHECK(!pic8259_read(&p, 0x4d0, 1, NULL));
    CHECK(!pic8259_read(&p, 0x4d1, 1, NULL));
    CHECK(trigger_mask(&p) == 0xdef8);
    CHECK(in_byte(&p, 0x21) == 0xff && in_byte(&p, 0xa1) == 0xff && !pic8259_ready(&p));

    /* save_ELCR/restore_ELCR uses two bytes. Even while the master is
     * expecting ICW2, accesses to either ELCR must not consume an ICW. */
    uint8_t master = in_byte(&p, 0x4d0), slave = in_byte(&p, 0x4d1);
    CHECK(pic8259_write(&p, 0x20, 1, 0x11));
    CHECK(trigger_mask(&p) == 0xdef8);
    CHECK(pic8259_write(&p, 0x4d0, 1, 0) && pic8259_write(&p, 0x4d1, 1, 0));
    CHECK(trigger_mask(&p) == 0);
    CHECK(pic8259_write(&p, 0x4d0, 1, master) && pic8259_write(&p, 0x4d1, 1, slave));
    CHECK(pic8259_write(&p, 0x21, 1, 0x30) && pic8259_write(&p, 0x21, 1, 4));
    CHECK(pic8259_write(&p, 0x21, 1, 1));
    CHECK(in_byte(&p, 0x21) == 0 && in_byte(&p, 0xa1) == 0xff && trigger_mask(&p) == 0xdef8);
}

/* Host caller replay of Linux v6.18.60 arch/x86/kernel/acpi/boot.c:590-637.
 * Preserve/rebuild, no-change skip, and the two OUTs are guest policy, not
 * behavior of the ELCR registers. Return the number of accepted byte OUTs. */
static int configure(Pic8259 *p, unsigned irq, int noirq, int level)
{
    unsigned old = trigger_mask(p), next = noirq ? old : 0;
    unsigned bit = 1u << irq; /* caller checked this is FADT SCI_INT 9 */
    next = level ? next | bit : next & ~bit;
    if (old == next) return 0;
    int writes = pic8259_write(p, 0x4d0, 1, (uint8_t)next);
    return writes + pic8259_write(p, 0x4d1, 1, (uint8_t)(next >> 8));
}

static void fadt_sci_configuration(void)
{
    static uint8_t pages[PowerTableBytes];
    Pic8259 p;
    PowerPm1aEventRegs pm = {0, 0};
    uint32_t status = UINT32_MAX;
    CHECK(power_build_tables(pages, sizeof(pages), PowerRsdpGpa));
    const uint8_t *fadt = pages + PowerFadtOffset;
    CHECK(memcmp(fadt, "FACP", 4) == 0);
    unsigned irq = linux_le16(fadt + 46); /* SCI_INT, not an interrupt vector */
    CHECK(irq == 9);
    pic8259_init(&p);
    CHECK(pic8259_write(&p, 0x4d0, 1, 0x98) && pic8259_write(&p, 0x4d1, 1, 0x44));
    CHECK(configure(&p, irq, 1, 1) == 2 && trigger_mask(&p) == 0x4698);
    CHECK(configure(&p, irq, 1, 1) == 0 && trigger_mask(&p) == 0x4698);
    CHECK(configure(&p, irq, 1, 0) == 2 && trigger_mask(&p) == 0x4498);
    CHECK(configure(&p, irq, 0, 1) == 2 && trigger_mask(&p) == 0x0200);
    CHECK(configure(&p, irq, 0, 1) == 0 && trigger_mask(&p) == 0x0200);
    /* IRQ9 configured level while still masked, with no pending PM1 status.
     * No SCI assertion, CPU interrupt delivery, or Linux execution is tested. */
    CHECK(in_byte(&p, 0xa1) == 0xff);
    CHECK(power_event_io(&pm, PowerScalarIn, 0x600, 2, &status) == PowerIoHandled && status == 0);
    CHECK(configure(&p, irq, 0, 0) == 2 && trigger_mask(&p) == 0);
}

int main(void)
{
    port_contract();
    fadt_sci_configuration();
    puts("PASS: ELCR masks f8/de; FADT IRQ9 preserve 4698/4498, rebuild 0200/0000; PM1 status 0");
    return 0;
}
