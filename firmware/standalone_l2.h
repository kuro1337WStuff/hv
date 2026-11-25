#ifndef HV_STANDALONE_L2_H
#define HV_STANDALONE_L2_H
#include "fixed_vmcb.h"

#ifdef HV_NPT_WRITE
#include "fixed_npt_write.h"
enum { StandaloneGuestPages=Data+1 };
typedef struct {
    UINT64 rw_exit, rw_rip, rw_byte, ro_exit, ro_rip, info1, gpa, before, after, entries;
} StandaloneSample;

static const char *standalone_store_run(UINT8 *vmcb, StandaloneSample *s)
{
    volatile UINT8 *target=vmcb+Data*EFI_PAGE_SIZE; /* owned backing, never EXITINFO2 */
    volatile UINT64 *leaf=&((FixedNpt *)(vmcb+Pml4*EFI_PAGE_SIZE))->table[3][DataGpa/NptPageBytes];
    *target=InitialByte;
    *leaf=(UINTN)target|7;
    if (*target!=InitialByte) return "RW INITIAL BYTE";
    s->entries=1;
    __svm_vmrun((UINTN)vmcb);
    s->rw_exit=FIELD(UINT64,ExitCode);
    s->rw_rip=FIELD(UINT64,Rip);
    s->rw_byte=*target;
    if (s->rw_exit!=0x78 || s->rw_rip!=FixedGuestGpa+sizeof(fixed_store_hlt)-1 ||
        s->rw_byte!=StoredByte) return "RW CONTROL";

    *target=InitialByte;
    s->before=*target;
    if (s->before!=InitialByte) return "RO RESET BYTE";
    *leaf &= ~2ull; /* retain the frame, including any hardware A/D updates */
    FIELD(UINT64,Rip)=FixedGuestGpa;
    FIELD(UINT8,TlbControl)=1; /* full local flush at the next VMRUN */
    FIELD(UINT64,ExitCode)=~0ull;
    FIELD(UINT64,Info1)=FIELD(UINT64,Info2)=0;
    s->entries=2;
    __svm_vmrun((UINTN)vmcb);
    s->ro_exit=FIELD(UINT64,ExitCode);
    s->ro_rip=FIELD(UINT64,Rip);
    s->info1=FIELD(UINT64,Info1);
    s->gpa=FIELD(UINT64,Info2);
    s->after=*target;
    if (!final_write_fault(s->ro_exit,s->info1) || s->ro_rip!=FixedGuestGpa ||
        s->gpa!=DataGpa) return "RO FAULT";
    if (s->after!=s->before) return "RO BYTE CHANGED";
    return NULL;
}
#else
enum { StandaloneGuestPages=Pages };
#ifdef HV_CPUID_RESUME
#include "fixed_cpuid.h"
typedef CpuidSample StandaloneSample;
#else
typedef struct { UINT64 exitcode, rip, rax; } StandaloneSample;
#endif
#endif

/* BSP only, interrupts already disabled. reservation is a contiguous identity
 * mapping retained by L1 after ExitBootServices; no firmware calls below. */
static const char *run_standalone_l2(void *reservation, StandaloneSample *sample)
{
    UINTN base=(UINTN)reservation;
    UINT8 *vmcb=(UINT8 *)reservation;
    int r[4];
#ifdef HV_NPT_WRITE
    __stosb((UINT8 *)sample,0,sizeof(*sample));
    sample->rw_exit=sample->ro_exit=~0ull;
#else
    sample->exitcode=~0ull;
    sample->rip=sample->rax=0;
#ifdef HV_CPUID_RESUME
    sample->first_exit=~0ull;
    sample->first_rip=sample->leaf=sample->subleaf=sample->resume_rip=sample->entries=0;
    sample->nrips=FALSE;
    UINTN length=(UINTN)L2End-(UINTN)L2Start;
    if (!cpuid_layout_ok(length)) return "CPUID GUEST LAYOUT";
#endif
#endif
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
#ifdef HV_CPUID_RESUME
    sample->nrips=(r[3] & (1u<<3))!=0;
#endif
    if (!(r[3]&1)) return "NO NESTED PAGING";
    if ((UINT32)r[1]<2) return "NO GUEST ASID";
    if (__readcr4() & (1u<<12)) return "HOST PAGING";
    if (__readmsr(MSR_VM_CR) & (1u<<4)) return "SVM DISABLED";
    UINT64 efer=__readmsr(MSR_EFER);
    if (efer & SVME) return "SVM IN USE";

    __stosb(vmcb,0,Iopm*EFI_PAGE_SIZE);
    __stosb(vmcb+Iopm*EFI_PAGE_SIZE,0xff,(Pages-Iopm)*EFI_PAGE_SIZE);
    build_fixed_npt((FixedNpt *)(vmcb+Pml4*EFI_PAGE_SIZE),base+Pml4*EFI_PAGE_SIZE,base+Code*EFI_PAGE_SIZE);
#ifdef HV_CPUID_RESUME
    __movsb(vmcb+Code*EFI_PAGE_SIZE,L2Start,length);
#elif defined(HV_NPT_WRITE)
    __stosb(vmcb+Data*EFI_PAGE_SIZE,0,EFI_PAGE_SIZE);
    __movsb(vmcb+Code*EFI_PAGE_SIZE,fixed_store_hlt,sizeof(fixed_store_hlt));
#else
    __movsb(vmcb+Code*EFI_PAGE_SIZE,fixed_mov_hlt,sizeof(fixed_mov_hlt));
#endif
    fixed_vmcb_defaults(vmcb,base);
#ifdef HV_CPUID_RESUME
    FIELD(UINT32,Intercepts)|=1u<<18;
#endif
    fixed_real_mode_state(vmcb);

    UINT64 cr2=__readcr2(), dr6=__readdr(6), dr7=__readdr(7);
    __writemsr(MSR_EFER,efer|SVME);
    UINT64 hsave=__readmsr(MSR_HSAVE);
    __svm_clgi();
    __writemsr(MSR_HSAVE,base+HostSave*EFI_PAGE_SIZE);
#ifdef HV_CPUID_RESUME
    const char *error=fixed_cpuid_run(vmcb,sample);
#elif defined(HV_NPT_WRITE)
    const char *error=standalone_store_run(vmcb,sample);
#else
    __svm_vmrun(base);
#endif
    __writecr2(cr2);
    __writedr(6,dr6);
    __writedr(7,dr7);
    __writemsr(MSR_HSAVE,hsave);
    __svm_stgi();
    __writemsr(MSR_EFER,efer);

#if defined(HV_CPUID_RESUME) || defined(HV_NPT_WRITE)
    return error;
#else
    sample->exitcode=FIELD(UINT64,ExitCode);
    sample->rip=FIELD(UINT64,Rip);
    sample->rax=FIELD(UINT64,Rax);
    if (sample->exitcode!=0x78) return "EXPECTED HLT";
    if (sample->rip!=FixedGuestGpa+sizeof(fixed_mov_hlt)-1) return "UNEXPECTED RIP";
    if (sample->rax!=MARKER) return "UNEXPECTED RAX";
    return NULL;
#endif
}
#endif
