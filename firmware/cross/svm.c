#include "svm.h"
#include "../fixed_npt.h"

/* AMD64 APM volume 2, appendix B: offsets from the VMCB page base. */
enum {
    Exceptions = 0x008, Intercepts = 0x00c, SvmIntercepts = 0x010,
    IopmPa = 0x040, MsrpmPa = 0x048, Asid = 0x058, TlbControl = 0x05c,
    IntControl = 0x060, ExitCode = 0x070, NpEnable = 0x090, NestedCr3 = 0x0b0,
    Es = 0x400, Cs = 0x410, Ds = 0x430, GuestEfer = 0x4d0, Cr0 = 0x558,
    Dr7 = 0x560, Dr6 = 0x568, Flags = 0x570, Rip = 0x578, Rsp = 0x5d8,
    Rax = 0x5f8, Pat = 0x668
};
#define FIELD(type, offset) (*(volatile type *)(vmcb + (offset)))
#define MSR_EFER 0xc0000080u
#define MSR_VM_CR 0xc0010114u
#define MSR_HSAVE 0xc0010117u
#define SVME (UINT64_C(1) << 12)

enum { DataGpa = 0x2000, IopmPages = 3, MsrpmPages = 2 };
struct cpu_pages {
    uint8_t vmcb[NptPageBytes];
    uint8_t hsave[NptPageBytes];
};
/* All members consist of whole pages in the kernel's contiguous, zero-filled BSS. */
_Alignas(NptPageBytes) static struct {
    struct cpu_pages cpu[2];
    FixedNpt npt;
    uint8_t code[NptPageBytes];
    uint16_t frame[2][NptPageBytes / sizeof(uint16_t)];
    uint64_t maps[IopmPages + MsrpmPages][NptPageBytes / sizeof(uint64_t)];
} pages;
_Static_assert(sizeof(FixedNpt) == NptTableCount * NptPageBytes, "NPT layout");
static uint64_t pa_offset;

static uint64_t pa(const void *kernel_pointer)
{
    return (uintptr_t)kernel_pointer + pa_offset;
}

struct cpuid_regs { uint32_t a, b, c, d; };
static inline struct cpuid_regs cpuid(uint32_t leaf)
{
    struct cpuid_regs r;
    __asm__ volatile ("cpuid" : "=a"(r.a), "=b"(r.b), "=c"(r.c), "=d"(r.d)
                      : "0"(leaf), "2"(0u));
    return r;
}

static inline uint64_t rdmsr(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t value)
{
    __asm__ volatile ("wrmsr" : : "c"(msr), "a"((uint32_t)value),
                      "d"((uint32_t)(value >> 32)) : "memory");
}

int cross_supported(void)
{
    struct cpuid_regs r = cpuid(0);
    if (r.a < 1 || r.b != 0x68747541 || r.d != 0x69746e65 || r.c != 0x444d4163)
        return 0; /* AuthenticAMD */
    r = cpuid(1);
    if (!(r.c & (1u << 31)) || !(r.d & (1u << 5)) || !(r.d & (1u << 16)))
        return 0; /* hypervisor present, MSR, PAT */
    r = cpuid(0x40000000);
    if (r.a < 0x40000001 || r.b != 0x7263694d || r.c != 0x666f736f ||
        r.d != 0x76482074 || cpuid(0x40000001).a != 0x31237648)
        return 0; /* Microsoft Hv, Hv#1 */
    if (cpuid(0x80000000).a < 0x8000000a || !(cpuid(0x80000001).c & 4))
        return 0;
    r = cpuid(0x8000000a);
    uint64_t cr4;
    __asm__ volatile ("mov %%cr4,%0" : "=r"(cr4));
    if (!(r.d & 1) || r.b < 2 || (cr4 & (1u << 12)))
        return 0; /* NP, available ASID 1, four-level host translation */
    return !(rdmsr(MSR_VM_CR) & (1u << 4)) && !(rdmsr(MSR_EFER) & SVME);
}

void cross_init(uint64_t kernel_va, uint64_t kernel_pa)
{
    pa_offset = kernel_pa - kernel_va;
    build_fixed_npt(&pages.npt, pa(&pages.npt), pa(pages.code));
    /* 16-bit MOV AX,moffs16; HLT. No stack, I/O, MSRs or other GPRs. */
    pages.code[0] = 0xa1;
    pages.code[1] = DataGpa & 0xff;
    pages.code[2] = DataGpa >> 8;
    pages.code[3] = 0xf4;
    pages.frame[0][0] = 0xa11a;
    pages.frame[1][0] = 0xb22b;
    for (unsigned i = 0; i < IopmPages + MsrpmPages; ++i) {
        volatile uint64_t *map = pages.maps[i];
        for (unsigned j = 0; j < NptPageBytes / sizeof(*map); ++j)
            map[j] = UINT64_MAX;
    }
    for (unsigned i = 0; i < 2; ++i) {
        uint8_t *vmcb = pages.cpu[i].vmcb;
        FIELD(uint32_t, Exceptions) = UINT32_MAX;
        FIELD(uint32_t, Intercepts) = 0x0fu | (1u << 24) | (1u << 27) | (1u << 28) | (1u << 31);
        FIELD(uint32_t, SvmIntercepts) = 0x7f; /* includes mandatory VMRUN intercept */
        FIELD(uint64_t, IopmPa) = pa(pages.maps);
        FIELD(uint64_t, MsrpmPa) = pa(pages.maps[IopmPages]);
        FIELD(uint32_t, Asid) = 1;
        FIELD(uint32_t, IntControl) = 1u << 24; /* V_INTR_MASKING */
        FIELD(uint64_t, NpEnable) = 1;
        FIELD(uint64_t, NestedCr3) = pa(&pages.npt);
        for (unsigned seg = Es; seg <= Ds; seg += 16) {
            FIELD(uint16_t, seg + 2) = seg == Cs ? 0x9b : 0x93;
            FIELD(uint32_t, seg + 4) = 0xffff;
        }
        FIELD(uint64_t, GuestEfer) = SVME;
        FIELD(uint64_t, Cr0) = 0x30;
        FIELD(uint64_t, Dr6) = 0xffff0ff0u;
        FIELD(uint64_t, Dr7) = 0x400;
        FIELD(uint64_t, Flags) = 2;
        FIELD(uint64_t, Rsp) = FixedGuestGpa + NptPageBytes - 16;
    }
}

void cross_map(unsigned epoch)
{
    /* Only these two read-only frames can ever back the fixed data GPA. */
    __atomic_store_n(&pages.npt.table[3][DataGpa / NptPageBytes],
                     pa(pages.frame[epoch != 0]) | 5, __ATOMIC_RELEASE);
}

struct cross_sample cross_read(unsigned slot)
{
    struct cpu_pages *cpu = &pages.cpu[slot];
    uint8_t *vmcb = cpu->vmcb;
    uint64_t flags, cr2, dr6, dr7;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    __asm__ volatile ("mov %%cr2,%0; mov %%dr6,%1; mov %%dr7,%2"
                      : "=r"(cr2), "=r"(dr6), "=r"(dr7));
    FIELD(uint64_t, Pat) = rdmsr(0x277);
    FIELD(uint64_t, Rip) = FixedGuestGpa;
    FIELD(uint64_t, Rax) = 0;
    FIELD(uint64_t, ExitCode) = UINT64_MAX;
    FIELD(uint8_t, TlbControl) = 1; /* full local flush on every entry, even for epoch A */

    uint64_t efer = rdmsr(MSR_EFER);
    wrmsr(MSR_EFER, efer | SVME);
    __asm__ volatile ("clgi" : : : "memory");
    uint64_t hsave = rdmsr(MSR_HSAVE);
    wrmsr(MSR_HSAVE, pa(cpu->hsave));
    /* Host RAX is restored by VMEXIT. The immutable guest changes only guest AX. */
    __asm__ volatile ("vmrun" : : "a"(pa(vmcb)) : "memory", "cc");
    __asm__ volatile ("mov %0,%%cr2; mov %1,%%dr6; mov %2,%%dr7"
                      : : "r"(cr2), "r"(dr6), "r"(dr7) : "memory");
    wrmsr(MSR_HSAVE, hsave);
    __asm__ volatile ("stgi" : : : "memory");
    wrmsr(MSR_EFER, efer);
    /* Limine's IF=0 stays clear. Never enable interrupts as a side effect of STGI. */
    __asm__ volatile ("pushq %0; popfq" : : "r"(flags) : "memory", "cc");

    return (struct cross_sample){ FIELD(uint64_t, ExitCode), FIELD(uint64_t, Rip), FIELD(uint64_t, Rax) };
}
