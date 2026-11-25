#ifndef HV_FIXED_NPT_H
#define HV_FIXED_NPT_H

#include <stdint.h>

enum { FixedGuestGpa = 0x1000, NptPageBytes = 4096, NptTableCount = 4 };
typedef struct {
    uint64_t table[NptTableCount][512]; /* PML4, PDPT, PD, PT in physical order */
} FixedNpt;

/* Caller supplies four contiguous zeroed pages and aligned physical addresses.
 * The writable host view need not have the same numeric address as root_pa. */
static inline void build_fixed_npt(FixedNpt *npt, uint64_t root_pa, uint64_t code_pa)
{
    for (unsigned level = 0; level < NptTableCount - 1; ++level)
        npt->table[level][0] = (root_pa + (level + 1) * NptPageBytes) | 7; /* P/RW/US */
    npt->table[3][FixedGuestGpa / NptPageBytes] = code_pa | 5; /* P/US, read-only */
}

#endif
