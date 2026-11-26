#ifndef HV_OPTICAL_SELECT_H
#define HV_OPTICAL_SELECT_H

#include "linux_layout.h" /* freestanding LE loads and byte copy */

/* Synthetic profile only: <=64 optical blocks, fixed boot record at 17,
 * one BIOS initial entry, one final EFI section. This is not an ISO/PE
 * validator. Counts in catalog entries are 512-byte units, RBAs are 2048s.
 * The EFI count covers the entire FAT12 fixture: 16 sectors, one reserved,
 * one FAT, one root sector and 13 single-sector, single-cluster objects.
 * Only the root and three referenced objects need to be present/allocated. */
enum { OpticalBlock = 2048, OpticalSector = 512, OpticalFileMax = 64 };

/* Return a read-only view valid until the next call; set bytes to its actual
 * size. NULL or anything but one complete optical block means read failure. */
typedef const uint8_t *(*OpticalRead)(void *ctx, uint32_t lba, size_t *bytes);

typedef struct {
    uint32_t image_lba, image_blocks, file_lba, file_offset, file_bytes;
    uint8_t content[OpticalFileMax]; /* first file_bytes are meaningful */
} OpticalSelection;

static inline const uint8_t *optical_block(OpticalRead read, void *ctx, uint32_t lba)
{
    size_t bytes = 0;
    const uint8_t *p = read(ctx, lba, &bytes);
    return bytes == OpticalBlock ? p : NULL;
}

static inline int optical_equal(const uint8_t *p, const char *s, unsigned n)
{
    for (unsigned i = 0; i < n; ++i) if (p[i] != (uint8_t)s[i]) return 0;
    return 1;
}

static inline int optical_boot_entry(const uint8_t *e)
{
    return e[0] == 0x88 && e[1] == 0 && linux_le16(e + 2) == 0 &&
           e[4] == 0 && e[5] == 0;
}

static inline int optical_bpb(const uint8_t *p)
{
    return linux_le16(p + 11) == OpticalSector && p[13] == 1 &&
           linux_le16(p + 14) == 1 && p[16] == 1 && linux_le16(p + 17) == 16 &&
           linux_le16(p + 19) == 16 && p[21] == 0xf8 && linux_le16(p + 22) == 1 &&
           linux_le32(p + 28) == 0 && linux_le32(p + 32) == 0 &&
           p[510] == 0x55 && p[511] == 0xaa && p[512] == 0xf8 &&
           p[513] == 0xff && p[514] == 0xff;
}

/* Single 512-byte directory, standard end-of-directory/deleted semantics.
 * The fixed profile uses ordinary short directories and archive files only. */
static inline const uint8_t *optical_find(const uint8_t *dir, const char *name, uint8_t attr)
{
    for (unsigned i = 0; i < OpticalSector && dir[i]; i += 32) {
        const uint8_t *e = dir + i;
        if (e[11] == attr && optical_equal(e, name, 11)) return e;
    }
    return NULL;
}

/* No writes to out on failure. out must not alias the reader's storage. */
static inline int optical_select_x64(OpticalRead read, void *ctx, uint32_t blocks,
                                     OpticalSelection *out)
{
    static const char system_id[32] = "EL TORITO SPECIFICATION";
    static const char path[3][12] = { "EFI        ", "BOOT       ", "BOOTX64 EFI" };
    if (!read || !out || blocks <= 17 || blocks > 64) return 0;
    const uint8_t *p = optical_block(read, ctx, 17);
    if (!p || p[0] != 0 || !optical_equal(p + 1, "CD001", 5) || p[6] != 1 ||
        !optical_equal(p + 7, system_id, sizeof(system_id))) return 0;
    uint32_t catalog = linux_le32(p + 71);
    if (catalog <= 17 || catalog >= blocks) return 0;
    p = optical_block(read, ctx, catalog);
    if (!p || p[0] != 1 || p[1] != 0 || linux_le16(p + 2) != 0 ||
        p[30] != 0x55 || p[31] != 0xaa) return 0;
    uint32_t sum = 0;
    for (unsigned i = 0; i < 32; i += 2) sum += linux_le16(p + i);
    if ((uint16_t)sum != 0 || !optical_boot_entry(p + 32) ||
        p[64] != 0x91 || p[65] != 0xef || linux_le16(p + 66) != 1 ||
        !optical_boot_entry(p + 96) || p[108] != 0) return 0;

    uint32_t bios = linux_le32(p + 40), image = linux_le32(p + 104);
    uint32_t bios_blocks = (linux_le16(p + 38) + 3u) / 4u;
    /* Layout is catalog, then disjoint BIOS and EFI extents in either order. */
    if (!bios_blocks || bios <= catalog || bios >= blocks || bios_blocks > blocks - bios ||
        linux_le16(p + 102) != 16 || image <= catalog || image >= blocks ||
        4 > blocks - image || (bios < image + 4 && image < bios + bios_blocks)) return 0;

    uint32_t lba = image;
    p = optical_block(read, ctx, lba);
    if (!p || !optical_bpb(p)) return 0;
    /* Only FAT entries 0..14 can be addressed. Preserve their packed bytes
     * before a subsequent callback invalidates the boot-sector view. */
    uint8_t fat[23];
    linux_copy(fat, p + OpticalSector, sizeof(fat));
    const uint8_t *dir = p + 2 * OpticalSector;
    unsigned seen = 0;
    for (unsigned level = 0; level < 3; ++level) {
        const uint8_t *e = optical_find(dir, path[level], level == 2 ? 0x20 : 0x10);
        if (!e || linux_le16(e + 20) != 0) return 0;
        unsigned cluster = linux_le16(e + 26);
        uint32_t size = linux_le32(e + 28);
        if (cluster < 2 || cluster > 14 || (seen & (1u << cluster)) ||
            (level == 2 ? !size || size > OpticalFileMax : size != 0)) return 0;
        seen |= 1u << cluster;
        unsigned pair = linux_le16(fat + cluster + cluster / 2);
        if (((cluster & 1 ? pair >> 4 : pair) & 0xfff) < 0xff8) return 0;
        unsigned sector = 3 + (cluster - 2); /* bounded above by 15 */
        uint32_t next = image + sector / 4;
        unsigned offset = (sector % 4) * OpticalSector;
        if (next != lba) {
            lba = next;
            p = optical_block(read, ctx, lba);
            if (!p) return 0;
        }
        dir = p + offset;
        if (level == 2) {
            out->image_lba = image;
            out->image_blocks = 4;
            out->file_lba = lba;
            out->file_offset = offset;
            out->file_bytes = size;
            linux_copy(out->content, dir, size);
            return 1;
        }
    }
    return 0;
}

#endif
