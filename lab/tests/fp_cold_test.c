/* Trial 118: host memory only. No hardware FP state is sampled or restored.
 * cc -std=c11 -O2 -Wall -Wextra -Werror lab/tests/fp_cold_test.c -o /tmp/fp_cold && /tmp/fp_cold
 * cl /nologo /std:c11 /O2 /W4 /WX lab\tests\fp_cold_test.c && fp_cold_test.exe
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t UINT8;
typedef uint16_t UINT16;
typedef uint32_t UINT32;
typedef uint64_t UINT64;
#include "../../firmware/vcpu_fp_image.h"

typedef struct { unsigned char before[16]; FpBank bank; unsigned char after[16]; } GuardedBank;
_Static_assert(sizeof(GuardedBank) == 16 + 512 + 112 + 16, "unexpected control layout");

/* Little-endian FXSAVE64 wire offsets, deliberately not derived from C
 * members. Every unspecified byte must be zero, even empty ST payloads,
 * their slot padding, instruction/data pointers, mask and the 96-byte tail. */
static const unsigned char cold[512] = { [0] = 0x7f, [1] = 0x03, [24] = 0x80, [25] = 0x1f };

static unsigned le16(const unsigned char *p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

static unsigned le32(const unsigned char *p)
{
    return le16(p) | (le16(p + 2) << 16);
}

static int check(unsigned char prior)
{
    GuardedBank fixture;
    unsigned char *raw = (unsigned char *)&fixture;
    const unsigned char *fx = raw + 16;
    unsigned char expected[sizeof fixture];

    /* Vary every neighboring byte so swapping or clearing GPR slots is
     * distinguishable from leaving them alone. Both outside guards are live. */
    for (size_t i = 0; i < sizeof fixture; ++i)
        expected[i] = (unsigned char)(1 + (i + prior) % 255);
    memcpy(raw, expected, sizeof fixture);
    memset(raw + 16, prior, sizeof cold);
    memcpy(expected + 16, cold, sizeof cold);
    fp_cold_image(&fixture.bank.fx);

    for (size_t i = 0; i < sizeof fixture; ++i) {
        if (raw[i] != expected[i]) {
            fprintf(stderr, "prior=%02X frame[%zu]: expected=%02X observed=%02X\n",
                    (unsigned)prior, i, (unsigned)expected[i], (unsigned)raw[i]);
            return 1;
        }
    }
    printf("prior=%02X FCW=%04X FSW=%04X TOP=%u FTW=%02X MXCSR=%08X MASK=%08X; "
           "%zu image bytes matched, 112 GPR + 32 guard bytes unchanged\n",
           (unsigned)prior, le16(fx), le16(fx + 2), (le16(fx + 2) >> 11) & 7u,
           (unsigned)fx[4], le32(fx + 24), le32(fx + 28), sizeof cold);
    return 0;
}

int main(void)
{
    return check(0xa5) || check(0x5a);
}
