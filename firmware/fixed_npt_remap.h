#ifndef HV_FIXED_NPT_REMAP_H
#define HV_FIXED_NPT_REMAP_H
#include "fixed_vmcb.h"

enum { DataGpa=0x2000, FrameA=Pages, FrameB, RemapPages, MarkerA=0xa11a, MarkerB=0xb22b };
/* Real-mode MOV AX, word ptr DS:[2000h]; HLT. */
static const UINT8 fixed_read_hlt[]={0xa1,DataGpa & 0xff,DataGpa >> 8,0xf4};
typedef struct { UINT64 exit, ip, ax; } ReadSample;
typedef struct { ReadSample arm[2]; UINT64 entries, backing_a, backing_b; } RemapSample;

static inline void fixed_remap_init_sample(RemapSample *s)
{
    __stosb((UINT8 *)s,0,sizeof(*s));
    s->arm[0].exit=s->arm[1].exit=~0ull;
}

/* Both frames are separate zeroed pages in the contiguous L1 reservation. */
static inline void fixed_remap_prepare(UINT8 *vmcb)
{
    __movsb(vmcb+Code*EFI_PAGE_SIZE,fixed_read_hlt,sizeof(fixed_read_hlt));
    *(volatile UINT16 *)(vmcb+FrameA*EFI_PAGE_SIZE)=MarkerA;
    *(volatile UINT16 *)(vmcb+FrameB*EFI_PAGE_SIZE)=MarkerB;
}

static inline const char *fixed_remap_run(UINT8 *vmcb, RemapSample *s)
{
    volatile UINT16 *a=(volatile UINT16 *)(vmcb+FrameA*EFI_PAGE_SIZE);
    volatile UINT16 *b=(volatile UINT16 *)(vmcb+FrameB*EFI_PAGE_SIZE);
    volatile UINT64 *leaf=&((FixedNpt *)(vmcb+Pml4*EFI_PAGE_SIZE))->table[3][DataGpa/NptPageBytes];
    const UINT64 frame_mask=0x000ffffffffff000ull;
    *leaf=(UINTN)a|5; /* P/US, read-only */
    for (UINTN i=0; i<2; ++i) {
        if (i) *leaf=(*leaf & ~frame_mask) | ((UINTN)b & frame_mask); /* preserve A/D and permissions */
        FIELD(UINT64,Rip)=FixedGuestGpa;
        FIELD(UINT64,Rax)=0;
        FIELD(UINT64,ExitCode)=~0ull;
        FIELD(UINT8,TlbControl)=1; /* full local flush on entry, same ASID and N_CR3 */
        ++s->entries;
        __svm_vmrun((UINTN)vmcb);
        ReadSample *arm=&s->arm[i];
        arm->exit=FIELD(UINT64,ExitCode);
        arm->ip=FIELD(UINT64,Rip);
        arm->ax=FIELD(UINT64,Rax);
        s->backing_a=*a;
        s->backing_b=*b;
        if (arm->exit!=0x78 || arm->ip!=FixedGuestGpa+sizeof(fixed_read_hlt)-1 ||
            arm->ax!=(i ? MarkerB : MarkerA)) return i ? "FRAME B READ" : "FRAME A READ";
        if (s->backing_a!=MarkerA) return "FRAME A CHANGED";
        if (s->backing_b!=MarkerB) return "FRAME B CHANGED";
    }
    return NULL;
}
#endif
