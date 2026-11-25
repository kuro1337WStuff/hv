#ifndef HV_POWER_REQUEST_H
#define HV_POWER_REQUEST_H

#include <stdint.h>

/* Fixed L2 platform values, not host port assignments. No port access here. */
enum {
    PowerPm1aEvent = 0x600, PowerPm1aControl = 0x604,
    PowerSciEnable = 1, PowerS5Type = 5, PowerSleepEnable = 1 << 13,
    PowerS5Select = (PowerS5Type << 10) | PowerSciEnable,
    PowerS5Request = PowerS5Select | PowerSleepEnable
};

enum PowerLifecycle { PowerRunning, PowerWaitingForIrq, PowerStopped, PowerFailed };
enum PowerIoForm { PowerOtherIo, PowerScalarIn, PowerScalarOut };
typedef enum { PowerIoRejected, PowerIoHandled, PowerIoShutdown } PowerIoResult;
typedef struct {
    enum PowerLifecycle lifecycle;
    uint16_t sleep_type_bits;
} PowerPm1;

/* Only the PM1a control request subset, not a PM1 event block, SCI or PIC.
 * The caller must classify a real scalar I/O exit (not a string or REP),
 * pass the operand sized to its width, and keep this state per fresh L2.
 * Reads return SCI_EN=1 (ACPI-only machine), SLP_EN reads zero. A terminal
 * result occurs ONCE. The accepted OUT is never forwarded or resumed. */
static inline PowerIoResult power_pm1_io(PowerPm1 *pm, enum PowerIoForm form,
    uint16_t port, unsigned bytes, uint32_t *value)
{
    if (!pm || !value || pm->lifecycle != PowerRunning ||
        port != PowerPm1aControl || bytes != 2)
        return PowerIoRejected;
    if (form == PowerScalarIn) {
        *value = pm->sleep_type_bits | PowerSciEnable;
        return PowerIoHandled;
    }
    if (form != PowerScalarOut) return PowerIoRejected;
    if (*value == PowerS5Request) {
        pm->lifecycle = PowerStopped;
        return PowerIoShutdown;
    }
    if (*value != PowerSciEnable && *value != PowerS5Select) return PowerIoRejected;
    pm->sleep_type_bits = (uint16_t)(*value & (7u << 10));
    return PowerIoHandled;
}
#endif
