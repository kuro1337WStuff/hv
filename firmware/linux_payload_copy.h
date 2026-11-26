#ifndef HV_LINUX_PAYLOAD_COPY_H
#define HV_LINUX_PAYLOAD_COPY_H
#include "linux_entry_guard.h"

/* LinuxImageLen is the pinned vmlinuz file length, not the staging capacity. */
enum { LinuxPinnedSetup = LinuxImageLen - EntryKernelBytes };
_Static_assert(LinuxPinnedSetup > 0 && LinuxPinnedSetup % 512 == 0,
               "protected image must follow complete setup sectors");

enum LinuxPayloadCopy { LinuxPayloadExact, LinuxPayloadExtent, LinuxPayloadChanged };

/* Caller has authenticated the original image and owns both buffers. This is
 * a snapshot of the file-backed decompressor image, not its zero-fill workspace,
 * a later W->X transition, or protection against an effective writer. */
static inline enum LinuxPayloadCopy linux_payload_copy(const LinuxLayout *layout,
    const uint8_t *image, size_t image_bytes, const uint8_t *ram, size_t ram_bytes)
{
    if (!image || !ram || layout->setup_bytes != LinuxPinnedSetup ||
        layout->kernel != EntryKernel || layout->kernel_bytes != EntryKernelBytes ||
        image_bytes != LinuxImageLen || ram_bytes < EntryKernel + EntryKernelBytes)
        return LinuxPayloadExtent;

    /* Both complete extents are proved before forming either payload pointer. */
    const uint8_t *original = image + LinuxPinnedSetup, *loaded = ram + EntryKernel;
    for (size_t i = 0; i < EntryKernelBytes; ++i)
        if (loaded[i] != original[i]) return LinuxPayloadChanged;
    return LinuxPayloadExact;
}
#endif
