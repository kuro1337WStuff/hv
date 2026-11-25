#ifndef HV_LINUX_LAYOUT_H
#define HV_LINUX_LAYOUT_H

#include <stddef.h>
#include <stdint.h>
#ifdef _MSC_VER
#include <intrin.h>
#endif

/* Fixed PC, one CPU, ordinary bzImage (not the EFI entry point). All constants
 * and layout outputs are GPAs, never host physical addresses or C pointers. */
enum {
    LinuxRamBytes = 128 * 1024 * 1024, LinuxPage = 4096, LinuxLargePage = 0x200000,
    LinuxHoleStart = 0xa0000, LinuxHighRam = 0x100000,
    LinuxParams = 0x10000, LinuxCmdline = 0x11000, LinuxGdt = 0x12000,
    LinuxPml4 = 0x13000, LinuxPdpt = 0x14000, LinuxPd = 0x15000, LinuxPt = 0x16000,
    LinuxStackTop = 0x18000, LinuxCs = 0x10, LinuxDs = 0x18, LinuxGdtLimit = 0x1f
};

typedef struct {
    uint32_t kernel, kernel_bytes, init_bytes, setup_bytes;
    uint32_t initrd, entry;
} LinuxLayout;

static inline uint16_t linux_le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}
static inline uint32_t linux_le32(const uint8_t *p)
{
    return linux_le16(p) | ((uint32_t)linux_le16(p + 2) << 16);
}
static inline uint64_t linux_le64(const uint8_t *p)
{
    return linux_le32(p) | ((uint64_t)linux_le32(p + 4) << 32);
}
static inline void linux_put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static inline void linux_put64(uint8_t *p, uint64_t v)
{
    linux_put32(p, (uint32_t)v); linux_put32(p + 4, (uint32_t)(v >> 32));
}
static inline void linux_clear(uint8_t *p, size_t n)
{
#ifdef _MSC_VER
    __stosb(p, 0, n);
#else
    while (n--) *p++ = 0;
#endif
}
static inline void linux_copy(uint8_t *p, const uint8_t *src, size_t n)
{
#ifdef _MSC_VER
    __movsb(p, src, n);
#else
    while (n--) *p++ = *src++;
#endif
}

static inline void linux_e820(uint8_t *p, uint32_t base, uint32_t bytes, uint32_t type)
{
    linux_put64(p, base); linux_put64(p + 8, bytes); linux_put32(p + 16, type);
}

/* arch/x86/boot.rst, 64-bit boot protocol; arch/x86/include/uapi/asm/bootparam.h.
 * ram is an exclusively owned linear host view, indexed by guest byte offset.
 * Its conventional-memory hole is neither accessed nor mapped. Inputs and out
 * must be disjoint from ram and out must not alias inputs. On failure neither
 * ram nor out is changed. cmd_bytes includes exactly one trailing NUL.
 * On success enter long mode with paging enabled, CR3=LinuxPml4, IF clear,
 * RIP=entry, RSI=LinuxParams, RSP=LinuxStackTop, GDTR={LinuxGdt,LinuxGdtLimit},
 * CS=LinuxCs and DS/ES/SS=LinuxDs. Guest paging here is NOT an L1 NPT. */
static inline const char *build_linux_layout(uint8_t *ram, size_t ram_bytes,
    const uint8_t *image, size_t image_bytes, const uint8_t *rd, size_t rd_bytes,
    const uint8_t *cmd, size_t cmd_bytes, LinuxLayout *out)
{
    uint32_t header_end, setup, kernel, code, init, align, payload, payload_bytes, initrd;
    uint64_t pref, ceiling;
    uint8_t *bp;
    if (!ram || !image || !rd || !cmd || !out || ram_bytes != LinuxRamBytes)
        return "missing buffer or wrong fixed guest span";
    /* The jump displacement bounds the bytes copied into the zero page. Only
     * the known 2.15 header is in scope: never copy setup code after it. */
    if (image_bytes < 0x26c || linux_le16(image + 0x1fe) != 0xaa55 ||
        image[0x200] != 0xeb || linux_le32(image + 0x202) != UINT32_C(0x53726448) ||
        linux_le16(image + 0x206) != 0x020f) return "not a Linux 2.15 bzImage";
    header_end = 0x202u + image[0x201];
    setup = (1u + (image[0x1f1] ? image[0x1f1] : 4u)) * 512u;
    if (header_end != 0x26c || setup < header_end || setup > 32768 || setup >= image_bytes)
        return "invalid setup extent";
    if (!(linux_le16(image + 0x236) & 1) || !(image[0x211] & 1) || !image[0x234])
        return "requires relocatable, high-loaded x86-64 bzImage";

    /* syssize is in paragraphs. This pinned image has no appended signature
     * or trailer outside the protected-mode image. Copy the decompressor too,
     * not merely its compressed sub-payload described by 0x248/0x24c. */
    if ((uint64_t)linux_le32(image + 0x1f4) * 16 != image_bytes - setup ||
        image_bytes - setup > LinuxRamBytes) return "protected image size mismatch";
    code = (uint32_t)(image_bytes - setup);
    payload = linux_le32(image + 0x248); payload_bytes = linux_le32(image + 0x24c);
    if (code <= 0x200 || !payload || !payload_bytes || payload > code || payload_bytes > code - payload)
        return "invalid protected payload extent";
    pref = linux_le64(image + 0x258); align = linux_le32(image + 0x230);
    init = linux_le32(image + 0x260);
    if (align < LinuxPage || (align & (align - 1)) || (pref & (align - 1)) ||
        image[0x235] >= 32 || (UINT32_C(1) << image[0x235]) > align ||
        pref < LinuxHighRam || pref >= LinuxRamBytes || init < code || init > LinuxRamBytes - pref)
        return "invalid aligned kernel initialization window";
    kernel = (uint32_t)pref;
    if (!cmd_bytes || cmd_bytes > LinuxPage || cmd_bytes - 1 > linux_le32(image + 0x238) ||
        cmd[cmd_bytes - 1] != 0) return "invalid command line length or terminator";
    for (size_t i = 0; i < cmd_bytes - 1; ++i)
        if (!cmd[i]) return "embedded command line terminator";
    ceiling = (uint64_t)linux_le32(image + 0x22c) + 1; /* inclusive last byte -> exclusive end */
    if (ceiling > LinuxRamBytes) ceiling = LinuxRamBytes;
    if (!rd_bytes || rd_bytes > ceiling) return "initramfs exceeds ceiling";
    initrd = (uint32_t)(ceiling - rd_bytes) & ~(uint32_t)(LinuxPage - 1);
    if (initrd < kernel + init) return "initramfs overlaps kernel initialization window";

    /* Scrub every otherwise uninitialized RAM byte before disclosure to L2.
     * Large staging images are copied just once into their final positions. */
    linux_clear(ram, LinuxHoleStart);
    linux_clear(ram + LinuxHighRam, kernel - LinuxHighRam);
    linux_copy(ram + kernel, image + setup, code);
    linux_clear(ram + kernel + code, initrd - (kernel + code));
    linux_copy(ram + initrd, rd, rd_bytes);
    linux_clear(ram + initrd + rd_bytes, LinuxRamBytes - initrd - rd_bytes);
    linux_copy(ram + LinuxCmdline, cmd, cmd_bytes);

    bp = ram + LinuxParams;
    linux_copy(bp + 0x1f1, image + 0x1f1, header_end - 0x1f1);
    bp[0x210] = 0xff; /* unassigned loader */
    bp[0x211] = 1;    /* LOADED_HIGH, no 16-bit heap, no KEEP_SEGMENTS */
    linux_put32(bp + 0x208, 0); /* no realmode_swtch hook */
    linux_put32(bp + 0x214, kernel);
    linux_put32(bp + 0x218, initrd); linux_put32(bp + 0x21c, (uint32_t)rd_bytes);
    linux_put32(bp + 0x224, 0); /* heap_end_ptr, ext_loader_ver/type */
    linux_put32(bp + 0x228, LinuxCmdline);
    linux_put32(bp + 0x23c, 0); linux_put64(bp + 0x240, 0); /* ordinary x86/PC */
    linux_put64(bp + 0x250, 0); /* no setup_data list */
    bp[0x1e8] = 3;
    linux_e820(bp + 0x2d0, 0, LinuxHoleStart, 1);
    linux_e820(bp + 0x2d0 + 20, LinuxHoleStart, LinuxHighRam - LinuxHoleStart, 2);
    linux_e820(bp + 0x2d0 + 40, LinuxHighRam, LinuxRamBytes - LinuxHighRam, 1);

    linux_put64(ram + LinuxGdt + LinuxCs, UINT64_C(0x00af9b000000ffff)); /* L=1, D=0 */
    linux_put64(ram + LinuxGdt + LinuxDs, UINT64_C(0x00cf93000000ffff)); /* flat RW */
    linux_put64(ram + LinuxPml4, LinuxPdpt | 3); linux_put64(ram + LinuxPdpt, LinuxPd | 3);
    linux_put64(ram + LinuxPd, LinuxPt | 3); /* split low 2 MiB around reserved hole */
    for (uint32_t gpa = 0; gpa < LinuxLargePage; gpa += LinuxPage)
        if (gpa < LinuxHoleStart || gpa >= LinuxHighRam)
            linux_put64(ram + LinuxPt + 8 * (gpa / LinuxPage), gpa | 3);
    for (uint32_t gpa = LinuxLargePage; gpa < LinuxRamBytes; gpa += LinuxLargePage)
        linux_put64(ram + LinuxPd + 8 * (gpa / LinuxLargePage), gpa | 0x83);

    out->kernel = kernel; out->kernel_bytes = code; out->init_bytes = init;
    out->setup_bytes = setup; out->initrd = initrd; out->entry = kernel + 0x200;
    return NULL;
}
#endif
