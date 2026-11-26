/* Offline decoded scalar accesses: no guest, hardware or physical I/O. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint8_t UINT8, BOOLEAN;
typedef uint16_t UINT16;
enum { FALSE = 0, TRUE = 1 };
#include "../../firmware/platform_s5.h"

#define CHECK(e) do { if (!(e)) { fprintf(stderr, "line %d: %s\n", __LINE__, #e); exit(EXIT_FAILURE); } } while (0)
enum { N = 20, Max = N + 2, Guard = 0xad, THR = Uart8250Base };
typedef struct { PlatformS5 s; uint8_t raw[Max + 2]; } Fixture;

static void fresh(Fixture *f, size_t cap)
{
    memset(f->raw, Guard, sizeof f->raw);
    CHECK(platform_s5_init_tx(&f->s, f->raw + 1, cap));
    CHECK(!f->s.irq.edge.tx_count && f->s.irq.edge.tx_capacity == cap);
}

static void out(PlatformS5 *s, uint16_t port, uint32_t operand)
{
    uint32_t v = operand;
    CHECK(platform_s5_io(s, PowerScalarOut, port, 1, &v) == PowerIoHandled && v == operand);
}

static uint32_t in(PlatformS5 *s, uint16_t port)
{
    uint32_t v = UINT32_C(0xcafebabe);
    CHECK(platform_s5_io(s, PowerScalarIn, port, 1, &v) == PowerIoHandled);
    return v;
}

/* The sole permitted output for a refusal is its return code. Snapshot
 * object representations before the call, never compare padding across runs. */
static void refused(Fixture *f, enum PowerIoForm form, uint16_t port, unsigned width, uint32_t operand)
{
    unsigned char before[sizeof *f];
    uint32_t v = operand;
    memcpy(before, f, sizeof before);
    CHECK(platform_s5_io(&f->s, form, port, width, &v) == PowerIoRejected);
    CHECK(v == operand && !memcmp(before, f, sizeof before));
}

static uint8_t take4(PlatformS5 *s)
{
    uint8_t v = Guard;
    CHECK(platform_s5_take(s, &v) && v == 0x34);
    CHECK(!s->irq.edge.irr && s->irq.edge.isr == PitUartIrq4);
    return v;
}

static uint8_t payload(unsigned i, uint8_t last)
{
    return i + 1 == N ? last : (i % 3 == 0 ? 0 : i % 3 == 1 ? 0xff : (uint8_t)i);
}

static void fill(Fixture *f, uint8_t last)
{
    PlatformS5 *s = &f->s;
    const PitUartPic *e = &s->irq.edge;
    static const struct { uint16_t port; uint8_t byte; } init[] = {
        {0x20,0x11}, {0x21,0x30}, {0x21,4}, {0x21,1},
        {0xa0,0x11}, {0xa1,0x38}, {0xa1,2}, {0xa1,1},
        {0x21,0xef}, {THR+1,2}, {THR+4,8}
    };
    for (size_t i = 0; i < sizeof init / sizeof *init; ++i) out(s, init[i].port, init[i].byte);
    take4(s); /* keep IRQ4 in service while new edges accumulate */
    refused(f, PowerScalarOut, THR, 1, UINT32_C(0xdecafbad));
    refused(f, PowerScalarIn, THR, 2, UINT32_MAX);
    for (unsigned i = 0; i < N; ++i) {
        uint8_t b = payload(i, last);
        out(s, THR, b);
        CHECK(e->uart.base.busy && !e->uart.thre_pending && !uart16450_irq4(&e->uart));
        CHECK(!e->uart.base.count && e->uart.base.tx[0] == b);
        CHECK(e->tx_count == i + 1 && f->raw[i+1] == b && f->raw[0] == Guard && f->raw[i+2] == Guard);
        if (!i || i+1 == N) refused(f, PowerScalarOut, THR, 1, 0xa7); /* busy, including with room */
        CHECK(in(s, THR+5) == 0 && !e->uart.base.busy && e->uart.base.polls_busy == i + 1);
        CHECK(e->uart.thre_pending && uart16450_irq4(&e->uart));
        CHECK(e->irr == PitUartIrq4 && e->isr == PitUartIrq4);
    }
    for (unsigned i = 0; i < N; ++i) CHECK(f->raw[i+1] == payload(i, last));
}

static uint8_t service(Fixture *f)
{
    PlatformS5 *s = &f->s;
    const PitUartPic *e = &s->irq.edge;
    uint8_t v = Guard;
    CHECK(!platform_s5_take(s, &v) && v == Guard);
    out(s, THR+3, 0x80); out(s, THR, 0x59); out(s, THR+1, 0xfa);
    CHECK(in(s, THR) == 0x59 && in(s, THR+1) == 0xfa);
    out(s, THR+3, 3);
    CHECK(in(s, THR) == 0 && in(s, THR+5) == Uart8250Ready);
    CHECK(in(s, THR+2) == 2 && !e->uart.thre_pending && !uart16450_irq4(&e->uart));
    CHECK(in(s, THR+2) == 1 && e->irr == PitUartIrq4 && e->isr == PitUartIrq4);
    out(s, 0x20, 0x64);
    v = take4(s);
    out(s, 0x20, 0x64);
    CHECK(!e->isr && !e->irr && e->tx_count == N);
    return v;
}

static void stop(Fixture *f)
{
    uint32_t v = PowerS5Request;
    CHECK(platform_s5_io(&f->s, PowerScalarOut, PowerPm1aControl, 2, &v) == PowerIoShutdown);
    CHECK(v == PowerS5Request && !platform_s5_running(&f->s));
    refused(f, PowerScalarOut, THR, 1, 0x73);
    refused(f, PowerScalarIn, THR+5, 1, UINT32_MAX);
    refused(f, PowerScalarOut, PowerPm1aControl, 2, PowerS5Request);
}

int main(void)
{
    Fixture exact, contrast, reference, zero;
    fresh(&zero, 0);
    refused(&zero, PowerScalarOut, THR, 1, 0xff);
    PlatformS5 previous;
    memcpy(&previous, &zero.s, sizeof previous);
    CHECK(!platform_s5_init_tx(&zero.s, NULL, Max) && !memcmp(&previous, &zero.s, sizeof previous));
    PlatformS5 fixed = platform_s5_new();
    for (unsigned i = 0; i < sizeof fixed.irq.edge.uart.base.tx; ++i) {
        out(&fixed, THR, i); CHECK(in(&fixed, THR+5) == 0 && fixed.irq.edge.uart.base.tx[i] == i);
    }
    uint32_t v = 0xfe;
    CHECK(platform_s5_io(&fixed, PowerScalarOut, THR, 1, &v) == PowerIoRejected && v == 0xfe);
    CHECK(fixed.irq.edge.uart.base.count == 16 && !fixed.irq.edge.uart.base.busy);

    fresh(&exact, N); fresh(&contrast, Max); fresh(&reference, Max);
    fill(&exact, 0x52); fill(&contrast, 0x53); fill(&reference, 0x52);
    CHECK(!memcmp(exact.raw+1, contrast.raw+1, N-1) && exact.raw[N] != contrast.raw[N]);
    CHECK(!memcmp(exact.raw+1, reference.raw+1, N));
    refused(&exact, PowerScalarOut, THR, 1, 0x91); /* ready, capacity alone blocks */
    uint8_t vector = service(&exact);
    service(&contrast); service(&reference);
    refused(&exact, PowerScalarOut, THR, 1, 0x91);
    out(&reference.s, THR, 0x91);
    CHECK(reference.s.irq.edge.uart.base.busy && reference.s.irq.edge.tx_count == N+1);
    CHECK(reference.raw[N+1] == 0x91 && !memcmp(exact.raw+1, reference.raw+1, N));
    stop(&exact); stop(&contrast); /* ready THR, respectively zero and two bytes free */
    CHECK(exact.raw[0] == Guard && exact.raw[N+1] == Guard && contrast.raw[0] == Guard);
    CHECK(contrast.raw[N+1] == Guard && contrast.raw[Max+1] == Guard && reference.raw[N+2] == Guard);
    CHECK(reference.raw[0] == Guard && reference.raw[Max+1] == Guard);
    printf("fixed=%u prefix=%zu last=%02x/%02x drains=%u vector=%02x stopped-room=%zu/%zu reference[%zu]=%02x guards=%02x/%02x\n",
        fixed.irq.edge.uart.base.count, exact.s.irq.edge.tx_count-1, (unsigned)exact.raw[N], (unsigned)contrast.raw[N],
        exact.s.irq.edge.uart.base.polls_busy, (unsigned)vector,
        exact.s.irq.edge.tx_capacity-exact.s.irq.edge.tx_count, contrast.s.irq.edge.tx_capacity-contrast.s.irq.edge.tx_count,
        reference.s.irq.edge.tx_count-1, (unsigned)reference.raw[N+1], (unsigned)exact.raw[0], (unsigned)exact.raw[N+1]);
    return 0;
}
