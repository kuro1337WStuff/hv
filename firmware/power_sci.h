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
static inline int power_sci_asserted(const PowerSci *s)
{
    return s && (s->event.status & s->event.enable & PowerEventGlobal) != 0;
}

/* One scalar PIC OUT. Delegate ordinary programming, including IMRs during
 * service. Reject ICW restarts on either chip while either ISR bit survives.
 * Specific EOIs address independent chips; no mask prerequisite or ordering
 * imposed by hardware here. Unmatched/duplicate/other EOIs are unsupported
 * (fixture refusal, not an 8259A bus fault). On refusal nothing changes. */
static inline int power_sci_pic_write(PowerSci *s, uint16_t port, unsigned bytes, uint32_t value)
{
    if (!s || bytes != 1 || value > UINT8_MAX) return 0;
    if (port == PIC8259_SLAVE_CMD || port == PIC8259_MASTER_CMD) {
        if (value == 0x11 && (s->slave_isr || s->master_isr)) return 0;
        if (port == PIC8259_SLAVE_CMD && value == 0x61 && s->slave_isr) {
            s->slave_isr = 0;
            return 1;
        }
        if (port == PIC8259_MASTER_CMD && value == 0x62 && s->master_isr) {
            s->master_isr = 0;
            return 1;
        }
    }
    return pic8259_write(&s->pic, port, bytes, (uint8_t)value);
}

/* Sample a level, never enqueue an edge. Edge mode refuses acceptance; ELCR
 * changes do not latch requests. Clear-before-take disappears; persistent
 * assertion is eligible again only after BOTH EOIs and BOTH unmaskings.
 * Success is PIC acceptance only. Failure preserves state and output. */
static inline int power_sci_take(PowerSci *s, uint8_t *vector)
{
    if (!vector || !power_sci_asserted(s) || !pic8259_ready(&s->pic) ||
        !(s->pic.slave.elcr & PowerSciSlaveBit) ||
        (s->pic.slave.imr & PowerSciSlaveBit) || (s->pic.master.imr & PowerSciCascadeBit) ||
        s->slave_isr || s->master_isr) return 0;
    s->slave_isr = PowerSciSlaveBit;
    s->master_isr = PowerSciCascadeBit;
    *vector = PIC8259_SLAVE_BASE + 1;
    return 1;
}
#endif
