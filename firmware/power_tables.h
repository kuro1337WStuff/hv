#ifndef HV_POWER_TABLES_H
#define HV_POWER_TABLES_H

#include "linux_layout.h" /* freestanding little-endian guest byte helpers */
#include "power_request.h"

enum {
    PowerRsdpGpa = 0x90000, PowerFacsGpa = 0x91000,
    PowerXsdtOffset = 0x40, PowerFadtOffset = 0x80, PowerDsdtOffset = 0x200,
    PowerTableBytes = 2 * LinuxPage, PowerFadtLength = 276
};
_Static_assert(PowerFacsGpa == PowerRsdpGpa + LinuxPage &&
    (unsigned)PowerRsdpGpa >= LinuxStackTop && PowerRsdpGpa + PowerTableBytes <= LinuxHoleStart,
    "fixed ACPI bytes must fit below the hole outside the existing low boot objects");

static inline void power_checksum(uint8_t *p, size_t n, size_t byte)
{
    uint8_t sum = 0;
    for (size_t i = 0; i < n; ++i) sum = (uint8_t)(sum + p[i]);
    p[byte] = (uint8_t)(0u - sum); /* byte was zero on entry */
}

static inline void power_sdt_header(uint8_t *p, const char *sig, uint32_t length, uint8_t rev)
{
    linux_copy(p, (const uint8_t *)sig, 4);
    linux_put32(p + 4, length);
    p[8] = rev;
    linux_copy(p + 10, (const uint8_t *)"HVLAB ", 6);
    linux_copy(p + 16, (const uint8_t *)"HVL2PC  ", 8);
    linux_put32(p + 24, 1);
    linux_copy(p + 28, (const uint8_t *)"HVLB", 4);
    linux_put32(p + 32, 1);
}

static inline void power_io_gas(uint8_t *p, unsigned bits, uint32_t port)
{
    p[0] = 1; /* System I/O */
    p[1] = (uint8_t)bits;
    /* Bit offset zero. AccessSize zero: ACPICA splits the PM1 event block
     * into two half-width registers and accesses control as a word. */
    linux_put64(p + 4, port);
}

/* Two contiguous, exclusively L2-owned pages, not an arbitrary RAM pointer.
 * The caller must reserve the first page as firmware data and the second as
 * ACPI NVS, then publish the RSDP GPA in boot_params. No host ACPI copying,
 * E820 mutation, guest execution, I/O, allocation or privileged operation. */
static inline int power_build_tables(uint8_t *pages, size_t available, uint64_t view_gpa)
{
    /* Root-scope Name (_S5_, Package (4) {5, 5, Zero, Zero}). PkgLength
     * includes itself and NumElements but not the PackageOp. No AML methods. */
    static const uint8_t s5[] = {
        0x08, '_', 'S', '5', '_', 0x12, 0x08, 0x04,
        0x0a, PowerS5Type, 0x0a, PowerS5Type, 0x00, 0x00
    };
    if (!pages || available < PowerTableBytes || view_gpa != PowerRsdpGpa) return 0;
    linux_clear(pages, PowerTableBytes);
    uint8_t *xsdt = pages + PowerXsdtOffset, *fadt = pages + PowerFadtOffset;
    uint8_t *dsdt = pages + PowerDsdtOffset, *facs = pages + LinuxPage;

    linux_copy(pages, (const uint8_t *)"RSD PTR ", 8);
    linux_copy(pages + 9, (const uint8_t *)"HVLAB ", 6);
    pages[15] = 2;
    linux_put32(pages + 20, 36);
    linux_put64(pages + 24, PowerRsdpGpa + PowerXsdtOffset);
    power_checksum(pages, 20, 8);
    power_checksum(pages, 36, 32);

    power_sdt_header(xsdt, "XSDT", 44, 1);
    linux_put64(xsdt + 36, PowerRsdpGpa + PowerFadtOffset);
    power_checksum(xsdt, 44, 9);

    power_sdt_header(fadt, "FACP", PowerFadtLength, 6);
    linux_put32(fadt + 36, PowerFacsGpa);
    linux_put32(fadt + 40, PowerRsdpGpa + PowerDsdtOffset);
    fadt[46] = 9; /* SCI, on guest PIC IRQ9. No SMI_CMD: always in ACPI mode. */
    linux_put32(fadt + 56, PowerPm1aEvent);
    linux_put32(fadt + 64, PowerPm1aControl);
    fadt[88] = 4; fadt[89] = 2;
    linux_put32(fadt + 96, UINT32_MAX); /* C2/C3 not supported (two u16 latencies) */
    fadt[109] = 1; /* IA-PC legacy devices; no 8042 or MSI promise */
    linux_put32(fadt + 112, (1u << 4) | (1u << 5) | (1u << 6)); /* no fixed power/sleep buttons or RTC wake */
    fadt[131] = 5; /* ACPI 6.5 */
    linux_put64(fadt + 132, PowerFacsGpa);
    linux_put64(fadt + 140, PowerRsdpGpa + PowerDsdtOffset);
    power_io_gas(fadt + 148, 32, PowerPm1aEvent);
    power_io_gas(fadt + 172, 16, PowerPm1aControl);
    power_checksum(fadt, PowerFadtLength, 9);

    power_sdt_header(dsdt, "DSDT", 36 + (uint32_t)sizeof(s5), 2);
    linux_copy(dsdt + 36, s5, sizeof(s5));
    power_checksum(dsdt, 36 + sizeof(s5), 9);

    /* FACS is 64-byte aligned, writable, and has NO SDT header/checksum. */
    linux_copy(facs, (const uint8_t *)"FACS", 4);
    linux_put32(facs + 4, 64);
    facs[32] = 2;
    return 1;
}
#endif
