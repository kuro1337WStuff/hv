#ifndef LAB_OVMF_FLASH_H
#define LAB_OVMF_FLASH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Host fixture only. Lengths describe the available prefix of each backing.
 * The read edge supplies count immutable bytes at the requested backing offset
 * (or NULL); it never receives a GPA or the destination. */
typedef enum { OvmfVars, OvmfCode } OvmfBacking;
typedef struct {
    size_t vars_bytes, code_bytes;
    const uint8_t *(*read)(void *context, OvmfBacking backing,
                           size_t offset, size_t count);
    void *context;
} OvmfFlash;

/* Accept a nonempty range wholly inside ONE window and its available prefix.
 * Reject zero, straddles (even VARS -> CODE), and short destinations without
 * touching a backing. No partial copies or fallback lookups. The caller owns
 * capacity writable bytes at dest, disjoint from both immutable backings. */
static inline bool ovmf_flash_read(const OvmfFlash *flash, uint64_t gpa,
                                   size_t count, uint8_t *dest, size_t capacity)
{
    const uint64_t base = UINT64_C(0xffc00000);
    const uint64_t split = base + UINT64_C(0x84000);
    const uint64_t end = split + UINT64_C(0x37c000);
    if (!flash || !flash->read || !dest || !count || count > capacity ||
        gpa < base || gpa >= end) return false;

    const OvmfBacking backing = gpa < split ? OvmfVars : OvmfCode;
    const uint64_t offset = gpa - (backing == OvmfVars ? base : split);
    const uint64_t remaining = (backing == OvmfVars ? split : end) - gpa;
    const size_t available = backing == OvmfVars ? flash->vars_bytes : flash->code_bytes;
    if (count > remaining || offset > available || count > available - (size_t)offset)
        return false;

    const uint8_t *src = flash->read(flash->context, backing, (size_t)offset, count);
    if (!src) return false;
    memcpy(dest, src, count);
    return true;
}

#endif
