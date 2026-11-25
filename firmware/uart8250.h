#ifndef HV_UART8250_H
#define HV_UART8250_H

enum { Uart8250Base=0x3f8, Uart8250Ready=0x60 };
typedef struct {
    UINT8 dll, dlm, lcr, ier, mcr, scratch;
    BOOLEAN busy;
    UINT8 tx[16];
    unsigned count, polls_busy;
} Uart8250;

/* One-way polled COM1. No FIFO, external modem inputs, loopback, interrupts,
 * or physical port accesses. A rejected request leaves both u and *byte alone. */
static BOOLEAN uart8250_access(Uart8250 *u, UINT16 port, BOOLEAN input, UINT8 *byte)
{
    if (port<Uart8250Base || port>Uart8250Base+7) return FALSE;
    unsigned reg=port-Uart8250Base;
    if ((u->lcr&0x80) && reg<2) {
        UINT8 *latch=reg ? &u->dlm : &u->dll;
        if (input) *byte=*latch; else *latch=*byte;
        return TRUE;
    }
    if (input) {
        switch (reg) {
        case 0: *byte=0; break; /* RBR: no receive data */
        case 1: *byte=u->ier; break;
        case 2: *byte=1; break; /* IIR: no interrupt pending; no FIFO bits */
        case 3: *byte=u->lcr; break;
        case 4: *byte=u->mcr; break;
        case 5:
            *byte=u->busy ? 0 : Uart8250Ready; /* LSR: THRE | TEMT */
            if (u->busy) { u->busy=FALSE; ++u->polls_busy; }
            break;
        case 6: *byte=0; break; /* MSR: no modem inputs or deltas */
        case 7: *byte=u->scratch; break;
        }
    } else {
        switch (reg) {
        case 0:
            if (u->busy || u->count>=sizeof(u->tx)) return FALSE;
            u->tx[u->count++]=*byte;
            u->busy=TRUE;
            break;
        case 1:
            if (*byte) return FALSE; /* IRQ-enabled operation isn't modeled */
            u->ier=*byte;
            break;
        case 2: break; /* 8250: no FIFO, ignore FCR */
        case 3: u->lcr=*byte; break;
        case 4:
            if (*byte&0x10) return FALSE; /* modem loopback isn't modeled */
            u->mcr=*byte&0x0f;
            break;
        case 7: u->scratch=*byte; break;
        default: return FALSE; /* read-only LSR/MSR */
        }
    }
    return TRUE;
}
#endif
