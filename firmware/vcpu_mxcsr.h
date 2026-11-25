#ifndef HV_VCPU_MXCSR_H
#define HV_VCPU_MXCSR_H

extern const UINT8 MxStart[];
extern const struct { UINT32 size, call, resume, pass, fail; } MxLayout;
typedef struct { UINT32 guest, seed, saved, observed; } MxFrame;
extern void RunMxGuest(UINTN vmcb_pa, MxFrame *frame);
typedef struct { UINT32 incoming, rounded, after; } MxProbe;
_Static_assert(sizeof(MxLayout)==20 && sizeof(MxFrame)==16 && sizeof(MxProbe)==12,
               "MXCSR assembly ABI mismatch");
_Static_assert(LongStackGpa==0x7000 && 2*sizeof(MxProbe)<LongInitialRsp-LongStackGpa,
               "MXCSR probes must fit below the guest stack");

typedef struct {
    struct { UINT64 code, ip; MxFrame frame; MxProbe probe; } entry[2];
    UINTN count, checked;
} MxTrace;

static BOOLEAN mx_layout_ok(UINTN length)
{
    if (length!=MxLayout.size || length>EFI_PAGE_SIZE || MxLayout.fail>=length ||
        !(0<MxLayout.call && MxLayout.call<MxLayout.resume && MxLayout.resume<MxLayout.pass &&
          MxLayout.pass<MxLayout.fail)) return FALSE;
    return MxLayout.resume-MxLayout.call==3 && MxStart[MxLayout.call]==0x0f &&
           MxStart[MxLayout.call+1]==1 && MxStart[MxLayout.call+2]==0xd9 &&
           MxStart[MxLayout.pass]==0xf4 && MxStart[MxLayout.fail]==0xf4;
}

static void mx_run(UINT8 *vmcb, MxTrace *trace)
{
    const volatile MxProbe *probe=(const volatile MxProbe *)(vmcb+GuestStack*EFI_PAGE_SIZE);
    MxFrame frame={0x3f80,0,0,0}; /* L2 starts rounding down; all flags clear. */
    trace->count=trace->checked=0;
    for (UINTN i=0; i<2; ++i) {
        UINT32 incoming=i ? 0x5f80 : 0x3f80;
        UINT32 host=i ? 0x7f84 : 0x1f81; /* truncation + ZE vs nearest + IE */
        frame.seed=host;
        RunMxGuest((UINTN)vmcb,&frame);
        trace->count=i+1;
        trace->entry[i].code=FIELD(UINT64,ExitCode);
        trace->entry[i].ip=FIELD(UINT64,Rip);
        trace->entry[i].frame=frame;
        trace->entry[i].probe=probe[i];
        if (frame.saved!=host || frame.observed!=host || frame.guest!=(i ? 0x5fa0u : 0x5f80u) ||
            trace->entry[i].probe.incoming!=incoming || trace->entry[i].probe.rounded!=i+1 ||
            trace->entry[i].probe.after!=(incoming|0x20u) || FIELD(UINT64,Rsp)!=LongInitialRsp) return;
        if (!i) {
            if (trace->entry[i].code!=0x81 || trace->entry[i].ip!=FixedGuestGpa+MxLayout.call) return;
            /* Advance only this exact, opcode-validated VMMCALL; keep captured L2 MXCSR. */
            FIELD(UINT64,Rip)=FixedGuestGpa+MxLayout.resume;
            FIELD(UINT64,ExitCode)=~0ull;
            FIELD(UINT8,TlbControl)=0;
        } else if (trace->entry[i].code!=0x78 || trace->entry[i].ip!=FixedGuestGpa+MxLayout.pass ||
                   FIELD(UINT64,Rax)!=0x50415353) return;
        ++trace->checked;
    }
}

static void mx_column(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *out, UINT32 value)
{
    CHAR16 buf[]=L"00000000 ";
    for (UINTN n=8; n; value>>=4) buf[--n]=L"0123456789ABCDEF"[value&15];
    out->OutputString(out,buf);
}

static void mx_report(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *out, UINT8 *vmcb, const MxTrace *trace)
{
    out->OutputString(out,L"# L1 seed  saved    restored L2 entry round    post-CVT exit-MX  (hex)\r\n");
    for (UINTN i=0; i<trace->count; ++i) {
        const MxFrame *f=&trace->entry[i].frame;
        const MxProbe *p=&trace->entry[i].probe;
        out->OutputString(out,i ? L"2 " : L"1 ");
        mx_column(out,f->seed); mx_column(out,f->saved); mx_column(out,f->observed);
        mx_column(out,p->incoming); mx_column(out,p->rounded); mx_column(out,p->after);
        mx_column(out,f->guest); out->OutputString(out,L"\r\n");
        hex(out,L"  EXITCODE: ",trace->entry[i].code);
        hex(out,L"  RIP: ",trace->entry[i].ip);
    }
    hex(out,L"Final RAX: ",FIELD(UINT64,Rax));
    if (trace->checked==2)
        out->OutputString(out,L"PASS: MXCSR restored; 1.5 rounded down to 1, up to 2 (PE=0x20).\r\n");
    else
        out->OutputString(out,L"FAIL: MXCSR switch, conversion or fixed exit mismatch.\r\n");
}
#endif
