/** @file uart16450.h
 * Fixed, transmit-only 16450-compatible guest platform. No host port or real IRQ.
 */
#ifndef HV_UART16450_H
#define HV_UART16450_H
#include "uart8250.h"

typedef struct { Uart8250 base; BOOLEAN thre_pending; } Uart16450;

/** Read the modeled PC IRQ4 level after a register transaction. OUT2 gates the
 * external line, not the IIR register. An interrupt controller must latch its
 * own rising edge; this helper neither injects an interrupt nor acknowledges it. */
static inline BOOLEAN uart16450_irq4(const Uart16450 *u)
{
    return u->thre_pending && (u->base.ier & 2) && (u->base.mcr & 8);
}

/** Access one COM1 byte on the opt-in non-FIFO model. Caller owns the device
 * and validates the scalar IO exit before calling. Bit 1 of IER enables THRE;
 * the other low three IER bits read back but have no sources on this TX-only
 * platform. Reading an active IIR THRE cause or writing THR clears that cause.
 * A busy-to-empty transition or newly enabling THRE when empty can rearm it.
 * The existing fixed one-poll drain policy is logical time, not a baud rate.
 * @return FALSE on unsupported access, with byte and device unchanged.
 */
static inline BOOLEAN uart16450_access(Uart16450 *u, UINT16 port, BOOLEAN input, UINT8 *byte)
{
    Uart8250 *b=&u->base;
    if (port<Uart8250Base || port>Uart8250Base+7) return FALSE;
    unsigned reg=port-Uart8250Base;
    if (!(b->lcr&0x80) && reg==1 && !input) {
        if (*byte&0xf0) return FALSE;
        if (!(*byte&2)) u->thre_pending=FALSE;
        else if (!(b->ier&2) && !b->busy) u->thre_pending=TRUE;
        b->ier=*byte;
        return TRUE;
    }
    if (reg==2 && input) {
        *byte=(u->thre_pending && (b->ier&2)) ? 2 : 1;
        u->thre_pending=FALSE;
        return TRUE;
    }
    BOOLEAN was_busy=b->busy;
    if (!uart8250_access(b,port,input,byte)) return FALSE;
    if (!(b->lcr&0x80) && reg==0 && !input) u->thre_pending=FALSE;
    if (was_busy && !b->busy && (b->ier&2)) u->thre_pending=TRUE;
    return TRUE;
}
#endif
