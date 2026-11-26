/* Host-only mixed scalar tuples; no guest execution or physical I/O. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint8_t UINT8, BOOLEAN;
typedef uint16_t UINT16;
enum { FALSE = 0, TRUE = 1 };
#include "../../firmware/com_pm1_scalar_io.h"

#define CHECK(e) do { if (!(e)) { fprintf(stderr, "line %d: %s\n", __LINE__, #e); exit(EXIT_FAILURE); } } while (0)

static void refused(ComPm1Devices *d, enum PowerIoForm form, uint16_t port, unsigned bytes, uint32_t operand)
{
    unsigned char before[sizeof *d];
    memcpy(before, d, sizeof before);
    uint32_t value = operand;
    CHECK(com_pm1_scalar_io(d, form, port, bytes, &value) == PowerIoRejected);
    CHECK(value == operand && !memcmp(before, d, sizeof before));
}

static void out(ComPm1Devices *d, uint16_t port, unsigned bytes, uint32_t operand, PowerIoResult result)
{
    uint32_t value = operand;
    CHECK(com_pm1_scalar_io(d, PowerScalarOut, port, bytes, &value) == result && value == operand);
}

static void in(ComPm1Devices *d, uint16_t port, unsigned bytes, uint32_t expected)
{
    uint32_t value = UINT32_MAX;
    CHECK(com_pm1_scalar_io(d, PowerScalarIn, port, bytes, &value) == PowerIoHandled && value == expected);
}

int main(void)
{
    ComPm1Devices d = {0};
    static const uint16_t absent[] = {0x2f8, 0x3e8, 0x2e8};
    out(&d, 0x3f8, 1, 'A', PowerIoHandled);
    for (size_t i = 0; i < sizeof absent / sizeof absent[0]; ++i) {
        out(&d, absent[i], 1, 'X', PowerIoHandled);
        in(&d, (uint16_t)(absent[i] + 7), 1, 0xff);
    }
    refused(&d, PowerScalarIn, 0x3fd, 2, UINT32_MAX);
    refused(&d, PowerScalarOut, 0x3fd, 1, 0);
    refused(&d, PowerOtherIo, 0x3f8, 1, 'C');
    refused(&d, PowerScalarIn, 0x601, 2, UINT32_MAX);
    refused(&d, PowerOtherIo, 0x602, 2, 0x20);
    refused(&d, PowerScalarOut, 0x604, 1, 0x3401);
    refused(&d, PowerOtherIo, 0x604, 2, 0x3401);
    refused(&d, PowerScalarIn, 0x605, 1, UINT32_MAX);
    refused(&d, PowerScalarOut, 0x80, 1, 0x20);

    CHECK(power_event_latch(&d.event, 0x8020));
    out(&d, 0x601, 1, 0x80, PowerIoHandled);
    in(&d, 0x600, 2, 0x20);
    out(&d, 0x602, 2, 0xffff, PowerIoHandled);
    in(&d, 0x602, 2, 0x20);
    in(&d, 0x604, 2, 1);
    out(&d, 0x604, 2, 0x1401, PowerIoHandled);
    in(&d, 0x604, 2, 0x1401);
    in(&d, 0x3fd, 1, 0);
    out(&d, 0x3f8, 1, 'B', PowerIoHandled);
    out(&d, 0x604, 2, 0x3401, PowerIoShutdown);
    CHECK(d.control.lifecycle == PowerStopped && d.control.sleep_type_bits == 0x1400);
    CHECK(d.event.status == 0x20 && d.event.enable == 0x20);
    CHECK(d.com1.base.count == 2 && !memcmp(d.com1.base.tx, "AB", 2));

    refused(&d, PowerScalarIn, 0x3fd, 1, UINT32_MAX); /* busy read would advance TX */
    refused(&d, PowerScalarOut, 0x3f8, 1, 'C');
    refused(&d, PowerScalarOut, 0x3ff, 1, 0xa5);
    for (size_t i = 0; i < sizeof absent / sizeof absent[0]; ++i)
        refused(&d, PowerScalarIn, absent[i], 1, UINT32_MAX);
    refused(&d, PowerScalarOut, 0x600, 2, 0x20);
    refused(&d, PowerScalarOut, 0x602, 2, 0);
    refused(&d, PowerScalarIn, 0x602, 2, UINT32_MAX);
    refused(&d, PowerScalarIn, 0x604, 2, UINT32_MAX);
    refused(&d, PowerScalarOut, 0x604, 2, 0x3401);
    refused(&d, PowerScalarIn, 0x80, 1, UINT32_MAX);
    puts("COM/PM1 scalar I/O: mixed traffic preserved, first S5 stops all ports");
    return 0;
}
