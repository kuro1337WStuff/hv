#ifndef HV_IRQ_WAKE_H
#define HV_IRQ_WAKE_H

extern const UINT8 IrqStart[];
extern const struct { UINT32 size, wait, after, handler, pass, fail; } IrqLayout;

enum { IrqVector=0x41, IrqPriority=IrqVector>>4, VIrq=1u<<8, IntShadow=0x068,
       Gdtr=0x460, Idtr=0x480, DescGpa=0x6000, IdtOffset=0x100 };

typedef struct { UINT64 count, return_ip, progress; } IrqEvidence;
typedef struct { UINT64 wait_ip; BOOLEAN resumed; } IrqTrace;

static UINT64 irq_ip(UINT32 offset)
{
    return FixedGuestGpa+offset;
}

static BOOLEAN irq_layout_ok(UINTN length)
{
    const UINT32 sites[]={IrqLayout.wait,IrqLayout.after,IrqLayout.handler,IrqLayout.pass,IrqLayout.fail};
    if (!length || length>EFI_PAGE_SIZE) return FALSE;
    for (UINTN i=0; i<sizeof(sites)/sizeof(sites[0]); ++i) {
        if (sites[i]>=length) return FALSE;
        for (UINTN j=0; j<i; ++j) if (sites[j]==sites[i]) return FALSE;
    }
    return IrqLayout.after==IrqLayout.wait+1 && IrqStart[IrqLayout.wait]==0xf4 &&
           IrqStart[IrqLayout.pass]==0xf4 && IrqStart[IrqLayout.fail]==0xf4;
}

static void irq_guest_tables(UINT8 *vmcb, EFI_PHYSICAL_ADDRESS base)
{
    UINT64 *gdt=(UINT64 *)(vmcb+GuestDescriptors*EFI_PAGE_SIZE);
    FixedNpt *guest=(FixedNpt *)(vmcb+GuestTables*EFI_PAGE_SIZE);
    FixedNpt *npt=(FixedNpt *)(vmcb+Pml4*EFI_PAGE_SIZE);
    guest->table[3][DescGpa/EFI_PAGE_SIZE]=DescGpa|0x21;
    npt->table[3][DescGpa/EFI_PAGE_SIZE]=(base+GuestDescriptors*EFI_PAGE_SIZE)|5;
    /* Pre-accessed 64-bit code and flat data; the descriptor page is read-only. */
    gdt[1]=0x00af9b000000ffffull;
    gdt[2]=0x00cf93000000ffffull;
    FIELD(UINT32,Gdtr+4)=3*8-1;
    FIELD(UINT64,Gdtr+8)=DescGpa;
    UINT64 handler=irq_ip(IrqLayout.handler);
    UINT64 *gate=gdt+(IdtOffset+16*IrqVector)/8;
    gate[0]=(handler&0xffff)|(8ull<<16)|(0x8eull<<40)|((handler&0xffff0000)<<32);
    gate[1]=handler>>32; /* present DPL0 interrupt gate, IST=0, selector 8 */
    FIELD(UINT32,Idtr+4)=16*(IrqVector+1)-1;
    FIELD(UINT64,Idtr+8)=DescGpa+IdtOffset;
}

static void irq_run(UINT8 *vmcb, IrqTrace *trace)
{
    volatile IrqEvidence *e=(volatile IrqEvidence *)(vmcb+GuestStack*EFI_PAGE_SIZE);
    __svm_vmrun((UINTN)vmcb);
    trace->wait_ip=FIELD(UINT64,Rip);
    if (FIELD(UINT64,ExitCode)!=0x78 || trace->wait_ip!=irq_ip(IrqLayout.wait) ||
        !(FIELD(UINT64,Flags)&(1u<<9)) || (FIELD(UINT64,IntShadow)&1) ||
        (FIELD(UINT32,IntControl)&VIrq) || FIELD(UINT64,Rsp)!=LongInitialRsp ||
        e->count || e->return_ip || e->progress) return;

    /* Model only our intercepted one-byte HLT. The CPU, not L1, chooses
     * the handler from the guest IDT and pushes this continuation address.
     * AMD APM vol. 2 §§15.21.4–5 and Table B-1. V_TPR=0; VINTR not intercepted. */
    FIELD(UINT64,Rip)=irq_ip(IrqLayout.after);
    FIELD(UINT64,IntControl)=(1ull<<24)|VIrq|(IrqPriority<<16)|((UINT64)IrqVector<<32);
    FIELD(UINT8,TlbControl)=0;
    FIELD(UINT64,ExitCode)=~0ull;
    trace->resumed=TRUE;
    __svm_vmrun((UINTN)vmcb);
}

static void irq_report(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *out, UINT8 *vmcb, const IrqTrace *trace)
{
    volatile IrqEvidence *e=(volatile IrqEvidence *)(vmcb+GuestStack*EFI_PAGE_SIZE);
    hex(out,L"Wait RIP: ",trace->wait_ip);
    if (trace->resumed) {
        out->OutputString(out,L"Wait HLT: IF=1, shadow=0, count=0, progress=0\r\n");
        out->OutputString(out,L"VMRUN 2: V_IRQ vector=41h, priority=4\r\n");
    }
    hex(out,L"Handler count: ",e->count);
    hex(out,L"Stacked return RIP: ",e->return_ip);
    hex(out,L"Continuation progress: ",e->progress);
    hex(out,L"Final RIP: ",FIELD(UINT64,Rip));
    hex(out,L"Final RAX: ",FIELD(UINT64,Rax));
    if (trace->resumed && FIELD(UINT64,ExitCode)==0x78 && FIELD(UINT64,Rip)==irq_ip(IrqLayout.pass) &&
        FIELD(UINT64,Rax)==0x50415353 && !(FIELD(UINT32,IntControl)&VIrq) &&
        FIELD(UINT64,Rsp)==LongInitialRsp && (FIELD(UINT64,Flags)&(1u<<9)) &&
        e->count==1 && e->return_ip==irq_ip(IrqLayout.after) && e->progress==1) {
        out->OutputString(out,L"PASS: virtual IRQ consumed; IRETQ resumed after HLT.\r\n");
        return;
    }
    out->OutputString(out,L"FAIL: unexpected exit, interrupt delivery or continuation.\r\n");
    hex(out,L"VMRUN count: ",trace->resumed ? 2 : 1);
    hex(out,L"EXITCODE: ",FIELD(UINT64,ExitCode));
    hex(out,L"EXITINFO1: ",FIELD(UINT64,Info1));
    hex(out,L"EXITINFO2: ",FIELD(UINT64,Info2));
    hex(out,L"EXITINTINFO: ",FIELD(UINT64,0x088));
    hex(out,L"Interrupt control: ",FIELD(UINT64,IntControl));
    hex(out,L"Interrupt shadow: ",FIELD(UINT64,IntShadow));
    hex(out,L"RFLAGS: ",FIELD(UINT64,Flags));
    hex(out,L"RSP: ",FIELD(UINT64,Rsp));
}
#endif
