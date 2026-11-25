/* From the repository root:
 * Linux: cc -std=c11 -Wall -Wextra -Werror lab/tests/fetch_walk_test.c -o /tmp/fetch_walk_test && /tmp/fetch_walk_test
 * Windows (VS Developer Command Prompt): cl /nologo /std:c11 /W4 /WX lab\tests\fetch_walk_test.c /Fe:fetch_walk_test.exe && fetch_walk_test.exe
 */
#include <stdio.h>
#include "../../firmware/fetch_walk.h"

#define CHECK(test) do { if (!(test)) { \
    fprintf(stderr, "line %d: %s\n", __LINE__, #test); return 1; \
} } while (0)

static uint8_t ram[0x400000];

static void put(size_t slot, uint64_t entry)
{
    for (unsigned i = 0; i < 8; ++i) ram[slot + i] = (uint8_t)(entry >> (8 * i));
}

int main(void)
{
    /* Independent GPA offsets, not host pointers. PML4[256], PDPT[9], PD[10],
     * PT[11..13]: two guest linear pages alias a frame, the third does not. */
    const size_t slots[] = {0x1000 + 256*8, 0x5000 + 9*8, 0x7000 + 10*8, 0x9000 + 11*8};
    const uint64_t entries[] = {0x5001, 0x7001, 0x9001, 0xb081}; /* read-only, PTE PAT */
    const uint64_t va = UINT64_C(0xffff800000000000) + (UINT64_C(9) << 30) + 0x140b000;
    const uint64_t big_va = va - 0xb000 + 0x200000; /* next PDE */
    const size_t big_slot = 0x7000 + 11*8;
    uint64_t a, b, c, gpa;
    for (unsigned i = 0; i < 4; ++i) put(slots[i], entries[i]);
    put(slots[3] + 8, 0xb001);
    put(slots[3] + 16, 0x1a001);
    CHECK(fetch_walk(ram, sizeof ram, 0x1000, va + 0x123, &a) && a == 0xb123);
    CHECK(fetch_walk(ram, sizeof ram, 0x1000, va + 0x1fed, &b) && b == 0xbfed);
    CHECK(fetch_walk(ram, sizeof ram, 0x1000, va + 0x2123, &c) && c == 0x1a123);
    CHECK(fetch_precision(a, 0xb123) == FETCH_EXACT);
    CHECK(fetch_precision(a, 0xb000) == FETCH_PAGE_BASE);
    CHECK(fetch_precision(b, 0xbfed) == FETCH_EXACT);
    CHECK(fetch_precision(b, 0xb000) == FETCH_PAGE_BASE);
    CHECK(fetch_precision(a, b) == FETCH_INVALID);       /* same frame, wrong offset */
    CHECK(fetch_precision(a, 0xc000) == FETCH_INVALID);  /* adjacent page base */
    CHECK(fetch_precision(a, c) == FETCH_INVALID);       /* same offset, another frame */
    CHECK(fetch_precision(a, va + 0x123) == FETCH_INVALID);
    CHECK(fetch_precision(0xb000, 0xb000) == FETCH_INVALID); /* ambiguous */

    put(big_slot, 0x201083); /* 2 MiB base, PS + PAT */
    CHECK(fetch_walk(ram, sizeof ram, 0x1000, big_va + 0x1d5123, &gpa) && gpa == 0x3d5123);
    CHECK(fetch_precision(gpa, 0x3d5000) == FETCH_PAGE_BASE); /* still a 4 KiB comparison */
    CHECK(fetch_precision(gpa, 0x200000) == FETCH_INVALID);
    CHECK(!fetch_walk(ram, sizeof ram - 1, 0x1000, big_va, &gpa)); /* partial large frame */
    put(big_slot, 0x202083); /* bit 13 is reserved, not part of a 2 MiB frame */
    CHECK(!fetch_walk(ram, sizeof ram, 0x1000, big_va, &gpa));

    for (unsigned i = 0; i < 4; ++i) {
        put(slots[i], entries[i] | (UINT64_C(1) << 63)); /* guest NX, not NPT NX */
        CHECK(!fetch_walk(ram, sizeof ram, 0x1000, va, &gpa));
        put(slots[i], entries[i] & ~UINT64_C(1));
        CHECK(!fetch_walk(ram, sizeof ram, 0x1000, va, &gpa));
        put(slots[i], entries[i]);
    }
    for (unsigned i = 0; i < 2; ++i) {
        put(slots[i], entries[i] | 0x80); /* PS reserved at root, 1 GiB not in fixture */
        CHECK(!fetch_walk(ram, sizeof ram, 0x1000, va, &gpa));
        put(slots[i], entries[i]);
    }
    CHECK(!fetch_walk(ram, sizeof ram, 0x1008, va, &gpa)); /* plain CR3 required */
    CHECK(!fetch_walk(ram, sizeof ram, sizeof ram, va, &gpa));
    CHECK(!fetch_walk(ram, sizeof ram, 0x1000, va & UINT64_C(0x0000ffffffffffff), &gpa));
    CHECK(!fetch_walk(ram, 0x9fff, 0x1000, va, &gpa)); /* short final table */
    CHECK(!fetch_walk(ram, 0xbfff, 0x1000, va, &gpa)); /* short final frame */
    put(slots[1], UINT64_C(0x000ffffffffff001)); /* out-of-range next table */
    gpa = UINT64_MAX;
    CHECK(!fetch_walk(ram, sizeof ram, 0x1000, va, &gpa) && gpa == UINT64_MAX);
    puts("PASS: bounded guest fetch walk and exact/page-base GPA comparisons.");
    return 0;
}
