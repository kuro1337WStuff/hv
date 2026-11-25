#ifndef HV_GUEST_TX_CAPTURE_H
#define HV_GUEST_TX_CAPTURE_H
#include "uart8250.h"

/* Zero-initialize once per cold run. bytes[0..capacity) is caller-owned L1
 * storage, never L2 RAM; the populated prefix is binary, not NUL-terminated.
 * Use this entry point exclusively on uart. No capture of diagnostics or RX. */
typedef struct {
    Uart8250 uart;
    UINT8 *bytes;
    UINTN capacity, count;
    BOOLEAN overflow;
} GuestTxCapture;

typedef enum { GuestTxOk, GuestTxRefused, GuestTxOverflow } GuestTxResult;

static GuestTxResult guest_tx_access(GuestTxCapture *c, UINT16 port, BOOLEAN input, UINT8 *byte)
{
    if (c->overflow) return GuestTxOverflow;
    /* At capacity, non-TX operations (including the final drain and DLL)
     * still work. An otherwise valid THR write stops before changing the UART. */
    if (c->count == c->capacity && !input && port == Uart8250Base &&
        !(c->uart.lcr & 0x80) && !c->uart.busy) {
        c->overflow = TRUE;
        return GuestTxOverflow;
    }
    if (!uart8250_access(&c->uart, port, input, byte)) return GuestTxRefused;
    /* The register model, not the proposed AL byte, is the wire witness.
     * Consume its one accepted byte on every THR write; leave busy intact. */
    if (c->uart.count) {
        c->bytes[c->count++] = c->uart.tx[0];
        c->uart.count = 0;
    }
    return GuestTxOk;
}
#endif
