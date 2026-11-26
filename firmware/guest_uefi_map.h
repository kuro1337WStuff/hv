#ifndef HV_GUEST_UEFI_MAP_H
#define HV_GUEST_UEFI_MAP_H

#include "linux_layout.h" /* little-endian guest byte writers only */

/* A synthetic x64 guest has five backed pages starting at 1 MiB, no other
 * memory. No descriptor contains a host pointer. The single loader and its
 * valid image handle are implicit; no EFI dispatch table is installed. */
enum {
    GuestMapBase = 0x100000, GuestMapPage = 4096,
    GuestMapLoaderGpa = GuestMapBase + GuestMapPage,
    GuestMapLoaderData = 2, GuestMapBootData = 4, GuestMapConventional = 7,
    GuestMapStride = 48 /* 40-byte x64 v1 prefix + 8 fixture extension bytes */
};

typedef struct {
    int claimed; /* the one supported AllocateAddress request has succeeded */
    int exited;
} GuestUefiMap; /* initialize to {0}; only the operations below change it */

/* Named outcomes, not EFI_STATUS ABI values. The last tag means dispatch was
 * impossible: a real loader cannot call a boot service to get an EFI error. */
typedef enum {
    GuestEfiSuccess, GuestEfiBufferTooSmall, GuestEfiInvalidParameter,
    GuestEfiNotFound, GuestBootApiUnavailable
} GuestBootOutcome;

typedef struct {
    GuestBootOutcome outcome;
    uint32_t version;
    size_t stride;
    uint64_t key;
} GuestMapReply; /* metadata is for successful GetMemoryMap only */

static inline uint64_t guest_map_key(const GuestUefiMap *map)
{
    /* Only two reachable maps; keys are opaque to the consumer. No frees. */
    return map->claimed ? UINT64_C(2) : UINT64_C(1);
}

static inline void guest_map_descriptor(uint8_t *out, uint32_t type, uint64_t gpa, uint64_t pages)
{
    linux_put64(out, type);        /* Type and four zero x64 alignment bytes */
    linux_put64(out + 8, gpa);
    linux_put64(out + 16, 0);     /* no runtime virtual mapping */
    linux_put64(out + 24, pages);
    linux_put64(out + 32, 8);     /* EFI_MEMORY_WB capability */
    linux_put64(out + 40, 0);     /* consumers must advance by returned stride */
}

/* UEFI 2.10 sec. 7.2.3. size describes the actual caller-owned byte span at out.
 * Nonnull outputs and model state are disjoint. Group the other three EFI
 * outputs in the returned value so no additional host pointers are needed. */
static inline GuestMapReply guest_get_memory_map(const GuestUefiMap *map, size_t *size, uint8_t *out)
{
    if (map->exited) return (GuestMapReply){GuestBootApiUnavailable, 0, 0, 0};
    if (!size) return (GuestMapReply){GuestEfiInvalidParameter, 0, 0, 0};
    size_t needed = (map->claimed ? 3u : 2u) * GuestMapStride;
    if (*size < needed) {
        *size = needed;
        return (GuestMapReply){GuestEfiBufferTooSmall, 0, 0, 0};
    }
    if (!out) return (GuestMapReply){GuestEfiInvalidParameter, 0, 0, 0};

    guest_map_descriptor(out, GuestMapBootData, GuestMapBase, 1);
    out += GuestMapStride;
    if (map->claimed) {
        guest_map_descriptor(out, GuestMapLoaderData, GuestMapLoaderGpa, 1);
        guest_map_descriptor(out + GuestMapStride, GuestMapConventional, GuestMapLoaderGpa + GuestMapPage, 3);
    } else {
        guest_map_descriptor(out, GuestMapConventional, GuestMapLoaderGpa, 4);
    }
    *size = needed;
    return (GuestMapReply){GuestEfiSuccess, 1, GuestMapStride, guest_map_key(map)};
}

/* Only AllocatePages(AllocateAddress, EfiLoaderData, 1) at the fixed loader
 * GPA is modeled. A second claim cannot find that page in conventional RAM. */
static inline GuestBootOutcome guest_claim_loader_page(GuestUefiMap *map)
{
    if (map->exited) return GuestBootApiUnavailable;
    if (map->claimed) return GuestEfiNotFound;
    map->claimed = 1;
    return GuestEfiSuccess;
}

/* UEFI 2.10 sec. 7.4.6. Model the key check, not shutdown events, timers, runtime
 * services or a system table. A failed exit can partially shut real firmware
 * down; this fixture's retry needs only GetMemoryMap and ExitBootServices. */
static inline GuestBootOutcome guest_exit_boot_services(GuestUefiMap *map, uint64_t key)
{
    if (map->exited) return GuestBootApiUnavailable;
    if (key != guest_map_key(map)) return GuestEfiInvalidParameter;
    map->exited = 1;
    return GuestEfiSuccess;
}

#endif
