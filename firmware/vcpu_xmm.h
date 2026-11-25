#ifndef HV_VCPU_XMM_H
#define HV_VCPU_XMM_H

extern const UINT8 XmmStart[];
extern const struct { UINT32 size, call, resume, pass, fail; } XmmLayout;
_Static_assert(sizeof(XmmLayout)==5*sizeof(UINT32), "XMM layout ABI mismatch");

enum { XmmLanes=32, XmmEntries=2 };
typedef UINT64 XmmBank[XmmLanes]; /* XMM0 low, XMM0 high, ... XMM15 high */
typedef struct { XmmBank guest, saved, seed, observed; } XmmWorkspace;
_Static_assert(sizeof(XmmBank)==0x100 && sizeof(XmmWorkspace)==0x400 &&
               sizeof(XmmWorkspace)<=EFI_PAGE_SIZE, "XMM workspace ABI mismatch");
_Static_assert(LongStackGpa==0x7000 && 2*sizeof(XmmBank)<LongInitialRsp-LongStackGpa,
               "guest entry snapshots must fit below the guest stack");
extern void RunXmmGuest(UINTN vmcb_pa, XmmWorkspace *workspace);

typedef struct {
    struct { UINT64 code, ip, host_last, guest_last; } exit[XmmEntries];
    UINTN count, checked, bad_lane;
    CHAR16 *bad_bank;
    UINT64 actual, expected;
} XmmTrace;

static BOOLEAN xmm_layout_ok(UINTN length)
{
    if (length!=XmmLayout.size || length>EFI_PAGE_SIZE || XmmLayout.fail>=length ||
        !(0<XmmLayout.call && XmmLayout.call<XmmLayout.resume &&
          XmmLayout.resume<XmmLayout.pass && XmmLayout.pass<XmmLayout.fail)) return FALSE;
    return XmmLayout.resume-XmmLayout.call==3 && XmmStart[XmmLayout.call]==0x0f &&
           XmmStart[XmmLayout.call+1]==0x01 && XmmStart[XmmLayout.call+2]==0xd9 &&
           XmmStart[XmmLayout.pass]==0xf4 && XmmStart[XmmLayout.fail]==0xf4 &&
           length-XmmLayout.fail-1==3*sizeof(XmmBank);
}

/* Both halves distinguish the owner, generation and physical lane. Derive
 * the reference independently of the guest's read-only MASM tables. */
#define XMM_HOST_BASE 0x48530000b1c70000ull
#define XMM_GUEST_BASE 0x47530000a6e90000ull
static UINT64 xmm_pattern(UINT64 base, UINTN generation, UINTN lane)
{
    UINT64 id=(generation<<8)+lane;
    return base+(id<<32)+id;
}

static BOOLEAN xmm_bank_ok(XmmTrace *trace, CHAR16 *name, const volatile UINT64 *bank,
                           UINT64 base, UINTN generation)
{
    for (UINTN lane=0; lane<XmmLanes; ++lane) {
        UINT64 expected=xmm_pattern(base,generation,lane), actual=bank[lane];
        if (actual!=expected) {
            trace->bad_bank=name;
            trace->bad_lane=lane;
            trace->expected=expected;
            trace->actual=actual;
            return FALSE;
        }
    }
    return TRUE;
}

static void xmm_run(UINT8 *vmcb, XmmTrace *trace)
{
    XmmWorkspace *w=(XmmWorkspace *)(vmcb+XmmScratch*EFI_PAGE_SIZE);
    const volatile XmmBank *snap=(const volatile XmmBank *)(vmcb+GuestStack*EFI_PAGE_SIZE);
    trace->count=trace->checked=0;
    trace->bad_bank=NULL;
    for (UINTN lane=0; lane<XmmLanes; ++lane) w->guest[lane]=xmm_pattern(XMM_GUEST_BASE,0,lane);
    for (UINTN i=0; i<XmmEntries; ++i) {
        for (UINTN lane=0; lane<XmmLanes; ++lane) w->seed[lane]=xmm_pattern(XMM_HOST_BASE,i,lane);
        RunXmmGuest((UINTN)vmcb,w);
        trace->count=i+1;
        trace->exit[i].code=FIELD(UINT64,ExitCode);
        trace->exit[i].ip=FIELD(UINT64,Rip);
        trace->exit[i].host_last=w->observed[XmmLanes-1];
        trace->exit[i].guest_last=w->guest[XmmLanes-1];
        if (!xmm_bank_ok(trace,L"live host after restore",w->observed,XMM_HOST_BASE,i) ||
            !xmm_bank_ok(trace,L"host save frame",w->saved,XMM_HOST_BASE,i) ||
            !xmm_bank_ok(trace,L"L2 entry snapshot",snap[i],XMM_GUEST_BASE,i) ||
            !xmm_bank_ok(trace,L"guest exit frame",w->guest,XMM_GUEST_BASE,i+1) ||
            FIELD(UINT64,Rsp)!=LongInitialRsp) return;
        if (i==0) {
            if (trace->exit[i].code!=0x81 || trace->exit[i].ip!=FixedGuestGpa+XmmLayout.call) return;
            /* Advance only the immutable, opcode-checked VMMCALL to its label. */
            FIELD(UINT64,Rip)=FixedGuestGpa+XmmLayout.resume;
            FIELD(UINT64,ExitCode)=~0ull;
            FIELD(UINT8,TlbControl)=0;
        } else if (trace->exit[i].code!=0x78 || trace->exit[i].ip!=FixedGuestGpa+XmmLayout.pass ||
                   FIELD(UINT64,Rax)!=0x50415353) return;
        ++trace->checked;
    }
}

static void xmm_report(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *out, UINT8 *vmcb, const XmmTrace *trace)
{
    for (UINTN i=0; i<trace->count; ++i) {
        out->OutputString(out,i ? L"Entry 2 (resume):\r\n" : L"Entry 1 (initial):\r\n");
        hex(out,L"  EXITCODE: ",trace->exit[i].code);
        hex(out,L"  RIP: ",trace->exit[i].ip);
        hex(out,L"  Restored host XMM15.high: ",trace->exit[i].host_last);
        hex(out,L"  Captured guest XMM15.high: ",trace->exit[i].guest_last);
    }
    if (trace->checked==XmmEntries) {
        out->OutputString(out,L"PASS: all 32 XMM lanes matched on both sides of VMMCALL/resume.\r\n");
    } else {
        if (trace->bad_bank) {
            out->OutputString(out,L"First mismatched bank: ");
            out->OutputString(out,trace->bad_bank);
            out->OutputString(out,L"\r\n");
            hex(out,L"XMM index: ",trace->bad_lane/2);
            hex(out,L"Lane (0=low, 1=high): ",trace->bad_lane%2);
            hex(out,L"Expected: ",trace->expected);
            hex(out,L"Actual: ",trace->actual);
        }
        hex(out,L"Final RSP: ",FIELD(UINT64,Rsp));
        hex(out,L"Final RAX: ",FIELD(UINT64,Rax));
        out->OutputString(out,L"FAIL: XMM switch, entry snapshot or guest checkpoint mismatch.\r\n");
    }
}
#endif
