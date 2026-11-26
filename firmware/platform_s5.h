#ifndef HV_PLATFORM_S5_H
#define HV_PLATFORM_S5_H

#include "legacy_com.h"
#include "pit_uart_sci_pic.h"
#include "power_request.h"

/* Host-only fixed IRQ9 fixture. The shared PIC model owns all device state
 * except PM1 control, which owns the sole lifecycle. Treat members as
 * observations only; drive every operation through this API. No hardware
 * I/O, callbacks, CPU delivery, guest resume, or reset of a stopped machine. */
typedef struct {
    PitUartSciPic irq;
    PowerPm1 control;
} PlatformS5;

/* Construct a fresh machine; never takes an existing machine as input. */
static inline PlatformS5 platform_s5_new(void)
{
    PlatformS5 s;
    pit_uart_sci_pic_init(&s.irq, 9);
    s.control = (PowerPm1){ PowerRunning, 0 };
    return s;
}

static inline int platform_s5_running(const PlatformS5 *s)
{
    return s && s->control.lifecycle == PowerRunning;
}

/* One classified scalar transaction and a separate operand/result cell.
 * COM1 goes through the shared router so UART pin transitions reach IR4.
 * Only successful IN replaces the cell, and only the first S5 is terminal. */
static inline PowerIoResult platform_s5_io(PlatformS5 *s, enum PowerIoForm form,
    uint16_t port, unsigned bytes, uint32_t *value)
{
    if (!platform_s5_running(s) || !value ||
        (form != PowerScalarIn && form != PowerScalarOut)) return PowerIoRejected;
    if (port == PowerPm1aControl || port == PowerPm1aControl + 1)
        return power_pm1_io(&s->control, form, port, bytes, value);
    if (legacy_com_absent_port(port))
        return legacy_com_io(&s->irq.edge.uart,
            form == PowerScalarIn ? LegacyComScalarIn : LegacyComScalarOut,
            port, bytes, value) ? PowerIoHandled : PowerIoRejected;
    return pit_uart_sci_pic_io(&s->irq, form == PowerScalarIn ? PitUartPicIn : PitUartPicOut,
        port, bytes, value) ? PowerIoHandled : PowerIoRejected;
}

static inline int platform_s5_advance(PlatformS5 *s, uint64_t tick)
{
    return platform_s5_running(s) && pit_uart_pic_advance(&s->irq.edge, tick);
}

static inline int platform_s5_latch(PlatformS5 *s, uint32_t bits)
{
    return platform_s5_running(s) && power_event_latch(&s->irq.event, bits);
}

static inline int platform_s5_take(PlatformS5 *s, uint8_t *vector)
{
    return platform_s5_running(s) && pit_uart_sci_pic_take(&s->irq, vector);
}

#endif
