/* Host-only consumer; addresses decoded below are numbers in a synthetic
 * guest's map, never addresses at which this process should read/write. */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "../../firmware/guest_uefi_map.h"

#define CHECK(e) do { if (!(e)) { fprintf(stderr, "line %d: %s\n", __LINE__, #e); return 1; } } while (0)

static int untouched(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; ++i) if (p[i] != 0xa5) return 0;
    return 1;
}

static int region(const uint8_t *p, uint32_t type, uint64_t gpa, uint64_t pages)
{
    return linux_le32(p) == type && linux_le64(p + 8) == gpa && linux_le64(p + 16) == 0 &&
        linux_le64(p + 24) == pages && linux_le64(p + 32) == 8;
}

int main(void)
{
    GuestUefiMap map = {0};
    /* Leave room for a whole stray descriptor *beyond the offered span*. The
     * successful oversized call must preserve slack *inside* its span too. */
    enum { Prefix = 8, Capacity = 144 + 16, Tail = 48 };
    uint8_t storage[Prefix + Capacity + Tail], *bytes = storage + Prefix;
    memset(storage, 0xa5, sizeof(storage));
    size_t size = 0;
    CHECK(guest_get_memory_map(&map, NULL, bytes).outcome == GuestEfiInvalidParameter);
    CHECK(guest_get_memory_map(&map, &size, NULL).outcome == GuestEfiBufferTooSmall && size == 96);
    CHECK(guest_get_memory_map(&map, &size, NULL).outcome == GuestEfiInvalidParameter);
    size = 95; /* enough for one full descriptor, but not the map */
    CHECK(guest_get_memory_map(&map, &size, bytes).outcome == GuestEfiBufferTooSmall && size == 96);
    CHECK(untouched(storage, sizeof(storage)));

    GuestMapReply first = guest_get_memory_map(&map, &size, bytes);
    CHECK(first.outcome == GuestEfiSuccess && first.version == 1 && first.stride == 48 && size == 2 * first.stride);
    CHECK(region(bytes, 4, 0x100000, 1) && region(bytes + first.stride, 7, 0x101000, 4));
    CHECK(untouched(storage, Prefix) && untouched(bytes + size, Capacity + Tail - size));
    size_t first_bytes = size;
    GuestMapReply again = guest_get_memory_map(&map, &size, bytes);
    CHECK(again.outcome == GuestEfiSuccess && again.key == first.key);

    CHECK(guest_claim_loader_page(&map) == GuestEfiSuccess);
    CHECK(guest_claim_loader_page(&map) == GuestEfiNotFound);
    CHECK(guest_exit_boot_services(&map, first.key) == GuestEfiInvalidParameter);
    /* After that failed exit, use only GetMemoryMap and the exit retry. No
     * assumption that the platform's other boot services still work. */
    memset(storage, 0xa5, sizeof(storage));
    size = first_bytes;
    CHECK(guest_get_memory_map(&map, &size, bytes).outcome == GuestEfiBufferTooSmall && size == 144);
    CHECK(untouched(storage, sizeof(storage)));
    size = Capacity;
    GuestMapReply fresh = guest_get_memory_map(&map, &size, bytes);
    CHECK(fresh.outcome == GuestEfiSuccess && fresh.version == 1 && fresh.stride == 48 && size == 3 * fresh.stride);
    CHECK(fresh.key != first.key && region(bytes, 4, 0x100000, 1));
    CHECK(region(bytes + fresh.stride, 2, 0x101000, 1) && region(bytes + 2 * fresh.stride, 7, 0x102000, 3));
    CHECK(untouched(storage, Prefix) && untouched(bytes + size, Capacity + Tail - size));
    CHECK(guest_exit_boot_services(&map, fresh.key) == GuestEfiSuccess);
    printf("map: %zu -> %zu bytes, stride %zu v%" PRIu32 ", key %" PRIu64 " -> %" PRIu64
        "; 0x101000 conventional -> loader data\n", first_bytes, size, fresh.stride, fresh.version, first.key, fresh.key);

    memset(storage, 0xa5, sizeof(storage));
    size = Capacity;
    CHECK(guest_get_memory_map(&map, &size, bytes).outcome == GuestBootApiUnavailable);
    CHECK(size == Capacity && untouched(storage, sizeof(storage)));
    CHECK(guest_claim_loader_page(&map) == GuestBootApiUnavailable);
    CHECK(guest_exit_boot_services(&map, fresh.key) == GuestBootApiUnavailable);
    puts("short buffers untouched; stale exit EFI_INVALID_PARAMETER; fresh exit EFI_SUCCESS; subsequent map/claim/exit unavailable (no EFI status)");
    return 0;
}
