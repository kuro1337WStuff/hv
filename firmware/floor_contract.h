#ifndef HV_FLOOR_CONTRACT_H
#define HV_FLOOR_CONTRACT_H

extern const UINT8 FloorStart[], FloorEnd[];
enum { FloorCalls=7, FloorStride=16, FloorStop=(FloorCalls+1)*FloorStride,
       FloorFms=0x00020f32, FloorBasic=0x07008161, FloorExt=0x21008161,
       FloorNop=0x6e6f706c, FloorCookie=0x464c5231 };

typedef struct { UINT32 ax, bx, cx, dx; } FloorReply;
/* These non-indexed leaves ignore input ECX. The dispatcher separately binds
 * the exact seven requests; no arbitrary leaf, index or RIP is serviced. */
static FloorReply floor_reply(UINT32 leaf)
{
    switch (leaf) {
    case 0: return (FloorReply){1,0x68747541,0x444d4163,0x69746e65}; /* B,D,C */
    case 1: return (FloorReply){FloorFms,0x00010000,0x80000000,FloorBasic};
    case 0x80000000: return (FloorReply){0x80000001,0,0,0};
    case 0x80000001: return (FloorReply){FloorFms,0,0,FloorExt};
    case 0x40000000: return (FloorReply){0x40000000,0x614c7648,0x444d4162,0x74736554}; /* B,C,D */
    default: return (FloorReply){0,0,0,0}; /* fixed out-of-range leaf 7 */
    }
}

typedef struct {
    UINT64 operand, returned;
    UINT32 success_zf, failure_zf, taken, untaken, nop, sse, cookie, guard;
    UINT32 fms, basic, again, ext;
} FloorEvidence;
typedef struct { volatile UINT64 before; FpWorkspace work; volatile UINT64 after; } FloorScratch;
_Static_assert(LongInitialRsp-LongStackGpa==0xff0 && !(LongStackGpa&7) &&
               sizeof(FloorEvidence)==64 && offsetof(FloorEvidence,success_zf)==16 &&
               offsetof(FloorEvidence,nop)==32 && offsetof(FloorEvidence,fms)==48 &&
               sizeof(FloorScratch)<=EFI_PAGE_SIZE && offsetof(FloorScratch,work)%16==0 &&
               GuestStack!=FpScratch,"floor private workspace and guest ABI");

/* Check both sides of each checkpoint. Old or prematurely populated words
 * cannot satisfy a fresh run; in particular there is no cookie at leaf 7. */
static BOOLEAN floor_evidence(const volatile FloorEvidence *e, UINTN i)
{
    const UINT32 poison=0xffffffffu;
    return e->fms==(i>=2 ? (UINT32)FloorFms : poison) &&
           e->basic==(i>=2 ? (UINT32)FloorBasic : poison) &&
           e->again==(i>=3 ? (UINT32)FloorBasic : poison) &&
           e->ext==(i>=5 ? (UINT32)FloorExt : poison) &&
           e->operand==(i>=6 ? 9u : 5u) && e->returned==(i>=6 ? 9ull : ~0ull) &&
           e->success_zf==(i>=6 ? 1u : poison) && e->failure_zf==(i>=6 ? 0u : poison) &&
           e->taken==(i>=6 ? 0x22u : poison) && e->untaken==(i>=6 ? 0x33u : poison) &&
           e->sse==(i==FloorCalls ? 42u : poison) &&
           e->cookie==(i==FloorCalls ? (UINT32)FloorCookie : poison);
}

static UINT64 floor_pair(UINT32 hi, UINT32 lo) { return ((UINT64)hi<<32)|lo; }

static const char *floor_contract_run(UINT8 *vmcb)
{
    static const UINT32 leaves[]={0,1,1,0x80000000,0x80000001,0x40000000,7};
    static const char *const labels[]={"CPUID A ","CPUID B ","CPUID C ","CPUID D ",
                                       "CPUID E ","CPUID F ","CPUID G "};
    const UINT64 guard=0xc1a4e3792f61bd85ull;
    UINTN size=(UINTN)FloorEnd-(UINTN)FloorStart;
    if (size<=FloorStop+2 || size>EFI_PAGE_SIZE || FloorStart[FloorStop]!=0x0f ||
        FloorStart[FloorStop+1]!=0x32 || FloorStart[FloorStop+2]!=0xf4) return "FLOOR BLOB LAYOUT";
    for (UINTN i=1; i<=FloorCalls; ++i)
        if (FloorStart[i*FloorStride]!=0x0f || FloorStart[i*FloorStride+1]!=0xa2)
            return "FLOOR CPUID LAYOUT";

    int host[4];
    __cpuid(host,1); /* prerequisites only, never a source of guest CPUID data */
    if (((UINT32)host[3]&FloorBasic)!=FloorBasic) return "FLOOR PREREQUISITES";
    if ((__readcr0()&0xcu) || !(__readcr4()&(1u<<9)) || (__readmsr(MSR_EFER)&(1ull<<14)))
        return "FLOOR FP MODE";

    pkg_reset(vmcb);
    __movsb(vmcb+Code*EFI_PAGE_SIZE,FloorStart,size);
    long_guest_state(vmcb,(UINTN)vmcb);
    FIELD(UINT64,Cr4)|=1u<<9;
    FIELD(UINT32,Intercepts)|=(1u<<18)|(1u<<28); /* CPUID and deny-all MSR interception */
    FloorScratch *s=(FloorScratch *)(vmcb+FpScratch*EFI_PAGE_SIZE);
    FpWorkspace *w=&s->work;
    volatile FloorEvidence *e=(volatile FloorEvidence *)(vmcb+GuestStack*EFI_PAGE_SIZE);
    __stosb(vmcb+GuestStack*EFI_PAGE_SIZE,0xff,sizeof(*e));
    e->operand=5; e->nop=FloorNop; e->guard=(UINT32)guard;
    s->before=guard; s->after=~guard;
    fp_pattern(&w->guest,1,FALSE,0,0);
    FpTrace trace;
    FpBank want;
    UINT32 entries=0, restored=0;
    const char *error=NULL;
    for (UINTN i=0; i<=FloorCalls; ++i) {
        UINTN g=1+(i&1);
        fp_pattern(&w->seed,g,TRUE,(UINTN)vmcb,(UINTN)&w->guest);
        FIELD(UINT64,ExitCode)=~0ull;
        ObserveLegacyRun((UINTN)vmcb,w);
        ++entries;
        if (!fp_match(&trace,L"genuine caller",&w->before,&w->after,TRUE)) {
            error="FLOOR CALLER STATE"; break;
        }
        fp_pattern(&want,g,TRUE,(UINTN)vmcb,(UINTN)&w->guest);
        if (!fp_match(&trace,L"alternating L1",&want,&w->live,FALSE)) {
            error="FLOOR LIVE L1"; break;
        }
        ++restored;
        if (s->before!=guard || s->after!=~guard || e->guard!=(UINT32)guard || e->nop!=FloorNop) {
            error="FLOOR GUARD"; break;
        }
        if (FIELD(UINT64,Rsp)!=LongInitialRsp) { error="FLOOR STACK"; break; }
        UINT64 ip=FixedGuestGpa+(i+1)*FloorStride;
        if (i==FloorCalls) {
            if (FIELD(UINT64,ExitCode)!=0x7c || FIELD(UINT64,Rip)!=ip || FIELD(UINT64,Info1)!=0 ||
                w->guest.gpr[1]!=0xdeadbeefull) error="FLOOR RDMSR STOP";
            else if (!floor_evidence(e,i) || w->guest.fx.xmm[0]!=42 || w->guest.fx.xmm[1]!=0 ||
                     FIELD(UINT64,Rax)!=42) error="FLOOR FINAL EVIDENCE";
            break; /* no MSR access, RIP advance, reply or ninth entry */
        }
        if (FIELD(UINT64,ExitCode)!=0x72 || FIELD(UINT64,Rip)!=ip) { error="FLOOR CPUID SITE"; break; }
        UINT64 leaf=FIELD(UINT64,Rax), cx=w->guest.gpr[1];
        pkg_observe(labels[i],floor_pair((UINT32)leaf,(UINT32)cx));
        if (leaf!=leaves[i] || cx!=(i==2 ? 0xa5a5a5a5u : 0u)) { error="FLOOR CPUID INPUT"; break; }
        if (!floor_evidence(e,i)) { error="FLOOR CHECKPOINT"; break; }
        if (i==FloorCalls-1) {
            pkg_observe("L2 XMM0 IN ",w->guest.fx.xmm[0]);
            if (w->guest.fx.xmm[0]!=21 || w->guest.fx.xmm[1]!=0) { error="FLOOR SSE2 INPUT"; break; }
        }
        FloorReply r=floor_reply((UINT32)leaf);
        FIELD(UINT64,Rax)=r.ax;
        w->guest.gpr[0]=r.bx; w->guest.gpr[1]=r.cx; w->guest.gpr[2]=r.dx;
        FIELD(UINT64,Rip)=ip+2;
        FIELD(UINT8,TlbControl)=0;
    }
    pkg_observe("L2 FMS:BITS ",floor_pair(e->fms,e->basic));
    pkg_observe("L2 NZ:EXT ",floor_pair(e->again,e->ext));
    pkg_observe("L2 CX8 ",floor_pair((UINT32)e->operand,(UINT32)e->returned));
    pkg_observe("L2 CX8 ZF ",floor_pair(e->success_zf,e->failure_zf));
    pkg_observe("L2 CMOV ",floor_pair(e->taken,e->untaken));
    pkg_observe("L2 NOP ",e->nop);
    pkg_observe("L2 SSE2 ",e->sse);
    pkg_observe("L2 COOKIE ",e->cookie);
    pkg_observe("L1 RUN:REST ",floor_pair(entries,restored));
    pkg_observe("L1 EXIT ",FIELD(UINT64,ExitCode));
    pkg_observe("L1 RIP ",FIELD(UINT64,Rip));
    pkg_observe("L1 INFO1 ",FIELD(UINT64,Info1));
    pkg_observe("L1 RCX ",w->guest.gpr[1]);
    pkg_observe("L1 RAX ",FIELD(UINT64,Rax));
    return error;
}
#endif
