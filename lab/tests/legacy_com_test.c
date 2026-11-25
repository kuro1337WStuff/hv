/* Host only: cl /nologo /std:c11 /W4 /WX /TC lab\tests\legacy_com_test.c */
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint8_t UINT8, BOOLEAN;
typedef uint16_t UINT16;
enum { FALSE = 0, TRUE = 1 };
#include "../../firmware/legacy_com.h"

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "line %d: %s\n", __LINE__, #expr); exit(EXIT_FAILURE); \
} } while (0)

static const uint16_t bases[] = { 0x3f8, 0x2f8, 0x3e8, 0x2e8 };
#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

static uint32_t read8(Uart16450 *u, uint16_t port)
{
    uint32_t value = UINT32_MAX;
    CHECK(legacy_com_io(u, LegacyComScalarIn, port, 1, &value));
    return value;
}

static void write8(Uart16450 *u, uint16_t port, uint32_t byte)
{
    uint32_t value = byte;
    CHECK(legacy_com_io(u, LegacyComScalarOut, port, 1, &value) && value == byte);
}

static void refused(Uart16450 *u, enum LegacyComForm form, uint16_t port,
    unsigned bytes, uint32_t initial)
{
    unsigned char before[sizeof(*u)];
    uint32_t value = initial;
    memcpy(before, u, sizeof before);
    CHECK(!legacy_com_io(u, form, port, bytes, &value));
    CHECK(value == initial && memcmp(before, u, sizeof before) == 0);
}

/* https://github.com/gregkh/linux/blob/v6.18.60/drivers/tty/serial/8250/8250_port.c#L1093-L1132
 * x86-64 does not issue the i386-only writes to 0x80. No extra register
 * accesses between saving and restoring IER; classification is caller policy. */
static void replay(Uart16450 *u, uint16_t base, int present)
{
    uint16_t ier = (uint16_t)(base + 1);
    uint32_t saved = read8(u, ier);
    write8(u, ier, 0);
    uint32_t zero = read8(u, ier) & 0x0fu;
    write8(u, ier, 0x0f);
    uint32_t ones = read8(u, ier) & 0x0fu;
    write8(u, ier, saved);
    CHECK(saved == (present ? 0x06u : 0xffu));
    CHECK(zero == (present ? 0u : 0x0fu) && ones == 0x0fu);
    int detected = zero == 0 && ones == 0x0f;
    CHECK(detected == present);
    CHECK(read8(u, ier) == saved);
    printf("base=%03x saved=%02x masked=%x/%x %s\n", (unsigned)base,
        (unsigned)saved, (unsigned)zero, (unsigned)ones, detected ? "present" : "absent");
}

static void probe_and_isolate(void)
{
    Uart16450 u = {0};
    write8(&u, 0x3fb, 3); /* DLAB clear */
    write8(&u, 0x3fc, 8); /* OUT2, not LOOP */
    write8(&u, 0x3f9, 6);
    write8(&u, 0x3f8, 'T');
    CHECK(read8(&u, 0x3fd) == 0 && u.base.polls_busy == 1);
    CHECK(read8(&u, 0x3fa) == 2 && !uart16450_irq4(&u)); /* acknowledge THRE */
    replay(&u, bases[0], 1);
    /* The register was restored, not the old cause: enabling THRE again
     * while empty rearmed the model even though it was clear at probe entry. */
    CHECK(u.base.ier == 6 && u.thre_pending && uart16450_irq4(&u));
    write8(&u, 0x3ff, 0x5a);
    CHECK(read8(&u, 0x3ff) == 0x5a && u.base.count == 1 && u.base.tx[0] == 'T');

    unsigned char before[sizeof u];
    memcpy(before, &u, sizeof before);
    for (size_t i = 1; i < COUNT(bases); ++i) {
        replay(&u, bases[i], 0);
        for (unsigned reg = 0; reg < 8; ++reg) {
            uint16_t port = (uint16_t)(bases[i] + reg);
            write8(&u, port, 0xa5);
            CHECK(read8(&u, port) == 0xff);
            CHECK(memcmp(before, &u, sizeof before) == 0 && uart16450_irq4(&u));
        }
    }
}

static void transaction_controls(void)
{
    static const unsigned bad_width[] = { 0, 2, 4, 8, 257, UINT_MAX };
    Uart16450 u = {0};
    write8(&u, 0x3f8, 'A');
    refused(&u, LegacyComScalarOut, 0x3f8, 1, 'B'); /* busy THR */
    refused(&u, LegacyComScalarOut, 0x3f9, 1, 0x80); /* unsupported IER bits */
    refused(&u, LegacyComScalarOut, 0x3fd, 1, 0); /* read-only LSR */
    for (size_t i = 0; i < COUNT(bases); ++i) {
        uint16_t lsr = (uint16_t)(bases[i] + 5), scr = (uint16_t)(bases[i] + 7);
        uint16_t holes[] = { (uint16_t)(bases[i] - 1), (uint16_t)(bases[i] + 8),
            (uint16_t)(bases[i] + 0x1000) };
        for (size_t j = 0; j < COUNT(holes); ++j) {
            refused(&u, LegacyComScalarIn, holes[j], 1, 0xdeadbeef);
            refused(&u, LegacyComScalarOut, holes[j], 1, 0xa5);
        }
        for (size_t j = 0; j < COUNT(bad_width); ++j) {
            refused(&u, LegacyComScalarIn, lsr, bad_width[j], UINT32_MAX);
            refused(&u, LegacyComScalarOut, scr, bad_width[j], 0xa5);
        }
        refused(&u, LegacyComOther, lsr, 1, 0x3c);
        refused(&u, (enum LegacyComForm)99, scr, 1, 0x3c);
        refused(&u, LegacyComScalarOut, scr, 1, 0x100);
        refused(&u, LegacyComScalarOut, scr, 1, UINT32_MAX);
        CHECK(!legacy_com_io(&u, LegacyComScalarIn, lsr, 1, NULL));
    }
    uint32_t dst = 0xbeef;
    CHECK(!legacy_com_io(NULL, LegacyComScalarIn, 0x2f8, 1, &dst) && dst == 0xbeef);
    CHECK(u.base.busy && !u.base.polls_busy && u.base.tx[0] == 'A' && u.base.count == 1);
    CHECK(read8(&u, 0x3fd) == 0 && read8(&u, 0x3fd) == Uart8250Ready);
    write8(&u, 0x3f8, 'B');
    CHECK(u.base.tx[1] == 'B' && u.base.count == 2);
    CHECK(read8(&u, 0x3f8) == 0 && read8(&u, 0x3fd) == 0 && !u.base.busy);

    /* 047 calls the fixed 8250 directly, never this opt-in decoder. */
    UINT8 byte = 0xa5;
    unsigned char before[sizeof u.base];
    memcpy(before, &u.base, sizeof before);
    CHECK(!uart8250_access(&u.base, 0x2f8, FALSE, &byte));
    CHECK(byte == 0xa5 && memcmp(before, &u.base, sizeof before) == 0);
}

int main(void)
{
    probe_and_isolate();
    transaction_controls();
    puts("PASS: COM1 IER restored, THRE rearmed; COM2-4 isolated; fixed 047 COM2 refused");
    return 0;
}
