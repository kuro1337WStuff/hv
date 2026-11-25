#ifndef HV_EXCEPTION_UD_H
#define HV_EXCEPTION_UD_H

extern const UINT8 UdStart[];
extern const struct { UINT32 size, fault, after, handler, pass, fail; } UdLayout;
_Static_assert(sizeof(UdLayout)==6*sizeof(UINT32), "#UD layout ABI mismatch");

enum { UdEventInj=0x0a8, UdExit=0x40+6, UdHltExit=0x78 };
/* The guest writes these at the bottom of its stack page, outside the IDT frame. */
typedef struct { UINT64 count, fault_ip, phase; } UdEvidence;
typedef struct { UINT64 first_exit, first_ip, injection; } UdTrace;

static BOOLEAN ud_layout_ok(UINTN length)
{
    if (length!=UdLayout.size || length>EFI_PAGE_SIZE || UdLayout.handler>=length ||
        !(UdLayout.fault<UdLayout.after && UdLayout.after<UdLayout.pass &&
          UdLayout.pass<UdLayout.fail && UdLayout.fail<UdLayout.handler)) return FALSE;
    return UdLayout.after-UdLayout.fault==2 && UdStart[UdLayout.fault]==0x0f &&
           UdStart[UdLayout.fault+1]==0x0b && UdStart[UdLayout.pass]==0xf4 &&
           UdStart[UdLayout.fail]==0xf4;
}

static void ud_run(UINT8 *vmcb, UdTrace *trace)
{
    volatile UdEvidence *e=(volatile UdEvidence *)(vmcb+GuestStack*EFI_PAGE_SIZE);
    trace->injection=0;
    __svm_vmrun((UINTN)vmcb);
    trace->first_exit=FIELD(UINT64,ExitCode);
    trace->first_ip=FIELD(UINT64,Rip);
    if (trace->first_exit!=UdExit || trace->first_ip!=FixedGuestGpa+UdLayout.fault ||
        FIELD(UINT64,Rsp)!=LongInitialRsp || e->phase!=1 || e->count || e->fault_ip) return;

    /* VMRUN injects the event without intercepting it. Leave every exception
     * intercept armed: a replay of the actual UD2 must exit, not be retried. */
    FIELD(UINT64,UdEventInj)=(1ull<<31)|(3u<<8)|6; /* valid, exception, no error code */
    trace->injection=FIELD(UINT64,UdEventInj);
    FIELD(UINT8,TlbControl)=0;
    FIELD(UINT64,ExitCode)=~0ull;
    __svm_vmrun((UINTN)vmcb); /* RIP still names UD2, never the instruction after it. */
}

static BOOLEAN ud_passed(UINT8 *vmcb, const UdTrace *trace)
{
    volatile UdEvidence *e=(volatile UdEvidence *)(vmcb+GuestStack*EFI_PAGE_SIZE);
    return trace->injection==0x80000306u && e->count==1 && e->fault_ip==trace->first_ip &&
           e->phase==3 && FIELD(UINT64,ExitCode)==UdHltExit &&
           FIELD(UINT64,Rip)==FixedGuestGpa+UdLayout.pass && FIELD(UINT64,Rsp)==LongInitialRsp &&
           FIELD(UINT64,Rax)==0x50415353;
}

#ifndef HV_PACKAGE
static void ud_report(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *out, UINT8 *vmcb, const UdTrace *trace)
{
    volatile UdEvidence *e=(volatile UdEvidence *)(vmcb+GuestStack*EFI_PAGE_SIZE);
    hex(out,L"Fault EXITCODE: ",trace->first_exit);
    hex(out,L"Intercepted RIP: ",trace->first_ip);
    hex(out,L"Submitted EVENTINJ: ",trace->injection);
    hex(out,L"Handler count: ",e->count);
    hex(out,L"Stacked fault RIP: ",e->fault_ip);
    hex(out,L"Continuation phase: ",e->phase);
    hex(out,L"Final EXITCODE: ",FIELD(UINT64,ExitCode));
    hex(out,L"Final RIP: ",FIELD(UINT64,Rip));
    hex(out,L"Final RSP: ",FIELD(UINT64,Rsp));
    hex(out,L"Final RAX: ",FIELD(UINT64,Rax));
    if (ud_passed(vmcb,trace)) {
        out->OutputString(out,L"PASS: #UD delivered once; IRETQ resumed after UD2.\r\n");
    } else {
        out->OutputString(out,L"FAIL: #UD intercept, exception frame or return mismatch.\r\n");
    }
}
#endif

#endif
