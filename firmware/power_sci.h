#ifndef HV_POWER_SCI_H
#define HV_POWER_SCI_H

#include "power_event.h"
#include "pic8259.h"

/* Host-only ACPI-only platform: the caller supplies the generated FADT's
 * SCI_INT (an IRQ, not a vector). Only the slave IR1 -> master IR2 route
 * exists here. No competing IRQs, priorities, spurious IRQs, CPU eligibility,
 * injection, or IRQ0 device. PM1 words and ELCR/ICW/IMR keep their owners. */
typedef struct {
    PowerPm1aEventRegs event;
    Pic8259 pic;
    uint8_t slave_isr, master_isr; /* only this route's bits, not read ports */
} PowerSci;

enum { PowerSciSlaveBit = 1u << 1, PowerSciCascadeBit = 1u << 2 };

/* Out-of-band fresh machine/reset, never a guest transaction. Discards even
 * active service and starts masked with a low source. Bad route: no effect. */
static inline int power_sci_init(PowerSci *s, uint16_t sci_int)
{
    if (!s || sci_int != 9) return 0;
    s->event.status = s->event.enable = 0;
    pic8259_init(&s->pic);
    s->slave_isr = s->master_isr = 0;
    return 1;
}

/* Logical assertion, not pin polarity. ACPI 6.5 4.8.3.1.1/.2: WAK_STS has
 * no enable. PIC configuration cannot manufacture or drop the PM1 level.
 * power_event_latch is a synthetic register input, not a FACS lock release. */
static inline int power_sci_level(const PowerPm1aEventRegs *event)
{
    return (event->status & event->enable & PowerEventGlobal) != 0;
}

static inline int power_sci_asserted(const PowerSci *s)
{
    return s && power_sci_level(&s->event);
}

/* Slave INT drives master IR2 only while IR1 is a serviceable level.
 * Do not latch this computed request in either chip's edge storage. */
static inline uint8_t power_sci_cascade(const PowerPm1aEventRegs *event,
    const Pic8259 *pic, uint8_t slave_isr)
{
    return pic8259_ready(pic) && power_sci_level(event) &&
        (pic->slave.elcr & PowerSciSlaveBit) && !(pic->slave.imr & PowerSciSlaveBit) &&
        !slave_isr ? PowerSciCascadeBit : 0;
}

/* One scalar PIC OUT. Delegate ordinary programming, including IMRs during
 * service. Reject ICW restarts on either chip while either ISR bit survives.
 * Specific EOIs address independent chips; no mask prerequisite or ordering
 * imposed by hardware here. Unmatched/duplicate/other EOIs are unsupported
 * (fixture refusal, not an 8259A bus fault). On refusal nothing changes. */
static inline int power_sci_route_write(Pic8259 *pic, uint8_t *master_isr, uint8_t *slave_isr,
    uint16_t port, unsigned bytes, uint32_t value)
{
    if (bytes != 1 || value > UINT8_MAX) return 0;
    if (port == PIC8259_SLAVE_CMD || port == PIC8259_MASTER_CMD) {
        if (value == 0x11 && (*slave_isr || *master_isr)) return 0;
        if (port == PIC8259_SLAVE_CMD && value == 0x61 && (*slave_isr & PowerSciSlaveBit)) {
            *slave_isr &= (uint8_t)~PowerSciSlaveBit;
            return 1;
        }
        if (port == PIC8259_MASTER_CMD && value == 0x62 && (*master_isr & PowerSciCascadeBit)) {
            *master_isr &= (uint8_t)~PowerSciCascadeBit;
            return 1;
        }
    }
    return pic8259_write(pic, port, bytes, (uint8_t)value);
}

static inline int power_sci_pic_write(PowerSci *s, uint16_t port, unsigned bytes, uint32_t value)
{
    return s && power_sci_route_write(&s->pic, &s->master_isr, &s->slave_isr, port, bytes, value);
}

/* Sample a level, never enqueue an edge. Edge mode refuses acceptance; ELCR
 * changes do not latch requests. Clear-before-take disappears; persistent
 * assertion is eligible again only after BOTH EOIs and BOTH unmaskings.
 * Success is PIC acceptance only. Failure preserves state and output. */
static inline int power_sci_take(PowerSci *s, uint8_t *vector)
{
    if (!s || !vector || !power_sci_cascade(&s->event, &s->pic, s->slave_isr) ||
        (s->pic.master.imr & PowerSciCascadeBit) || s->master_isr) return 0;
    s->slave_isr = PowerSciSlaveBit;
    s->master_isr = PowerSciCascadeBit;
    *vector = PIC8259_SLAVE_BASE + 1;
    return 1;
}
#endif
