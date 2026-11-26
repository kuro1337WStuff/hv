#ifndef HV_COM_PM1_SCALAR_IO_H
#define HV_COM_PM1_SCALAR_IO_H

#include "legacy_com.h"
#include "power_event.h"

/** One zero-initialized virtual device set per guest; PM1 control owns its lifecycle. */
typedef struct {
    Uart16450 com1;
    PowerPm1aEventRegs event;
    PowerPm1 control;
} ComPm1Devices;

/** Route one decoded transaction; port is a guest I/O address and bytes is the
 * operand width in octets. The caller owns a separate value cell. Only a
 * successful IN replaces it. Refusal preserves devices and value; shutdown
 * is terminal. This model neither forwards hardware I/O nor resumes a vCPU. */
static inline PowerIoResult com_pm1_scalar_io(ComPm1Devices *devices, enum PowerIoForm form,
    uint16_t port, unsigned bytes, uint32_t *value)
{
    if (!devices || devices->control.lifecycle != PowerRunning || !value) return PowerIoRejected;
    if (port >= PowerPm1aEvent && port <= PowerPm1aEnable + 1)
        return power_event_io(&devices->event, form, port, bytes, value);
    if (port >= PowerPm1aControl && port <= PowerPm1aControl + 1)
        return power_pm1_io(&devices->control, form, port, bytes, value);

    /* Only serial candidates remain. Its decoder owns all four windows and
     * rejects everything outside them; no other owner retries a refusal. */
    enum LegacyComForm serial = form == PowerScalarIn ? LegacyComScalarIn :
        form == PowerScalarOut ? LegacyComScalarOut : LegacyComOther;
    return legacy_com_io(&devices->com1, serial, port, bytes, value) ? PowerIoHandled : PowerIoRejected;
}
#endif
