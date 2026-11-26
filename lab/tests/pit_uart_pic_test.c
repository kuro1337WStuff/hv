/* Trial 115: host-side register transactions, absolute PIT pulses and PIC
 * acceptance. Trace columns are observations of the model under test. */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint8_t UINT8, BOOLEAN;
typedef uint16_t UINT16;
enum { FALSE = 0, TRUE = 1 };
#include "../../firmware/pit_uart_pic.h"

#define CHECK(e) do { if (!(e)) { fprintf(stderr, "line %d: %s\n", __LINE__, #e); exit(EXIT_FAILURE); } } while (0)

static void outb(PitUartPic *s, uint16_t port, uint32_t value)
{
    CHECK(pit_uart_pic_io(s, PitUartPicOut, port, 1, &value));
}

static uint32_t inb(PitUartPic *s, uint16_t port)
{
    uint32_t value = 0xfeed;
    CHECK(pit_uart_pic_io(s, PitUartPicIn, port, 1, &value));
    return value;
}

static void trace(const PitUartPic *s, const char *label)
{
    printf("%-24s tick=%" PRIu64 " OUT0=%d IRQ4=%u IMR=%02x IRR=%02x ISR=%02x\n",
           label, s->pit.tick, pit_periodic_out(&s->pit), (unsigned)uart16450_irq4(&s->uart),
           (unsigned)s->pic.master.imr, (unsigned)s->irr, (unsigned)s->isr);
}

static void refused(PitUartPic *s, enum PitUartPicIo form, uint16_t port, unsigned bytes, uint32_t value)
{
    unsigned char before[sizeof(*s)];
    uint32_t original = value;
    memcpy(before, s, sizeof(before));
    CHECK(!pit_uart_pic_io(s, form, port, bytes, &value));
    CHECK(value == original && memcmp(before, s, sizeof(before)) == 0);
}

static void no_take(PitUartPic *s)
{
    unsigned char before[sizeof(*s)];
    uint8_t vector = 0xad;
    memcpy(before, s, sizeof(before));
    CHECK(!pit_uart_pic_take(s, &vector));
    CHECK(vector == 0xad && memcmp(before, s, sizeof(before)) == 0);
}

static void take(PitUartPic *s, uint8_t want_vector, uint8_t want_irr, uint8_t want_isr)
{
    uint8_t vector = 0xad;
    CHECK(pit_uart_pic_take(s, &vector));
    printf("accepted vector=0x%02x\n", (unsigned)vector);
    trace(s, "after acceptance");
    CHECK(vector == want_vector && s->irr == want_irr && s->isr == want_isr);
}

static void icws(PitUartPic *s)
{
    outb(s, 0x20, 0x11); outb(s, 0x21, 0x30);
    outb(s, 0x21, 0x04); outb(s, 0x21, 0x01);
    CHECK(inb(s, 0x21) == 0);
}

static void arm_uart(PitUartPic *s)
{
    outb(s, 0x3f9, 2); /* THRE cause, but OUT2 still low */
    CHECK(s->uart.thre_pending && !uart16450_irq4(&s->uart) && !(s->irr & 0x10));
    outb(s, 0x3fc, 8);
    CHECK(uart16450_irq4(&s->uart) && (s->irr & 0x10));
}

static void programming_and_reset(void)
{
    PitUartPic s;
    pit_uart_pic_init(&s);
    CHECK(inb(&s, 0x21) == 0xff && inb(&s, 0x4d0) == 0);
    no_take(&s);
    outb(&s, 0x21, 0); arm_uart(&s); no_take(&s); /* open IMR is not initialization */
    outb(&s, 0x20, 0x11);
    CHECK(s.irr == 0 && uart16450_irq4(&s.uart)); /* first ICW1, not a device reset */
    outb(&s, 0x3fc, 0); outb(&s, 0x3fc, 8); /* new edge during ICWs */
    CHECK(s.irr == 0x10);
    refused(&s, PitUartPicOut, 0x20, 1, 0x11); /* no partial restart */
    refused(&s, PitUartPicIn, 0x21, 1, 0xfade);
    refused(&s, PitUartPicOut, 0x21, 1, 0x38); /* wrong ICW2 */
    outb(&s, 0x21, 0x30); outb(&s, 0x21, 0x04); no_take(&s);
    trace(&s, "before ICW4");
    outb(&s, 0x21, 1);
    refused(&s, PitUartPicOut, 0x20, 1, 0x11); /* IRR only */
    take(&s, 0x34, 0, 0x10);
    refused(&s, PitUartPicOut, 0x20, 1, 0x11); /* ISR only */
    outb(&s, 0x20, 0x64); icws(&s); no_take(&s); /* idle restart while pin high */
    outb(&s, 0x3fc, 0); outb(&s, 0x3fc, 8); take(&s, 0x34, 0, 0x10);
    outb(&s, 0x43, 0x30); outb(&s, 0x43, 0x38); CHECK(s.irr == 1);
    CHECK(pit_uart_pic_advance(&s, 10));
    pit_uart_pic_init(&s); /* explicit reset may discard pending AND active */
    CHECK(inb(&s, 0x21) == 0xff && s.irr == 0 && s.isr == 0 && s.pit.tick == 0);
    CHECK(pit_periodic_out(&s.pit) && !uart16450_irq4(&s.uart) && inb(&s, 0x3fa) == 1);
    CHECK(s.pic.master.phase == PIC8259_PROBE); no_take(&s);
}

static void simultaneous(void)
{
    PitUartPic s;
    unsigned char before[sizeof(s)];
    pit_uart_pic_init(&s); icws(&s);
    outb(&s, 0x4d0, 0);
    refused(&s, PitUartPicOut, 0x4d0, 1, 0x10); /* level triggering excluded */
    refused(&s, PitUartPicOut, 0xa0, 1, 0x11);
    refused(&s, PitUartPicIn, 0xa1, 1, 0xbeef);
    outb(&s, 0x21, 0xff); arm_uart(&s);
    outb(&s, 0x43, 0x34); outb(&s, 0x40, 4); outb(&s, 0x40, 0);
    CHECK(pit_uart_pic_advance(&s, 4) && !pit_periodic_out(&s.pit) && s.irr == 0x10);
    /* OUT0 is low at both endpoints, but rises on pulses 5, 9, 13. */
    CHECK(pit_uart_pic_advance(&s, 16) && !pit_periodic_out(&s.pit) && s.irr == 0x11);
    trace(&s, "both masked"); no_take(&s);
    refused(&s, PitUartPicOut, 0x21, 2, 0xee);
    refused(&s, PitUartPicOut, 0x21, 1, 0x1ee);
    refused(&s, PitUartPicIn, 0x3fa, 2, 0x1234); /* no read-to-clear */
    refused(&s, PitUartPicOut, 0x43, 2, 0x34); /* no programming edge */
    memcpy(before, &s, sizeof(before));
    CHECK(!pit_uart_pic_advance(&s, 15) && !memcmp(before, &s, sizeof(before)));
    outb(&s, 0x21, 0xee); CHECK(inb(&s, 0x21) == 0xee);
    memcpy(before, &s, sizeof(before));
    CHECK(!pit_uart_pic_take(&s, NULL) && !memcmp(before, &s, sizeof(before)));
    trace(&s, "both unmasked");
    take(&s, 0x30, 0x10, 1); no_take(&s); /* IRQ4 cannot nest within IRQ0 */
    refused(&s, PitUartPicOut, 0x20, 1, 0x64);
    refused(&s, PitUartPicOut, 0x20, 1, 0x20); /* nonspecific EOI */
    refused(&s, PitUartPicOut, 0x20, 2, 0x60);
    outb(&s, 0x20, 0x60); refused(&s, PitUartPicOut, 0x20, 1, 0x60);
    take(&s, 0x34, 0, 0x10); /* several timer rises counted once */
    outb(&s, 0x20, 0x64); CHECK(uart16450_irq4(&s.uart));
    trace(&s, "EOI with IRQ4 held"); no_take(&s);
    CHECK(inb(&s, 0x3fd) == 0x60); outb(&s, 0x3fc, 8); /* same high line */
    no_take(&s); refused(&s, PitUartPicOut, 0x20, 1, 0x64);
    CHECK(inb(&s, 0x3fa) == 2 && !uart16450_irq4(&s.uart)); no_take(&s);
}

static void nested_and_rearmed(void)
{
    PitUartPic s;
    pit_uart_pic_init(&s); icws(&s); outb(&s, 0x21, 0xef); /* expose IRQ4 only */
    arm_uart(&s); take(&s, 0x34, 0, 0x10);
    CHECK(inb(&s, 0x3fa) == 2 && s.isr == 0x10 && !uart16450_irq4(&s.uart));
    for (unsigned i = 0; i < 2; ++i) {
        outb(&s, 0x3f8, 'T'); /* THR busy; LSR poll drains and raises THRE */
        CHECK(inb(&s, 0x3fd) == 0 && uart16450_irq4(&s.uart) && s.irr == 0x10);
        CHECK(inb(&s, 0x3fa) == 2 && !uart16450_irq4(&s.uart) && s.irr == 0x10);
    }
    trace(&s, "IIR clear, edge pending"); no_take(&s);
    outb(&s, 0x43, 0x30); CHECK(!pit_periodic_out(&s.pit));
    outb(&s, 0x43, 0x34); CHECK(pit_periodic_out(&s.pit) && s.irr == 0x11);
    trace(&s, "PIT mode-write rise"); no_take(&s);
    outb(&s, 0x21, 0xee); take(&s, 0x30, 0x10, 0x11); /* IRQ0 nests within IRQ4 */
    outb(&s, 0x43, 0x34); CHECK(s.irr == 0x10); /* no new transition */
    outb(&s, 0x43, 0x30); outb(&s, 0x43, 0x38); CHECK(s.irr == 0x11);
    no_take(&s); /* even IRQ0 cannot preempt itself */
    outb(&s, 0x20, 0x60); CHECK(s.isr == 0x10 && s.irr == 0x11);
    trace(&s, "specific EOI IRQ0");
    take(&s, 0x30, 0x10, 0x11); /* IRQ4 still in service */
    outb(&s, 0x43, 0x30); outb(&s, 0x43, 0x38);
    outb(&s, 0x21, 0xff);
    outb(&s, 0x20, 0x64); CHECK(s.isr == 1 && s.irr == 0x11);
    refused(&s, PitUartPicOut, 0x20, 1, 0x64);
    outb(&s, 0x20, 0x60); CHECK(!s.isr && s.irr == 0x11); no_take(&s);
    trace(&s, "EOIs while masked");
    outb(&s, 0x21, 0xee); take(&s, 0x30, 0x10, 1); outb(&s, 0x20, 0x60);
    take(&s, 0x34, 0, 0x10); outb(&s, 0x20, 0x64); no_take(&s);
    trace(&s, "all requests retired");
}

int main(void)
{
    programming_and_reset(); simultaneous(); nested_and_rearmed();
    return 0;
}
