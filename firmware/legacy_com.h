/** @file legacy_com.h
 * Opt-in host model of the four x86 legacy serial candidates, not an ISA bus
 * default. Candidate addresses: https://github.com/gregkh/linux/blob/v6.18.60/arch/x86/include/asm/serial.h
 */
#ifndef HV_LEGACY_COM_H
#define HV_LEGACY_COM_H

#include <stdint.h>
#include "uart16450.h"

enum LegacyComForm { LegacyComOther, LegacyComScalarIn, LegacyComScalarOut };

static inline int legacy_com_absent_port(uint16_t port)
{
    switch (port & ~7u) {
    case 0x2f8: /* COM2 */
    case 0x3e8: /* COM3 */
    case 0x2e8: /* COM4 */
        return 1;
    default:
        return 0;
    }
}

/** Apply one classified transaction at a virtual 16-bit I/O port. The caller
 * owns the initialized COM1 model and a separate value cell. Classify string,
 * REP and unrecognized forms as Other; bytes is the operand width in bytes.
 * OUT requires a width-sized operand; IN replaces the full cell on success.
 * COM1 keeps all 16450 register effects and refusals. The three absent windows
 * have no state. FALSE leaves device and value unchanged; no operation forwards
 * I/O, advances an instruction, or delivers an interrupt.
 */
static inline BOOLEAN legacy_com_io(Uart16450 *com1, enum LegacyComForm form,
    uint16_t port, unsigned bytes, uint32_t *value)
{
    if (!com1 || !value || bytes != 1 ||
        (form != LegacyComScalarIn && form != LegacyComScalarOut)) return FALSE;
    if (form == LegacyComScalarOut && *value > UINT8_MAX) return FALSE;

    if (legacy_com_absent_port(port)) {
        if (form == LegacyComScalarIn) *value = UINT8_MAX;
        return TRUE;
    } else {
        BOOLEAN input = (form == LegacyComScalarIn);
        UINT8 byte = input ? 0 : (UINT8)*value;
        if (!uart16450_access(com1, port, input, &byte)) return FALSE;
        if (input) *value = byte;
        return TRUE;
    }
}
#endif
