#ifndef HV_EFER_READ_H
#define HV_EFER_READ_H
extern const UINT8 EferStart[], EferEnd[];
typedef struct { UINT64 first, second; UINT32 cookie, guard; } EferEvidence;
typedef struct { UINT64 lo; FpWorkspace work; UINT64 hi; } EferScratch;
_Static_assert(sizeof(EferScratch)<=EFI_PAGE_SIZE && offsetof(EferScratch,work)%16==0 &&
               sizeof(EferEvidence)==24 && LongInitialRsp-LongStackGpa==0xff0,"EFER fixture ABI");

static const char *efer_arm(UINT8 *vmcb, UINTN size, BOOLEAN refuse)
{
    const UINT32 id=0xc0000080u, second=refuse ? 0xdeadbeefu : id;
    const UINT64 guard=0xca7e51b10cace17full;
    pkg_reset(vmcb); /* fresh private maps (all MSRPM bits set), code, stack and banks */
    __movsb(vmcb+Code*EFI_PAGE_SIZE,EferStart,size);
    long_guest_state(vmcb,(UINTN)vmcb);
    FIELD(UINT64,Cr4)|=1u<<9;
    FIELD(UINT32,Intercepts)|=1u<<18;
    EferScratch *s=(EferScratch *)(vmcb+FpScratch*EFI_PAGE_SIZE);
    volatile EferEvidence *e=(volatile EferEvidence *)(vmcb+GuestStack*EFI_PAGE_SIZE);
    volatile UINT64 *top=(volatile UINT64 *)((UINT8 *)e+EFI_PAGE_SIZE-8);
    e->first=e->second=~0ull; e->cookie=0xffffffffu; e->guard=(UINT32)guard; *top=guard;
    s->lo=guard; s->hi=~guard;
    FpWorkspace *w=&s->work;
    fp_pattern(&w->guest,1,FALSE,0,0);
    w->guest.gpr[6]=second;
    UINTN entries=0, restored=0;
    FpTrace trace;
    FpBank want;
    for (UINTN i=0; i<(refuse ? 4u : 5u); ++i) {
        UINTN g=1+(i&1);
        fp_pattern(&w->seed,g,TRUE,(UINTN)vmcb,(UINTN)&w->guest);
        FIELD(UINT64,ExitCode)=~0ull;
        ObserveLegacyRun((UINTN)vmcb,w);
        ++entries;
        fp_pattern(&want,g,TRUE,(UINTN)vmcb,(UINTN)&w->guest);
        if (!fp_match(&trace,L"caller",&w->before,&w->after,TRUE) ||
            !fp_match(&trace,L"live L1",&want,&w->live,FALSE)) return "EFER CALLER STATE";
        ++restored;
        for (UINTN j=3; j<14; ++j)
            if (w->guest.gpr[j]!=(j==6 ? second : fp_lane(0x67000000b7000000ull,1,j)))
                return "EFER L2 GPRS";
        if (s->lo!=guard || s->hi!=~guard || e->guard!=(UINT32)guard || *top!=guard ||
            FIELD(UINT64,Rsp)!=LongInitialRsp) return "EFER GUARDS";
        if (FIELD(UINT64,GuestEfer)!=0x1500 || FIELD(UINT64,Cr4)!=0x220 ||
            FIELD(UINT64,NpEnable)!=1 || FIELD(UINT64,NestedCr3)!=(UINTN)(vmcb+Pml4*EFI_PAGE_SIZE) ||
            FIELD(UINT64,MsrpmPa)!=(UINTN)(vmcb+Msrpm*EFI_PAGE_SIZE)) return "EFER BACKING";
        UINT64 ip=FixedGuestGpa+16*(i+1);
        if (FIELD(UINT64,Rip)!=ip || e->first!=(i ? 0x500ull : ~0ull) ||
            e->second!=(i==4 ? 0x500ull : ~0ull) || e->cookie!=(i==4 ? 0x45464552u : 0xffffffffu))
            return "EFER CHECKPOINT";
        if (i==4) {
            if (FIELD(UINT64,ExitCode)!=0x78) return "EFER HLT";
            break;
        }
        if (i==1 || i==2) {
            if (FIELD(UINT64,ExitCode)!=0x72 || FIELD(UINT64,Rax)!=7 || w->guest.gpr[1]!=i-1)
                return "EFER CPUID BARRIER";
            /* Existing floor leaf 7 is all zero, independent of subleaf. */
            FIELD(UINT64,Rax)=w->guest.gpr[0]=w->guest.gpr[1]=w->guest.gpr[2]=0;
        } else {
            if (FIELD(UINT64,ExitCode)!=0x7c || FIELD(UINT64,Info1)!=0 ||
                w->guest.gpr[1]!=(i==0 ? id : second)) return "EFER MSR SITE";
            if (i==3 && (FIELD(UINT64,Rax)!=0xfeedbeef || w->guest.gpr[2]!=0xbadc0de))
                return "EFER READ INPUT";
            if (i==3 && refuse) break; /* no reply, RIP advance, injection or further entry */
            FIELD(UINT64,Rax)=0x500; w->guest.gpr[2]=0; /* virtual LME/LMA only, never backing SVME */
        }
        FIELD(UINT64,Rip)=ip+2;
        FIELD(UINT8,TlbControl)=0;
    }
    pkg_observe(refuse ? "N RUN:REST " : "P RUN:REST ",((UINT64)entries<<32)|restored);
    pkg_observe("BACK EFER ",FIELD(UINT64,GuestEfer));
    pkg_observe("READ ONE ",e->first); pkg_observe("READ TWO ",e->second);
    pkg_observe("COOKIE ",e->cookie); pkg_observe("EXIT ",FIELD(UINT64,ExitCode));
    pkg_observe("RIP ",FIELD(UINT64,Rip));
    if (refuse) { pkg_observe("INFO1 ",FIELD(UINT64,Info1)); pkg_observe("INDEX ",w->guest.gpr[1]); }
    return NULL;
}

static const char *efer_read_run(UINT8 *vmcb, UINT64 host_efer)
{
    UINTN size=(UINTN)EferEnd-(UINTN)EferStart;
    if (size<=80 || size>EFI_PAGE_SIZE || EferStart[80]!=0xf4) return "EFER HLT LAYOUT";
    for (UINTN i=1; i<=4; ++i)
        if (EferStart[16*i]!=0x0f || EferStart[16*i+1]!=(i==1 || i==4 ? 0x32 : 0xa2))
            return "EFER EXIT LAYOUT";
    int host[4];
    __cpuid(host,1);
    if (((UINT32)host[3]&0x07008161)!=0x07008161 || (__readcr0()&0xc) ||
        !(__readcr4()&(1u<<9)) || (host_efer&(1ull<<14))) return "EFER LEGACY MODE";
    const char *error=efer_arm(vmcb,size,FALSE);
    return error ? error : efer_arm(vmcb,size,TRUE);
}
#endif
