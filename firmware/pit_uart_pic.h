#ifndef HV_PIT_UART_PIC_H
#define HV_PIT_UART_PIC_H

#include "pic8259.h"
#include "pit_periodic.h"
#include "uart16450.h"

#include <stddef.h>

/* Opt-in host fixture, decoded scalar bytes only (caller supplies uart16450.h's
 * byte/BOOLEAN types). Drive devices through io/advance so no pin transition
 * is missed. PIT's absolute pulse cursor and the UART's read-driven TX drain
 * are independent. Neither a host interrupt controller nor a guest injector.
 *
 * Only master IR0 and IR4, edge-triggered, fixed fully nested priority, normal
 * specific EOI. The PIC parser owns the sole master IMR; its unused slave has
 * no ports, cascade or delivery here. Master ELCR is pinned to zero. No OCW3,
 * level mode, spurious interrupt, priority rotation, CPU eligibility or INTA
 * timing. Inspect irr/isr as host observations, not command-port reads. */
typedef struct {
    Pic8259 pic;
    PitPeriodic pit;
    Uart16450 uart;
    uint8_t *tx_bytes; /* NULL keeps the UART's fixed internal transcript */
    size_t tx_capacity, tx_count;
    uint8_t irr, isr;
} PitUartPic;

enum { PitUartIrq0 = 0x01, PitUartIrq4 = 0x10 };
enum PitUartPicIo { PitUartPicIn, PitUartPicOut };

/* Out-of-band reset also discards service and device activity. Source pins
 * start OUT0 high/IRQ4 low, neither is a request. No implicit ICWs. */
static inline void pit_uart_pic_init(PitUartPic *s)
{
    const Uart16450 idle = {0};
    pic8259_init(&s->pic);
    pit_periodic_init(&s->pit);
    s->uart = idle;
    s->tx_bytes = NULL;
    s->tx_capacity = s->tx_count = 0;
    s->irr = s->isr = 0;
}

static inline void pit_uart_pic_transfer_out0(PitUartPic *s)
{
    if (pit_periodic_take(&s->pit)) s->irr |= PitUartIrq0;
}

static inline int pit_uart_pic_advance(PitUartPic *s, uint64_t tick)
{
    if (!s || !pit_periodic_advance(&s->pit, tick)) return 0;
    pit_uart_pic_transfer_out0(s); /* even if a jump ends with OUT0 low */
    return 1;
}

/* ICW1 in probe mode clears pre-init requests. A READY master may restart
 * only with empty IRR and ISR. Mid-ICW restarts are refused by pic8259_write.
 * ICW1 never resets either source; fresh edges after ICW1 latch even during
 * programming, but no request is accepted until 11/30/04/01 completes.
 * An EOI addresses exactly one active ISR bit without changing IRR or pins. */
static inline int pit_uart_pic_command(PitUartPic *s, uint8_t byte)
{
    if (byte == 0x11) {
        if (s->pic.master.phase == PIC8259_READY && (s->irr || s->isr)) return 0;
        if (!pic8259_write(&s->pic, PIC8259_MASTER_CMD, 1, byte)) return 0;
        s->irr = 0;
        return 1;
    }
    uint8_t bit;
    if (byte == 0x60) bit = PitUartIrq0;
    else if (byte == 0x64) bit = PitUartIrq4;
    else return 0;
    if (!(s->isr & bit)) return 0;
    s->isr &= (uint8_t)~bit;
    return 1;
}

/* One classified scalar access; don't lower string/REP I/O to these calls.
 * IN replaces the caller's separate output cell only on success. A refused
 * transaction changes neither the machine nor the cell. */
static inline int pit_uart_pic_io(PitUartPic *s, enum PitUartPicIo form,
    uint16_t port, unsigned bytes, uint32_t *value)
{
    if (!s || !value || bytes != 1 || (form != PitUartPicIn && form != PitUartPicOut)) return 0;
    if (form == PitUartPicOut && *value > UINT8_MAX) return 0;
    uint8_t byte = form == PitUartPicOut ? (uint8_t)*value : 0;
    if (port >= Uart8250Base && port <= Uart8250Base + 7) {
        Uart8250 *b = &s->uart.base;
        /* Busy is the UART's own refusal. Full storage refuses an otherwise
         * writable THR, not DLL/RBR or a drain/ack. Neither refusal latches. */
        if (s->tx_bytes && s->tx_count == s->tx_capacity && !b->busy &&
            form == PitUartPicOut && port == Uart8250Base && !(b->lcr & 0x80)) return 0;
        int was_high = uart16450_irq4(&s->uart);
        if (!uart16450_access(&s->uart, port, form == PitUartPicIn, &byte)) return 0;
        if (s->tx_bytes && b->count) {
            /* Consume the accepted register-model byte, not the OUT operand.
             * Only its transcript slot is released; TX remains busy. */
            s->tx_bytes[s->tx_count++] = b->tx[0];
            b->count = 0;
        }
        if (!was_high && uart16450_irq4(&s->uart)) s->irr |= PitUartIrq4;
    } else if (port == PIC8259_MASTER_DATA || port == PIC8259_MASTER_ELCR) {
        if (form == PitUartPicIn) {
            if (!pic8259_read(&s->pic, port, bytes, &byte)) return 0;
        } else {
            if (port == PIC8259_MASTER_ELCR && byte != 0) return 0;
            return pic8259_write(&s->pic, port, bytes, byte);
        }
    } else if (port == PIC8259_MASTER_CMD && form == PitUartPicOut) {
        return pit_uart_pic_command(s, byte);
    } else {
        if (form != PitUartPicOut || !pit_periodic_write(&s->pit, port, bytes, byte)) return 0;
        pit_uart_pic_transfer_out0(s); /* a mode write can raise OUT0 */
    }
    if (form == PitUartPicIn) *value = byte;
    return 1;
}

/* Fixed priority: only bits strictly below the lowest numbered service
 * bit can preempt it. With no service, (0 - 1) admits all eight bits.
 * pending may also include a sampled cascade on a shared master. */
static inline uint8_t pit_uart_pic_eligible(const PitUartPic *s, uint8_t pending)
{
    unsigned service = s->isr;
    unsigned higher = (service & (0u - service)) - 1u;
    return (uint8_t)(pending & ~s->pic.master.imr & higher);
}

/* Acknowledge at most one unmasked pending edge strictly above every active
 * priority. Never resample pins here or during EOI. Acceptance is not guest
 * entry, handler execution, or acknowledgment of a device's source cause. */
static inline int pit_uart_pic_take_edge(PitUartPic *s, uint8_t eligible, uint8_t *vector)
{
    uint8_t bit;
    if (eligible & PitUartIrq0) {
        bit = PitUartIrq0;
        *vector = PIC8259_MASTER_BASE;
    } else if (eligible & PitUartIrq4) {
        bit = PitUartIrq4;
        *vector = PIC8259_MASTER_BASE + 4;
    } else return 0;
    s->irr &= (uint8_t)~bit;
    s->isr |= bit;
    return 1;
}

static inline int pit_uart_pic_take(PitUartPic *s, uint8_t *vector)
{
    if (!s || !vector || s->pic.master.phase != PIC8259_READY) return 0;
    return pit_uart_pic_take_edge(s, pit_uart_pic_eligible(s, s->irr), vector);
}
#endif
