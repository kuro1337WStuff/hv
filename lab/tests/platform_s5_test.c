/* Trial 119: decoded host tuples only. The two machines differ by one PM1
 * SLP_EN bit; no simulated CPU, interrupt injection or guest shutdown claim. */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint8_t UINT8, BOOLEAN;
typedef uint16_t UINT16;
enum { FALSE = 0, TRUE = 1 };
#include "../../firmware/platform_s5.h"

#define CHECK(e) do { if (!(e)) { fprintf(stderr, "line %d: %s\n", __LINE__, #e); exit(EXIT_FAILURE); } } while (0)

typedef struct {
    PlatformS5 stopped, running;
    unsigned char sealed[sizeof(PlatformS5)];
} Pair;

static void out(PlatformS5 *s, uint16_t port, unsigned bytes, uint32_t operand, PowerIoResult result)
{
    uint32_t value = operand;
    CHECK(platform_s5_io(s, PowerScalarOut, port, bytes, &value) == result && value == operand);
}

static void refused(PlatformS5 *s, enum PowerIoForm form, uint16_t port, unsigned bytes, uint32_t operand)
{
    unsigned char before[sizeof *s];
    memcpy(before, s, sizeof before);
    uint32_t value = operand;
    CHECK(platform_s5_io(s, form, port, bytes, &value) == PowerIoRejected);
    CHECK(value == operand && !memcmp(before, s, sizeof before));
}

static void trace(const char *stage, const PlatformS5 *s)
{
    const PitUartPic *e = &s->irq.edge;
    printf("%-20s %s tick=%" PRIu64 " OUT0=%d busy=%u IRQ4=%u IRR=%02x ISR(M/S)=%02x/%02x "
           "STS/EN=%04x/%04x\n", stage, platform_s5_running(s) ? "running" : "stopped",
           e->pit.tick, pit_periodic_out(&e->pit), (unsigned)e->uart.base.busy,
           (unsigned)uart16450_irq4(&e->uart), (unsigned)e->irr, (unsigned)e->isr,
           (unsigned)s->irq.slave_isr, (unsigned)s->irq.event.status, (unsigned)s->irq.event.enable);
}

/* IRQ9 in service on both chips, plus independently grantable IRQ0, queued
 * IRQ4, PIT one pulse from another rise, UART busy and GBL_STS clear. */
static void prepare(PlatformS5 *s)
{
    static const struct { uint16_t port; uint8_t byte; } setup[] = {
        {0x20,0x11}, {0x21,0x30}, {0x21,4}, {0x21,1},
        {0xa0,0x11}, {0xa1,0x38}, {0xa1,2}, {0xa1,1},
        {0x21,0xea}, {0xa1,0xfd}, {0x4d1,2}, /* unmask IRQ0/2/4 and level IRQ9 */
        {0x3f9,2}, {0x3fc,8}, {0x3f8,'A'}, /* OUT2 rising edge survives the THR write */
        {0x43,0x34}, {0x40,4}, {0x40,0}
    };
    for (size_t i = 0; i < sizeof setup / sizeof setup[0]; ++i)
        out(s, setup[i].port, 1, setup[i].byte, PowerIoHandled);
    out(s, PowerPm1aEnable, 2, PowerEventGlobal, PowerIoHandled);
    CHECK(platform_s5_latch(s, PowerEventGlobal | PowerEventWake));
    uint8_t vector = 0xad;
    CHECK(platform_s5_take(s, &vector) && vector == 0x39);
    out(s, PowerPm1aEvent, 2, PowerEventGlobal, PowerIoHandled);
    CHECK(platform_s5_advance_ns(s, 6705)); /* 8 completed PIT pulses */
    CHECK(s->irq.edge.pit.tick == 8 && !pit_periodic_out(&s->irq.edge.pit));
    CHECK(s->irq.edge.uart.base.busy && !s->irq.edge.uart.base.polls_busy);
    CHECK(s->irq.edge.irr == 0x11 && s->irq.edge.isr == 4 && s->irq.slave_isr == 2);
    CHECK(pit_uart_sci_pic_eligible(&s->irq) == 1 && s->irq.event.status == PowerEventWake);
}

/* Every stopped attempt is checked against the ORIGINAL sealed bytes; even
 * an operation later undone by another attempt would fail immediately. */
static void frozen(const Pair *p)
{
    CHECK(!memcmp(p->sealed, &p->stopped, sizeof p->sealed));
}

static void paired_io(Pair *p, enum PowerIoForm form, uint16_t port, unsigned bytes,
    uint32_t operand, uint32_t expected)
{
    uint32_t value = operand;
    CHECK(platform_s5_io(&p->stopped, form, port, bytes, &value) == PowerIoRejected);
    CHECK(value == operand);
    frozen(p);
    CHECK(platform_s5_io(&p->running, form, port, bytes, &value) == PowerIoHandled);
    CHECK(value == expected);
}

static void write_both(Pair *p, uint16_t port, unsigned bytes, uint32_t operand)
{
    paired_io(p, PowerScalarOut, port, bytes, operand, operand);
}

static void read_both(Pair *p, uint16_t port, unsigned bytes, uint32_t expected)
{
    paired_io(p, PowerScalarIn, port, bytes, UINT32_MAX, expected);
}

static void grant(Pair *p, uint8_t expected)
{
    uint8_t vector = 0xad;
    CHECK(!platform_s5_take(&p->stopped, &vector) && vector == 0xad);
    frozen(p);
    CHECK(platform_s5_take(&p->running, &vector) && vector == expected);
    printf("PIC take             stopped: refused; running: vector=%02x\n", (unsigned)vector);
}

int main(void)
{
    Pair p;
    p.running = platform_s5_new();
    prepare(&p.running);
    refused(&p.running, PowerScalarOut, PowerPm1aControl, 2, PowerS5Request ^ (1u << 10));
    refused(&p.running, PowerScalarOut, PowerPm1aControl, 2, PowerS5Request & ~1u);
    refused(&p.running, PowerScalarOut, PowerPm1aControl, 1, PowerS5Request);
    refused(&p.running, PowerScalarOut, PowerPm1aControl + 1, 2, PowerS5Request);
    refused(&p.running, PowerOtherIo, PowerPm1aControl, 2, PowerS5Request);
    refused(&p.running, PowerOtherIo, 0x3fd, 1, UINT32_MAX);
    refused(&p.running, PowerScalarIn, 0x3fd, 2, UINT32_MAX);
    refused(&p.running, PowerScalarIn, PowerPm1aEvent + 1, 2, UINT32_MAX);
    refused(&p.running, PowerScalarOut, 0x80, 1, 0x20);

    memcpy(&p.stopped, &p.running, sizeof p.stopped);
    trace("matched start", &p.running);
    out(&p.stopped, PowerPm1aControl, 2, PowerS5Request, PowerIoShutdown);
    CHECK(p.stopped.control.lifecycle == PowerStopped && !p.stopped.control.sleep_type_bits);
    CHECK(!memcmp(&p.stopped.irq, &p.running.irq, sizeof p.stopped.irq));
    memcpy(p.sealed, &p.stopped, sizeof p.sealed);
    out(&p.running, PowerPm1aControl, 2, PowerS5Select, PowerIoHandled);
    CHECK(platform_s5_running(&p.running));
    refused(&p.stopped, PowerScalarOut, PowerPm1aControl, 2, PowerS5Request); /* no second shutdown */
    frozen(&p);
    read_both(&p, PowerPm1aControl, 2, PowerS5Select);
    grant(&p, 0x30); /* independently eligible even if no inputs advance */
    const PitUartPic *edge = &p.running.irq.edge;
    CHECK(edge->irr == 0x10 && edge->isr == 5);
    CHECK(!platform_s5_advance_ns(&p.stopped, 7543)); frozen(&p);
    CHECK(platform_s5_advance_ns(&p.running, 7543)); /* pulse 9 */
    CHECK(edge->pit.tick == 9 && pit_periodic_out(&edge->pit) && edge->irr == 0x11);
    read_both(&p, 0x3fd, 1, 0); /* LSR busy poll drains TX and rearms THRE */
    CHECK(!edge->uart.base.busy && edge->uart.base.polls_busy == 1 && uart16450_irq4(&edge->uart));
    CHECK(!platform_s5_latch(&p.stopped, PowerEventGlobal)); frozen(&p);
    CHECK(platform_s5_latch(&p.running, PowerEventGlobal));
    read_both(&p, PowerPm1aEvent, 2, PowerEventGlobal | PowerEventWake);
    CHECK(power_sci_level(&p.running.irq.event));
    trace("new edge/drain/cause", &p.running);
    write_both(&p, 0x20, 1, 0x60); grant(&p, 0x30); /* the NEW edge */
    write_both(&p, 0x20, 1, 0x60);
    write_both(&p, 0xa0, 1, 0x61); write_both(&p, 0x20, 1, 0x62);
    grant(&p, 0x39);
    write_both(&p, PowerPm1aEvent, 2, PowerEventGlobal | PowerEventWake);
    CHECK(!power_sci_level(&p.running.irq.event) && !p.running.irq.event.status);
    write_both(&p, 0xa0, 1, 0x61); write_both(&p, 0x20, 1, 0x62);
    grant(&p, 0x34); write_both(&p, 0x20, 1, 0x64);
    read_both(&p, 0x3fa, 1, 2); CHECK(!uart16450_irq4(&edge->uart));
    CHECK(!edge->isr && !edge->irr && !p.running.irq.slave_isr);

    write_both(&p, 0x3f8, 1, 'B');
    CHECK(edge->uart.base.count == 2 && !memcmp(edge->uart.base.tx, "AB", 2));
    write_both(&p, 0x3ff, 1, 0xa5); read_both(&p, 0x3ff, 1, 0xa5); /* legal even with TX busy */
    static const uint16_t absent[] = { 0x2f8, 0x3e8, 0x2e8 };
    for (size_t i = 0; i < sizeof absent / sizeof absent[0]; ++i) {
        write_both(&p, absent[i], 1, 'X');
        read_both(&p, absent[i] + 7, 1, 0xff);
    }
    write_both(&p, 0x43, 1, 0x30); write_both(&p, 0x40, 1, 3);
    CHECK(edge->pit.mode == PIT_CH0_TERMINAL && edge->pit.phase == PIT_PERIODIC_MSB);
    write_both(&p, 0x21, 1, 0xff); read_both(&p, 0x21, 1, 0xff);
    write_both(&p, 0xa1, 1, 0xff); read_both(&p, 0xa1, 1, 0xff);
    write_both(&p, 0x4d1, 1, 0); read_both(&p, 0x4d1, 1, 0);
    write_both(&p, 0x4d0, 1, 0); read_both(&p, 0x4d0, 1, 0);
    write_both(&p, PowerPm1aEnable, 2, 0); read_both(&p, PowerPm1aEnable, 2, 0);
    write_both(&p, PowerPm1aControl, 2, PowerSciEnable); read_both(&p, PowerPm1aControl, 2, PowerSciEnable);
    trace("final", &p.running);
    trace("sealed unchanged", &p.stopped);
    return 0;
}
