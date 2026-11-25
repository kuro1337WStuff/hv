/* BSP-only, x64 Gen2 lab image. The post-exit side receives no EFI pointers. */
#include <Uefi.h>
#include <Protocol/GraphicsOutput.h>
#include <intrin.h>

#ifdef HV_STANDALONE_L2
#include "standalone_l2.h"
#define ARENA_PAGES ((UINTN)StandaloneGuestPages)
#else
#define ARENA_PAGES 8u
#endif
#define STACK_PAGES 4u
#define TOTAL_PAGES (ARENA_PAGES + STACK_PAGES)
#define COM1 0x3f8

extern void L1PrepareIdt(void);
extern void L1DisableInterrupts(void);
__declspec(noreturn) extern void L1Enter(UINTN stack_top);
__declspec(noreturn) extern void L1Halt(void);

static EFI_PHYSICAL_ADDRESS owned_base;
static UINTN arena_used;
static __declspec(align(16)) UINT8 map_storage[128 * 1024];
static volatile UINT32 *pixels;
static UINT32 width, height, pitch;
#ifdef HV_STANDALONE_CONTRACT
#define LOG_ROWS 24u
#elif defined(HV_PACKAGE)
#define LOG_ROWS 42u
#define LOG_Y(i) (80u + 16u * (i))
#elif defined(HV_ENTRY_TEST)
#define LOG_ROWS 35u /* 044 observations plus two refusals and restored state */
#define LOG_Y(i) (80u + 16u * (i))
#elif defined(HV_INPUT_TEST)
#define LOG_ROWS 33u /* run 044 rows plus bad input ID and entry count for four arms */
#define LOG_Y(i) (80u + 16u * (i))
#elif defined(HV_LINUX_ADMISSION)
#define LOG_ROWS 25u /* handoff, three five-field controls, nine Linux observations */
#elif defined(HV_STANDALONE_L2) && defined(HV_NPT_WRITE)
#define LOG_ROWS 11u  /* HANDOFF and ten sampled values */
#define NPT_INFO_LABEL "EXITINFO1 "
#elif defined(HV_STANDALONE_L2) && defined(HV_NPT_REMAP)
#define LOG_ROWS 10u  /* HANDOFF, six arm observations, entries and two backing words */
#define REMAP_BACKING_LABEL "BACKING A "
#elif defined(HV_STANDALONE_L2) && defined(HV_CPUID_RESUME)
#define LOG_ROWS 10u  /* HANDOFF and nine sampled values */
#else
#define LOG_ROWS 4u
#endif
#ifndef LOG_Y
#define LOG_Y(i) (80u + 20u * (i))
#endif
static const char *log_lines[LOG_ROWS];
static unsigned logged;

/* Five-column alphabet used by firmware/cross/main.c. Drawing uses only
 * copied mode geometry and the framebuffer, never the GOP driver. */
static const UINT8 letters[][5] = {
    {126,17,17,17,126},{127,73,73,73,54},{62,65,65,65,34},{127,65,65,34,28},
    {127,73,73,73,65},{127,9,9,9,1},{62,65,73,73,122},{127,8,8,8,127},
    {0,65,127,65,0},{32,64,65,63,1},{127,8,20,34,65},{127,64,64,64,64},
    {127,2,12,2,127},{127,4,8,16,127},{62,65,65,65,62},{127,9,9,9,6},
    {62,65,81,33,94},{127,9,25,41,70},{70,73,73,73,49},{1,1,127,1,1},
    {63,64,64,64,63},{31,32,64,32,31},{63,64,56,64,63},{99,20,8,20,99},
    {7,8,112,8,7},{97,81,73,69,67}
};

static void draw_line(UINTN y, const char *s, UINT32 color)
{
    static const UINT8 digits[][5] = {
        {62,81,73,69,62},{0,66,127,64,0},{66,97,81,73,70},{33,65,69,75,49},
        {24,20,18,127,16},{39,69,69,69,57},{60,74,73,73,48},{1,113,9,5,3},
        {54,73,73,73,54},{6,73,73,41,30}
    };
    static const UINT8 colon[] = {0,0,36,0,0};
    static const UINT8 underscore[] = {64,64,64,64,64};
    for (UINTN x = 20; *s && x + 10 <= width; ++s, x += 12) {
        const UINT8 *glyph = *s >= 'A' && *s <= 'Z' ? letters[*s - 'A'] :
            *s >= '0' && *s <= '9' ? digits[*s - '0'] :
            *s == ':' ? colon : *s == '_' ? underscore : NULL;
        if (!glyph) continue;
        for (unsigned col = 0; col < 5; ++col) {
            for (unsigned row = 0; row < 7; ++row) {
                if (glyph[col] & (1u << row)) {
                    volatile UINT32 *p = pixels + (y + 2 * row) * pitch + x + 2 * col;
                    p[0] = p[1] = p[pitch] = p[pitch + 1] = color;
                }
            }
        }
    }
}

static void screen(const char *title, const char *reason, UINT32 color)
{
    if (!pixels) return;
    for (UINTN y = 0; y < height; ++y) {
        volatile UINT32 *row = pixels + y * pitch;
        for (UINTN x = 0; x < width; ++x) row[x] = 0;
    }
    draw_line(20, title, color);
    if (reason) draw_line(48, reason, 0xffffff);
    for (unsigned i = 0; i < logged; ++i) draw_line(LOG_Y(i), log_lines[i], 0xffffff);
    _mm_sfence();
}

static int use_gop(EFI_GRAPHICS_OUTPUT_PROTOCOL *gop)
{
    if (!gop || !gop->Mode || !gop->Mode->Info ||
        gop->Mode->SizeOfInfo < sizeof(EFI_GRAPHICS_OUTPUT_MODE_INFORMATION)) return 0;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *m = gop->Mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *i = m->Info;
    if (i->Version != 0 || (i->PixelFormat != PixelBlueGreenRedReserved8BitPerColor &&
        i->PixelFormat != PixelRedGreenBlueReserved8BitPerColor)) return 0;
#if defined(HV_PACKAGE) || defined(HV_LINUX_ADMISSION)
    if (i->HorizontalResolution < 20u + 12u * 32u) return 0;
#elif defined(HV_STANDALONE_L2) && defined(HV_NPT_WRITE)
    if (i->HorizontalResolution < 20u + 12u * (sizeof(NPT_INFO_LABEL) - 1u + 16u)) return 0;
#elif defined(HV_STANDALONE_L2) && defined(HV_NPT_REMAP)
    if (i->HorizontalResolution < 20u + 12u * (sizeof(REMAP_BACKING_LABEL) - 1u + 16u)) return 0;
#endif
    if (i->HorizontalResolution < 320 || i->VerticalResolution < 200 ||
        i->VerticalResolution < LOG_Y(LOG_ROWS - 1) + 14 ||
        i->PixelsPerScanLine < i->HorizontalResolution || !m->FrameBufferBase ||
        (m->FrameBufferBase & 3) || m->FrameBufferSize > MAX_UINTN - m->FrameBufferBase ||
        (UINT64)i->PixelsPerScanLine * i->VerticalResolution > m->FrameBufferSize / 4) return 0;
    width = i->HorizontalResolution;
    height = i->VerticalResolution;
    pitch = i->PixelsPerScanLine;
    pixels = (volatile UINT32 *)(UINTN)m->FrameBufferBase;
    return 1;
}

static int find_screen(EFI_BOOT_SERVICES *bs, EFI_HANDLE console)
{
    EFI_GUID guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop;
    if (bs->HandleProtocol(console, &guid, (void **)&gop) == EFI_SUCCESS && use_gop(gop)) return 1;
    /* Some console splitters expose Blt only. Try the backing adapter. */
    EFI_HANDLE *handles;
    UINTN count;
    if (bs->LocateHandleBuffer(ByProtocol, &guid, NULL, &count, &handles) != EFI_SUCCESS) return 0;
    for (UINTN n = 0; n < count; ++n) {
        if (bs->HandleProtocol(handles[n], &guid, (void **)&gop) == EFI_SUCCESS && use_gop(gop)) break;
    }
    bs->FreePool(handles);
    return pixels != NULL;
}

#ifndef HV_STANDALONE_L2
static void uart_init(void)
{
    __outbyte(COM1 + 3, 3);    /* DLAB clear, 8N1 */
    __outbyte(COM1 + 1, 0);    /* polling only */
    __outbyte(COM1 + 3, 0x83);
    __outbyte(COM1, 1);        /* 115200 */
    __outbyte(COM1 + 1, 0);
    __outbyte(COM1 + 3, 3);
    __outbyte(COM1 + 2, 7);    /* enable FIFO, discard firmware traffic */
    __outbyte(COM1 + 4, 3);    /* DTR, RTS */
}

static void wait_request(void)
{
    for (;;) {
        UINT8 lsr = __inbyte(COM1 + 5);
        if (lsr != 0xff && (lsr & 1)) {
            UINT8 c = __inbyte(COM1);
            if (!(lsr & 0x1e) && c == '?') return;
        }
        for (unsigned i = 0; i < 256; ++i) _mm_pause();
    }
}

static int uart_ready(UINT8 mask)
{
    for (unsigned tries = 0; tries < 1000000; ++tries) {
        UINT8 lsr = __inbyte(COM1 + 5);
        if (lsr == 0xff) return 0;
        if (lsr & mask) return 1;
        _mm_pause();
    }
    return 0;
}

static int send_text(const char *s)
{
    while (*s) {
        if (!uart_ready(0x20)) return 0;
        __outbyte(COM1, (UINT8)*s++);
    }
    return 1;
}
#else
/* Callers reserve space for the label, 16 hex digits and the terminator. */
static void log_hex(char *line, const char *label, UINT64 value)
{
    static const char hex[] = "0123456789ABCDEF";
    log_lines[logged++] = line;
    while (*label) *line++ = *label++;
    for (unsigned i = 16; i; value >>= 4) line[--i] = hex[value & 15];
    line[16] = '\0';
}
#endif

__declspec(noreturn) static void fail(const char *reason)
{
    L1DisableInterrupts();
#if defined(HV_FLOOR_CONTRACT)
    screen("L1 CPU FLOOR FAIL", reason, 0xff00ff);
#elif defined(HV_PAGE_PAIR)
    screen("L1 PAGE PAIR FAIL", reason, 0xff00ff);
#elif defined(HV_CR4_GUARD)
    screen("L1 CR4 GUARD FAIL", reason, 0xff00ff);
#elif defined(HV_CPU_CONTRACT)
    screen("L1 CPU CONTRACT FAIL", reason, 0xff00ff);
#elif defined(HV_SERIAL_CONTRACT)
    screen("L1 SERIAL CONTRACT FAIL", reason, 0xff00ff);
#elif defined(HV_TICK_CONTRACT)
    screen("L1 PIT IRQ0 FAIL", reason, 0xff00ff);
#elif defined(HV_TX_CONTRACT)
    screen("L1 CONSOLE TX FAIL", reason, 0xff00ff);
#elif defined(HV_POWER_CONTRACT)
    screen("L1 POWER S5 FAIL", reason, 0xff00ff);
#elif defined(HV_PACKAGE)
    screen("L1 PACKAGE FAIL", reason, 0xff00ff);
#elif defined(HV_ENTRY_TEST)
    screen("L1 ENTRY GUARD FAIL", reason, 0xff00ff);
#elif defined(HV_INPUT_TEST)
    screen("L1 STAGED INPUT FAIL", reason, 0xff00ff);
#elif defined(HV_LINUX_ADMISSION)
    screen("L1 LINUX ADMISSION FAIL", reason, 0xff00ff);
#elif defined(HV_STANDALONE_L2) && defined(HV_NPT_WRITE)
    screen("L1 NPT WRITE FAIL", reason, 0xff00ff);
#elif defined(HV_STANDALONE_L2) && defined(HV_NPT_REMAP)
    screen("L1 NPT REMAP FAIL", reason, 0xff00ff);
#elif defined(HV_STANDALONE_L2) && defined(HV_CPUID_RESUME)
    screen("L1 CPUID FAIL", reason, 0xff00ff);
#elif defined(HV_STANDALONE_L2)
    screen("L1 L2 FAIL", reason, 0xff00ff);
#else
    screen("L1 HANDOFF FAIL", reason, 0xff00ff);
    wait_request();
    if (send_text("HV:FAIL:") && send_text(reason) && send_text("\r\n")) (void)uart_ready(0x40);
#endif
    L1Halt();
}

#if defined(HV_PACKAGE) || defined(HV_LINUX_ADMISSION)
static void pkg_observe(const char *label, UINT64 value)
{
    static char lines[LOG_ROWS][12 + 16 + 1];
    UINTN len=0;
    while (label[len] && len<12) ++len;
    if (label[len] || logged>=LOG_ROWS) fail("OBSERVATION LIMIT");
    log_hex(lines[logged],label,value);
}

#endif
#if defined(HV_PACKAGE) && !defined(HV_STANDALONE_CONTRACT)
static void pkg_text(const char *text)
{
    if (logged>=LOG_ROWS) fail("OBSERVATION LIMIT");
    log_lines[logged++]=text;
}
#endif

/* Only whole pages from the reserved arena; the adjacent stack is excluded.
 * A rejected request cannot change the cursor, including for huge counts. */
__declspec(noinline) static void *take_pages(UINTN count)
{
    if (!count || arena_used > ARENA_PAGES || count > ARENA_PAGES - arena_used) return NULL;
    UINTN start = (UINTN)owned_base + arena_used * EFI_PAGE_SIZE;
    arena_used += count;
    return (void *)start;
}

__declspec(noreturn) void L1OwnedMain(void)
{
    volatile UINTN stack_probe = 0;
    UINTN bottom = (UINTN)owned_base + ARENA_PAGES * EFI_PAGE_SIZE;
    log_lines[logged++] = "HV:HANDOFF";
    if ((UINTN)&stack_probe < bottom || (UINTN)&stack_probe >= bottom + STACK_PAGES * EFI_PAGE_SIZE)
        fail("STACK");
#ifdef HV_STANDALONE_L2
    void *reservation = take_pages(StandaloneGuestPages);
    if (!reservation || (UINTN)reservation != owned_base || arena_used != ARENA_PAGES) fail("ARENA");
#ifdef HV_LINUX_ADMISSION
    const char *reason=run_standalone_l2(reservation);
    if (reason) fail(reason);
#ifdef HV_INPUT_TEST
    screen("L1 STAGED INPUT PASS", "FETCH DENIED: NO LINUX BOOT", 0x00ff00);
#elif defined(HV_ENTRY_TEST)
    screen("L1 ENTRY GUARD PASS", "FETCH DENIED: NO LINUX BOOT", 0x00ff00);
#else
    screen("L1 LINUX ADMISSION PASS", "FETCH DENIED: NO LINUX BOOT", 0x00ff00);
#endif
#elif defined(HV_STANDALONE_CONTRACT)
    const char *reason=run_standalone_l2(reservation);
    if (reason) fail(reason);
#ifdef HV_SERIAL_CONTRACT
    screen("L1 SERIAL CONTRACT PASS", "POLLED COM1: FIXED GUEST", 0x00ff00);
#elif defined(HV_TICK_CONTRACT)
    screen("L1 PIT IRQ0 PASS", "TWO IRQ0: SPECIFIC PIC EOI", 0x00ff00);
#elif defined(HV_TX_CONTRACT)
    screen("L1 CONSOLE TX PASS", "TWO PHASES: BOUNDED CAPTURE", 0x00ff00);
#elif defined(HV_POWER_CONTRACT)
    screen("L1 POWER S5 PASS", "FIXED GUEST: VIRTUAL POWER", 0x00ff00);
#elif defined(HV_FLOOR_CONTRACT)
    screen("L1 CPU FLOOR PASS", "FIXED GUEST: FEATURE FLOOR", 0x00ff00);
#elif defined(HV_PAGE_PAIR)
    screen("L1 PAGE PAIR PASS", "GUEST PF AND NESTED FAULT", 0x00ff00);
#elif defined(HV_CR4_GUARD)
    screen("L1 CR4 GUARD PASS", "WRITE REFUSED BEFORE EFFECT", 0x00ff00);
#else
    screen("L1 CPU CONTRACT PASS", "ONE VIRTUAL CPU: FIXED GUEST", 0x00ff00);
#endif
#else
    StandaloneSample sample;
    const char *reason = run_standalone_l2(reservation, &sample);
#ifdef HV_PACKAGE
    pkg_observe("COMPLETED ", sample.completed);
    if (reason) fail(reason);
    screen("L1 PACKAGE PASS", NULL, 0x00ff00);
#elif defined(HV_NPT_WRITE)
    static char lines[LOG_ROWS - 1][sizeof(NPT_INFO_LABEL) + 16];
    unsigned n = 0;
    log_hex(lines[n++], "RW EXIT ", sample.rw_exit);
    log_hex(lines[n++], "RW RIP ", sample.rw_rip);
    log_hex(lines[n++], "RW BYTE ", sample.rw_byte);
    log_hex(lines[n++], "RO EXIT ", sample.ro_exit);
    log_hex(lines[n++], "RO RIP ", sample.ro_rip);
    log_hex(lines[n++], NPT_INFO_LABEL, sample.info1);
    log_hex(lines[n++], "GPA ", sample.gpa);
    log_hex(lines[n++], "BEFORE ", sample.before);
    log_hex(lines[n++], "AFTER ", sample.after);
    log_hex(lines[n++], "ENTRIES ", sample.entries);
    if (reason) fail(reason);
    screen("L1 NPT WRITE PASS", NULL, 0x00ff00);
#elif defined(HV_NPT_REMAP)
    static char lines[LOG_ROWS - 1][sizeof(REMAP_BACKING_LABEL) + 16];
    unsigned n = 0;
    log_hex(lines[n++], "A EXIT ", sample.arm[0].exit);
    log_hex(lines[n++], "A RIP ", sample.arm[0].ip);
    log_hex(lines[n++], "A RAX ", sample.arm[0].ax);
    log_hex(lines[n++], "B EXIT ", sample.arm[1].exit);
    log_hex(lines[n++], "B RIP ", sample.arm[1].ip);
    log_hex(lines[n++], "B RAX ", sample.arm[1].ax);
    log_hex(lines[n++], "ENTRIES ", sample.entries);
    log_hex(lines[n++], REMAP_BACKING_LABEL, sample.backing_a);
    log_hex(lines[n++], "BACKING B ", sample.backing_b);
    if (reason) fail(reason);
    screen("L1 NPT REMAP PASS", NULL, 0x00ff00);
#else
    static char lines[LOG_ROWS - 1][sizeof("EXITCODE ") + 16];
    unsigned n = 0;
#ifdef HV_CPUID_RESUME
    log_hex(lines[n++], "1ST EXIT ", sample.first_exit);
    log_hex(lines[n++], "1ST RIP ", sample.first_rip);
    log_hex(lines[n++], "LEAF ", sample.leaf);
    log_hex(lines[n++], "SUBLEAF ", sample.subleaf);
    log_hex(lines[n++], sample.nrips ? "TO NRIP " : "TO FIXED ", sample.resume_rip);
    log_hex(lines[n++], "ENTRIES ", sample.entries);
#endif
    log_hex(lines[n++], "EXITCODE ", sample.exitcode);
    log_hex(lines[n++], "RIP ", sample.rip);
    log_hex(lines[n++], "RAX ", sample.rax);
    if (reason) fail(reason);
#ifdef HV_CPUID_RESUME
    screen("L1 CPUID PASS", NULL, 0x00ff00);
#else
    screen("L1 L2 PASS", NULL, 0x00ff00);
#endif
#endif
#endif
    L1Halt();
#else
    volatile UINT32 *page = take_pages(1);
    if ((UINTN)page != owned_base || arena_used != 1) fail("ARENA");
    page[0] = 0x13579bdf;
    page[EFI_PAGE_SIZE / sizeof(*page) - 1] = 0x2468ace0;
    if (page[0] != 0x13579bdf || page[EFI_PAGE_SIZE / sizeof(*page) - 1] != 0x2468ace0) fail("MEMORY");
    log_lines[logged++] = "HV:OWNED";
    UINTN before = arena_used;
    if (take_pages(ARENA_PAGES + 1) || arena_used != before) fail("OOM CURSOR");
    if ((UINTN)take_pages(1) != owned_base + EFI_PAGE_SIZE || arena_used != before + 1) fail("OOM RECOVERY");
    log_lines[logged++] = "HV:EXPECTED_OOM";
    screen("L1 HANDOFF WAIT", "SERIAL CAPTURE", 0xffffff);
    wait_request();
    log_lines[logged++] = "HV:HALT";
    for (unsigned i = 0; i < logged; ++i) {
        if (!send_text(log_lines[i]) || !send_text("\r\n")) goto tx_failed;
    }
    if (!uart_ready(0x40)) goto tx_failed;  /* transmitter, not just FIFO, empty */
    screen("L1 HANDOFF PASS", NULL, 0x00ff00);
    L1Halt();
tx_failed:
    screen("L1 HANDOFF FAIL", "SERIAL TX", 0xff00ff);
    L1Halt();
#endif
}

/* AllocatePages returned one contiguous reservation. Its backing descriptor
 * may also cover neighboring loader pages. Do not claim any of those. */
static int map_covers_reservation(UINTN bytes, UINTN stride, UINT32 version)
{
    if (bytes > sizeof(map_storage) || version != EFI_MEMORY_DESCRIPTOR_VERSION ||
        stride < sizeof(EFI_MEMORY_DESCRIPTOR) || (stride & 7) || bytes % stride) return 0;
    for (UINTN n = 0; n < bytes; n += stride) {
        const EFI_MEMORY_DESCRIPTOR *d = (const EFI_MEMORY_DESCRIPTOR *)(map_storage + n);
        if (d->Type == EfiLoaderData && !(d->Attribute & EFI_MEMORY_RUNTIME) &&
            !(d->PhysicalStart & (EFI_PAGE_SIZE - 1)) && d->PhysicalStart <= owned_base &&
            d->NumberOfPages >= TOTAL_PAGES + (owned_base - d->PhysicalStart) / EFI_PAGE_SIZE) return 1;
    }
    return 0;
}

EFI_STATUS EFIAPI EfiMain(EFI_HANDLE image, EFI_SYSTEM_TABLE *st)
{
    EFI_BOOT_SERVICES *bs = st->BootServices;
    int regs[4];
    __cpuid(regs, 0x40000000);
    /* No hardware bring-up on physical machines or other hypervisors. */
    if (regs[1] != 0x7263694d || regs[2] != 0x666f736f || regs[3] != 0x76482074) return EFI_UNSUPPORTED;
    EFI_STATUS watchdog = bs->SetWatchdogTimer(0, 0, 0, NULL);
#ifndef HV_STANDALONE_L2
    uart_init();
#endif
    if (!find_screen(bs, st->ConsoleOutHandle)) {
#if defined(HV_STANDALONE_L2) && defined(HV_NPT_WRITE)
        if (st->ConOut) st->ConOut->OutputString(st->ConOut, L"L1 NPT WRITE FAIL: NO DIRECT FRAMEBUFFER\r\n");
#elif defined(HV_STANDALONE_L2) && defined(HV_CPUID_RESUME)
        if (st->ConOut) st->ConOut->OutputString(st->ConOut, L"L1 CPUID FAIL: NO DIRECT FRAMEBUFFER\r\n");
#elif defined(HV_STANDALONE_L2)
        if (st->ConOut) st->ConOut->OutputString(st->ConOut, L"L1 L2 FAIL: NO DIRECT FRAMEBUFFER\r\n");
#else
        if (st->ConOut) st->ConOut->OutputString(st->ConOut, L"L1 HANDOFF FAIL: NO DIRECT FRAMEBUFFER\r\n");
#endif
        fail("FRAMEBUFFER");
    }
    if (watchdog != EFI_SUCCESS) fail("WATCHDOG");
    if (bs->AllocatePages(AllocateAnyPages, EfiLoaderData, TOTAL_PAGES, &owned_base) != EFI_SUCCESS ||
        !owned_base || (owned_base & (EFI_PAGE_SIZE - 1)) || owned_base > MAX_UINTN - TOTAL_PAGES * EFI_PAGE_SIZE)
        fail("RESERVATION");
#ifdef HV_LINUX_ADMISSION
    const char *media_error=linux_read_media(image,st,linux_stage((UINT8 *)(UINTN)owned_base));
    if (media_error) fail(media_error);
#endif
    L1PrepareIdt();
    /* Nothing below this point calls EFI except GetMemoryMap/ExitBootServices.
     * The fixed map buffer also survives a partially completed EFI shutdown. */
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
        UINTN bytes = sizeof(map_storage), key, stride;
        UINT32 version;
        EFI_STATUS s = bs->GetMemoryMap(&bytes, (EFI_MEMORY_DESCRIPTOR *)map_storage, &key, &stride, &version);
        if (s != EFI_SUCCESS || !map_covers_reservation(bytes, stride, version)) fail("MEMORY MAP");
        s = bs->ExitBootServices(image, key);
        if (s == EFI_SUCCESS) L1Enter((UINTN)owned_base + TOTAL_PAGES * EFI_PAGE_SIZE);
        if (s != EFI_INVALID_PARAMETER) fail("EXIT SERVICES");
    }
    fail("STALE MAP KEY");
}
