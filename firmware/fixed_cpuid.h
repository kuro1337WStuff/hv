#ifndef HV_FIXED_CPUID_H
#define HV_FIXED_CPUID_H
#include "fixed_vmcb.h"

/* Register order and code boundaries are the ABI of cpuid_resume.asm. */
typedef struct { UINT64 bx, cx, dx; } CpuidFrame;
extern void RunCpuidGuest(UINTN vmcb_pa, CpuidFrame *frame);
extern const UINT8 L2Start[], L2Cpuid[], L2AfterCpuid[], L2PassHlt[], L2End[];
typedef struct {
    UINT64 exitcode, rip, rax, first_exit, first_rip, leaf, subleaf, resume_rip, entries;
    BOOLEAN nrips;
} CpuidSample;

/* Unsigned offsets reject labels below the start as well as beyond the end.
 * Never decode a host pointer derived from an exit RIP or hardware nRIP. */
static BOOLEAN cpuid_layout_ok(UINTN length)
{
    UINTN site=(UINTN)L2Cpuid-(UINTN)L2Start, after=(UINTN)L2AfterCpuid-(UINTN)L2Start;
    UINTN halt=(UINTN)L2PassHlt-(UINTN)L2Start;
    return length<=EFI_PAGE_SIZE && site>0 && site<length && after==site+2 &&
           after<halt && halt<length && L2Start[site]==0x0f && L2Start[site+1]==0xa2 &&
           L2Start[halt]==0xf4;
}

static UINT64 cpuid_ip(const UINT8 *label)
{
    return FixedGuestGpa+((UINTN)label-(UINTN)L2Start);
}

/* Caller owns host save/restore. No loops, no native CPUID forwarding. */
static const char *fixed_cpuid_run(UINT8 *vmcb, CpuidSample *s)
{
    CpuidFrame frame={0,0,0};
    RunCpuidGuest((UINTN)vmcb,&frame);
    s->entries=1;
    s->first_exit=s->exitcode=FIELD(UINT64,ExitCode);
    s->first_rip=s->rip=FIELD(UINT64,Rip);
    s->leaf=s->rax=FIELD(UINT64,Rax);
    s->subleaf=frame.cx;
    if (s->first_exit!=0x72) return "EXPECTED CPUID";
    if (s->first_rip!=cpuid_ip(L2Cpuid)) return "CPUID RIP";
    if (s->leaf!=0x400000fe || s->subleaf!=7) return "CPUID REQUEST";
    s->resume_rip=s->nrips ? FIELD(UINT64,NextRip) : s->first_rip+2;
    if (s->resume_rip!=cpuid_ip(L2AfterCpuid)) return "CPUID RESUME RIP";

    FIELD(UINT64,Rax)=0x11223344u;
    frame.bx=0x55667788u; frame.cx=0x99aabbccu; frame.dx=0xddeeff00u;
    FIELD(UINT64,Rip)=s->resume_rip;
    FIELD(UINT8,TlbControl)=0; /* same ASID and unchanged mappings */
    FIELD(UINT64,ExitCode)=~0ull;
    RunCpuidGuest((UINTN)vmcb,&frame);
    s->entries=2;
    s->exitcode=FIELD(UINT64,ExitCode);
    s->rip=FIELD(UINT64,Rip);
    s->rax=FIELD(UINT64,Rax);
    if (s->exitcode!=0x78) return "EXPECTED HLT";
    if (s->rip!=cpuid_ip(L2PassHlt)) return "UNEXPECTED RIP";
    if (s->rax!=0x50415353u) return "UNEXPECTED RAX";
    return NULL;
}
#endif
