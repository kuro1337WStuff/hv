#ifndef HV_FIXED_VMCB_H
#define HV_FIXED_VMCB_H
#include "fixed_npt.h"

/* AMD64 APM Vol. 2, Appendix B: offsets from the VMCB page base. */
enum {
    Exceptions=0x008, Intercepts=0x00c, SvmIntercepts=0x010, IopmPa=0x040,
    MsrpmPa=0x048, Asid=0x058, TlbControl=0x05c, IntControl=0x060,
    ExitCode=0x070, Info1=0x078, Info2=0x080, NpEnable=0x090, NestedCr3=0x0b0, NextRip=0x0c8,
    Es=0x400, Cs=0x410, Ss=0x420, Ds=0x430, GuestEfer=0x4d0, Cr4=0x548, Cr3=0x550, Cr0=0x558,
    Dr7=0x560, Dr6=0x568, Flags=0x570, Rip=0x578, Rsp=0x5d8, Rax=0x5f8, Pat=0x668
};
enum { Vmcb, HostSave, Pml4, Code=Pml4+NptTableCount, Iopm, Msrpm=Iopm+3, Pages=Msrpm+2 };
_Static_assert(sizeof(FixedNpt)==NptTableCount*EFI_PAGE_SIZE, "NPT allocation size mismatch");
#define MSR_EFER 0xc0000080u
#define MSR_VM_CR 0xc0010114u
#define MSR_HSAVE 0xc0010117u
#define SVME (1ull << 12)
#define MARKER 0x4c32u
#define FIELD(t, offset) (*(volatile t *)(vmcb + (offset)))
static const UINT8 fixed_mov_hlt[]={0xb8, MARKER & 0xff, MARKER >> 8, 0xf4};

/* Called on a zeroed VMCB. Individual smoke profiles may override these defaults. */
static inline void fixed_vmcb_defaults(UINT8 *vmcb, UINT64 base)
{
    FIELD(UINT32,Exceptions)=0xffffffffu;
    FIELD(UINT32,Intercepts)=0x0fu | (1u<<24) | (1u<<27) | (1u<<28) | (1u<<31);
    FIELD(UINT32,SvmIntercepts)=0x7f; /* includes mandatory VMRUN intercept */
    FIELD(UINT64,IopmPa)=base+Iopm*EFI_PAGE_SIZE;
    FIELD(UINT64,MsrpmPa)=base+Msrpm*EFI_PAGE_SIZE;
    FIELD(UINT32,Asid)=1;
    FIELD(UINT8,TlbControl)=1; /* flush before ASID reuse */
    FIELD(UINT32,IntControl)=1u<<24;
    FIELD(UINT64,NpEnable)=1;
    FIELD(UINT64,NestedCr3)=base+Pml4*EFI_PAGE_SIZE;
    FIELD(UINT64,Dr6)=0xffff0ff0u;
    FIELD(UINT64,Dr7)=0x400;
    FIELD(UINT64,Flags)=2;
    FIELD(UINT64,Rip)=FixedGuestGpa;
    FIELD(UINT64,Pat)=__readmsr(0x277);
    FIELD(UINT64,ExitCode)=~0ull;
}

static inline void fixed_real_mode_state(UINT8 *vmcb)
{
    for (UINTN seg=Es; seg<=Ds; seg+=16) {
        FIELD(UINT16,seg+2)=(seg==Cs)?0x9b:0x93;
        FIELD(UINT32,seg+4)=0xffff;
    }
    FIELD(UINT64,GuestEfer)=SVME;
    FIELD(UINT64,Cr0)=0x30;
    FIELD(UINT64,Rsp)=FixedGuestGpa+EFI_PAGE_SIZE-16;
}
#endif
