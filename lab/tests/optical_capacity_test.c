/* Host-side firmware reader: authored sectors only; no image or device access. */
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif
#include "../optical_media.h"

#define CHECK(e) do { if (!(e)) { fprintf(stderr, "line %d: %s\n", __LINE__, #e); return 1; } } while (0)

enum { sector = 2048, sectors = 3, guard = 8, canary = 0xa5 };
static const uint8_t capacity_cdb[10] = {0x25};

typedef struct {
    uint8_t data[sectors * sector];
    unsigned calls;
    uint32_t last_lba;
    size_t last_bytes;
} Backing;

static const uint8_t *view(void *context, uint32_t lba, size_t bytes)
{
    Backing *b = context;
    ++b->calls;
    b->last_lba = lba;
    b->last_bytes = bytes;
    if (lba >= sectors) return NULL;
    const size_t offset = (size_t)lba * sector;
    return bytes <= sizeof b->data - offset ? b->data + offset : NULL;
}

static bool untouched(const uint8_t *bytes, size_t length)
{
    for (size_t i = 0; i < length; ++i)
        if (bytes[i] != canary) return false;
    return true;
}

static uint32_t read_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void read10_lba(uint8_t *cdb, uint32_t lba)
{
    cdb[2] = (uint8_t)(lba >> 24);
    cdb[3] = (uint8_t)(lba >> 16);
    cdb[4] = (uint8_t)(lba >> 8);
    cdb[5] = (uint8_t)lba;
}

/* Even the straddling two-sector request fits: range, not output space,
 * must stop it before a backing lookup or partial transfer. */
static bool rejected(const OpticalMedium *media, const uint8_t *cdb, size_t length, size_t capacity)
{
    uint8_t out[guard + 2 * sector + guard];
    const Backing *b = media->context;
    const unsigned before = b->calls;
    memset(out, canary, sizeof out);
    return !optical_media_command(media, cdb, length, out + guard, capacity) &&
           before == b->calls && untouched(out, sizeof out);
}

static int authored_controls(void)
{
    Backing backing = {0};
    const OpticalMedium media = {sectors, view, &backing};
    for (size_t i = 0; i < sizeof backing.data; ++i)
        backing.data[i] = (uint8_t)(53 * (i / sector) + i % sector + i / 256);

    static const uint8_t expected[8] = {0, 0, 0, 2, 0, 0, 8, 0};
    uint8_t reply[guard + 8 + guard];
    memset(reply, canary, sizeof reply);
    CHECK(optical_media_command(&media, capacity_cdb, sizeof capacity_cdb, reply + guard, sizeof reply - guard));
    CHECK(backing.calls == 0);
    CHECK(memcmp(reply + guard, expected, sizeof expected) == 0);
    CHECK(untouched(reply, guard) && untouched(reply + guard + 8, guard));

    const uint32_t last = read_be32(reply + guard);
    const uint32_t block_bytes = read_be32(reply + guard + 4);
    CHECK(last == sectors - 1 && block_bytes == sector);
    uint8_t cdb[10] = {0x28, 0, 0, 0, 0, 0, 0, 0, 1, 0};
    uint8_t out[guard + sector + guard];
    memset(out, canary, sizeof out);
    read10_lba(cdb, last);
    CHECK(optical_media_command(&media, cdb, sizeof cdb, out + guard, block_bytes));
    CHECK(backing.calls == 1 && backing.last_lba == last && backing.last_bytes == block_bytes);
    CHECK(memcmp(out + guard, backing.data + (sectors - 1) * sector, sector) == 0);
    CHECK(untouched(out, guard) && untouched(out + guard + sector, guard));

    cdb[8] = 2;
    CHECK(rejected(&media, cdb, sizeof cdb, 2 * sector));
    cdb[8] = 1;
    read10_lba(cdb, last + 1);
    CHECK(rejected(&media, cdb, sizeof cdb, sector));

    uint8_t malformed[11] = {0x25};
    for (size_t field = 1; field < sizeof capacity_cdb; ++field) {
        for (unsigned bit = 0; bit < 8; ++bit) {
            malformed[field] = (uint8_t)(1u << bit);
            CHECK(rejected(&media, malformed, sizeof capacity_cdb, 8));
        }
        malformed[field] = 0;
    }
    CHECK(rejected(&media, NULL, sizeof capacity_cdb, 8));
    CHECK(rejected(&media, malformed, 9, 8));
    CHECK(rejected(&media, malformed, 11, 8));
    malformed[0] = 0x26;
    CHECK(rejected(&media, malformed, sizeof capacity_cdb, 8));
    const uint8_t rc16[16] = {0x9e, 0x10};
    CHECK(rejected(&media, rc16, sizeof rc16, 32));
    for (size_t n = 0; n < 8; ++n) CHECK(rejected(&media, capacity_cdb, sizeof capacity_cdb, n));
    CHECK(!optical_media_command(&media, capacity_cdb, sizeof capacity_cdb, NULL, 8));
    CHECK(backing.calls == 1);
    const OpticalMedium empty = {0, view, &backing};
    CHECK(rejected(&empty, capacity_cdb, sizeof capacity_cdb, 8));

    static const struct { uint64_t blocks; uint8_t wire[8]; } cases[] = {
        {1,                     {0, 0, 0, 0, 0, 0, 8, 0}},
        {UINT64_C(0x12345679),  {0x12, 0x34, 0x56, 0x78, 0, 0, 8, 0}},
        {UINT64_C(0xffffffff),  {0xff, 0xff, 0xff, 0xfe, 0, 0, 8, 0}},
        {UINT64_C(0x100000000), {0xff, 0xff, 0xff, 0xff, 0, 0, 8, 0}},
        {UINT64_C(0x100000001), {0xff, 0xff, 0xff, 0xff, 0, 0, 8, 0}},
        {UINT64_MAX,            {0xff, 0xff, 0xff, 0xff, 0, 0, 8, 0}}
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        const OpticalMedium geometry = {cases[i].blocks, view, &backing};
        memset(reply, canary, sizeof reply);
        CHECK(optical_media_command(&geometry, capacity_cdb, sizeof capacity_cdb, reply + guard, 8));
        CHECK(memcmp(reply + guard, cases[i].wire, 8) == 0);
        CHECK(untouched(reply, guard) && untouched(reply + guard + 8, guard));
        CHECK(backing.calls == 1);
    }
    return 0;
}

static int emit_capacity(const char *text)
{
    uint64_t bytes = 0;
    for (; *text; ++text) {
        if (*text < '0' || *text > '9') return 1;
        const unsigned digit = (unsigned)(*text - '0');
        if (bytes > (UINT64_MAX - digit) / 10) return 1;
        bytes = bytes * 10 + digit;
    }
    if (bytes == 0 || bytes % sector != 0) return 1;
    const OpticalMedium media = {bytes / sector, NULL, NULL};
    uint8_t reply[8];
    if (!optical_media_command(&media, capacity_cdb, sizeof capacity_cdb, reply, sizeof reply)) return 1;

    static const char hex[] = "0123456789abcdef";
    char line[17];
    for (size_t i = 0; i < sizeof reply; ++i) {
        line[2 * i] = hex[reply[i] >> 4];
        line[2 * i + 1] = hex[reply[i] & 15];
    }
    line[16] = '\n';
#ifdef _WIN32
    if (_setmode(_fileno(stdout), _O_BINARY) == -1) return 1;
#endif
    return fwrite(line, 1, sizeof line, stdout) == sizeof line && fflush(stdout) == 0 ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (argc == 1) return authored_controls();
    if (argc == 3 && strcmp(argv[1], "--media-bytes") == 0) return emit_capacity(argv[2]);
    fprintf(stderr, "usage: optical_capacity_test [--media-bytes DECIMAL]\n");
    return 1;
}
