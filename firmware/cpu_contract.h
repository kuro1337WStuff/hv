#ifndef HV_CPU_CONTRACT_H
#define HV_CPU_CONTRACT_H

extern const UINT8 CpuStart[], CpuStop[], CpuEnd[];
extern const UINT32 CpuCpuidOffsets[4]; /* offsets into CpuStart, not guest addresses */

enum { CpuLegacyFeatures=0x07000001u }; /* x87, FXSR, SSE, SSE2; no XSAVE/AVX */
typedef struct { UINT32 ax, bx, cx, dx; } CpuReply;

/* This is a fixed virtual profile, not a host CPUID filter or a Linux CPU model.
 * Unlisted requests (including subleaves) return four zero-extended zeroes. */
static CpuReply cpu_contract_reply(UINT32 leaf, UINT32 subleaf)
{
    CpuReply r={0,0,0,0};
    if (subleaf) return r;
    switch (leaf) {
    case 0:
        r.ax=1;
        r.bx=0x68747541; r.dx=0x69746e65; r.cx=0x444d4163; /* AuthenticAMD: B,D,C */
        break;
    case 1:
        r.bx=1u<<16; r.cx=1u<<31; r.dx=CpuLegacyFeatures;
        break;
    case 0x40000000:
        r.ax=0x40000000;
        r.bx=0x614c7648; r.cx=0x444d4162; r.dx=0x74736554; /* HvLabAMDTest: B,C,D */
        break;
    }
    return r;
}

static BOOLEAN cpu_contract_layout(UINTN size, UINTN stop)
{
    if (size<2 || size>EFI_PAGE_SIZE || stop>size-2 ||
        CpuStart[stop]!=0x0f || CpuStart[stop+1]!=0x32) return FALSE; /* RDMSR */
    UINTN next=0;
    for (UINTN i=0; i<4; ++i) {
        UINTN off=CpuCpuidOffsets[i];
        if (off<next || off+2>stop || CpuStart[off]!=0x0f || CpuStart[off+1]!=0xa2) return FALSE;
        next=off+2;
    }
    return TRUE;
}

typedef struct {
    volatile UINT64 before;
    FpWorkspace work;
    volatile UINT64 after;
} CpuScratch;
typedef struct { UINT32 count, ecx, edx, result, cookie, guard; } CpuEvidence;
_Static_assert(sizeof(CpuScratch)<=EFI_PAGE_SIZE && offsetof(CpuScratch,work)%16==0 &&
               FpScratch!=GuestStack && offsetof(CpuEvidence,guard)==20 &&
               LongStackGpa+sizeof(CpuEvidence)<LongInitialRsp,"CPU fixture storage ABI");

static const char *cpu_contract_run(UINT8 *vmcb)
{
    static const UINT32 request[]={0,1,0x40000000,7};
    static const char *const labels[]={"CPUID A ","CPUID B ","CPUID C ","CPUID D "};
    const UINT64 guard=0x32ab54cd7698ef01ull;
    UINTN size=(UINTN)CpuEnd-(UINTN)CpuStart, stop=(UINTN)CpuStop-(UINTN)CpuStart;
    if (!cpu_contract_layout(size,stop)) return "CPU BLOB LAYOUT";
    int host[4];
    __cpuid(host,1); /* host prerequisite only: never an input to the virtual replies */
    if (((UINT32)host[3]&CpuLegacyFeatures)!=CpuLegacyFeatures) return "CPU FP FEATURES";
    if ((__readcr0()&0xcu) || !(__readcr4()&(1u<<9)) || (__readmsr(MSR_EFER)&(1ull<<14)))
        return "CPU FP MODE"; /* CR0.EM/TS=0, CR4.OSFXSR=1, EFER.FFXSR=0 */

    pkg_reset(vmcb);
    __movsb(vmcb+Code*EFI_PAGE_SIZE,CpuStart,size);
    long_guest_state(vmcb,(UINTN)vmcb);
    FIELD(UINT64,Cr4)|=1u<<9;
    FIELD(UINT32,Intercepts)|=(1u<<18)|(1u<<28); /* CPUID, MSR (all-ones MSRPM) */

    /* Both pointers use L1's arena layout, never a register or an exit address.
     * Only GuestStack is mapped at LongStackGpa. FpScratch is private to L1. */
    CpuScratch *s=(CpuScratch *)(vmcb+FpScratch*EFI_PAGE_SIZE);
    FpWorkspace *w=&s->work;
    volatile CpuEvidence *e=(volatile CpuEvidence *)(vmcb+GuestStack*EFI_PAGE_SIZE);
    e->count=e->ecx=e->edx=e->result=e->cookie=0xffffffffu;
    e->guard=(UINT32)guard;
    s->before=guard; s->after=~guard;
    fp_pattern(&w->guest,1,FALSE,0,0);
    FpBank want;
    FpTrace trace;
    UINTN entries=0, restored=0;
    const char *error=NULL;
    /* At most four replies and one terminal entry. Each reply advances exactly
     * one trusted CPUID, never an arbitrary exit RIP or decoder-supplied nRIP. */
    for (UINTN i=0; i<5; ++i) {
        UINTN g=1+(i&1); /* fp_pattern defines two valid synthetic L1 generations */
        fp_pattern(&w->seed,g,TRUE,(UINTN)vmcb,(UINTN)&w->guest);
        ObserveLegacyRun((UINTN)vmcb,w);
        ++entries;
        if (!fp_match(&trace,L"genuine caller",&w->before,&w->after,TRUE)) {
            error="CPU CALLER STATE"; break;
        }
        fp_pattern(&want,g,TRUE,(UINTN)vmcb,(UINTN)&w->guest);
        if (!fp_match(&trace,L"live L1",&want,&w->live,FALSE)) {
            error="CPU LIVE L1 STATE"; break;
        }
        ++restored;
        if (s->before!=guard || s->after!=~guard || e->guard!=(UINT32)guard) {
            error="CPU SCRATCH GUARD"; break;
        }
        if (FIELD(UINT64,Rsp)!=LongInitialRsp) { error="CPU GUEST RSP"; break; }
        if (i==4) {
            /* Refusal is an L1 stop, not a forwarded RDMSR or injected fault.
             * No write to RIP, register reply, or path back to VMRUN here. */
            if (FIELD(UINT64,ExitCode)!=0x7c || FIELD(UINT64,Info1)!=0 ||
                FIELD(UINT64,Rip)!=FixedGuestGpa+stop || w->guest.gpr[1]!=0xdeadbeefu)
                error="CPU RDMSR STOP";
            else if (e->count!=1 || e->ecx!=0x80000000u || e->edx!=CpuLegacyFeatures ||
                     e->result!=42 || e->cookie!=0x43505531 || w->guest.fx.xmm[0]!=42 || w->guest.fx.xmm[1]!=0)
                error="CPU GUEST EVIDENCE";
            break;
        }
        UINT64 leaf=FIELD(UINT64,Rax), subleaf=w->guest.gpr[1];
        UINT64 ip=FixedGuestGpa+CpuCpuidOffsets[i];
        if (FIELD(UINT64,ExitCode)!=0x72 || FIELD(UINT64,Rip)!=ip) { error="CPU CPUID EXIT"; break; }
        pkg_observe(labels[i],(leaf<<32)|(UINT32)subleaf);
        if (leaf!=request[i] || subleaf!=0) { error="CPU CPUID REQUEST"; break; }
        if (i==3) {
            pkg_observe("L2 XMM0 IN ",w->guest.fx.xmm[0]);
            if (w->guest.fx.xmm[0]!=21 || w->guest.fx.xmm[1]!=0) { error="CPU SSE2 INPUT"; break; }
        }
        CpuReply r=cpu_contract_reply((UINT32)leaf,(UINT32)subleaf);
        FIELD(UINT64,Rax)=r.ax;
        w->guest.gpr[0]=r.bx; w->guest.gpr[1]=r.cx; w->guest.gpr[2]=r.dx;
        FIELD(UINT64,Rip)=ip+2;
        FIELD(UINT64,ExitCode)=~0ull;
        FIELD(UINT8,TlbControl)=0;
    }
    pkg_observe("L2 LOGICAL ",e->count);
    pkg_observe("L2 ECX ",e->ecx);
    pkg_observe("L2 EDX ",e->edx);
    pkg_observe("L2 SSE2 ",e->result);
    pkg_observe("L2 COOKIE ",e->cookie);
    pkg_observe("L1 ENTRIES ",entries);
    pkg_observe("L1 RESTORES ",restored);
    pkg_observe("L1 EXIT ",FIELD(UINT64,ExitCode));
    pkg_observe("L1 RIP ",FIELD(UINT64,Rip));
    pkg_observe("L1 INFO1 ",FIELD(UINT64,Info1));
    pkg_observe("L1 RCX ",w->guest.gpr[1]);
    pkg_observe("L1 RAX ",FIELD(UINT64,Rax)); /* stage marker for unexpected guest HLT */
    return error;
}
#endif
