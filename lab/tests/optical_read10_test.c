/* Synthetic host fixture: literal wire requests, no device or file access. */
#include <stdio.h>
#include "../optical_read10.h"

#define CHECK(e) do { if (!(e)) { fprintf(stderr, "line %d: %s\n", __LINE__, #e); return 1; } } while (0)

enum { sector = 2048, pair = 4096, guard = 8, untouched = 0xa5 };
static const uint32_t base = UINT32_C(0x89abcdef);
static const uint64_t edge_blocks = UINT64_C(0x100000000);
static const uint8_t read_pair[10] = {0x28, 0, 0x89, 0xab, 0xcd, 0xef, 0, 0, 2, 0};
static const uint8_t read_last[10] = {0x28, 0, 0xff, 0xff, 0xff, 0xff, 0, 0, 1, 0};

/* Only two sparse extents exist, regardless of advertised media size. */
static uint8_t consecutive[pair], last[sector];
static uint8_t guarded[guard + pair + guard];
typedef struct {
    unsigned calls;
    uint32_t lba;
    size_t bytes, available;
} Probe;

static const uint8_t *lookup(void *context, uint32_t lba, size_t bytes)
{
    Probe *p = context;
    ++p->calls;
    p->lba = lba;
    p->bytes = bytes;
    if (lba == base && bytes <= p->available && bytes <= sizeof consecutive) return consecutive;
    if (lba == UINT32_MAX && bytes <= sizeof last) return last;
    return NULL;
}

static bool clean(size_t start, size_t end)
{
    for (size_t i = start; i < end; ++i)
        if (guarded[i] != untouched) return false;
    return true;
}

static bool rejected(const uint8_t *cdb, size_t length, uint64_t blocks, size_t capacity)
{
    Probe p = {0, 0, 0, pair};
    memset(guarded, untouched, sizeof guarded);
    const OpticalReply result = optical_read10(cdb, length, blocks, lookup, &p, guarded + guard, capacity);
    return !result.accepted && result.transferred == 0 &&
           p.calls == 0 && clean(0, sizeof guarded);
}

int main(void)
{
    /* Expected independently from the backing fill: tail then head, in order. */
    static const uint8_t boundary[64] = {
        0xf0,0xf1,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,0xf9,0xfa,0xfb,0xfc,0xfd,0xfe,0xff,
        0x40,0x41,0x42,0x43,0x44,0x45,0x46,0x47,0x48,0x49,0x4a,0x4b,0x4c,0x4d,0x4e,0x4f,
        0x50,0x51,0x52,0x53,0x54,0x55,0x56,0x57,0x58,0x59,0x5a,0x5b,0x5c,0x5d,0x5e,0x5f,
        0x60,0x61,0x62,0x63,0x64,0x65,0x66,0x67,0x68,0x69,0x6a,0x6b,0x6c,0x6d,0x6e,0x6f
    };
    for (size_t i = 0; i < sector; ++i) {
        consecutive[i] = (uint8_t)i;
        consecutive[sector + i] = (uint8_t)(0x40 + i);
        last[i] = (uint8_t)(0xdb - (uint8_t)i);
    }
    uint8_t *out = guarded + guard;
    Probe p = {0, 0, 0, pair};
    memset(guarded, untouched, sizeof guarded);
    OpticalReply result = optical_read10(read_pair, 10, (uint64_t)base + 2, lookup, &p, out, pair);
    CHECK(result.accepted && result.transferred == pair);
    CHECK(p.calls == 1 && p.lba == base && p.bytes == pair);
    CHECK(!memcmp(out, consecutive, pair));
    CHECK(!memcmp(out + sector - 16, boundary, sizeof boundary));
    CHECK(out[sector - 17] == 0xef && out[sector + 48] == 0x70);
    CHECK(clean(0, guard) && clean(guard + pair, sizeof guarded));

    memset(guarded, untouched, sizeof guarded);
    p.calls = 0;
    result = optical_read10(read_last, 10, edge_blocks, lookup, &p, out, pair);
    CHECK(result.accepted && result.transferred == sector);
    CHECK(p.calls == 1 && p.lba == UINT32_MAX && p.bytes == sector);
    CHECK(!memcmp(out, last, sector));
    CHECK(clean(0, guard) && clean(guard + sector, sizeof guarded));

    uint8_t cmd[11] = {0};
    memcpy(cmd, read_last, 10);
    cmd[8] = 2; /* The exclusive end cannot wrap to one. */
    CHECK(rejected(cmd, 10, edge_blocks, pair));
    CHECK(rejected(read_last, 10, UINT32_MAX, pair));
    CHECK(rejected(read_pair, 10, (uint64_t)base + 1, pair));
    CHECK(rejected(read_pair, 10, edge_blocks, pair - 1));
    CHECK(rejected(NULL, 10, edge_blocks, pair));
    memcpy(cmd, read_pair, 10);
    CHECK(rejected(cmd, 9, edge_blocks, pair));
    CHECK(rejected(cmd, sizeof cmd, edge_blocks, pair));
    cmd[7] = 2; cmd[8] = 0; /* Swapped count: 512, not two. */
    CHECK(rejected(cmd, 10, edge_blocks, pair));
    cmd[7] = 0; cmd[8] = 3;
    CHECK(rejected(cmd, 10, edge_blocks, pair));
    static const size_t fields[] = {0, 1, 6, 9};
    for (size_t i = 0; i < sizeof fields / sizeof fields[0]; ++i) {
        size_t field = fields[i];
        cmd[field] = field == 0 ? 0x2a : 1;
        cmd[8] = 2;
        CHECK(rejected(cmd, 10, edge_blocks, pair));
        cmd[8] = 0; /* Even a zero transfer requires a valid CDB. */
        CHECK(rejected(cmd, 10, edge_blocks, pair));
        cmd[field] = read_pair[field];
    }

    p.calls = 0;
    result = optical_read10(cmd, 10, 0, lookup, &p, out, 0);
    CHECK(result.accepted && result.transferred == 0);
    CHECK(p.calls == 0 && clean(0, sizeof guarded));
    result = optical_read10(cmd, 10, 0, NULL, NULL, NULL, 0);
    CHECK(result.accepted && result.transferred == 0);
    result = optical_read10(read_pair, 10, edge_blocks, lookup, &p, NULL, pair);
    CHECK(!result.accepted && result.transferred == 0);
    result = optical_read10(read_pair, 10, edge_blocks, NULL, &p, out, pair);
    CHECK(!result.accepted && result.transferred == 0);
    CHECK(p.calls == 0 && clean(0, sizeof guarded));

    p.available = pair - 1; /* No partial copy of a short backing. */
    result = optical_read10(read_pair, 10, edge_blocks, lookup, &p, out, pair);
    CHECK(!result.accepted && result.transferred == 0);
    CHECK(p.calls == 1 && p.lba == base && p.bytes == pair && clean(0, sizeof guarded));
    cmd[5] = 0xee; cmd[8] = 1; /* In-media but unmapped. */
    p.calls = 0;
    result = optical_read10(cmd, 10, edge_blocks, lookup, &p, out, pair);
    CHECK(!result.accepted && result.transferred == 0);
    CHECK(p.calls == 1 && p.lba == base - 1 && p.bytes == sector && clean(0, sizeof guarded));
    puts("Optical READ(10): exact sector order, 16+48 boundary, bounds and refusal checks passed.");
    return 0;
}
