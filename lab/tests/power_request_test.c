/* Host-only C11 test; no VM, host I/O or power action. */
#include <stdio.h>
#include "../../firmware/power_request.h"

#define CHECK(e) do { if (!(e)) { fprintf(stderr, "line %d: %s\n", __LINE__, #e); return 1; } } while (0)

int main(void)
{
    PowerPm1 pm = {0};
    uint32_t value = 0;
    CHECK(power_pm1_io(&pm, PowerScalarIn, 0x604, 2, &value) == PowerIoHandled && value == 1);
    /* Literal tuples are the external oracle, not values copied from decoder enums. */
    static const struct { enum PowerIoForm form; uint16_t port; unsigned bytes; uint32_t value; } bad[] = {
        {PowerScalarOut, 0x605, 2, 0x3401}, {PowerScalarOut, 0x604, 1, 0x3401},
        {PowerScalarOut, 0x604, 4, 0x3401}, {PowerScalarOut, 0x604, 2, 0x3001},
        {PowerScalarOut, 0x604, 2, 0x3403}, {PowerOtherIo, 0x604, 2, 0x3401},
        {PowerScalarOut, 0x604, 2, 0x13401}, {PowerScalarOut, 0x600, 2, 0x3401}
    };
    for (size_t i = 0; i < sizeof(bad)/sizeof(bad[0]); ++i) {
        value = bad[i].value;
        CHECK(power_pm1_io(&pm, bad[i].form, bad[i].port, bad[i].bytes, &value) == PowerIoRejected);
        CHECK(value == bad[i].value && pm.lifecycle == PowerRunning && !pm.sleep_type_bits);
    }
    value = 0x1401;
    CHECK(power_pm1_io(&pm, PowerScalarOut, 0x604, 2, &value) == PowerIoHandled);
    CHECK(pm.lifecycle == PowerRunning);
    value = 0x3401; /* direction matters: an IN is not a request */
    CHECK(power_pm1_io(&pm, PowerScalarIn, 0x604, 2, &value) == PowerIoHandled);
    CHECK(pm.lifecycle == PowerRunning && value == 0x1401);
    value = 0x3401;
    CHECK(power_pm1_io(&pm, PowerScalarOut, 0x604, 2, &value) == PowerIoShutdown);
    CHECK(pm.lifecycle == PowerStopped);
    CHECK(power_pm1_io(&pm, PowerScalarOut, 0x604, 2, &value) == PowerIoRejected && pm.lifecycle == PowerStopped);
    pm.lifecycle = PowerWaitingForIrq;
    CHECK(power_pm1_io(&pm, PowerScalarOut, 0x604, 2, &value) == PowerIoRejected && pm.lifecycle == PowerWaitingForIrq);
    pm.lifecycle = PowerFailed;
    CHECK(power_pm1_io(&pm, PowerScalarOut, 0x604, 2, &value) == PowerIoRejected && pm.lifecycle == PowerFailed);
    puts("power request: exact S5, nonterminal select, malformed/duplicate/waiting/failed refused");
    return 0;
}
