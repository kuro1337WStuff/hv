#ifndef HV_FETCH_WALK_H
#define HV_FETCH_WALK_H

#include <stddef.h>
#include <stdint.h>

/* Fixed lab model: four-level paging, supervisor fetch, no SMEP or PCID.
 * va is a guest linear address. cr3 and all table/frame bases are guest
 * physical byte offsets in ram[0..size); none is a host pointer or NPT entry.
 * Require complete in-bounds tables and leaves. Only 4 KiB and 2 MiB leaves
 * are in scope; the fixtures use no high entry bits or CR3 flags. */
static inline int fetch_walk(const uint8_t *ram, size_t size, uint64_t cr3,
                             uint64_t va, uint64_t *gpa)
{
    const uint64_t frame = UINT64_C(0x000ffffffffff000);
    uint64_t table = cr3;
    if (!ram || !gpa || size < 4096 || (cr3 & ~frame) ||
        (va >> 47 != 0 && va >> 47 != UINT64_C(0x1ffff))) return 0;

    for (unsigned shift = 39; ; shift -= 9) {
        if (table > size - 4096) return 0;
        const uint8_t *slot = ram + (size_t)table + ((va >> shift) & 511) * 8;
        uint64_t entry = 0;
        for (unsigned i = 0; i < 8; ++i) entry |= (uint64_t)slot[i] << (8 * i);
        /* Bit 63 is NX at every level. RW and U/S do not restrict this fetch. */
        if (!(entry & 1) || (entry >> 52)) return 0;
        table = entry & frame;
        if (shift > 21 && (entry & 0x80)) return 0;
        if (shift == 12 || (shift == 21 && (entry & 0x80))) {
            const uint64_t bytes = UINT64_C(1) << shift;
            if (shift == 21) {
                /* Bits 20:13 are reserved, bit 12 is PAT, not an address bit. */
                if (entry & UINT64_C(0x1fe000)) return 0;
                table &= ~(bytes - 1);
            }
            if (size < bytes || table > size - bytes) return 0;
            *gpa = table + (va & (bytes - 1));
            return 1;
        }
    }
}

enum { FETCH_INVALID, FETCH_EXACT, FETCH_PAGE_BASE };

/* Both arguments are GPAs. raw is the unmodified EXITINFO2; compare it,
 * never round it. An aligned walked address cannot distinguish the two cases. */
static inline int fetch_precision(uint64_t walked, uint64_t raw)
{
    const uint64_t offset = walked & UINT64_C(0xfff);
    if (!offset) return FETCH_INVALID;
    if (raw == walked) return FETCH_EXACT;
    return raw == walked - offset ? FETCH_PAGE_BASE : FETCH_INVALID;
}

#endif
