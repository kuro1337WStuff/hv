#ifndef LAB_OPTICAL_MEDIA_H
#define LAB_OPTICAL_MEDIA_H

#include "optical_read10.h"

typedef struct {
    uint64_t blocks;
    OpticalRead10View view;
    void *context;
} OpticalMedium;

static inline void optical_media_store_be32(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)(value >> 24);
    out[1] = (uint8_t)(value >> 16);
    out[2] = (uint8_t)(value >> 8);
    out[3] = (uint8_t)value;
}

static inline bool optical_media_command(const OpticalMedium *media, const uint8_t *cdb,
                                         size_t cdb_size, uint8_t *out, size_t capacity)
{
    if (!media || !cdb || cdb_size != 10) return false;
    if (cdb[0] == 0x28)
        return optical_read10(cdb, cdb_size, media->blocks, media->view, media->context,
                              out, capacity);
    if (cdb[0] != 0x25 || !media->blocks || !out || capacity < 8) return false;
    for (size_t i = 1; i < cdb_size; ++i)
        if (cdb[i] != 0) return false;

    /* LBAs start at zero: the last address is one less than the block count. */
    const uint64_t last = media->blocks - 1;
    optical_media_store_be32(out, last >= UINT32_MAX ? UINT32_MAX : (uint32_t)last);
    optical_media_store_be32(out + 4, OPTICAL_BLOCK_BYTES);
    return true;
}

#endif
