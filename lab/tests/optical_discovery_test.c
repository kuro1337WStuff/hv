/* Hosted drive discovery using authored bytes only; no image or device access. */
#include <stdio.h>
#include <string.h>
#include "../optical_media.h"

#define CHECK(e) do { if (!(e)) { fprintf(stderr, "line %d: %s\n", __LINE__, #e); return 1; } } while (0)

enum { sector_size = 2048, canary = 0xa5 };
static const uint8_t identity[] = {
    5, 0x80, 0, 2, 31, 0, 0, 0,
    'H','V','L','A','B',' ',' ',' ',
    'R','E','A','D','-','O','N','L','Y',' ','C','D','-','R','O','M',
    '0','0','0','1'
};
static const uint8_t metadata[] = {0,0,0,2, 0,0,8,0}; /* Three 2048-byte blocks. */
static const uint8_t sector[sector_size] = {
    'L','B','A','2', [512] = 0xc7, [sector_size - 1] = 0x3e
};
static const uint8_t inquiry[6] = {0x12,0,0,0,36,0};
static const uint8_t tur[6] = {0};
static const uint8_t capacity10[10] = {0x25};
static const uint8_t read_last[10] = {0x28,0,0,0,0,2,0,0,1,0};
static const uint8_t read_zero[10] = {0x28};

static unsigned lookups;
static const uint8_t *view(void *context, uint32_t lba, size_t bytes)
{
    (void)context;
    ++lookups;
    return lba == 2 && bytes == sizeof(sector) ? sector : NULL;
}

/* Compare the entire buffer, including both guards and all untransferred bytes. */
static bool expect(const OpticalMedium *drive, const uint8_t *cdb, size_t length,
                   size_t capacity, bool accepted, const uint8_t *expected, size_t n,
                   unsigned expected_lookups)
{
    uint8_t buf[1 + sector_size + 1];
    const unsigned before = lookups;
    memset(buf, canary, sizeof(buf));
    const OpticalReply r = optical_media_command(drive, cdb, length, buf + 1, capacity);
    if (r.accepted != accepted || r.transferred != n || lookups - before != expected_lookups)
        return false;
    for (size_t i = 0; i < sizeof(buf); ++i) {
        if (buf[i] != (i > 0 && i <= n ? expected[i - 1] : canary)) return false;
    }
    return true;
}

static int inquiry_limits(const OpticalMedium *drive)
{
    uint8_t cdb[sizeof(inquiry)];
    memcpy(cdb, inquiry, sizeof(cdb));
    cdb[4] = 0;
    CHECK(expect(drive, cdb, sizeof(cdb), 36, true, NULL, 0, 0));
    const unsigned before = lookups;
    OpticalReply r = optical_media_command(drive, cdb, sizeof(cdb), NULL, 0);
    CHECK(r.accepted && r.transferred == 0 && lookups == before);
    cdb[4] = 13;
    CHECK(expect(drive, cdb, sizeof(cdb), 36, true, identity, 13, 0));
    CHECK(expect(drive, cdb, sizeof(cdb), 12, false, NULL, 0, 0));
    r = optical_media_command(drive, cdb, sizeof(cdb), NULL, 13);
    CHECK(!r.accepted && r.transferred == 0 && lookups == before);
    cdb[4] = 255;
    CHECK(expect(drive, cdb, sizeof(cdb), 36, true, identity, 36, 0));
    CHECK(expect(drive, cdb, sizeof(cdb), 35, false, NULL, 0, 0));
    return 0;
}

static int malformed(const OpticalMedium *drive)
{
    const uint8_t *const commands[] = {inquiry, tur};
    for (size_t k = 0; k < sizeof(commands) / sizeof(commands[0]); ++k) {
        uint8_t cdb[7] = {0};
        memcpy(cdb, commands[k], 6);
        CHECK(expect(drive, cdb, 5, 36, false, NULL, 0, 0));
        CHECK(expect(drive, cdb, 7, 36, false, NULL, 0, 0));
        for (size_t i = 1; i < 6; ++i) {
            if (commands[k] == inquiry && i == 4) continue; /* Allocation is supported. */
            cdb[i] = 1; /* EVPD, page code, reserved or control, one at a time. */
            CHECK(expect(drive, cdb, 6, 36, false, NULL, 0, 0));
            cdb[i] = 0;
        }
    }
    return 0;
}

int main(void)
{
    OpticalMedium drive = {3, view, NULL};
    CHECK(expect(&drive, inquiry, 6, 36, true, identity, sizeof(identity), 0));
    CHECK(expect(&drive, tur, 6, 36, true, NULL, 0, 0));
    CHECK(expect(&drive, capacity10, 10, 36, true, metadata, sizeof(metadata), 0));
    CHECK(expect(&drive, read_last, 10, sector_size, true, sector, sizeof(sector), 1));
    CHECK(expect(&drive, read_zero, 10, 0, true, NULL, 0, 0));
    CHECK(inquiry_limits(&drive) == 0);
    CHECK(malformed(&drive) == 0);

    drive.blocks = 0; /* Eject the authored medium; the same drive remains attached. */
    CHECK(expect(&drive, inquiry, 6, 36, true, identity, sizeof(identity), 0));
    CHECK(expect(&drive, tur, 6, 36, false, NULL, 0, 0));
    CHECK(expect(&drive, capacity10, 10, 36, false, NULL, 0, 0));
    CHECK(expect(&drive, read_last, 10, sector_size, false, NULL, 0, 0));
    CHECK(expect(&drive, read_zero, 10, 0, true, NULL, 0, 0));
    CHECK(inquiry_limits(&drive) == 0);

    CHECK(expect(NULL, inquiry, 6, 36, false, NULL, 0, 0));
    CHECK(expect(NULL, tur, 6, 36, false, NULL, 0, 0));
    CHECK(expect(NULL, capacity10, 10, 36, false, NULL, 0, 0));
    CHECK(expect(NULL, read_last, 10, sector_size, false, NULL, 0, 0));
    CHECK(expect(NULL, read_zero, 10, 0, false, NULL, 0, 0));
    puts("Optical discovery: present, empty and absent target sequences passed");
    return 0;
}
