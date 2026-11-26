#ifndef HV_PLATFORM_S5_H
#define HV_PLATFORM_S5_H

#include "legacy_com.h"
#include "pit_cycle_clock.h"
#include "pit_uart_sci_pic.h"
#include "power_request.h"

/* Host-only fixed IRQ9 fixture. irq owns the only CH0 and both PIC chips;
 * CH2 shares its pulse epoch. PM1 control owns the sole lifecycle. Treat
 * members as observations only; drive every operation through this API.
 * No hardware I/O, physical TSC, CPU delivery, guest resume or reset. */
#define PLATFORM_S5_VIRTUAL_HZ UINT32_C(2500000000)

typedef struct {
    PitUartSciPic irq;
    PowerPm1 control;
    PitChannel2 ch2;
    uint64_t elapsed_ns, cycles;
} PlatformS5;

/* Construct a fresh machine; never takes an existing machine as input. */
static inline PlatformS5 platform_s5_new(void)
{
    PlatformS5 s;
    pit_uart_sci_pic_init(&s.irq, 9);
    s.control = (PowerPm1){ PowerRunning, 0 };
    pit2_init(&s.ch2);
    s.elapsed_ns = s.cycles = 0;
    return s;
}

/* Construct in fresh caller-owned space; not a reset operation. Storage is
 * exclusive and writable for the lifetime of this instance; never alias it
 * with machine/operand cells or run copies sharing it. Count starts at zero
 * (irq.edge.tx_count); unused bytes are untouched. A zero-capacity buffer
 * rejects every THR. Missing storage fails before writing the machine, never
 * silently selecting the 16-byte fixture. No sticky overflow flag. */
static inline int platform_s5_init_tx(PlatformS5 *s, uint8_t *storage, size_t capacity)
{
    if (!s || !storage) return 0;
    *s = platform_s5_new();
    s->irq.edge.tx_bytes = storage;
    s->irq.edge.tx_capacity = capacity;
    return 1;
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
    /* CH2 has its own count and gate ports. Only a channel-2 control
     * word goes to it on 43h; CH0 still uses the shared IRQ router. */
    if (port == 0x42 || port == 0x61 ||
        (port == 0x43 && form == PowerScalarOut && (*value >> 6) == 2)) {
        if (form == PowerScalarOut)
            return *value <= UINT8_MAX && pit2_write(&s->ch2, port, bytes, (uint8_t)*value)
                ? PowerIoHandled : PowerIoRejected;
        uint8_t byte;
        if (!pit2_read(&s->ch2, port, bytes, &byte)) return PowerIoRejected;
        *value = byte;
        return PowerIoHandled;
    }
    if (legacy_com_absent_port(port))
        return legacy_com_io(&s->irq.edge.uart,
            form == PowerScalarIn ? LegacyComScalarIn : LegacyComScalarOut,
            port, bytes, value) ? PowerIoHandled : PowerIoRejected;
    return pit_uart_sci_pic_io(&s->irq, form == PowerScalarIn ? PitUartPicIn : PitUartPicOut,
        port, bytes, value) ? PowerIoHandled : PowerIoRejected;
}

/* Sole time input: absolute elapsed ns from construction. PIT ticks and
 * declared virtual cycles are separately floored. Unlike the isolated
 * wrapping clock, refuse an unrepresentable cycle total before any effect. */
static inline int platform_s5_advance_ns(PlatformS5 *s, uint64_t ns)
{
    uint64_t ticks, cycles;
    if (!platform_s5_running(s) || ns < s->elapsed_ns) return 0;
    if (ns == s->elapsed_ns) return 1;
    if (!pit_cycle_clock_count(ns, PLATFORM_S5_VIRTUAL_HZ, &cycles)) return 0;
    pit_pic_ns_to_ticks(ns, &ticks);
    if (!pit_shared_advance_channels(&s->irq.edge.pit, &s->ch2, ticks)) return 0;
    pit_uart_pic_transfer_out0(&s->irq.edge);
    s->elapsed_ns = ns;
    s->cycles = cycles;
    return 1;
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
