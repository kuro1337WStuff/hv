#ifndef LAB_OPTICAL_READ10_H
#define LAB_OPTICAL_READ10_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum { OPTICAL_BLOCK_BYTES = 2048 };

/* Host-only view: return all bytes starting at lba, immutable until copied,
 * or NULL on any short/unavailable read. The output must be disjoint. */
typedef const uint8_t *(*OpticalRead10View)(void *context, uint32_t lba, size_t bytes);

/* Refusal transfers no bytes and leaves the output untouched. */
typedef struct {
    bool accepted;
    size_t transferred;
} OpticalReply;

static inline OpticalReply optical_read10(const uint8_t *cdb, size_t cdb_size, uint64_t media_blocks,
                                          OpticalRead10View view, void *context, uint8_t *out, size_t capacity)
{
    if (!cdb || cdb_size != 10 || cdb[0] != 0x28 || cdb[1] || cdb[6] || cdb[9])
        return (OpticalReply){false, 0};

    const uint32_t count = ((uint32_t)cdb[7] << 8) | cdb[8];
    if (!count) return (OpticalReply){true, 0};
    if (count > 2) return (OpticalReply){false, 0};

    const uint32_t lba = ((uint32_t)cdb[2] << 24) | ((uint32_t)cdb[3] << 16) |
                         ((uint32_t)cdb[4] << 8) | cdb[5];
    const size_t bytes = (size_t)count * OPTICAL_BLOCK_BYTES;
    if ((uint64_t)lba + count > media_blocks || bytes > capacity || !view || !out)
        return (OpticalReply){false, 0};

    const uint8_t *src = view(context, lba, bytes);
    if (!src) return (OpticalReply){false, 0};
    memcpy(out, src, bytes);
    return (OpticalReply){true, bytes};
}

#endif
