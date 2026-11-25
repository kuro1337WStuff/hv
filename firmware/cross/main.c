/* Removable-media, two-processor L1. Limine owns AP startup; only the BSP draws. */
#include <stdint.h>
#include <limine.h>
#include "svm.h"

#define REQUEST __attribute__((used,section(".limine_requests")))
__attribute__((used,section(".limine_requests_start"))) static volatile LIMINE_REQUESTS_START_MARKER
REQUEST static volatile LIMINE_BASE_REVISION(3)
REQUEST static volatile struct limine_smp_request smp = { .id=LIMINE_SMP_REQUEST };
REQUEST static volatile struct limine_kernel_address_request image = { .id=LIMINE_KERNEL_ADDRESS_REQUEST };
REQUEST static volatile struct limine_framebuffer_request screen = { .id=LIMINE_FRAMEBUFFER_REQUEST };
__attribute__((used,section(".limine_requests_end"))) static volatile LIMINE_REQUESTS_END_MARKER

/* Fixed uppercase console for the evidence screen; five columns per glyph. */
static const unsigned char glyph[][5]={
    {62,81,73,69,62},{0,66,127,64,0},{66,97,81,73,70},{33,65,69,75,49},
    {24,20,18,127,16},{39,69,69,69,57},{60,74,73,73,48},{1,113,9,5,3},
    {54,73,73,73,54},{6,73,73,41,30},
    {126,17,17,17,126},{127,73,73,73,54},{62,65,65,65,34},{127,65,65,34,28},
    {127,73,73,73,65},{127,9,9,9,1},{62,65,73,73,122},{127,8,8,8,127},
    {0,65,127,65,0},{32,64,65,63,1},{127,8,20,34,65},{127,64,64,64,64},
    {127,2,12,2,127},{127,4,8,16,127},{62,65,65,65,62},{127,9,9,9,6},
    {62,65,81,33,94},{127,9,25,41,70},{70,73,73,73,49},{1,1,127,1,1},
    {63,64,64,64,63},{31,32,64,32,31},{63,64,56,64,63},{99,20,8,20,99},
    {7,8,112,8,7},{97,81,73,69,67}
};
static struct limine_framebuffer *fb;
static unsigned x=16, y=16;
static void put(char c) {
    if (c=='\n') { x=16; y+=20; return; }
    unsigned i=c>='A' && c<='Z' ? (unsigned)(10+c-'A') : c>='0' && c<='9' ? (unsigned)(c-'0') : 36;
    if (i<36 && x+10<=fb->width && y+14<=fb->height) {
        for (unsigned j=0;j<5;++j) for (unsigned k=0;k<7;++k) if ((glyph[i][j]>>k)&1) {
            volatile uint32_t *p=(volatile uint32_t *)((char *)fb->address+(y+2*k)*fb->pitch)+x+2*j;
            p[0]=p[1]=p[fb->pitch/4]=p[fb->pitch/4+1]=0xffffff;
        }
    }
    x+=12;
}
static void text(const char *s) { while (*s) put(*s++); }
static void hex(uint64_t v) {
    static const char digits[]="0123456789ABCDEF";
    unsigned shift=60; while (shift && !(v>>shift)) shift-=4;
    for (;;) { put(digits[(v>>shift)&15]); if (!shift) return; shift-=4; }
}
__attribute__((noreturn)) static void halt(void) { for (;;) __asm__ volatile("cli; hlt"); }
__attribute__((noreturn)) static void fail(const char *why) { text("FAIL "); text(why); halt(); }

/* This lab has two xAPIC IDs; compare the local CPUID instruction with the loader's
 * routing identity. The roster itself is not evidence of execution on the AP. */
static uint32_t local_id(void) {
    uint32_t a=1,b,c,d;
    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(d));
    return b>>24;
}
static uint64_t ticks(void) {
    uint32_t lo,hi; __asm__ volatile("rdtsc":"=a"(lo),"=d"(hi));
    return ((uint64_t)hi<<32)|lo;
}
/* Limine does not hand over an IDT. The lab never enables maskable interrupts;
 * give each CPU a fail-stop exception target on its own stack instead of using
 * firmware vectors or rebooting on an unexpected nested-operation fault. */
struct __attribute__((packed)) gate {
    uint16_t low, cs; uint8_t ist, attr; uint16_t mid; uint32_t high, reserved;
};
static struct gate idt[2][256] __attribute__((aligned(16)));
static void install_idt(unsigned slot) {
    uintptr_t addr=(uintptr_t)halt;
    uint16_t cs;
    __asm__ volatile("mov %%cs,%0":"=r"(cs));
    for (unsigned i=0;i<256;++i) {
        struct gate *g=&idt[slot][i];
        g->cs=cs; g->low=(uint16_t)addr; g->mid=(uint16_t)(addr>>16);
        g->high=(uint32_t)(addr>>32); g->attr=0x8e;
    }
    const struct __attribute__((packed)) { uint16_t limit; uintptr_t base; }
        desc={sizeof(idt[slot])-1,(uintptr_t)idt[slot]};
    __asm__ volatile("lidt %0"::"m"(desc):"memory");
}
static unsigned request, completed;
static uint32_t peer_id;
static int peer_supported;
static struct cross_sample peer_read;
static void peer(struct limine_smp_info *info) {
    install_idt(1);
    peer_id=local_id(); peer_supported=(peer_id==info->lapic_id && cross_supported());
    __atomic_store_n(&completed,1,__ATOMIC_RELEASE);
    for (unsigned seq=2;seq<=3;++seq) {
        while (__atomic_load_n(&request,__ATOMIC_ACQUIRE)!=seq) __asm__ volatile("pause");
        peer_id=local_id();
        peer_read=cross_read(1);
        __atomic_store_n(&completed,seq,__ATOMIC_RELEASE);
    }
    halt();
}
static void wait_peer(unsigned seq) {
    uint64_t start=ticks();
    while (__atomic_load_n(&completed,__ATOMIC_ACQUIRE)!=seq) {
        if (ticks()-start>UINT64_C(20000000000)) fail("AP ACK TIMEOUT");
        __asm__ volatile("pause");
    }
}
static void verify(uint32_t actual,uint32_t expected,unsigned epoch,struct cross_sample s) {
    text("CPU "); hex(actual); text("  EXIT "); hex(s.exit); text("  RIP "); hex(s.rip); text("  AX "); hex(s.ax); put('\n');
    if (actual!=expected || s.exit!=0x78 || s.rip!=0x1003 || s.ax!=(epoch?0xb22b:0xa11a)) fail("WRONG CPU EXIT OR MARKER");
}
void cross_main(void) {
    if (!LIMINE_BASE_REVISION_SUPPORTED || !screen.response || !screen.response->framebuffer_count) halt();
    fb=screen.response->framebuffers[0];
    if (fb->bpp!=32 || fb->width<640 || fb->height<300 || fb->pitch%4) halt();
    install_idt(0);
    for (uint64_t row=0;row<fb->height;++row) {
        volatile uint32_t *p=(volatile uint32_t *)((char *)fb->address+row*fb->pitch);
        for (uint64_t col=0;col<fb->width;++col) p[col]=0;
    }
    text("AMD SVM    L1 NPT CROSS CPU\n");
    if (!image.response || !cross_supported()) fail("L1 LAB OR SVM UNAVAILABLE");
    struct limine_smp_response *mp=smp.response;
    if (!mp || mp->cpu_count!=2) fail("NEED TWO STARTED PROCESSORS");
    uint32_t bsp=local_id();
    struct limine_smp_info *ap=0;
    unsigned boot_matches=0;
    for (unsigned i=0;i<2;++i) {
        if (mp->cpus[i]->lapic_id==bsp) ++boot_matches; else ap=mp->cpus[i];
    }
    if (boot_matches!=1 || !ap || bsp!=mp->bsp_lapic_id || ap->lapic_id>255) fail("CPU ROUTING ID");
    __atomic_store_n(&ap->goto_address,peer,__ATOMIC_RELEASE);
    wait_peer(1);
    text("L1 LOCAL APIC IDS   BSP "); hex(bsp); text("   AP "); hex(peer_id); put('\n');
    if (!peer_supported || peer_id!=ap->lapic_id || peer_id==bsp) fail("AP ID OR SVM UNAVAILABLE");
    cross_init(image.response->virtual_base,image.response->physical_base);
    for (unsigned epoch=0;epoch<2;++epoch) {
        text(epoch?"FRAME B   GPA 2000\n":"FRAME A   GPA 2000\n");
        cross_map(epoch);
        verify(local_id(),bsp,epoch,cross_read(0));
        __atomic_store_n(&request,epoch+2,__ATOMIC_RELEASE);
        wait_peer(epoch+2);
        verify(peer_id,ap->lapic_id,epoch,peer_read);
    }
    text("PASS   BOTH CPUS READ A THEN B AFTER LOCAL FLUSHES\n");
    halt();
}
