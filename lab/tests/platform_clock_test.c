/* Trial 120. Model inputs/observations only: no host clock, RDTSC or port I/O. */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint8_t UINT8, BOOLEAN;
typedef uint16_t UINT16;
enum { FALSE = 0, TRUE = 1 };
#include "../../firmware/platform_s5.h"

#define CHECK(e) do { if (!(e)) { fprintf(stderr, "line %d: %s\n", __LINE__, #e); exit(EXIT_FAILURE); } } while (0)
#define UNCHANGED(s, e) do { unsigned char before[sizeof *(s)]; memcpy(before, (s), sizeof before); \
    CHECK(e); CHECK(!memcmp(before, (s), sizeof before)); } while (0)
/* At 5/2 virtual cycles per ns this exact epoch yields UINT64_MAX cycles. */
#define LAST_NS UINT64_C(7378697629483820646)

static uint32_t io(PlatformS5 *s, enum PowerIoForm form, uint16_t port, unsigned width,
    uint32_t operand, PowerIoResult result)
{
    uint64_t ns = s->elapsed_ns, cycles = s->cycles, ticks = s->ch2.tick;
    uint32_t value = operand;
    CHECK(platform_s5_io(s, form, port, width, &value) == result);
    CHECK(s->elapsed_ns == ns && s->cycles == cycles && s->ch2.tick == ticks && s->irq.edge.pit.tick == ticks);
    if (form != PowerScalarIn || result != PowerIoHandled) CHECK(value == operand);
    return value;
}

static void wr(PlatformS5 *s, uint16_t port, uint8_t byte)
{
    io(s, PowerScalarOut, port, 1, byte, PowerIoHandled);
}

static uint32_t rd(PlatformS5 *s, uint16_t port)
{
    return io(s, PowerScalarIn, port, 1, UINT32_MAX, PowerIoHandled);
}

static uint8_t take(PlatformS5 *s, uint8_t expected)
{
    uint8_t vector = 0xad;
    CHECK(platform_s5_take(s, &vector) == (expected != 0xad) && vector == expected);
    return vector;
}

/* Compare the same two endpoints, partitioning across different fractional
 * cycle/pulse boundaries. No I/O or acknowledgement between subdivisions. */
static void advance_pair(PlatformS5 *bulk, PlatformS5 *split, uint64_t ns)
{
    for (uint64_t t = split->elapsed_ns + 317; t < ns; t += 317)
        CHECK(platform_s5_advance_ns(split, t));
    CHECK(platform_s5_advance_ns(split, ns) && platform_s5_advance_ns(bulk, ns));
    CHECK(bulk->cycles == split->cycles && bulk->ch2.count == split->ch2.count);
    CHECK(bulk->irq.edge.pit.count == split->irq.edge.pit.count &&
          pit_periodic_out(&bulk->irq.edge.pit) == pit_periodic_out(&split->irq.edge.pit));
    CHECK(pit_uart_sci_pic_pending(&bulk->irq) == pit_uart_sci_pic_pending(&split->irq) &&
          pit_uart_sci_pic_eligible(&bulk->irq) == pit_uart_sci_pic_eligible(&split->irq));
}

static void open_gate(PlatformS5 *s)
{
    CHECK(s->elapsed_ns == 4201 && s->cycles == 10502 && s->ch2.tick == 5);
    CHECK(rd(s, 0x61) == 0 && s->ch2.count == 0x1202 && rd(s, 0x42) == 2);
    CHECK(s->irq.edge.pit.count == 4 && pit_periodic_out(&s->irq.edge.pit));
    CHECK(s->irq.edge.irr == 0x11 && rd(s, 0x21) == 0xef); /* IR0 latched while masked */
    UNCHANGED(s, platform_s5_advance_ns(s, 4201));
    UNCHANGED(s, !platform_s5_advance_ns(s, 4200)); /* same PIT pulse, earlier ns */
    UNCHANGED(s, !platform_s5_advance_ns(s, LAST_NS + 1));
    UNCHANGED(s, io(s, PowerScalarIn, 0x42, 257, UINT32_MAX, PowerIoRejected) == UINT32_MAX);
    UNCHANGED(s, io(s, PowerScalarOut, 0x61, 1, 0x101, PowerIoRejected) == 0x101);
    take(s, 0x34); take(s, 0xad); /* actual lower-priority UART grant; IR0 still masked */
    wr(s, 0x61, 1);
    wr(s, 0x21, 0xee);
    take(s, 0x30); /* preempts UART at the same absolute time */
    CHECK(!s->irq.edge.irr && s->irq.edge.isr == 0x11);
}

static uint8_t service_relatched(PlatformS5 *s)
{
    CHECK(s->elapsed_ns == 8401 && s->cycles == 21002 && s->ch2.tick == 10);
    CHECK(rd(s, 0x61) == 1 && rd(s, 0x42) == 0x11); /* live high from 11fdh, not 1202h */
    CHECK(rd(s, 0x42) == 0xfd && s->ch2.count == 0x11fd); /* high read now outstanding */
    CHECK(s->irq.edge.pit.count == 3 && s->irq.edge.irr == 1 && s->irq.edge.isr == 0x11);
    take(s, 0xad); /* another rise cannot preempt its own active service */
    wr(s, 0x20, 0x60);
    uint8_t vector = take(s, 0x30);
    wr(s, 0x20, 0x60); wr(s, 0x20, 0x64);
    CHECK(!s->irq.edge.irr && !s->irq.edge.isr);
    return vector;
}

int main(void)
{
    static const struct { uint16_t port; uint8_t byte; } setup[] = {
        {0x20,0x11}, {0x21,0x30}, {0x21,4}, {0x21,1},
        {0xa0,0x11}, {0xa1,0x38}, {0xa1,2}, {0xa1,1},
        {0x21,0xef}, {0x3f9,2}, {0x3fc,8}, /* only UART unmasked */
        {0x43,0x34}, {0x40,4}, {0x40,0}, {0x43,0xb0}, {0x42,2}, {0x42,0x12}
    };
    PlatformS5 bulk = platform_s5_new(), split;
    for (size_t i = 0; i < sizeof setup / sizeof setup[0]; ++i) wr(&bulk, setup[i].port, setup[i].byte);
    memcpy(&split, &bulk, sizeof split);
    advance_pair(&bulk, &split, 4201);
    open_gate(&bulk); open_gate(&split);
    advance_pair(&bulk, &split, 8401);
    uint8_t vector = service_relatched(&bulk);
    CHECK(service_relatched(&split) == vector);
    printf("bulk/split: ns=%" PRIu64 " virtual_cycles=%" PRIu64 " at %" PRIu32 " Hz ticks=%" PRIu64
           " CH2=%04x regrant=%02x\n", bulk.elapsed_ns, bulk.cycles, PLATFORM_S5_VIRTUAL_HZ,
           bulk.ch2.tick, (unsigned)bulk.ch2.count, (unsigned)vector);

    io(&bulk, PowerScalarOut, PowerPm1aControl, 2, 0x3401, PowerIoShutdown);
    io(&split, PowerScalarOut, PowerPm1aControl, 2, 0x1401, PowerIoHandled);
    UNCHANGED(&bulk, !platform_s5_advance_ns(&bulk, 9300));
    UNCHANGED(&bulk, io(&bulk, PowerScalarIn, 0x42, 1, UINT32_MAX, PowerIoRejected) == UINT32_MAX);
    UNCHANGED(&bulk, io(&bulk, PowerScalarOut, 0x61, 1, 0, PowerIoRejected) == 0);
    CHECK(platform_s5_advance_ns(&split, 9300) && split.cycles == 23250 && split.ch2.tick == 11);
    CHECK(rd(&split, 0x42) == 0x11 && rd(&split, 0x42) == 0xfc && split.ch2.count == 0x11fc);
    wr(&split, 0x61, 0);
    printf("3401/1401: ns=%" PRIu64 "/%" PRIu64 " virtual_cycles=%" PRIu64 "/%" PRIu64
           " CH2=%04x/%04x\n", bulk.elapsed_ns, split.elapsed_ns, bulk.cycles, split.cycles,
           (unsigned)bulk.ch2.count, (unsigned)split.ch2.count);
    CHECK(platform_s5_advance_ns(&split, LAST_NS) && split.cycles == UINT64_MAX);
    UNCHANGED(&split, !platform_s5_advance_ns(&split, LAST_NS + 1));
    return 0;
}
