#ifndef HV_VCPU_FPSTATE_H
#define HV_VCPU_FPSTATE_H

#include "vcpu_fp_image.h"

extern const UINT8 FpStart[];
extern const struct { UINT32 size, call, resume, pass, fail; } FpLayout;

/* Primitive ABI. The guest bank is in/out and includes no test metadata. */
extern void RunLegacyVcpu(UINTN vmcb_pa, FpBank *guest);
typedef struct { FpBank guest, seed, live, before, after; } FpWorkspace;
extern void ObserveLegacyRun(UINTN vmcb_pa, FpWorkspace *work);
_Static_assert(sizeof(FpWorkspace)==5*0x270 && sizeof(FpWorkspace)<=EFI_PAGE_SIZE,
               "legacy state assembly ABI mismatch");
_Static_assert(sizeof(FpLayout)==20 && LongStackGpa==0x7000 &&
               LongStackGpa+sizeof(FpBank)+2*sizeof(UINT32)<LongInitialRsp,
               "fixed guest snapshot must fit below its stack");

#ifndef HV_CPU_CONTRACT
static BOOLEAN fp_layout_ok(UINTN length)
{
    if (length!=FpLayout.size || length>EFI_PAGE_SIZE || FpLayout.fail>=length ||
        !(0<FpLayout.call && FpLayout.call<FpLayout.resume && FpLayout.resume<FpLayout.pass &&
          FpLayout.pass<FpLayout.fail)) return FALSE;
    return FpLayout.resume-FpLayout.call==3 && FpStart[FpLayout.call]==0x0f &&
           FpStart[FpLayout.call+1]==1 && FpStart[FpLayout.call+2]==0xd9 &&
           FpStart[FpLayout.pass]==0xf4 && FpStart[FpLayout.fail]==0xf4;
}
#endif

static UINT64 fp_lane(UINT64 base, UINT64 generation, UINT64 slot)
{
    return base|(generation<<48)|(slot<<32)|(generation<<8)|slot;
}

/* Only called on zero-backed scratch for a hardware restore. The local
 * expected bank passed to the comparator need not initialize reserved bytes,
 * empty ST slots or instruction/data pointers. No floating-point C code. */
static void fp_pattern(FpBank *b, UINTN g, BOOLEAN host, UINTN vmcb_pa, UINTN guest)
{
    UINTN rc=host ? g-1 : g, top=host ? 2*g-1 : 6;
    b->fx.cw=(UINT16)(0x037f|(rc<<10));
    b->fx.sw=(UINT16)(top<<11);
    b->fx.tw=(UINT8)(3u<<top);
    b->fx.mx=(UINT32)(0x1f80|(rc<<13));
    if (host) b->fx.mx|=g==1 ? 1u : 4u; /* masked sticky IE or ZE */
    for (UINTN i=0; i<2; ++i) {
        /* L2: (3,4) -> (5,6) -> (7,8); L1: (-9,-10) -> (-11,-12). */
        UINT64 n=2*g+(host ? 7u : 1u)+i;
        UINTN e=n>=8 ? 3 : (n>=4 ? 2 : 1);
        b->fx.st[i].sig=n<<(63-e);
        b->fx.st[i].se=(UINT16)((host ? 0x8000u : 0u)+0x3fffu+e);
    }
    for (UINTN i=0; i<32; ++i)
        b->fx.xmm[i]=fp_lane(host ? 0x48000000c6000000ull : 0x47000000a6000000ull,g,i);
    for (UINTN i=0; i<14; ++i)
        b->gpr[i]=fp_lane(host ? 0x68000000c7000000ull : 0x67000000b7000000ull,g,i);
    if (host) { b->gpr[1]=vmcb_pa; b->gpr[2]=guest; } /* live Microsoft x64 arguments */
}

typedef struct {
    UINT16 cw, top, tw, st0_high, xmm15_high, r15_low;
    UINT32 mx;
} FpClue;
typedef struct {
    struct { UINT64 code, ip, ax; FpClue host, guest; } exit[2];
    UINTN count, checked, index;
    CHAR16 *source, *field;
    UINT64 expected, actual;
    UINT32 sse_result, x87_result;
} FpTrace;

#ifndef HV_CPU_CONTRACT
static void fp_clue(FpClue *c, const FpBank *b)
{
    c->cw=b->fx.cw; c->top=(b->fx.sw>>11)&7; c->tw=b->fx.tw; c->mx=b->fx.mx;
    c->st0_high=(UINT16)(b->fx.st[0].sig>>48);
    c->xmm15_high=(UINT16)b->fx.xmm[31]; c->r15_low=(UINT16)b->gpr[13];
}
#endif

static BOOLEAN fp_equal(FpTrace *t, CHAR16 *field, UINTN index, UINT64 expected, UINT64 actual)
{
    if (actual==expected) return TRUE;
    t->field=field; t->index=index; t->expected=expected; t->actual=actual;
    return FALSE;
}

/* Compare architectural values, not the raw 512-byte save area. For a
 * genuine caller, test every occupied logical ST, not just the fixture pair.
 * AMD may omit FIP/FDP/FOP without a pending x87 exception. */
static BOOLEAN fp_match(FpTrace *t, CHAR16 *where, const FpBank *want,
                        const volatile FpBank *got, BOOLEAN caller)
{
    const FpImage *a=&want->fx;
    const volatile FpImage *b=&got->fx;
    UINTN top=(a->sw>>11)&7;
    t->source=where;
    if (!fp_equal(t,L"FCW",0,a->cw&0x0f3f,b->cw&0x0f3f) ||
        !fp_equal(t,L"TOP",0,top,(b->sw>>11)&7) ||
        !fp_equal(t,L"physical FTW",0,a->tw,b->tw) ||
        !fp_equal(t,L"MXCSR",0,a->mx,b->mx)) return FALSE;
    if (caller && !fp_equal(t,L"FSW",0,a->sw,b->sw)) return FALSE;
    for (UINTN i=0; i<8; ++i) {
        if (!(a->tw&(1u<<((top+i)&7)))) continue;
        if (!fp_equal(t,L"ST significand",i,a->st[i].sig,b->st[i].sig) ||
            !fp_equal(t,L"ST sign:exponent",i,a->st[i].se,b->st[i].se)) return FALSE;
    }
    for (UINTN i=0; i<32; ++i)
        if (!fp_equal(t,L"XMM qword (2*n+half)",i,a->xmm[i],b->xmm[i])) return FALSE;
    for (UINTN i=0; i<14; ++i)
        if (!fp_equal(t,L"GPR slot (BX,CX,DX,BP,SI,DI,R8..R15)",i,want->gpr[i],got->gpr[i]))
            return FALSE;
    return TRUE;
}

#ifndef HV_CPU_CONTRACT
static void fp_run(UINT8 *vmcb, FpTrace *t)
{
    FpWorkspace *w=(FpWorkspace *)(vmcb+FpScratch*EFI_PAGE_SIZE);
    const volatile FpBank *snap=(const volatile FpBank *)(vmcb+GuestStack*EFI_PAGE_SIZE);
    const volatile UINT32 *result=(const volatile UINT32 *)(snap+1);
    FpBank want;
    t->count=t->checked=0; t->field=NULL;
    fp_pattern(&w->guest,1,FALSE,0,0); /* exactly once; VMEXIT writes the resume bank */
    for (UINTN i=0; i<2; ++i) {
        UINTN g=i+1;
        fp_pattern(&w->seed,g,TRUE,(UINTN)vmcb,(UINTN)&w->guest);
        ObserveLegacyRun((UINTN)vmcb,w);
        t->count=g;
        t->exit[i].code=FIELD(UINT64,ExitCode);
        t->exit[i].ip=FIELD(UINT64,Rip);
        t->exit[i].ax=FIELD(UINT64,Rax);
        fp_clue(&t->exit[i].host,&w->live); fp_clue(&t->exit[i].guest,&w->guest);
        t->sse_result=result[0]; t->x87_result=result[1];
        if (!fp_match(t,L"genuine caller",&w->before,&w->after,TRUE)) return;
        fp_pattern(&want,g,TRUE,(UINTN)vmcb,(UINTN)&w->guest);
        if (!fp_match(t,L"live L1 after switch",&want,&w->live,FALSE)) return;
        fp_pattern(&want,g,FALSE,0,0);
        if (!fp_match(t,L"L2 entry snapshot",&want,snap,FALSE)) return;
        if (i) {
            t->source=L"resumed L2 calculation";
            if (!fp_equal(t,L"SSE +0.25 rounded up",0,1,t->sse_result) ||
                !fp_equal(t,L"x87 +11.25 rounded up",0,12,t->x87_result)) return;
        }
        t->source=L"VMCB exit";
        if (!fp_equal(t,L"EXITCODE",0,i ? 0x78 : 0x81,t->exit[i].code) ||
            !fp_equal(t,L"RIP",0,FixedGuestGpa+(i ? FpLayout.pass : FpLayout.call),t->exit[i].ip) ||
            !fp_equal(t,L"RAX",0,i ? 0x50415353 : 0x46504332,t->exit[i].ax) ||
            !fp_equal(t,L"RSP",0,LongInitialRsp,FIELD(UINT64,Rsp))) return;
        fp_pattern(&want,g+1,FALSE,0,0);
        if (!fp_match(t,L"captured L2 at exit",&want,&w->guest,FALSE)) return;
        if (!i) {
            FIELD(UINT64,Rip)=FixedGuestGpa+FpLayout.resume;
            FIELD(UINT64,ExitCode)=~0ull;
            FIELD(UINT8,TlbControl)=0;
        }
        ++t->checked;
    }
}
#endif

#ifndef HV_PACKAGE
/* Fixed-width hex, no formatting library or full-bank console dump. */
static void fp_word(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *out, UINT64 v, UINTN digits)
{
    CHAR16 buf[17];
    buf[digits]=0;
    while (digits) { buf[--digits]=L"0123456789ABCDEF"[v&15]; v>>=4; }
    out->OutputString(out,buf);
}
static void fp_row(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *out, CHAR16 *label, const FpClue *c)
{
    out->OutputString(out,label); fp_word(out,c->cw,4); out->OutputString(out,L"  ");
    fp_word(out,c->mx,8); out->OutputString(out,L"  "); fp_word(out,c->top,1);
    out->OutputString(out,L"/"); fp_word(out,c->tw,2); out->OutputString(out,L"    ");
    fp_word(out,c->st0_high,4); out->OutputString(out,L"      "); fp_word(out,c->xmm15_high,4);
    out->OutputString(out,L"      "); fp_word(out,c->r15_low,4); out->OutputString(out,L"\r\n");
}
static void fp_report(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *out, UINT8 *vmcb, const FpTrace *t)
{
    out->OutputString(out,L"     FCW   MXCSR     TOP/FTW ST0.sigHi X15H.lo16 R15.lo16 (hex)\r\n");
    for (UINTN i=0; i<t->count; ++i) {
        out->OutputString(out,i ? L"Resume:  EXIT " : L"Initial: EXIT "); fp_word(out,t->exit[i].code,16);
        out->OutputString(out,L" RIP "); fp_word(out,t->exit[i].ip,16);
        out->OutputString(out,L" RAX "); fp_word(out,t->exit[i].ax,16); out->OutputString(out,L"\r\n");
        fp_row(out,L" L1  ",&t->exit[i].host); fp_row(out,L" L2  ",&t->exit[i].guest);
    }
    if (t->checked==2) {
        out->OutputString(out,L"Resumed round-up: SSE(+0.25)="); fp_word(out,t->sse_result,2);
        out->OutputString(out,L", x87(5+6+0.25)="); fp_word(out,t->x87_result,2);
        out->OutputString(out,L" (hex)\r\nPASS: GPR, x87, XMM0-15 and MXCSR retained; caller restored.\r\n");
    } else {
        if (t->field) {
            out->OutputString(out,L"Mismatch: "); out->OutputString(out,t->source);
            out->OutputString(out,L" / "); out->OutputString(out,t->field);
            out->OutputString(out,L" ["); fp_word(out,t->index,2); out->OutputString(out,L"]\r\n");
            hex(out,L"Expected: ",t->expected); hex(out,L"Actual:   ",t->actual);
        }
        if (FIELD(UINT64,Rip)==FixedGuestGpa+FpLayout.fail)
            hex(out,L"L2 failure (generation:detail): ",FIELD(UINT64,Rax));
        out->OutputString(out,L"FAIL: legacy vCPU context or checkpoint mismatch.\r\n");
    }
}
#endif /* !HV_PACKAGE */
#endif
