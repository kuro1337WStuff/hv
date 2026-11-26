#ifndef HV_PIT_SHARED_H
#define HV_PIT_SHARED_H

#include "pit_periodic_irq0.h"
#include "pit_channel2.h"

/* Host-only fixture. Both devices consume the caller's same absolute number
 * of completed 1,193,182 Hz PIT pulses. I/O never generates time; these
 * observations do not measure a TSC rate or deliver a guest interrupt. */
typedef struct { PitPeriodicIrq0 ch0; PitChannel2 ch2; } PitShared;

static inline void pit_shared_init(PitShared *s)
{
    pit_periodic_irq0_init(&s->ch0);
    pit2_init(&s->ch2);
}

/* Common pulse domain, independent of which PIC owns the OUT0 edge. The
 * caller transfers CH0's source latch to that controller after advancing. */
static inline int pit_shared_advance_channels(PitPeriodic *ch0, PitChannel2 *ch2, uint64_t tick)
{
    /* Reject before either cursor or the IRQ latch moves. For synchronized
     * cursors a forward tick satisfies both device advance preconditions. */
    uint64_t previous = ch0->tick;
    if (ch2->tick != previous || tick < previous) return 0;
    if (tick == previous) return 1;
    pit_periodic_advance(ch0, tick);
    pit2_advance(ch2, tick);
    return 1;
}

static inline int pit_shared_advance(PitShared *s, uint64_t tick)
{
    uint64_t previous = s->ch0.pit.tick;
    if (!pit_shared_advance_channels(&s->ch0.pit, &s->ch2, tick)) return 0;
    if (tick != previous) pit_periodic_irq0_transfer(&s->ch0);
    return 1;
}

/* The selector routes only the control word, not subsequent count bytes.
 * The selected device owns command validation and its byte sequencing. */
static inline int pit_shared_write(PitShared *s, uint16_t port, unsigned width, uint8_t byte)
{
    switch (port) {
    case 0x43:
        switch (byte >> 6) {
        case 0: return pit_periodic_irq0_write(&s->ch0, port, width, byte);
        case 2: return pit2_write(&s->ch2, port, width, byte);
        default: return 0;
        }
    case 0x40:
    case PIC8259_MASTER_CMD:
    case PIC8259_MASTER_DATA:
        return pit_periodic_irq0_write(&s->ch0, port, width, byte);
    case 0x42:
    case 0x61:
        return pit2_write(&s->ch2, port, width, byte);
    default: return 0;
    }
}

static inline int pit_shared_read(PitShared *s, uint16_t port, unsigned width, uint8_t *byte)
{
    switch (port) {
    case PIC8259_MASTER_DATA:
        return width == 1 && pit_pic_read(&s->ch0.pic, port, 1, byte);
    case 0x42:
    case 0x61:
        return pit2_read(&s->ch2, port, width, byte);
    default: return 0;
    }
}

#endif
