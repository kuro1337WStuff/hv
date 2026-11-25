/* Host-only register tuples. No guest runs and no port instructions. */
#include <stdio.h>
#include "../../firmware/power_event.h"
#include "../../firmware/power_tables.h"

#define CHECK(e) do { if (!(e)) { fprintf(stderr, "line %d: %s\n", __LINE__, #e); return 1; } } while (0)

static int reads(PowerPm1aEventRegs *pm, uint16_t port, uint32_t expected)
{
    uint32_t word = UINT32_MAX, low = UINT32_MAX, high = UINT32_MAX;
    return power_event_io(pm, PowerScalarIn, port, 2, &word) == PowerIoHandled && word == expected &&
        power_event_io(pm, PowerScalarIn, port, 1, &low) == PowerIoHandled && low == (word & 0xff) &&
        power_event_io(pm, PowerScalarIn, port + 1, 1, &high) == PowerIoHandled && high == (word >> 8);
}

static int writes(PowerPm1aEventRegs *pm, uint16_t port, uint32_t word)
{
    uint32_t value = word;
    return power_event_io(pm, PowerScalarOut, port, 2, &value) == PowerIoHandled && value == word;
}

static int write_byte(PowerPm1aEventRegs *pm, uint16_t port, uint32_t byte)
{
    uint32_t value = byte;
    return power_event_io(pm, PowerScalarOut, port, 1, &value) == PowerIoHandled && value == byte;
}

static int io_gas(const uint8_t *gas, unsigned bits, uint16_t port)
{
    return gas[0] == 1 && gas[1] == bits && !gas[2] && !gas[3] && linux_le64(gas + 4) == port;
}

int main(void)
{
    static uint8_t pages[PowerTableBytes];
    CHECK(power_build_tables(pages, sizeof(pages), PowerRsdpGpa));
    const uint8_t *f = pages + PowerFadtOffset;
    CHECK(linux_le32(f + 56) == 0x600 && f[88] == 4 && io_gas(f + 148, 32, 0x600));
    CHECK(linux_le32(f + 64) == 0x604 && f[89] == 2 && io_gas(f + 172, 16, 0x604));
    CHECK(!linux_le32(f + 60) && !linux_le64(f + 164)); /* no PM1b event */
    CHECK(!linux_le32(f + 76) && !linux_le64(f + 212) && !f[91]); /* no PM timer */
    CHECK(linux_le32(f + 112) == 0x70 && linux_le16(f + 98) > 1000); /* flags and C3 latency */
    const uint16_t sts = (uint16_t)linux_le64(f + 152), en = (uint16_t)(sts + f[88] / 2);
    const uint16_t cnt = (uint16_t)linux_le64(f + 176);
    CHECK(en == 0x602);

    PowerPm1aEventRegs dirty = { UINT16_MAX, UINT16_MAX };
    CHECK(reads(&dirty, sts, 0x8020) && reads(&dirty, en, 0x0020)); /* unsupported bits read zero */
    PowerPm1aEventRegs pm = {0};
    CHECK(reads(&pm, sts, 0) && reads(&pm, en, 0));
    /* Independent bit oracle across both words, including ignored STS[11],
     * reserved bits, defined-but-absent features and EN[15] (not WAK_EN).
     * Device inputs OR into STS; an OUT never sets STS, even with EN set. */
    for (unsigned i = 0; i < 16; ++i) {
        uint32_t bit = 1u << i;
        CHECK(writes(&pm, en, bit) && reads(&pm, en, bit & 0x0020));
        CHECK(writes(&pm, sts, bit) && reads(&pm, sts, 0));
        CHECK(power_event_latch(&pm, bit) == (i == 5 || i == 15));
        CHECK(reads(&pm, sts, bit & 0x8020));
        CHECK(power_event_latch(&pm, 0x8020));
        CHECK(writes(&pm, sts, bit) && reads(&pm, sts, 0x8020 & ~bit));
        CHECK(reads(&pm, en, bit & 0x0020));
        CHECK(writes(&pm, sts, 0xffff));
    }

    CHECK(power_event_latch(&pm, 0x8020));
    CHECK(write_byte(&pm, sts, 0x20) && reads(&pm, sts, 0x8000));
    CHECK(power_event_latch(&pm, 0x20));
    CHECK(write_byte(&pm, sts + 1, 0x80) && reads(&pm, sts, 0x0020));
    CHECK(write_byte(&pm, en, 0xff) && reads(&pm, en, 0x20));
    CHECK(write_byte(&pm, en + 1, 0xff) && reads(&pm, en, 0x20));

    CHECK(writes(&pm, en, 0) && power_event_latch(&pm, 0x8000));
    CHECK(power_event_latch(&pm, 0x20) && reads(&pm, sts, 0x8020)); /* disabled still latches */
    CHECK(writes(&pm, en, 0xffff) && writes(&pm, sts, 0));
    CHECK(reads(&pm, sts, 0x8020) && reads(&pm, en, 0x20));

    /* Linux v6.18.60 drivers/acpi/acpica/{evevent,utglobal,hwxface}.c:
     * acpi_ev_fixed_event_initialize disables PMTIMER, GLOBAL, power button,
     * sleep button and RTC by enable-word read/modify/write. evxfevnt.c
     * acpi_enable_event later verifies a requested enable by reading it back. */
    static const uint16_t fixed_en[] = {0x1, 0x20, 0x100, 0x200, 0x400};
    for (size_t i = 0; i < sizeof(fixed_en)/sizeof(fixed_en[0]); ++i) {
        uint32_t value = UINT32_MAX;
        CHECK(power_event_io(&pm, PowerScalarIn, en, 2, &value) == PowerIoHandled);
        CHECK(writes(&pm, en, value & ~(uint32_t)fixed_en[i]));
        CHECK(reads(&pm, en, i == 0 ? 0x20u : 0u) && reads(&pm, sts, 0x8020));
    }
    CHECK(writes(&pm, en, 0x20) && reads(&pm, en, 0x20));
    /* hwsleep.c acpi_hw_legacy_sleep: WAK first; hwregs.c then writes
     * aclocal.h ACPI_BITMASK_ALL_FIXED_STATUS. Neither clear touches EN. */
    CHECK(writes(&pm, sts, 0x8000) && reads(&pm, sts, 0x20) && reads(&pm, en, 0x20));
    CHECK(writes(&pm, sts, 0xc731) && reads(&pm, sts, 0) && reads(&pm, en, 0x20));

    /* Nonzero state and I/O sentinels make partial commits visible. */
    CHECK(power_event_latch(&pm, 0x8020));
    static const struct { enum PowerIoForm form; uint16_t port; unsigned bytes; uint32_t value; } bad[] = {
        {PowerScalarIn, 0x5ff, 2, 0xabcd}, {PowerScalarOut, 0x601, 2, 0x8020},
        {PowerScalarIn, 0x603, 2, 0xabcd}, {PowerScalarOut, 0x604, 2, 0x3401},
        {PowerScalarOut, 0x604, 1, 0x20}, {PowerScalarIn, 0x5ff, 1, 0xabcd},
        {PowerScalarOut, 0x601, 1, 0x180}, {PowerScalarOut, 0x602, 1, 0x100},
        {PowerScalarOut, 0x600, 0, 0x20}, {PowerScalarIn, 0x603, 3, 0xabcd},
        {PowerScalarOut, 0x600, 257, 0x20}, {PowerOtherIo, 0x602, 1, 0},
        {PowerScalarOut, 0x600, 4, 0x8020}, {PowerScalarOut, 0x602, 4, 0},
        {PowerOtherIo, 0x600, 2, 0x8020}, {PowerOtherIo, 0x602, 2, 0},
        {PowerScalarOut, 0x600, 2, 0x18020}, {PowerScalarOut, 0x602, 2, 0x10000}
    };
    for (size_t i = 0; i < sizeof(bad)/sizeof(bad[0]); ++i) {
        uint32_t value = bad[i].value;
        CHECK(power_event_io(&pm, bad[i].form, bad[i].port, bad[i].bytes, &value) == PowerIoRejected);
        CHECK(value == bad[i].value && reads(&pm, sts, 0x8020) && reads(&pm, en, 0x20));
    }
    uint32_t value = 0x3401;
    CHECK(!power_event_latch(NULL, 0x20));
    CHECK(power_event_io(NULL, PowerScalarIn, sts, 2, &value) == PowerIoRejected && value == 0x3401);
    CHECK(power_event_io(&pm, PowerScalarOut, en, 2, NULL) == PowerIoRejected);
    CHECK(reads(&pm, en, 0x20) && writes(&pm, sts, 0x20));
    CHECK(!power_event_latch(&pm, 0x10020) && !power_event_latch(&pm, 0x21));
    CHECK(reads(&pm, sts, 0x8000)); /* reject even the supported portion */

    /* Same S5 payload, three addresses: only OUTW to control may stop. */
    PowerPm1 control = {0};
    CHECK(power_pm1_io(&control, PowerScalarOut, sts, 2, &value) == PowerIoRejected);
    CHECK(power_pm1_io(&control, PowerScalarOut, en, 2, &value) == PowerIoRejected);
    CHECK(writes(&pm, sts, value) && writes(&pm, en, value));
    CHECK(control.lifecycle == PowerRunning && !control.sleep_type_bits);
    value = 0x1401;
    CHECK(power_pm1_io(&control, PowerScalarOut, cnt, 2, &value) == PowerIoHandled);
    value = 0x3401;
    CHECK(power_event_io(&pm, PowerScalarOut, cnt, 2, &value) == PowerIoRejected);
    CHECK(power_pm1_io(&control, PowerScalarOut, cnt, 2, &value) == PowerIoShutdown);
    CHECK(control.lifecycle == PowerStopped && reads(&pm, sts, 0x8000) && reads(&pm, en, 0));
    puts("PM1a event: 0600/0601 W1C leaves 8000/0020; 0602/0603 EN 0020 retained; only 0604 stops");
    return 0;
}
