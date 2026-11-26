#ifndef HV_PIT_UART_SCI_PIC_H
#define HV_PIT_UART_SCI_PIC_H

#include "pit_uart_pic.h"
#include "power_sci.h"

/* Host-only, normal specific EOI, fixed priority IR0 > IR2 > IR4. edge.pic
 * owns BOTH IMRs/ICWs/ELCRs. edge.isr is the ENTIRE master ISR, including
 * cascade service. edge.irr latches only PIT/UART rises, never SCI. Use the
 * shared io/take below, pit_uart_pic_advance for pulses, and power_event_latch
 * for synthetic PM1 inputs. No implicit ICWs, INTA timing or CPU delivery. */
typedef struct {
    PitUartPic edge;
    PowerPm1aEventRegs event;
    uint8_t slave_isr;
} PitUartSciPic;

static inline int pit_uart_sci_pic_init(PitUartSciPic *s, uint16_t sci_int)
{
    if (!s || sci_int != 9) return 0;
    pit_uart_pic_init(&s->edge);
    s->event.status = s->event.enable = 0;
    s->slave_isr = 0;
    return 1;
}

/* Master request lines before master mask/priority. IR2 is the slave's
 * current output, not an edge queue: a PM1 W1C before take leaves no debt. */
static inline uint8_t pit_uart_sci_pic_pending(const PitUartSciPic *s)
{
    return s->edge.irr | power_sci_cascade(&s->event, &s->edge.pic, s->slave_isr);
}

static inline uint8_t pit_uart_sci_pic_eligible(const PitUartSciPic *s)
{
    return pic8259_ready(&s->edge.pic) ?
        pit_uart_pic_eligible(&s->edge, pit_uart_sci_pic_pending(s)) : 0;
}

/* One decoded scalar transaction. PM1 owns byte/aligned-word rules; all
 * other ports are byte-only. Master ELCR stays pinned to edge mode. A
 * slave ICW1 doesn't clear master edges; no grant until BOTH chips ready. */
static inline int pit_uart_sci_pic_io(PitUartSciPic *s, enum PitUartPicIo form,
    uint16_t port, unsigned bytes, uint32_t *value)
{
    if (!s || !value || (form != PitUartPicIn && form != PitUartPicOut)) return 0;
    if (port >= PowerPm1aEvent && port < PowerPm1aControl)
        return power_event_io(&s->event, form == PitUartPicIn ? PowerScalarIn : PowerScalarOut,
            port, bytes, value) == PowerIoHandled;
    if (port == PIC8259_SLAVE_CMD || port == PIC8259_SLAVE_DATA || port == PIC8259_SLAVE_ELCR ||
        (port == PIC8259_MASTER_CMD && form == PitUartPicOut && *value == 0x62)) {
        if (form == PitUartPicOut)
            return power_sci_route_write(&s->edge.pic, &s->edge.isr, &s->slave_isr, port, bytes, *value);
        uint8_t byte;
        if (!pic8259_read(&s->edge.pic, port, bytes, &byte)) return 0;
        *value = byte;
        return 1;
    }
    /* The edge-only command handler already guards a busy master. Also
     * protect service left on the slave after a master-first cascade EOI. */
    if (port == PIC8259_MASTER_CMD && form == PitUartPicOut && *value == 0x11 && s->slave_isr) return 0;
    return pit_uart_pic_io(&s->edge, form, port, bytes, value);
}

static inline int pit_uart_sci_pic_take(PitUartSciPic *s, uint8_t *vector)
{
    if (!s || !vector) return 0;
    uint8_t eligible = pit_uart_sci_pic_eligible(s);
    if ((eligible & (PitUartIrq0 | PowerSciCascadeBit)) == PowerSciCascadeBit) {
        s->edge.isr |= PowerSciCascadeBit;
        s->slave_isr |= PowerSciSlaveBit;
        *vector = PIC8259_SLAVE_BASE + 1;
        return 1;
    }
    return pit_uart_pic_take_edge(&s->edge, eligible, vector);
}
#endif
