/* Host-only verifier; no VM or privileged instructions. From the repository root
 * in an x64 VS Developer Command Prompt:
 * cl /nologo /std:c11 /O2 /W4 /WX lab\tests\linux_layout_test.c /Fobuild\linux_layout_test.obj /Febuild\linux_layout_test.exe
 * build\linux_layout_test.exe build\linux-inputs\vmlinuz-virt build\linux-inputs\initramfs.cpio build\linux-inputs\cmdline.bin
 */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../firmware/linux_layout.h"

typedef struct { uint8_t *p; size_t n; } FileBytes;

static int load(const char *path, FileBytes *b, size_t cap)
{
    FILE *f = fopen(path, "rb");
    long len;
    if (!f) { perror(path); return 0; }
    if (fseek(f, 0, SEEK_END) != 0 || (len = ftell(f)) <= 0 || (size_t)len > cap ||
        fseek(f, 0, SEEK_SET) != 0 || !(b->p = malloc((size_t)len)) ||
        fread(b->p, 1, (size_t)len, f) != (size_t)len) {
        fprintf(stderr, "cannot load bounded input: %s\n", path); fclose(f); return 0;
    }
    b->n = (size_t)len;
    return fclose(f) == 0;
}

/* Inspect every entry, including non-present paths. Following present entries
 * through offsets in the backing array also catches accidental host frames. */
static int verify_paging(const uint8_t *ram)
{
    uint32_t table = LinuxPml4;
    const uint32_t next[] = {LinuxPdpt, LinuxPd, LinuxPt};
    for (unsigned level = 0; level < 4; ++level) {
        for (unsigned i = 0; i < 512; ++i) {
            uint64_t want = 0, got = linux_le64(ram + table + 8 * i);
            if (level < 3 && !i) want = (uint64_t)next[level] + 3;
            else if (level == 2 && i < LinuxRamBytes / LinuxLargePage)
                want = (uint64_t)i * LinuxLargePage + 0x83;
            else if (level == 3 && (i < LinuxHoleStart / LinuxPage || i >= LinuxHighRam / LinuxPage))
                want = (uint64_t)i * LinuxPage + 3;
            if (got != want) {
                fprintf(stderr, "paging mismatch at level %u, entry %u\n", level, i); return 0;
            }
        }
        if (level < 3) table = (uint32_t)(linux_le64(ram + table) & ~UINT64_C(0xfff));
    }
    return 1;
}

#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "line %d: %s\n", __LINE__, #expr); return 0; } } while (0)

static int verify_bytes(const uint8_t *ram, const LinuxLayout *s, FileBytes bz, FileBytes rd, FileBytes cmd)
{
    const uint8_t *bp = ram + LinuxParams;
    const uint32_t starts[] = {0, 0xa0000, 0x100000, LinuxRamBytes};
    const uint8_t *e820 = bp + 0x2d0;
    /* Real pinned 3.24.4 kernel contract; a file that merely happens to have a
     * compatible header must not silently redefine the known test geometry. */
    CHECK(s->kernel == 0x1000000 && s->setup_bytes == 0x4000 && s->init_bytes == 0x260a000);
    CHECK(s->entry == 0x1000200 && s->kernel_bytes == bz.n - 0x4000);
    CHECK(!memcmp(ram + s->kernel, bz.p + s->setup_bytes, s->kernel_bytes));
    for (uint32_t i = s->kernel_bytes; i < s->init_bytes; ++i) CHECK(!ram[s->kernel + i]);
    CHECK(s->initrd == ((LinuxRamBytes - rd.n) & ~(size_t)0xfff));
    CHECK(!memcmp(ram + s->initrd, rd.p, rd.n));
    CHECK(!memcmp(ram + LinuxCmdline, cmd.p, cmd.n));
    CHECK(bp[0x210] == 0xff && bp[0x211] == 1);
    CHECK(linux_le32(bp + 0x214) == s->kernel && linux_le32(bp + 0x218) == s->initrd);
    CHECK(linux_le32(bp + 0x21c) == rd.n && linux_le32(bp + 0x228) == LinuxCmdline);
    CHECK(!linux_le32(bp + 0x208) && !linux_le32(bp + 0x224));
    CHECK(!linux_le32(bp + 0x23c) && !linux_le64(bp + 0x240) && !linux_le64(bp + 0x250));
    /* The rest of the setup header is copied verbatim; do not copy the MZ
     * stub, boot sector, or executable setup code on either side of it. */
    for (size_t i = 0; i < LinuxPage; ++i) {
        if (i >= 0x1f1 && i < 0x26c) {
            if ((i >= 0x208 && i < 0x20c) || (i >= 0x210 && i < 0x212) ||
                (i >= 0x214 && i < 0x220) || (i >= 0x224 && i < 0x22c) ||
                (i >= 0x23c && i < 0x248) || (i >= 0x250 && i < 0x258)) continue;
            CHECK(bp[i] == bz.p[i]);
        } else if (i != 0x1e8 && !(i >= 0x2d0 && i < 0x2d0 + 60)) CHECK(!bp[i]);
    }
    CHECK(bp[0x1e8] == 3);
    for (unsigned i = 0; i < 3; ++i) {
        CHECK(linux_le64(e820 + 20 * i) == starts[i]);
        CHECK(linux_le64(e820 + 20 * i + 8) == starts[i + 1] - starts[i]);
        CHECK(linux_le32(e820 + 20 * i + 16) == (i == 1 ? 2u : 1u));
    }
    for (uint32_t i = LinuxHoleStart; i < LinuxHighRam; ++i) CHECK(ram[i] == 0xa5);
    CHECK(!linux_le64(ram + LinuxGdt) && !linux_le64(ram + LinuxGdt + 8));
    CHECK(linux_le64(ram + LinuxGdt + 0x10) == UINT64_C(0x00af9b000000ffff));
    CHECK(linux_le64(ram + LinuxGdt + 0x18) == UINT64_C(0x00cf93000000ffff));
    CHECK(verify_paging(ram));
    return 1;
}

static int exercise(uint8_t *ram, FileBytes bz, FileBytes rd, FileBytes cmd)
{
    LinuxLayout s;
    const char *error;
    uint32_t old;
    memset(ram, 0xa5, LinuxRamBytes);
    error = build_linux_layout(ram, LinuxRamBytes, bz.p, bz.n, rd.p, rd.n, cmd.p, cmd.n, &s);
    if (error) { fprintf(stderr, "layout rejected: %s\n", error); return 0; }
    CHECK(verify_bytes(ram, &s, bz, rd, cmd));
    printf("RIP=%#x RSI=%#x CR3=%#x GDTR=%#x:%#x CS=%#x DS=%#x RSP=%#x\n",
           s.entry, LinuxParams, LinuxPml4, LinuxGdt, LinuxGdtLimit, LinuxCs, LinuxDs, LinuxStackTop);
    printf("setup=%#x kernel=%#x+%#x init_size=%#x initrd=%#x+%#zx cmdline=%#x+%zu\n",
           s.setup_bytes, s.kernel, s.kernel_bytes, s.init_bytes, s.initrd, rd.n, LinuxCmdline, cmd.n);
    /* Use the real input for rejection cases too. Poison the first RAM byte
     * and the output to detect mutation before a rejection. */
    ram[0] = 0x5a; s.entry = UINT32_MAX;
#define REJECT(ram_len, bz_len, cmd_len) do { \
    CHECK(build_linux_layout(ram, (ram_len), bz.p, (bz_len), rd.p, rd.n, cmd.p, (cmd_len), &s) != NULL); \
    CHECK(ram[0] == 0x5a && s.entry == UINT32_MAX); \
} while (0)
    REJECT(LinuxRamBytes, 0x201, cmd.n);
    REJECT(LinuxRamBytes, bz.n - 1, cmd.n);
    REJECT(LinuxRamBytes - LinuxPage, bz.n, cmd.n);
    REJECT(LinuxRamBytes, bz.n, cmd.n - 1);
    bz.p[0x201]++; REJECT(LinuxRamBytes, bz.n, cmd.n); bz.p[0x201]--;
    bz.p[0x236] ^= 1; REJECT(LinuxRamBytes, bz.n, cmd.n); bz.p[0x236] ^= 1;
    old = linux_le32(bz.p + 0x24c); linux_put32(bz.p + 0x24c, UINT32_MAX);
    REJECT(LinuxRamBytes, bz.n, cmd.n); linux_put32(bz.p + 0x24c, old);
    old = linux_le32(bz.p + 0x230); linux_put32(bz.p + 0x230, old + 1);
    REJECT(LinuxRamBytes, bz.n, cmd.n); linux_put32(bz.p + 0x230, old);
    old = linux_le32(bz.p + 0x22c); linux_put32(bz.p + 0x22c, s.kernel + s.init_bytes - 1);
    REJECT(LinuxRamBytes, bz.n, cmd.n); linux_put32(bz.p + 0x22c, old);
#undef REJECT
    puts("PASS: materialized bzImage, initramfs, boot_params, GDT and guest identity mappings; invalid layouts rejected.");
    return 1;
}

int main(int argc, char **argv)
{
    FileBytes bz = {0}, rd = {0}, cmd = {0};
    uint8_t *ram = NULL;
    int ok = 0;
    if (argc != 4) { fprintf(stderr, "usage: %s vmlinuz-virt initramfs.cpio cmdline.bin\n", argv[0]); return 2; }
    if (load(argv[1], &bz, LinuxRamBytes) && load(argv[2], &rd, LinuxRamBytes) &&
        load(argv[3], &cmd, LinuxPage)) {
        ram = malloc(LinuxRamBytes);
        if (ram) ok = exercise(ram, bz, rd, cmd);
        else perror("guest allocation");
    }
    free(ram); free(cmd.p); free(rd.p); free(bz.p);
    return ok ? 0 : 1;
}
