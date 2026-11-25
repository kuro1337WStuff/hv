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

/** Read-only check of the exact fixed e820 profile and ACPI 6.5 table graph.
 * All reads use fixed GPA offsets: malformed pointers and lengths are compared,
 * never followed. Checks signatures, both RSDP checksums and every SDT checksum;
 * FACS uses the ACPI non-SDT format. Does not interpret AML or execute Linux. */
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
