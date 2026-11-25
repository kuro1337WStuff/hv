#ifndef HV_PIT_PERIODIC_IRQ0_H
#define HV_PIT_PERIODIC_IRQ0_H

#include "pit_periodic.h"
#include "pit_pic.h"

/* All CH0 modes use pit's single absolute pulse cursor. pic supplies only
 * IRQ0 service; its legacy logical one-shot clock/deadline stay idle.
 * pit_pic_take(&s.pic) accepts the request at PIT_PIC_VECTOR; it does not
 * check CPU eligibility, inject a vector, or prove guest handler delivery. */
typedef struct { PitPeriodic pit; PitPic pic; } PitPeriodicIrq0;

static inline void pit_periodic_irq0_init(PitPeriodicIrq0 *s)
{
    pit_periodic_init(&s->pit);
    pit_pic_init(&s->pic);
}

/* Transfer an actual latched OUT rise, never a sampled level. An empty
 * source latch cannot retract a request already held at the PIC. */
static inline void pit_periodic_irq0_transfer(PitPeriodicIrq0 *s)
{
    if (pit_periodic_take(&s->pit)) pit_pic_latch_irq0(&s->pic);
}

static inline int pit_periodic_irq0_advance(PitPeriodicIrq0 *s, uint64_t tick)
{
    if (!pit_periodic_advance(&s->pit, tick)) return 0;
    pit_periodic_irq0_transfer(s);
    return 1;
}

/* At the current cursor a mode command can itself cause a low-to-high edge. */
static inline int pit_periodic_irq0_write(PitPeriodicIrq0 *s, uint16_t port, unsigned width, uint8_t byte)
{
    if (port == PIC8259_MASTER_CMD || port == PIC8259_MASTER_DATA)
        return width == 1 && pit_pic_write(&s->pic, port, 1, byte);
    if (!pit_periodic_write(&s->pit, port, width, byte)) return 0;
    pit_periodic_irq0_transfer(s);
    return 1;
}
#endif
