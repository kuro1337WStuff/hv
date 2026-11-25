/* From repo root: cc -std=c11 -Wall -Wextra -Werror lab/tests/guest_tx_capture_test.c -o /tmp/guest_tx && /tmp/guest_tx
 * Or: cl /nologo /std:c11 /W4 /WX lab\tests\guest_tx_capture_test.c /Fe:build\guest_tx_capture_test.exe */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t UINT8, BOOLEAN;
typedef uint16_t UINT16;
typedef size_t UINTN;
enum { FALSE = 0, TRUE = 1 };
#include "../../firmware/guest_tx_capture.h"

#define CHECK(t) do { if (!(t)) { fprintf(stderr, "line %d: %s\n", __LINE__, #t); return 1; } } while (0)

static GuestTxResult put(GuestTxCapture *c, UINT16 port, UINT8 value)
{
    return guest_tx_access(c, port, FALSE, &value);
}

static int status(GuestTxCapture *c, UINT8 expected)
{
    UINT8 value = 0xaa;
    return guest_tx_access(c, 0x3fd, TRUE, &value) == GuestTxOk && value == expected;
}

int main(void)
{
    static const UINT8 wire[] = "\x00\xff" "EARLY\r\nPASS\r\nTTY\r\nPASS\r\n";
    UINT8 storage[sizeof wire + 1];
    memset(storage, 0xa5, sizeof storage);
    GuestTxCapture c = {0};
    c.bytes = storage + 1;
    c.capacity = sizeof wire - 1;
    CHECK(c.count == 0 && !c.overflow && status(&c, 0x60));
    CHECK(put(&c, 0x3fb, 0x83) == GuestTxOk);
    CHECK(put(&c, 0x3f8, 1) == GuestTxOk && put(&c, 0x3f9, 0) == GuestTxOk);
    CHECK(put(&c, 0x3fb, 3) == GuestTxOk && c.count == 0 && status(&c, 0x60));
    UINT8 readback = 0xe0;
    CHECK(guest_tx_access(&c, 0x3f8, TRUE, &readback) == GuestTxOk && readback == 0 && c.count == 0);

    for (size_t i = 0; i < c.capacity; ++i) {
        CHECK(put(&c, 0x3f8, wire[i]) == GuestTxOk && c.count == i + 1);
        CHECK(put(&c, 0x3f8, 0xce) == GuestTxRefused && !c.overflow && c.count == i + 1);
        CHECK(status(&c, 0) && status(&c, 0x60));
    }
    CHECK(c.uart.polls_busy == c.count && !c.overflow && memcmp(c.bytes, wire, c.count) == 0);
    CHECK(put(&c, 0x2f8, 0xec) == GuestTxRefused && put(&c, 0x3f9, 0x0f) == GuestTxRefused);
    CHECK(put(&c, 0x3fb, 0x83) == GuestTxOk && put(&c, 0x3f8, 2) == GuestTxOk && c.uart.dll == 2);
    CHECK(put(&c, 0x3fb, 3) == GuestTxOk && !c.overflow);
    CHECK(put(&c, 0x3f8, 'X') == GuestTxOverflow && c.overflow && !c.uart.busy);
    CHECK(c.count == sizeof wire - 1 && memcmp(c.bytes, wire, c.count) == 0);
    CHECK(storage[0] == 0xa5 && storage[sizeof storage - 1] == 0xa5);
    readback = 0xae;
    CHECK(guest_tx_access(&c, 0x3fd, TRUE, &readback) == GuestTxOverflow && readback == 0xae);

    GuestTxCapture empty = {0}; /* Zero-length storage: no dereference. */
    CHECK(status(&empty, 0x60) && put(&empty, 0x3f8, 'P') == GuestTxOverflow && empty.count == 0);
    puts("Bounded COM1 capture: raw bytes, capacity, register aliases, refusal and sticky overflow.");
    return 0;
}
