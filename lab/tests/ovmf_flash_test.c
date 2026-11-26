/* From the repository root:
 * cc -std=c11 -Wall -Wextra -Werror lab/tests/ovmf_flash_test.c -o /tmp/ovmf_flash_test && /tmp/ovmf_flash_test
 * In a VS Developer Command Prompt:
 * cl /nologo /std:c11 /W4 /WX lab\tests\ovmf_flash_test.c /Fe:ovmf_flash_test.exe && ovmf_flash_test.exe
 */
#include <stdio.h>
#include "../ovmf_flash.h"

#define CHECK(e) do { if (!(e)) { fprintf(stderr, "line %d: %s\n", __LINE__, #e); return 1; } } while (0)

/* Literal offsets in separate synthetic objects, not decoder-derived GPAs.
 * VARS has a deliberately different tail. No firmware bytes are used. */
static const uint8_t vars[0x84000] = {
    [0] = 0xa1, [0x83ff0] = 0xb2, [0x83fff] = 0xc3
};
static const uint8_t code[0x37c000] = {
    [0] = 0xd4, [0x37afff] = 0xe5, [0x37bfef] = 0xf6,
    0x39, 0xa4, 0x71, 0x0b, 0xce, 0x52, 0x18, 0xf0,
    0x65, 0x2d, 0xbc, 0x83, 0x47, 0x9a, 0x06, 0xde
};

typedef struct {
    unsigned calls;
    OvmfBacking backing;
    size_t offset, count;
} Receipt;

/* This is the actual backing-read edge, not a report from the decoder. Log
 * even invalid requests, before any fixture bounds check. */
static const uint8_t *read_backing(void *context, OvmfBacking backing, size_t offset, size_t count)
{
    Receipt *r = context;
    ++r->calls;
    r->backing = backing;
    r->offset = offset;
    r->count = count;
    if (backing == OvmfVars && offset < sizeof vars && count <= sizeof vars - offset)
        return vars + offset;
    if (backing == OvmfCode && offset < sizeof code && count <= sizeof code - offset)
        return code + offset;
    return NULL;
}

enum { Capacity = 32, Guard = 0x5a };

static int read_ok(const OvmfFlash *f, uint64_t gpa, size_t count, OvmfBacking backing,
                   size_t offset, const uint8_t *expected)
{
    Receipt *r = f->context;
    uint8_t buf[Capacity + 2];
    *r = (Receipt){0};
    memset(buf, Guard, sizeof buf);
    CHECK(ovmf_flash_read(f, gpa, count, buf + 1, Capacity));
    CHECK(r->calls == 1 && r->backing == backing && r->offset == offset && r->count == count);
    CHECK(!memcmp(buf + 1, expected, count));
    CHECK(buf[0] == Guard);
    for (size_t i = 1 + count; i < sizeof buf; ++i) CHECK(buf[i] == Guard);
    return 0;
}

static int rejected(const OvmfFlash *f, uint64_t gpa, size_t count, size_t capacity)
{
    Receipt *r = f->context;
    uint8_t buf[Capacity + 2];
    *r = (Receipt){0};
    memset(buf, Guard, sizeof buf);
    CHECK(!ovmf_flash_read(f, gpa, count, buf + 1, capacity));
    CHECK(r->calls == 0);
    for (size_t i = 0; i < sizeof buf; ++i) CHECK(buf[i] == Guard);
    return 0;
}

int main(void)
{
    const uint8_t reset[] = {0x39, 0xa4, 0x71, 0x0b, 0xce, 0x52, 0x18, 0xf0,
                             0x65, 0x2d, 0xbc, 0x83, 0x47, 0x9a, 0x06, 0xde};
    Receipt r = {0};
    OvmfFlash f = {sizeof vars, sizeof code, read_backing, &r};
    CHECK(!read_ok(&f, 0xffc00000, 1, OvmfVars, 0, (const uint8_t[]){0xa1}));
    CHECK(!read_ok(&f, 0xffc83ff0, 1, OvmfVars, 0x83ff0, (const uint8_t[]){0xb2}));
    CHECK(!read_ok(&f, 0xffc83fff, 1, OvmfVars, 0x83fff, (const uint8_t[]){0xc3}));
    CHECK(!read_ok(&f, 0xffc84000, 1, OvmfCode, 0, (const uint8_t[]){0xd4}));
    CHECK(!read_ok(&f, 0xffffffef, 2, OvmfCode, 0x37bfef, (const uint8_t[]){0xf6, 0x39}));
    CHECK(!read_ok(&f, 0xffffffff, 1, OvmfCode, 0x37bfff, (const uint8_t[]){0xde}));
    CHECK(!read_ok(&f, 0xfffffff0, sizeof reset, OvmfCode, 0x37bff0, reset));
    printf("reset: backing=%s offset=0x%zx count=%zu reads=%u\n",
           r.backing == OvmfCode ? "CODE" : "VARS", r.offset, r.count, r.calls);

    /* Capacity admits +17. Even an oversized backing must not extend a window. */
    f.code_bytes++;
    CHECK(!rejected(&f, 0xfffffff0, 17, Capacity));
    CHECK(!rejected(&f, UINT64_C(0x100000000), 1, Capacity));
    CHECK(!rejected(&f, UINT64_MAX, 2, Capacity));
    CHECK(!rejected(&f, 0xffbfffff, 2, Capacity));
    f.vars_bytes++;
    CHECK(!rejected(&f, 0xffc83fff, 2, Capacity));
    CHECK(!rejected(&f, 0xffc84000, 0, Capacity));
    CHECK(!rejected(&f, 0xfffffff0, 16, 15));

    /* Missing final CODE page: last present byte works, a range starting in
     * it cannot partially succeed, and the reset read never reaches backing. */
    f.vars_bytes = sizeof vars;
    f.code_bytes = 0x37b000;
    CHECK(!read_ok(&f, 0xffffefff, 1, OvmfCode, 0x37afff, (const uint8_t[]){0xe5}));
    CHECK(!rejected(&f, 0xffffefff, 2, Capacity));
    CHECK(!rejected(&f, 0xfffff000, 1, Capacity));
    CHECK(!rejected(&f, 0xfffffff0, 16, Capacity));
    puts("PASS: split-flash boundaries; rejected spans leave destination and guards intact, zero backing reads.");
    return 0;
}
