/* Trial 116: host transactions and PIC grants only; M/S columns are master/slave. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint8_t UINT8, BOOLEAN;
typedef uint16_t UINT16;
enum { FALSE = 0, TRUE = 1 };
#include "../../firmware/pit_uart_sci_pic.h"

#define CHECK(e) do { if (!(e)) { fprintf(stderr, "line %d: %s\n", __LINE__, #e); exit(EXIT_FAILURE); } } while (0)

static void out(PitUartSciPic *s, uint16_t port, unsigned bytes, uint32_t value)
{
    uint32_t original = value;
    CHECK(pit_uart_sci_pic_io(s, PitUartPicOut, port, bytes, &value) && value == original);
}

static uint32_t inb(PitUartSciPic *s, uint16_t port)
{
    uint32_t value = 0xaaaa;
    CHECK(pit_uart_sci_pic_io(s, PitUartPicIn, port, 1, &value));
    return value;
}

static void refused(PitUartSciPic *s, enum PitUartPicIo form, uint16_t port, unsigned bytes, uint32_t value)
{
    unsigned char before[sizeof(*s)];
    uint32_t original = value;
    memcpy(before, s, sizeof(before));
    CHECK(!pit_uart_sci_pic_io(s, form, port, bytes, &value));
    CHECK(value == original && memcmp(before, s, sizeof(before)) == 0);
}

static void row(const PitUartSciPic *s, const char *label, uint8_t pending, uint8_t eligible,
    uint8_t master, uint8_t slave)
{
    uint8_t p = pit_uart_sci_pic_pending(s), e = pit_uart_sci_pic_eligible(s);
    printf("%-24s OUT0=%d IRQ4=%u SCI=%d IMR(M/S)=%02x/%02x edgeIRR=%02x "
           "M.pending=%02x M.eligible=%02x ISR(M/S)=%02x/%02x\n", label,
           pit_periodic_out(&s->edge.pit), (unsigned)uart16450_irq4(&s->edge.uart),
           power_sci_level(&s->event), (unsigned)s->edge.pic.master.imr, (unsigned)s->edge.pic.slave.imr,
           (unsigned)s->edge.irr, (unsigned)p, (unsigned)e, (unsigned)s->edge.isr, (unsigned)s->slave_isr);
    CHECK(p == pending && e == eligible && s->edge.isr == master && s->slave_isr == slave);
}

static void take(PitUartSciPic *s, uint8_t expected)
{
    uint8_t vector = 0xad;
    CHECK(pit_uart_sci_pic_take(s, &vector));
    printf("accepted vector=%02x\n", (unsigned)vector);
    CHECK(vector == expected);
}

static void none(PitUartSciPic *s)
{
    uint8_t vector = 0xad;
    unsigned char before[sizeof(*s)];
    memcpy(before, s, sizeof(before));
    CHECK(!pit_uart_sci_pic_take(s, &vector));
    CHECK(vector == 0xad && memcmp(before, s, sizeof(before)) == 0);
}

static void init(PitUartSciPic *s)
{
    CHECK(pit_uart_sci_pic_init(s, 9));
    CHECK(inb(s, 0x21) == 0xff && inb(s, 0xa1) == 0xff);
    out(s, 0x20, 1, 0x11); out(s, 0x21, 1, 0x30);
    out(s, 0x21, 1, 0x04); out(s, 0x21, 1, 1);
    out(s, 0xa0, 1, 0x11); out(s, 0xa1, 1, 0x38); out(s, 0xa1, 1, 2);
    out(s, 0xa1, 1, 1);
    out(s, 0x21, 1, 0xea); out(s, 0xa1, 1, 0xfd); /* IRQ0/2/4; IRQ9 */
    out(s, 0x4d1, 1, 2);
    out(s, PowerPm1aEnable, 2, PowerEventGlobal);
    CHECK(power_event_latch(&s->event, PowerEventWake));
    row(s, "quiescent (WAK only)", 0, 0, 0, 0); none(s);
}

static void timer_rises(PitUartSciPic *s)
{
    out(s, 0x43, 1, 0x34); out(s, 0x40, 1, 4); out(s, 0x40, 1, 0);
    CHECK(pit_uart_pic_advance(&s->edge, 4) && !pit_periodic_out(&s->edge.pit));
    CHECK(pit_uart_pic_advance(&s->edge, 16) && !pit_periodic_out(&s->edge.pit)); /* three rises */
}

static void uart_rises(PitUartSciPic *s)
{
    out(s, 0x3f9, 1, 2); out(s, 0x3fc, 1, 8);
    out(s, 0x3fc, 1, 0); out(s, 0x3fc, 1, 8); /* two rises, one IRR bit */
}

static void simultaneous(void)
{
    PitUartSciPic s;
    init(&s);
    out(&s, 0x21, 1, 0xff); out(&s, 0xa1, 1, 0xff);
    timer_rises(&s); uart_rises(&s); CHECK(power_event_latch(&s.event, PowerEventGlobal));
    row(&s, "both masked", 0x11, 0, 0, 0); none(&s);
    out(&s, 0x21, 1, 0xfb); row(&s, "slave masked", 0x11, 0, 0, 0); none(&s);
    out(&s, 0x21, 1, 0xff); out(&s, 0xa1, 1, 0xfd);
    row(&s, "master masked", 0x15, 0, 0, 0); none(&s);
    out(&s, 0x21, 1, 0xea); CHECK(inb(&s, 0x21) == 0xea && inb(&s, 0xa1) == 0xfd);
    row(&s, "all three ready", 0x15, 0x15, 0, 0);
    refused(&s, PitUartPicIn, 0x3fa, 2, 0xfade); /* no read-to-clear */
    refused(&s, PitUartPicOut, 0x4d0, 1, 0x10); /* edge-only IRQ4 */
    refused(&s, PitUartPicOut, 0xa1, 2, 0xff);
    refused(&s, PitUartPicOut, PowerPm1aEvent, 4, PowerEventGlobal);
    CHECK(!pit_uart_sci_pic_take(&s, NULL));
    take(&s, 0x30); row(&s, "timer in service", 0x14, 0, 1, 0); none(&s);
    refused(&s, PitUartPicOut, 0x20, 1, 0x62); refused(&s, PitUartPicOut, 0xa0, 1, 0x61);
    out(&s, 0x20, 1, 0x60); take(&s, 0x39);
    row(&s, "SCI in service", 0x10, 0, 4, 2); none(&s);
    out(&s, PowerPm1aEvent, 2, PowerEventGlobal); /* W1C is not either EOI */
    CHECK(!power_sci_level(&s.event) && s.event.status == PowerEventWake);
    row(&s, "W1C during service", 0x10, 0, 4, 2);
    out(&s, 0xa0, 1, 0x61); none(&s); out(&s, 0x20, 1, 0x62);
    take(&s, 0x34); row(&s, "UART in service", 0, 0, 0x10, 0);
    out(&s, 0x20, 1, 0x64);
    CHECK(uart16450_irq4(&s.edge.uart)); row(&s, "held UART, no new edge", 0, 0, 0, 0); none(&s);
    CHECK(inb(&s, 0x3fa) == 2 && !uart16450_irq4(&s.edge.uart)); none(&s);
}

static void persistent_and_nested(void)
{
    PitUartSciPic s;
    init(&s); CHECK(power_event_latch(&s.event, PowerEventGlobal));
    out(&s, 0x4d1, 1, 0); row(&s, "SCI not level-routed", 0, 0, 0, 0); none(&s);
    out(&s, 0x4d1, 1, 2); row(&s, "level armed", 4, 4, 0, 0);
    out(&s, PowerPm1aEvent, 1, PowerEventGlobal);
    row(&s, "cleared before take", 0, 0, 0, 0); none(&s);
    CHECK(power_event_latch(&s.event, PowerEventGlobal)); take(&s, 0x39);
    refused(&s, PitUartPicOut, 0xa0, 2, 0x61); refused(&s, PitUartPicOut, 0x20, 1, 0x162);
    refused(&s, PitUartPicOut, 0xa0, 1, 0x62); refused(&s, PitUartPicOut, 0x20, 1, 0x61);
    refused(&s, PitUartPicOut, 0x20, 1, 0x20); refused(&s, PitUartPicOut, 0x20, 1, 0x60);
    out(&s, 0xa0, 1, 0x61); row(&s, "slave-first EOI", 4, 0, 4, 0); none(&s);
    refused(&s, PitUartPicOut, 0xa0, 1, 0x61); refused(&s, PitUartPicOut, 0xa0, 1, 0x11);
    out(&s, 0x20, 1, 0x62); row(&s, "same SCI eligible", 4, 4, 0, 0); take(&s, 0x39);
    uart_rises(&s); row(&s, "UART below cascade", 0x10, 0, 4, 2); none(&s);
    out(&s, 0x20, 1, 0x62); row(&s, "master-first EOI", 0x10, 0x10, 0, 2);
    refused(&s, PitUartPicOut, 0x20, 1, 0x62); refused(&s, PitUartPicOut, 0x20, 1, 0x11);
    take(&s, 0x34); row(&s, "master free for IRQ4", 0, 0, 0x10, 2); none(&s);
    out(&s, 0xa0, 1, 0x61); row(&s, "SCI outranks IRQ4", 4, 4, 0x10, 0);
    take(&s, 0x39); row(&s, "SCI nested in UART", 0, 0, 0x14, 2);
    timer_rises(&s); take(&s, 0x30); row(&s, "timer nested in SCI", 0, 0, 0x15, 2);
    CHECK(pit_uart_pic_advance(&s.edge, 20)); /* new edge during timer service */
    row(&s, "timer cannot self-nest", 1, 0, 0x15, 2); none(&s);
    out(&s, 0x21, 1, 0xeb); out(&s, 0xa1, 1, 0xff); /* mask timer and SCI at slave */
    out(&s, 0x20, 1, 0x60); out(&s, 0x20, 1, 0x62); out(&s, 0xa0, 1, 0x61);
    row(&s, "masked after both EOIs", 1, 0, 0x10, 0); none(&s);
    out(&s, 0xa1, 1, 0xfd); take(&s, 0x39); /* no PM1 re-latch */
    out(&s, PowerPm1aEvent, 2, PowerEventGlobal);
    out(&s, 0xa0, 1, 0x61); out(&s, 0x20, 1, 0x62); out(&s, 0x20, 1, 0x64);
    out(&s, 0x21, 1, 0xea); take(&s, 0x30); out(&s, 0x20, 1, 0x60);
    row(&s, "retired", 0, 0, 0, 0); none(&s);
}

static void partially_programmed_pair(void)
{
    PitUartSciPic s;
    init(&s);
    out(&s, 0xa0, 1, 0x11); out(&s, 0xa1, 1, 0x38); out(&s, 0xa1, 1, 2);
    out(&s, 0x43, 1, 0x30); out(&s, 0x43, 1, 0x34);
    row(&s, "before slave ICW4", 1, 0, 0, 0); none(&s);
    out(&s, 0xa1, 1, 1); take(&s, 0x30); out(&s, 0x20, 1, 0x60); none(&s);
}

int main(void)
{
    simultaneous();
    persistent_and_nested();
    partially_programmed_pair();
    return 0;
}
