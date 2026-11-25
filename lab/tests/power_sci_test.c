/* Trial 098: decoded host tuples only, no port instructions or guest. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../firmware/power_sci.h"
#include "../../firmware/power_tables.h"

#define CHECK(e) do { if (!(e)) { fprintf(stderr, "line %d: %s\n", __LINE__, #e); exit(EXIT_FAILURE); } } while (0)

static void outb(PowerSci *s, uint16_t port, uint8_t byte)
{
    CHECK(power_sci_pic_write(s, port, 1, byte));
}

static uint8_t inb(const PowerSci *s, uint16_t port)
{
    uint8_t byte = 0;
    CHECK(pic8259_read(&s->pic, port, 1, &byte));
    return byte;
}

static void outw(PowerSci *s, uint16_t port, uint32_t word)
{
    CHECK(power_event_io(&s->event, PowerScalarOut, port, 2, &word) == PowerIoHandled);
}

static void no_take(PowerSci *s)
{
    unsigned char before[sizeof(*s)];
    uint8_t vector = 0xcd;
    memcpy(before, s, sizeof(before));
    CHECK(!power_sci_take(s, &vector) && vector == 0xcd);
    CHECK(memcmp(before, s, sizeof(before)) == 0);
}

static void refused(PowerSci *s, uint16_t port, unsigned bytes, uint32_t value)
{
    unsigned char before[sizeof(*s)];
    memcpy(before, s, sizeof(before));
    CHECK(!power_sci_pic_write(s, port, bytes, value));
    CHECK(memcmp(before, s, sizeof(before)) == 0);
}

static void take(PowerSci *s)
{
    uint8_t vector = 0xcd;
    CHECK(power_sci_take(s, &vector) && vector == 0x39);
    CHECK(s->slave_isr == 0x02 && s->master_isr == 0x04);
    no_take(s);
}

/* Use the real ICW parser. Even with both IMRs open and the source high,
 * neither partially programmed chip can participate in a cascade. */
static void init_pic(PowerSci *s)
{
    outb(s, 0x21, 0xfb); outb(s, 0xa1, 0xfd);
    outb(s, 0x4d1, 0x02);
    no_take(s);
    outb(s, 0x20, 0x11); outb(s, 0x21, 0x30);
    outb(s, 0x21, 0x04); outb(s, 0x21, 0x01);
    no_take(s);
    outb(s, 0xa0, 0x11); outb(s, 0xa1, 0x38);
    refused(s, 0xa1, 1, 0xfd); /* not the expected slave ICW3 */
    outb(s, 0xa1, 0x02); no_take(s); outb(s, 0xa1, 0x01);
    CHECK(inb(s, 0x21) == 0 && inb(s, 0xa1) == 0);
    outb(s, 0x21, 0xfb); outb(s, 0xa1, 0xfd);
}

static void hardware_service(PowerSci *s, uint16_t sts, uint16_t en)
{
    /* A high source is not an edge/queue. Test each gate independently. */
    outb(s, 0x4d1, 0); no_take(s);
    outb(s, 0x4d1, 2); outb(s, 0x21, 0xff); no_take(s);
    outb(s, 0xa1, 0xff); outb(s, 0x21, 0xfb); no_take(s);
    CHECK(power_sci_asserted(s));
    outw(s, sts, 0x20); /* cleared while masked: unmask must not replay it */
    outb(s, 0xa1, 0xfd); no_take(s);
    CHECK(!power_sci_asserted(s) && s->event.status == 0x8000);
    CHECK(power_event_latch(&s->event, 0x20));
    outw(s, en, 0); no_take(s);
    CHECK(s->event.status == 0x8020 && !power_sci_asserted(s));
    outw(s, en, 0x20);
    CHECK(!power_sci_take(s, NULL) && !s->slave_isr && !s->master_isr);
    take(s);

    /* With the line high and both IMRs open, either surviving ISR alone
     * blocks another take. Test both orders; neither EOI clears PM1. */
    refused(s, 0x20, 1, 0x61); refused(s, 0xa0, 1, 0x62);
    refused(s, 0xa0, 1, 0x20); refused(s, 0x20, 1, 0x60);
    refused(s, 0xa0, 2, 0x61); refused(s, 0xa0, 1, 0x161);
    refused(s, 0x22, 1, 0x62);
    outb(s, 0xa0, 0x61);
    CHECK(!s->slave_isr && s->master_isr == 4); no_take(s);
    refused(s, 0xa0, 1, 0x61); refused(s, 0xa0, 1, 0x11);
    outb(s, 0x20, 0x62); refused(s, 0x20, 1, 0x62);
    take(s); /* persistent level, no new latch or Linux mask sequence */
    outb(s, 0x20, 0x62);
    CHECK(s->slave_isr == 2 && !s->master_isr); no_take(s);
    refused(s, 0x20, 1, 0x62); refused(s, 0x20, 1, 0x11);
    outw(s, sts, 0x20); /* W1C disarms reassertion, not in-service */
    CHECK(s->slave_isr == 2 && !s->master_isr && !power_sci_asserted(s));
    outb(s, 0xa0, 0x61); no_take(s);
    CHECK(s->event.status == 0x8000 && s->event.enable == 0x20);
}

static void linux_service(PowerSci *s, uint16_t sts)
{
    /* Linux v6.18.60 arch/x86/kernel/i8259.c, mask_and_ack_8259A normal
     * real-IRQ path: dummy slave IMR read, mask IRQ9 ONLY, specific EOIs.
     * The W1C below is a modeled handler action, not Linux execution. */
    CHECK(power_event_latch(&s->event, 0x20));
    for (unsigned pass = 0; pass != 2; ++pass) {
        take(s);
        CHECK(inb(s, 0xa1) == 0xfd);
        outb(s, 0xa1, 0xff);
        CHECK(inb(s, 0x21) == 0xfb && s->slave_isr == 2 && s->master_isr == 4);
        outb(s, 0xa0, 0x61); no_take(s);
        outb(s, 0x20, 0x62); no_take(s);
        CHECK(power_sci_asserted(s));
        if (pass) outw(s, sts, 0x20);
        outb(s, 0xa1, 0xfd); /* persistent on first unmask only */
    }
    no_take(s);
}

int main(void)
{
    static uint8_t pages[PowerTableBytes];
    const uint8_t *fadt = pages + PowerFadtOffset;
    PowerSci s;
    uint8_t byte = 0xab;
    uint32_t word = 0x8020;
    unsigned char before[sizeof(s)];
    CHECK(power_build_tables(pages, sizeof(pages), PowerRsdpGpa));
    CHECK(!memcmp(fadt, "FACP", 4));
    uint16_t irq = linux_le16(fadt + 46), sts = (uint16_t)linux_le32(fadt + 56);
    uint16_t en = (uint16_t)(sts + fadt[88] / 2);
    CHECK(irq == 9 && sts == 0x600 && en == 0x602);
    CHECK(power_sci_init(&s, irq));
    outw(&s, en, 0xffff); CHECK(power_event_latch(&s.event, 0x8000));
    CHECK(!power_sci_asserted(&s)); /* WAK_STS alone despite GBL_EN */
    CHECK(power_event_latch(&s.event, 0x20) && power_sci_asserted(&s));
    init_pic(&s);
    hardware_service(&s, sts, en);
    linux_service(&s, sts);

    CHECK(power_event_latch(&s.event, 0x20)); take(&s);
    memcpy(before, &s, sizeof(before));
    CHECK(!power_sci_init(&s, 0x109)); /* don't truncate SCI_INT */
    CHECK(!pic8259_read(&s.pic, 0xa0, 1, &byte) && byte == 0xab);
    CHECK(power_event_io(&s.event, PowerOtherIo, sts, 2, &word) == PowerIoRejected && word == 0x8020);
    CHECK(power_event_io(&s.event, PowerScalarOut, sts, 1, &word) == PowerIoRejected && word == 0x8020);
    CHECK(!power_event_latch(&s.event, 0x10020));
    CHECK(!memcmp(before, &s, sizeof(before)));
    outw(&s, sts, 0x20);
    CHECK(!power_sci_asserted(&s) && s.slave_isr == 2 && s.master_isr == 4);
    CHECK(power_sci_init(&s, irq)); /* explicit reset discards active cycle */
    CHECK(!s.event.status && !s.event.enable && !s.slave_isr && !s.master_isr);
    CHECK(!pic8259_ready(&s.pic) && inb(&s, 0x21) == 0xff && inb(&s, 0xa1) == 0xff && !inb(&s, 0x4d1));
    refused(&s, 0xa0, 1, 0x61); refused(&s, 0x20, 1, 0x62); no_take(&s);
    puts("PASS: FADT SCI_INT 9; PM1 0600/0602; slave IR1/master IR2 accept 39; W1C and both specific EOIs independent");
    return 0;
}
