/* Host-only post-materialization checks; there is no VMCB or VMRUN here.
 * x64 VS prompt, from repo root (use the three 042 inputs from prepare.py):
 * cl /nologo /std:c11 /O2 /W4 /WX lab\tests\linux_entry_test.c /Fobuild\linux_entry_test.obj /Febuild\linux_entry_test.exe
 * build\linux_entry_test.exe build\linux-inputs\vmlinuz-virt build\linux-inputs\initramfs.cpio build\linux-inputs\cmdline.bin
 */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include "../../firmware/linux_layout.h"
#include "../../firmware/fetch_walk.h"

/* Input sizes and ABI expectations come from the fixed artifact, not from the
 * layout result. Identity authentication is separately tested by 045. */
static uint8_t image[0xc10400], initrd[0x168964], cmd[123], ram[LinuxRamBytes];

static int load_exact(const char *path, uint8_t *dest, size_t size)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return 0; }
    int ok = fread(dest, 1, size, f) == size && fgetc(f) == EOF && !ferror(f);
    if (fclose(f) != 0) ok = 0;
    if (!ok) fprintf(stderr, "wrong size or read error: %s\n", path);
    return ok;
}

static uint64_t ptr64(const uint8_t *bp, size_t lo, size_t hi)
{
    return linux_le32(bp + lo) | (uint64_t)linux_le32(bp + hi) << 32;
}

/* Half-open interval inside a known owned interval, no base+length wrap. */
static int within(uint64_t base, uint64_t length, uint64_t first, uint64_t end)
{
    return length && base >= first && base < end && length <= end - base;
}

enum result { ok, bad_rsi, bad_work, bad_entry, bad_rd, bad_cmd, bad_map };

static enum result inspect(uint64_t rip, uint64_t rsi)
{
    uint64_t walked;
    if (!within(rsi, 4096, 0x10000, 0x11000)) return bad_rsi;
    const uint8_t *bp = ram + (size_t)rsi;
    uint64_t base = linux_le32(bp + 0x214), size = linux_le32(bp + 0x260);
    if (base != 0x1000000 || size != 0x260a000 ||
        !within(base, size, 0x100000, sizeof ram)) return bad_work;
    if (rip != base + 0x200 || !fetch_walk(ram, sizeof ram, 0x13000, rip, &walked) ||
        walked != rip || ram[walked] != 0xfc) return bad_entry;
    uint64_t rd = ptr64(bp, 0x218, 0xc0), bytes = ptr64(bp, 0x21c, 0xc4);
    if (!within(rd, bytes, base + size, sizeof ram) ||
        rd != 0x7e97000 || bytes != sizeof initrd) return bad_rd;
    uint64_t line = ptr64(bp, 0x228, 0xc8);
    if (!within(line, sizeof cmd, 0x11000, 0x12000) || line != 0x11000 ||
        ram[(size_t)line + sizeof cmd - 1] != 0) return bad_cmd;
    /* Reject an e820 row extending outside the owned high RAM interval, even
     * when its wrapped end would appear to be below the ceiling. Run 042
     * already compares every emitted row to the fixed three-entry map. */
    const uint8_t *high = bp + 0x2d0 + 2 * 20;
    uint64_t start = linux_le64(high), length = linux_le64(high + 8);
    if (bp[0x1e8] != 3 || linux_le32(high + 16) != 1 ||
        !within(start, length, 0x100000, sizeof ram) ||
        !within(base, size, start, start + length) ||
        !within(rd, bytes, start, start + length)) return bad_map;
    return ok;
}

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (0)

int main(int argc, char **argv)
{
    if (argc != 4) { fputs("usage: linux_entry_test vmlinuz-virt initramfs.cpio cmdline.bin\n", stderr); return 2; }
    CHECK(load_exact(argv[1], image, sizeof image));
    CHECK(load_exact(argv[2], initrd, sizeof initrd));
    CHECK(load_exact(argv[3], cmd, sizeof cmd));
    LinuxLayout s;
    const char *err = build_linux_layout(ram, sizeof ram, image, sizeof image,
        initrd, sizeof initrd, cmd, sizeof cmd, &s);
    if (err) { fprintf(stderr, "layout: %s\n", err); return 1; }
    CHECK(inspect(s.entry, LinuxParams) == ok);
    CHECK(inspect(s.entry + 1u, LinuxParams) == bad_entry); /* valid page, wrong instruction */
    CHECK(inspect(s.entry, LinuxParams + 1u) == bad_rsi); /* page straddles command buffer */
    CHECK(inspect(s.entry, UINT64_MAX - 4094) == bad_rsi);

    /* Poison individual fields AFTER the real builder has returned, before any
     * entry. A valid low dword does not excuse a bad high dword. */
    uint8_t *bp = ram + LinuxParams;
    static const struct { unsigned offset; uint32_t value; enum result want; } cases[] = {
        {0x260, UINT32_MAX, bad_work}, {0xc0, 1, bad_rd}, {0xc4, UINT32_MAX, bad_rd},
        {0x218, 0x3609fff, bad_rd}, /* one byte inside initialization window */
        {0xc8, 1, bad_cmd}, {0x228, 0x11fff, bad_cmd},
        {0x2d0 + 40 + 8, 0x7eff963, bad_map} /* last byte of initrd no longer RAM */
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        uint8_t *p = bp + cases[i].offset;
        uint32_t old = linux_le32(p);
        linux_put32(p, cases[i].value);
        CHECK(inspect(s.entry, LinuxParams) == cases[i].want);
        linux_put32(p, old);
    }
    uint8_t *high_bytes = bp + 0x2d0 + 40 + 8;
    uint64_t length = linux_le64(high_bytes);
    linux_put64(high_bytes, UINT64_MAX);
    CHECK(inspect(s.entry, LinuxParams) == bad_map); /* base + length would wrap */
    linux_put64(high_bytes, length);
    CHECK(inspect(s.entry, LinuxParams) == ok);
    printf("Host layout: RIP=%#x RSI=%#x workspace end=%#x initrd end=%#x; pointer controls passed.\n",
        s.entry, (unsigned)LinuxParams, s.kernel + s.init_bytes, s.initrd + (unsigned)sizeof initrd);
    return 0;
}
