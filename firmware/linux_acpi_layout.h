/** @file linux_acpi_layout.h Opt-in host publication for the fixed Linux guest. */
#ifndef HV_LINUX_ACPI_LAYOUT_H
#define HV_LINUX_ACPI_LAYOUT_H
#include "power_tables.h"

/** Linux x86 boot protocol: boot_params offsets and packed e820 entry stride. */
enum { AcpiRootField = 0x70, AcpiMapCount = 0x1e8, AcpiMap = 0x2d0, AcpiRow = 20 };

/** Half-open GPA boundaries: RAM, ACPI reclaim, NVS, RAM, PC hole, RAM. */
static const uint32_t linux_acpi_edges[] = {0, PowerRsdpGpa, PowerFacsGpa,
    PowerFacsGpa + LinuxPage, LinuxHoleStart, LinuxHighRam, LinuxRamBytes};
static const uint8_t linux_acpi_types[] = {1, 3, 4, 1, 2, 1};

/** Compare all 64 address/length bits; never truncate untrusted guest fields. */
static inline int linux_acpi_row(const uint8_t *p, uint32_t lo, uint32_t hi, uint32_t type)
{
    return linux_le64(p) == lo && linux_le64(p + 8) == hi - lo && linux_le32(p + 16) == type;
}

/** Publish only into an exclusively owned, pre-execution build_linux_layout result.
 * ram is the GPA-zero host view, bytes its exact byte size. Refuses a noncanonical
 * active map, setup_data extension, existing RSDP or occupied destination bytes
 * without writes. Success changes only the two ACPI pages and boot_params fields;
 * no allocation, host firmware access or page-table changes. */
static inline int linux_acpi_publish(uint8_t *ram, size_t bytes)
{
    if (!ram || bytes != LinuxRamBytes) return 0;
    uint8_t *bp = ram + LinuxParams, *map = bp + AcpiMap;
    if (bp[AcpiMapCount] != 3 || linux_le64(bp + AcpiRootField) || linux_le64(bp + 0x250) ||
        !linux_acpi_row(map, 0, LinuxHoleStart, 1) ||
        !linux_acpi_row(map + AcpiRow, LinuxHoleStart, LinuxHighRam, 2) ||
        !linux_acpi_row(map + 2 * AcpiRow, LinuxHighRam, LinuxRamBytes, 1)) return 0;
    for (unsigned i = 3 * AcpiRow; i < 6 * AcpiRow; ++i) if (map[i]) return 0;
    for (unsigned i = 0; i < PowerTableBytes; ++i) if (ram[PowerRsdpGpa + i]) return 0;
    if (!power_build_tables(ram + PowerRsdpGpa, PowerTableBytes, PowerRsdpGpa)) return 0;
    for (unsigned i = 0; i < 6; ++i)
        linux_e820(map + i * AcpiRow, linux_acpi_edges[i], linux_acpi_edges[i+1] - linux_acpi_edges[i], linux_acpi_types[i]);
    linux_put64(bp + AcpiRootField, PowerRsdpGpa);
    bp[AcpiMapCount] = 6;
    return 1;
}

/* System I/O GAS, zero bit offset, unspecified access size. The event block
 * is split into status and enable words; the control block is one word. */
static inline int linux_acpi_pm_gas(const uint8_t *p, unsigned bits, unsigned port)
{
    return p[0] == 1 && p[1] == bits && !p[2] && !p[3] && linux_le64(p + 4) == port;
}

static inline int linux_acpi_zero(const uint8_t *p, unsigned n)
{
    for (unsigned i = 0; i < n; ++i) if (p[i]) return 0;
    return 1;
}

static inline int linux_acpi_power_fields(const uint8_t *f, const uint8_t *d)
{
    /* IRQ9 via the guest PIC, always in ACPI mode. Hardware-full PM1a only;
     * do not advertise PM1b, PM2, a PM timer, GPEs, reset or sleep GAS. */
    if (linux_le16(f+46) != 9 || !linux_acpi_zero(f+48, 8) ||
        linux_le32(f+56) != PowerPm1aEvent || linux_le32(f+60) ||
        linux_le32(f+64) != PowerPm1aControl || !linux_acpi_zero(f+68, 20) ||
        f[88] != 4 || f[89] != 2 || !linux_acpi_zero(f+90, 6) ||
        linux_le16(f+109) != 1 || linux_le32(f+112) != ((1u<<4) | (1u<<5) | (1u<<6)) ||
        !linux_acpi_zero(f+116, 13) || !linux_acpi_pm_gas(f+148, 32, PowerPm1aEvent) ||
        !linux_acpi_zero(f+160, 12) || !linux_acpi_pm_gas(f+172, 16, PowerPm1aControl) ||
        !linux_acpi_zero(f+184, 84)) return 0;

    /* Entire AML payload of this image: Name (_S5_, Package (4) {5,5,0,0}).
     * This is an image contract, not an AML parser. */
    static const uint8_t aml[] = {0x08, '_', 'S', '5', '_', 0x12, 8, 4,
        0x0a, PowerS5Type, 0x0a, PowerS5Type, 0, 0};
    for (unsigned i = 0; i < sizeof aml; ++i) if (d[36+i] != aml[i]) return 0;
    return 1;
}

/** Read-only check of the exact fixed e820 profile and ACPI 6.5 table graph.
 * All reads use fixed GPA offsets: malformed pointers and lengths are compared,
 * never followed. Checks signatures, both RSDP checksums and every SDT checksum;
 * FACS is writable NVS: check its header, not its mutable state. */
static inline int linux_acpi_valid(const uint8_t *ram, size_t bytes)
{
    if (!ram || bytes != LinuxRamBytes) return 0;
    const uint8_t *bp = ram + LinuxParams, *r = ram + PowerRsdpGpa;
    if (bp[AcpiMapCount] != 6 || linux_le64(bp + AcpiRootField) != PowerRsdpGpa || linux_le64(bp + 0x250)) return 0;
    for (unsigned i = 0; i < 6; ++i)
        if (!linux_acpi_row(bp + AcpiMap + i * AcpiRow, linux_acpi_edges[i], linux_acpi_edges[i+1], linux_acpi_types[i])) return 0;
    const uint8_t *x = r + PowerXsdtOffset, *f = r + PowerFadtOffset, *d = r + PowerDsdtOffset, *n = r + LinuxPage;
    if (r[15] != 2 || linux_le32(r+16) || linux_le32(r+20) != 36 || linux_le64(r+24) != PowerRsdpGpa + PowerXsdtOffset ||
        linux_le64(x+36) != PowerRsdpGpa + PowerFadtOffset || linux_le32(f+36) != PowerFacsGpa || linux_le64(f+132) != PowerFacsGpa ||
        linux_le32(f+40) != PowerRsdpGpa + PowerDsdtOffset || linux_le64(f+140) != PowerRsdpGpa + PowerDsdtOffset || linux_le32(n+4) != 64) return 0;
    for (unsigned i = 0; i < 8; ++i) if (r[i] != "RSD PTR "[i]) return 0;
    for (unsigned i = 0; i < 4; ++i)
        if (x[i] != "XSDT"[i] || f[i] != "FACP"[i] || d[i] != "DSDT"[i] || n[i] != "FACS"[i]) return 0;
    if (linux_le32(x+4) != 44 || linux_le32(f+4) != PowerFadtLength || linux_le32(d+4) != 50) return 0;
    if (x[8] != 1 || f[8] != 6 || f[131] != 5 || d[8] != 2 || n[32] != 2 ||
        !linux_acpi_power_fields(f, d)) return 0;
    const uint8_t *tables[] = {r, r, x, f, d};
    const unsigned lengths[] = {20, 36, 44, PowerFadtLength, 50};
    for (unsigned i = 0; i < 5; ++i) {
        uint8_t sum = 0;
        for (unsigned j = 0; j < lengths[i]; ++j) sum = (uint8_t)(sum + tables[i][j]);
        if (sum) return 0;
    }
    return 1;
}
#endif
