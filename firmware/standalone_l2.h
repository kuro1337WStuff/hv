#ifndef HV_STANDALONE_L2_H
#define HV_STANDALONE_L2_H
#include "fixed_vmcb.h"

enum { StandaloneGuestPages=Pages };
typedef struct { UINT64 exitcode, rip, rax; } StandaloneSample;

/* BSP only, interrupts already disabled. reservation is a contiguous identity
 * mapping retained by L1 after ExitBootServices; no firmware calls below. */
static const char *run_standalone_l2(void *reservation, StandaloneSample *sample)
{
    UINTN base=(UINTN)reservation;
    UINT8 *vmcb=(UINT8 *)reservation;
    int r[4];
    sample->exitcode=~0ull;
    sample->rip=sample->rax=0;
    if (!base || (base & (EFI_PAGE_SIZE-1)) || base>MAX_UINTN-(StandaloneGuestPages*EFI_PAGE_SIZE-1))
        return "GUEST RESERVATION";
    __cpuid(r,0);
    if (r[0]<1 || r[1]!=0x68747541 || r[3]!=0x69746e65 || r[2]!=0x444d4163) return "AMD REQUIRED";
    __cpuid(r,1);
    if (!((UINT32)r[2] & (1u<<31))) return "VM REQUIRED";
    if (!(r[3] & (1u<<16))) return "PAT REQUIRED";
    __cpuid(r,0x40000000);
    if (r[1]!=0x7263694d || r[2]!=0x666f736f || r[3]!=0x76482074) return "HYPER V REQUIRED";
    __cpuid(r,(int)0x80000000u);
    if ((UINT32)r[0]<0x8000000au) return "NO SVM LEAF";
    __cpuid(r,(int)0x80000001u);
    if (!(r[2]&4)) return "NO SVM";
    __cpuid(r,(int)0x8000000au);
    if (!(r[3]&1)) return "NO NESTED PAGING";
    if ((UINT32)r[1]<2) return "NO GUEST ASID";
    if (__readcr4() & (1u<<12)) return "HOST PAGING";
    if (__readmsr(MSR_VM_CR) & (1u<<4)) return "SVM DISABLED";
    UINT64 efer=__readmsr(MSR_EFER);
    if (efer & SVME) return "SVM IN USE";

    __stosb(vmcb,0,Iopm*EFI_PAGE_SIZE);
    __stosb(vmcb+Iopm*EFI_PAGE_SIZE,0xff,(Pages-Iopm)*EFI_PAGE_SIZE);
    build_fixed_npt((FixedNpt *)(vmcb+Pml4*EFI_PAGE_SIZE),base+Pml4*EFI_PAGE_SIZE,base+Code*EFI_PAGE_SIZE);
    __movsb(vmcb+Code*EFI_PAGE_SIZE,fixed_mov_hlt,sizeof(fixed_mov_hlt));
    fixed_vmcb_defaults(vmcb,base);
    fixed_real_mode_state(vmcb);

    UINT64 cr2=__readcr2(), dr6=__readdr(6), dr7=__readdr(7);
    __writemsr(MSR_EFER,efer|SVME);
    UINT64 hsave=__readmsr(MSR_HSAVE);
    __svm_clgi();
    __writemsr(MSR_HSAVE,base+HostSave*EFI_PAGE_SIZE);
    __svm_vmrun(base);
    __writecr2(cr2);
    __writedr(6,dr6);
    __writedr(7,dr7);
    __writemsr(MSR_HSAVE,hsave);
    __svm_stgi();
    __writemsr(MSR_EFER,efer);

    sample->exitcode=FIELD(UINT64,ExitCode);
    sample->rip=FIELD(UINT64,Rip);
    sample->rax=FIELD(UINT64,Rax);
    if (sample->exitcode!=0x78) return "EXPECTED HLT";
    if (sample->rip!=FixedGuestGpa+sizeof(fixed_mov_hlt)-1) return "UNEXPECTED RIP";
    if (sample->rax!=MARKER) return "UNEXPECTED RAX";
    return NULL;
}
#endif
