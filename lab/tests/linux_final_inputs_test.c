/* Trial 117, host only. From the repo root:
 * cc -std=c11 -O2 -Wall -Wextra -Werror lab/tests/linux_final_inputs_test.c -o /tmp/final_inputs
 * or (x64 VS prompt): cl /nologo /std:c11 /O2 /W4 /WX lab\tests\linux_final_inputs_test.c
 * Run the executable with vmlinuz-virt initramfs.cpio cmdline.bin, in that order.
 * No VM entry, permissions change, or protection against concurrent writes.
 */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <string.h>
#include "../../firmware/linux_layout.h"

/* linux_media.h also requires EFI protocols. Supply its fixed staging geometry
 * to the unmodified gate; the three SHA-256 trust anchors belong to that gate. */
typedef uint8_t UINT8;
enum {
    LinuxImageOff = 0, LinuxImageLen = 12649472,
    LinuxRdOff = LinuxImageOff + LinuxImageLen, LinuxRdLen = 1476964,
    LinuxCmdOff = LinuxRdOff + LinuxRdLen, LinuxCmdLen = 123
};
#include "../../firmware/linux_input_gate.h"

static uint8_t stage[LinuxCmdOff + LinuxCmdLen], ram[LinuxRamBytes];
enum { ExpectedRd = (LinuxRamBytes - LinuxRdLen) & ~(LinuxPage - 1) };

static int load_exact(const char *path, uint8_t *dst, size_t bytes)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return 0; }
    /* Check the open file before reading, then reject a short read or growth. */
    int ok = fseek(f, 0, SEEK_END) == 0 && ftell(f) == (long)bytes &&
        fseek(f, 0, SEEK_SET) == 0 && fread(dst, 1, bytes, f) == bytes &&
        fgetc(f) == EOF && !ferror(f);
    if (fclose(f) != 0) ok = 0;
    if (!ok) fprintf(stderr, "source size or I/O failure: %s (expected %zu bytes)\n", path, bytes);
    return ok;
}

static int authentic(void)
{
    enum LinuxInput bad = linux_bad_input(stage);
    if (bad == LinuxInputOK) return 1;
    fprintf(stderr, "source SHA-256 mismatch: input %u (1=kernel 2=initramfs 3=cmdline)\n", (unsigned)bad);
    return 0;
}

enum verdict { Exact, ParamsExtent, CmdExtent, RdExtent, CmdBytes, RdBytes };

static uint64_t split_field(const uint8_t *bp, size_t low, size_t high)
{
    return linux_le32(bp + low) | ((uint64_t)linux_le32(bp + high) << 32);
}

static int fits(uint64_t start, uint64_t bytes, size_t visible)
{
    return start <= visible && bytes <= visible - start;
}

/* Inspect the consumer-visible split fields, not the builder's LinuxLayout
 * result. cmdline_size is a ceiling, not a payload length: the authenticated
 * source defines the complete extent including its NUL. Neither destination
 * may be read until BOTH ranges have their expected position and full backing. */
static enum verdict inspect(size_t visible, const uint8_t *source)
{
    if (!fits(LinuxParams, LinuxPage, visible)) return ParamsExtent;
    const uint8_t *bp = ram + LinuxParams;
    uint64_t cmd = split_field(bp, 0x228, 0xc8);
    uint64_t rd = split_field(bp, 0x218, 0xc0), bytes = split_field(bp, 0x21c, 0xc4);
    if (cmd != LinuxCmdline || !fits(cmd, LinuxCmdLen, visible)) return CmdExtent;
    if (rd != ExpectedRd || bytes != LinuxRdLen || !fits(rd, bytes, visible)) return RdExtent;
    if (memcmp(ram + (size_t)cmd, source + LinuxCmdOff, LinuxCmdLen)) return CmdBytes;
    if (memcmp(ram + (size_t)rd, source + LinuxRdOff, LinuxRdLen)) return RdBytes;
    return Exact;
}

static int expect(const char *label, size_t visible, enum verdict want)
{
    static const char *const names[] = {"exact", "boot_params extent", "cmdline extent",
        "initramfs extent", "cmdline byte mismatch", "initramfs byte mismatch"};
    enum verdict got = inspect(visible, stage);
    if (got != want) {
        fprintf(stderr, "%s: expected %s, got %s\n", label, names[want], names[got]);
        return 0;
    }
    printf("%s: %s\n", label, names[got]);
    return 1;
}

static int controls(void)
{
    if (!expect("materialized", sizeof ram, Exact)) return 0;
    printf("cmdline GPA %#x + %u (includes NUL); initramfs GPA %#x + %#x\n",
        (unsigned)LinuxCmdline, (unsigned)LinuxCmdLen, (unsigned)ExpectedRd, (unsigned)LinuxRdLen);

    /* Change only destination RAM. The first mutation is non-NUL before and
     * after, with its real terminator and all length/pointer fields intact. */
    uint8_t first = ram[LinuxCmdline];
    ram[LinuxCmdline] = first == 'X' ? 'Y' : 'X';
    if (!expect("cmdline +0", sizeof ram, CmdBytes) ||
        !expect("bad cmdline byte and truncated initramfs", ExpectedRd + LinuxRdLen - 1, RdExtent)) return 0;
    ram[LinuxCmdline] = first;
    static const struct { const char *label; size_t offset; enum verdict want; } flips[] = {
        {"cmdline terminator", LinuxCmdline + LinuxCmdLen - 1, CmdBytes},
        {"initramfs +0", ExpectedRd, RdBytes},
        {"initramfs final byte", ExpectedRd + LinuxRdLen - 1, RdBytes}
    };
    for (size_t i = 0; i < sizeof flips / sizeof flips[0]; ++i) {
        ram[flips[i].offset] ^= 1;
        if (!expect(flips[i].label, sizeof ram, flips[i].want)) return 0;
        ram[flips[i].offset] ^= 1;
    }

    /* Noncanonical positions (even when still in RAM), wrong lengths, and a
     * nonzero high word are geometry failures, not content mismatches. */
    static const struct { const char *label; size_t field; uint32_t bad; enum verdict want; } fields[] = {
        {"cmdline shifted", 0x228, LinuxCmdline + 1, CmdExtent},
        {"cmdline high word", 0xc8, 1, CmdExtent},
        {"initramfs shifted", 0x218, ExpectedRd + 1, RdExtent},
        {"initramfs high word", 0xc0, 1, RdExtent},
        {"initramfs one byte short", 0x21c, LinuxRdLen - 1, RdExtent},
        {"initramfs length high word", 0xc4, UINT32_MAX, RdExtent}
    };
    for (size_t i = 0; i < sizeof fields / sizeof fields[0]; ++i) {
        uint8_t *p = ram + LinuxParams + fields[i].field;
        uint32_t saved = linux_le32(p);
        linux_put32(p, fields[i].bad);
        if (!expect(fields[i].label, sizeof ram, fields[i].want)) return 0;
        linux_put32(p, saved);
    }
    return expect("truncated boot_params", LinuxParams + LinuxPage - 1, ParamsExtent) &&
        expect("truncated cmdline", LinuxCmdline + LinuxCmdLen - 1, CmdExtent) &&
        expect("truncated initramfs", ExpectedRd + LinuxRdLen - 1, RdExtent) &&
        expect("restored", sizeof ram, Exact);
}

int main(int argc, char **argv)
{
    if (argc != 4) { fputs("usage: linux_final_inputs_test vmlinuz-virt initramfs.cpio cmdline.bin\n", stderr); return 2; }
    if (!load_exact(argv[1], stage + LinuxImageOff, LinuxImageLen) ||
        !load_exact(argv[2], stage + LinuxRdOff, LinuxRdLen) ||
        !load_exact(argv[3], stage + LinuxCmdOff, LinuxCmdLen) || !authentic()) return 1;
    LinuxLayout layout;
    const char *err = build_linux_layout(ram, sizeof ram, stage + LinuxImageOff, LinuxImageLen,
        stage + LinuxRdOff, LinuxRdLen, stage + LinuxCmdOff, LinuxCmdLen, &layout);
    if (err) { fprintf(stderr, "layout rejected: %s\n", err); return 1; }
    if (!controls() || !authentic()) return 1;
    puts("PASS: source pins unchanged; final-copy controls passed (host RAM snapshot only).");
    return 0;
}
