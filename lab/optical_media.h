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

/* An attached drive keeps its identity when empty (blocks == 0). NULL means
 * no target. Acceptance is a host result, not SCSI status or sense data. */
static inline OpticalReply optical_media_command(const OpticalMedium *media, const uint8_t *cdb,
                                                 size_t cdb_size, uint8_t *out, size_t capacity)
{
    if (!media || !cdb || !cdb_size) return (OpticalReply){false, 0};
    if (cdb[0] == 0x12) {
        /* Standard inquiry only, with an authored identity rather than host hardware IDs. */
        static const uint8_t identity[] = {
            0x05, 0x80, 0, 0x02, 31, 0, 0, 0,
            'H','V','L','A','B',' ',' ',' ',
            'R','E','A','D','-','O','N','L','Y',' ','C','D','-','R','O','M',
            '0','0','0','1'
        };
        if (cdb_size != 6 || cdb[1] || cdb[2] || cdb[3] || cdb[5])
            return (OpticalReply){false, 0};
        const size_t n = cdb[4] < sizeof identity ? cdb[4] : sizeof identity;
        if (n > capacity || (n && !out)) return (OpticalReply){false, 0};
        if (n) memcpy(out, identity, n);
        return (OpticalReply){true, n};
    }
    if (cdb[0] == 0x00) {
        if (cdb_size != 6 || !media->blocks) return (OpticalReply){false, 0};
        for (size_t i = 1; i < cdb_size; ++i)
            if (cdb[i]) return (OpticalReply){false, 0};
        return (OpticalReply){true, 0};
    }
    if (cdb[0] == 0x28)
        return optical_read10(cdb, cdb_size, media->blocks, media->view, media->context,
                              out, capacity);
    if (cdb[0] != 0x25 || cdb_size != 10 || !media->blocks || !out || capacity < 8)
        return (OpticalReply){false, 0};
    for (size_t i = 1; i < cdb_size; ++i)
        if (cdb[i] != 0) return (OpticalReply){false, 0};

    /* LBAs start at zero: the last address is one less than the block count. */
    const uint64_t last = media->blocks - 1;
    optical_media_store_be32(out, last >= UINT32_MAX ? UINT32_MAX : (uint32_t)last);
    optical_media_store_be32(out + 4, OPTICAL_BLOCK_BYTES);
    return (OpticalReply){true, 8};
}

#endif
