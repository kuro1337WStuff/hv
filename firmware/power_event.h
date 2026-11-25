#ifndef HV_POWER_EVENT_H
#define HV_POWER_EVENT_H

#include "power_request.h" /* fixed port map and scalar I/O tuple, not control state */

/* ACPI 6.5 4.8.3.1.1/.2, tables 4.11/4.12; power_build_tables in
 * power_tables.h. Hardware-full S5 needs the PM1a event block, not PM1b.
 * Implement GBL_STS/GBL_EN (5) as register cells, and sticky WAK_STS (15,
 * no enable). The FACS is present; there is no FADT global-lock-present
 * flag. GBL_EN readback is NOT a FACS lock protocol: no runtime firmware
 * releases a lock, no PM1_CNT.GBL_RLS signal and no SCI delivery here.
 * No S1 is advertised: WAK_STS need never be raised (4.8.2.3).
 *
 * Absent, not reserved: TMR (0; no optional PM_TMR_BLK), BM_STS (4;
 * optional per 8.1.4, no C3), fixed buttons and RTC (8..10; PWR_BUTTON,
 * SLP_BUTTON, FIX_RTC set), PCIEXP_WAKE_STS/DIS (14; PCI_EXP_WAK clear).
 * No BM enable exists. STS[11] is software-ignored; we return zero.
 * STS[3:1,7:6,13:12] and EN[4:1,7:6,13:11,15] are reserved. For all
 * unused positions reads return zero and writes have no effect (4.6/4.8).
 */
enum {
    PowerPm1aEnable = PowerPm1aEvent + 2,
    PowerEventGlobal = 1u << 5,
    PowerEventWake = 1u << 15,
    PowerEventStatusMask = PowerEventGlobal | PowerEventWake
};

typedef struct {
    uint16_t status, enable;
} PowerPm1aEventRegs; /* zero-initialize for fresh ACPI-only G0 entry */

/* Synthetic device-side latch input for host register tests only. Never a
 * guest OUT, a wake transition, a lock release, or an interrupt source. */
static inline int power_event_latch(PowerPm1aEventRegs *pm, uint32_t bits)
{
    if (!pm || (bits & ~(uint32_t)PowerEventStatusMask)) return 0;
    pm->status |= (uint16_t)bits;
    return 1;
}

/* Linux v6.18.60 drivers/acpi/acpica/tbfadt.c: acpi_tb_setup_fadt_registers
 * splits the 32-bit event GAS into aligned 16-bit halves. Only these scalar
 * words are modeled; ACPI also specifies byte accesses, not supported here.
 * The caller classifies direction/form and passes a width-sized OUT value.
 * This decoder owns neither a lifecycle nor the adjacent S5 control word. */
static inline PowerIoResult power_event_io(PowerPm1aEventRegs *pm, enum PowerIoForm form,
    uint16_t port, unsigned bytes, uint32_t *value)
{
    if (!pm || !value || bytes != 2 || (port != PowerPm1aEvent && port != PowerPm1aEnable))
        return PowerIoRejected;
    if (form == PowerScalarIn) {
        *value = port == PowerPm1aEvent ? pm->status : pm->enable;
        return PowerIoHandled;
    }
    if (form != PowerScalarOut || *value > UINT16_MAX) return PowerIoRejected;
    if (port == PowerPm1aEvent)
        pm->status &= (uint16_t)~*value; /* W1C, including when disabled */
    else
        pm->enable = (uint16_t)(*value & PowerEventGlobal);
    return PowerIoHandled;
}
#endif
