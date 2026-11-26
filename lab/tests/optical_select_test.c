/* In-memory wire-format control. No ISO builder, file access or executable payload. */
#include <stdio.h>
#include <string.h>
#include "../../firmware/optical_select.h"

#define CHECK(e) do { if (!(e)) { fprintf(stderr, "line %d: %s\n", __LINE__, #e); return 1; } } while (0)
typedef struct { uint8_t before[16], block[36][2048], after[16]; } Medium;
static Medium a, b, snapshot;
static const uint8_t marker[] = {0x48, 0x56, 0x00, 0xef, 0xff, 0x33, 0x12};
static const uint32_t route[] = {17, 19, 28, 29, 30, 31};

static void entry(uint8_t *p, const char *name, unsigned attr, unsigned cluster, unsigned size)
{
    memcpy(p, name, 11);
    p[11] = (uint8_t)attr;
    p[26] = (uint8_t)cluster;
    p[28] = (uint8_t)size;
}

/* All offsets below are independently chosen fixture bytes, not selector
 * helpers. LBAs 22 (BIOS decoy) and image..image+3 are disjoint. */
static void author(Medium *m, unsigned platform, unsigned image, const uint8_t *data, unsigned n)
{
    memset(m, 0, sizeof(*m));
    memset(m->before, 0x6d, sizeof(m->before));
    memset(m->after, 0xa7, sizeof(m->after));
    uint8_t *p = m->block[17];
    memcpy(p + 1, "CD001", 5);
    p[6] = 1;
    memcpy(p + 7, "EL TORITO SPECIFICATION", 23);
    p[71] = 19;
    p = m->block[19];
    p[0] = 1; p[4] = 'H'; p[5] = 'V';
    p[28] = 0x62; p[29] = 0xff; /* -(0x0001 + 0x5648 + 0xaa55) mod 65536 */
    p[30] = 0x55; p[31] = 0xaa;
    p[32] = 0x88; p[38] = 1; p[40] = 22;
    p[64] = 0x91; p[65] = (uint8_t)platform; p[66] = 1;
    p[96] = 0x88; p[102] = 16; p[104] = (uint8_t)image;
    memset(m->block[22], 0xbc, 512);

    p = m->block[image];
    p[0] = 0xeb; p[1] = 0x3c; p[2] = 0x90;
    p[12] = 2; p[13] = 1; p[14] = 1; p[16] = 1; p[17] = 16;
    p[19] = 16; p[21] = 0xf8; p[22] = 1; p[24] = 1; p[26] = 1;
    p[510] = 0x55; p[511] = 0xaa;
    uint8_t *fat = p + 512;
    fat[0] = 0xf8; fat[1] = fat[2] = 0xff;
    fat[4] = 0x80; fat[5] = 0xff;   /* odd cluster 3: 0xff8 */
    fat[12] = 0xff; fat[13] = 0x0f; /* even cluster 8: 0xfff */
    fat[21] = 0xfa; fat[22] = 0x0f; /* last cluster 14: 0xffa */
    uint8_t *root = p + 1024;
    entry(root, "FIXTURE    ", 8, 0, 0);
    root[32] = 0xe5;
    entry(root + 64, "EFI        ", 0x10, 3, 0);
    uint8_t *efi = m->block[image + 1]; /* cluster 3: logical sector 4 */
    entry(efi, ".          ", 0x10, 3, 0);
    entry(efi + 32, "..         ", 0x10, 0, 0);
    entry(efi + 64, "BOOT       ", 0x10, 8, 0);
    uint8_t *boot = m->block[image + 2] + 512; /* cluster 8: logical sector 9 */
    entry(boot, ".          ", 0x10, 8, 0);
    entry(boot + 32, "..         ", 0x10, 3, 0);
    for (unsigned i = 2; i < 15; ++i) boot[32*i] = 0xe5;
    entry(boot + 480, "BOOTX64 EFI", 0x20, 14, n);
    memcpy(m->block[image + 3] + 1536, data, n); /* sector 15, not a PE */
}

typedef struct {
    const Medium *m;
    const uint32_t *trace;
    unsigned count, calls, bad, refuse;
    size_t supplied;
    uint8_t view[2048]; /* invalidated on every read */
} Reader;

static const uint8_t *read_block(void *ctx, uint32_t lba, size_t *bytes)
{
    Reader *r = (Reader *)ctx;
    unsigned i = r->calls++;
    if (i >= r->count || lba >= 36 || lba != r->trace[i]) {
        r->bad = 1;
        return NULL;
    }
    if (i == r->refuse && !r->supplied) return NULL;
    *bytes = i == r->refuse ? r->supplied : 2048;
    memcpy(r->view, r->m->block[lba], 2048);
    return r->view;
}

/* n pins every attempted read (including a refusal). Expected failures leave
 * the complete result untouched, not just its length. All runs guard both
 * sides of the medium and result and compare the entire input snapshot. */
static int exercise(const Medium *m, const uint32_t *trace, unsigned n,
                    unsigned refuse, size_t supplied, const uint8_t *expect, size_t len)
{
    Reader reader = {m, trace, n, 0, 0, refuse, supplied, {0}};
    struct { uint8_t left[16]; OpticalSelection out; uint8_t right[16]; } guarded, initial;
    memset(&guarded, 0xa5, sizeof(guarded));
    memcpy(&initial, &guarded, sizeof(initial));
    memcpy(&snapshot, m, sizeof(snapshot));
    int result = optical_select_x64(read_block, &reader, 36, &guarded.out);
    CHECK(!reader.bad && reader.calls == n);
    CHECK(memcmp(&snapshot, m, sizeof(snapshot)) == 0);
    CHECK(result == (expect != NULL));
    if (!expect) {
        CHECK(memcmp(&initial, &guarded, sizeof(initial)) == 0);
    } else {
        CHECK(memcmp(initial.left, guarded.left, sizeof(initial.left)) == 0);
        CHECK(memcmp(initial.right, guarded.right, sizeof(initial.right)) == 0);
        CHECK(guarded.out.image_lba == trace[2] && guarded.out.image_blocks == 4);
        CHECK(guarded.out.file_lba == trace[n-1] && guarded.out.file_offset == 1536);
        CHECK(guarded.out.file_bytes == len && memcmp(guarded.out.content, expect, len) == 0);
    }
    return 0;
}

int main(void)
{
    author(&a, 0xef, 28, marker, sizeof(marker));
    author(&b, 0, 28, marker, sizeof(marker));
    CHECK(exercise(&a, route, 6, 6, 0, marker, sizeof(marker)) == 0);
    CHECK(exercise(&b, route, 2, 6, 0, NULL, 0) == 0);
    b.block[19][65] = 0xef;
    CHECK(memcmp(&a, &b, sizeof(a)) == 0); /* platform was the ONLY difference */

    /* Preserve the 16-bit validation sum to isolate the key and BIOS-platform checks. */
    b.block[19][30] = 0x54;
    b.block[19][28] = 0x63;
    CHECK(exercise(&b, route, 2, 6, 0, NULL, 0) == 0);
    b.block[19][30] = 0x55;
    b.block[19][28] = 0x62;
    b.block[19][1] = 0xef;
    b.block[19][29] = 0x10;
    CHECK(exercise(&b, route, 2, 6, 0, NULL, 0) == 0);
    b.block[19][1] = 0;
    b.block[19][29] = 0xff;

    /* Single-byte mutations, with exact safe prefix of the successful trace. */
    static const struct { unsigned lba, offset, value, reads; } rejected[] = {
        {17, 1, 'X', 1}, {17, 30, 'X', 1}, {17, 74, 0xff, 1},
        {19, 4, 'J', 2}, {19, 32, 0, 2},
        {19, 64, 0x90, 2}, {19, 67, 1, 2}, {19, 96, 0, 2}, {19, 97, 1, 2},
        {19, 102, 4, 2}, {19, 104, 33, 2}, {19, 107, 0xff, 2}, {19, 40, 29, 2},
        {28, 12, 4, 3}, {28, 19, 17, 3}, {28, 511, 0, 3}, {28, 512+5, 0xfe, 3},
        {28, 1024+64+26, 15, 3}, {28, 1024+32, 0, 3},
        {29, 64+11, 0x20, 4}, {29, 64+26, 3, 4},
        {30, 512+480+6, '2', 5}, {30, 512+480+20, 1, 5},
        {30, 512+480+28, 65, 5}, {30, 512+480+31, 1, 5},
        {28, 512+21, 0xf7, 5} /* file cluster marked bad, never read its data */
    };
    for (unsigned i = 0; i < sizeof(rejected)/sizeof(rejected[0]); ++i) {
        uint8_t *byte = &b.block[rejected[i].lba][rejected[i].offset];
        uint8_t old = *byte;
        *byte = (uint8_t)rejected[i].value;
        CHECK(exercise(&b, route, rejected[i].reads, 6, 0, NULL, 0) == 0);
        *byte = old;
    }
    for (unsigned i = 0; i < 6; ++i) {
        CHECK(exercise(&a, route, i+1, i, 0, NULL, 0) == 0);
        CHECK(exercise(&a, route, i+1, i, 2047, NULL, 0) == 0);
    }
    /* Same wire profile, different image RBA ending at disc boundary and
     * a different file (including its length). Selection isn't a marker test. */
    static const uint32_t moved[] = {17, 19, 32, 33, 34, 35};
    static const uint8_t other[] = {0xfa, 0, 0x39};
    author(&b, 0xef, 32, other, sizeof(other));
    CHECK(exercise(&b, moved, 6, 6, 0, other, sizeof(other)) == 0);
    puts("optical_select: BIOS first; EFI trace 17,19,28,29,30,31; other platform 17,19; bounds/refusals OK");
    return 0;
}
