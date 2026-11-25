/* From the repository root:
 * Linux: cc -std=c11 -Wall -Wextra -Werror lab/tests/fixed_npt_test.c -o /tmp/fixed_npt_test && /tmp/fixed_npt_test
 * Windows (VS Developer Command Prompt): cl /nologo /std:c11 /W4 /WX lab\tests\fixed_npt_test.c /Fe:fixed_npt_test.exe && fixed_npt_test.exe
 */
#include <stdio.h>
#include "../../firmware/fixed_npt.h"

int main(void)
{
    static FixedNpt pages; /* models the firmware's zeroed allocation */
    const uint64_t root = UINT64_C(0x1fffff000), code = UINT64_C(0x321abcdef000);
    const uint64_t gpa = 0x1000, frame_mask = UINT64_C(0x000ffffffffff000);
    const unsigned shifts[] = {39, 30, 21, 12};
    uint64_t pa = root;

    _Static_assert(sizeof(pages) == 4 * 4096, "expected four 4 KiB tables");
    build_fixed_npt(&pages, root, code);
    for (unsigned level = 0; level < 4; ++level) {
        /* Translate simulated PA to a host array index; never cast PA to a pointer. */
        const uint64_t *table = pages.table[(pa - root) / 4096];
        const unsigned index = (unsigned)((gpa >> shifts[level]) & 511);
        for (unsigned i = 0; i < 512; ++i) {
            if (i != index && table[i] != 0) {
                fprintf(stderr, "unexpected entry at level %u index %u\n", level, i);
                return 1;
            }
        }
        const uint64_t entry = table[index];
        const uint64_t flags = level == 3 ? 5 : 7; /* P+US; RW only on ancestors */
        pa = entry & frame_mask;
        if ((entry & ~frame_mask) != flags ||
            pa != (level == 3 ? code : root + (level + 1) * 4096)) {
            fprintf(stderr, "wrong frame or permissions at level %u index %u\n", level, index);
            return 1;
        }
    }
    puts("PASS: GPA 0x1000 maps only the read-only code frame; all other entries are zero.");
    return 0;
}
