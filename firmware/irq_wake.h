#ifndef HV_IRQ_WAKE_H
#define HV_IRQ_WAKE_H

extern const UINT8 IrqStart[];
#ifdef HV_IRQ_SHADOW
extern const struct { UINT32 size, checkpoint, sti, afterShadow, handler, pass, fail; } IrqLayout;
_Static_assert(sizeof(IrqLayout)==7*sizeof(UINT32), "IRQ shadow layout ABI mismatch");
#elif defined(HV_IRQ_PRIORITY)
extern const struct { UINT32 size, checkpoint, after, handler, pass, fail; } IrqLayout;
_Static_assert(sizeof(IrqLayout)==6*sizeof(UINT32), "IRQ priority layout ABI mismatch");
#else
extern const struct { UINT32 size, wait, after, handler, pass, fail; } IrqLayout;
_Static_assert(sizeof(IrqLayout)==6*sizeof(UINT32), "IRQ wake layout ABI mismatch");
#endif

enum { IrqVector=0x41, IrqPriority=IrqVector>>4, VIrq=1u<<8, IntShadow=0x068,
       Gdtr=0x460, Idtr=0x480, DescGpa=0x6000, IdtOffset=0x100 };

typedef struct { UINT64 count, return_ip, progress, captured_phase; } IrqEvidence;
typedef struct {
    UINT64 checkpoint_ip;
    UINT32 vmruns;
#if defined(HV_IRQ_SHADOW) || defined(HV_IRQ_PRIORITY)
    UINT64 checkpoint_flags, checkpoint_control, checkpoint_shadow, checkpoint_rsp;
    UINT64 checkpoint_phase, checkpoint_count;
    BOOLEAN checkpoint_ok;
#endif
} IrqTrace;

static UINT64 irq_ip(UINT32 offset)
{
    return FixedGuestGpa+offset;
}

static BOOLEAN irq_layout_ok(UINTN length)
{
#ifdef HV_IRQ_SHADOW
    const UINT32 sites[]={IrqLayout.checkpoint,IrqLayout.sti,IrqLayout.afterShadow,
                          IrqLayout.handler,IrqLayout.pass,IrqLayout.fail};
#elif defined(HV_IRQ_PRIORITY)
    const UINT32 sites[]={IrqLayout.checkpoint,IrqLayout.after,IrqLayout.handler,IrqLayout.pass,IrqLayout.fail};
#else
    const UINT32 sites[]={IrqLayout.wait,IrqLayout.after,IrqLayout.handler,IrqLayout.pass,IrqLayout.fail};
#endif
    if (!length || length>EFI_PAGE_SIZE) return FALSE;
    for (UINTN i=0; i<sizeof(sites)/sizeof(sites[0]); ++i) {
        if (sites[i]>=length) return FALSE;
        for (UINTN j=0; j<i; ++j) if (sites[j]==sites[i]) return FALSE;
    }
#ifdef HV_IRQ_SHADOW
    /* At the checkpoint the only legal path is VMMCALL; STI; INC qword [7010h].
     * Check that exact window before trusting its return boundary. */
    static const UINT8 window[]={0x0f,0x01,0xd9,0xfb,0x48,0xff,0x04,0x25,0x10,0x70,0,0};
    if (IrqLayout.sti!=IrqLayout.checkpoint+3 ||
        IrqLayout.afterShadow!=IrqLayout.checkpoint+sizeof(window)) return FALSE;
    for (UINTN i=0; i<sizeof(window); ++i)
        if (IrqStart[IrqLayout.checkpoint+i]!=window[i]) return FALSE;
    return IrqStart[IrqLayout.pass]==0xf4 && IrqStart[IrqLayout.fail]==0xf4;
#elif defined(HV_IRQ_PRIORITY)
    return IrqLayout.after==IrqLayout.checkpoint+3 &&
           IrqStart[IrqLayout.checkpoint]==0x0f && IrqStart[IrqLayout.checkpoint+1]==0x01 &&
           IrqStart[IrqLayout.checkpoint+2]==0xd9 &&
           IrqStart[IrqLayout.pass]==0xf4 && IrqStart[IrqLayout.fail]==0xf4;
#else
    return IrqLayout.after==IrqLayout.wait+1 && IrqStart[IrqLayout.wait]==0xf4 &&
           IrqStart[IrqLayout.pass]==0xf4 && IrqStart[IrqLayout.fail]==0xf4;
#endif
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
#if defined(HV_IRQ_SHADOW) || defined(HV_IRQ_PRIORITY)
    __svm_vmrun((UINTN)vmcb);
    trace->vmruns=1;
    trace->checkpoint_ip=FIELD(UINT64,Rip);
    trace->checkpoint_flags=FIELD(UINT64,Flags);
    trace->checkpoint_control=FIELD(UINT64,IntControl);
    trace->checkpoint_shadow=FIELD(UINT64,IntShadow);
    trace->checkpoint_rsp=FIELD(UINT64,Rsp);
    trace->checkpoint_phase=e->progress;
    trace->checkpoint_count=e->count;
    trace->checkpoint_ok=FIELD(UINT64,ExitCode)==0x81 &&
        trace->checkpoint_ip==irq_ip(IrqLayout.checkpoint) &&
#ifdef HV_IRQ_PRIORITY
        (trace->checkpoint_flags&(1u<<9)) && (trace->checkpoint_control&VIrq) &&
        !(trace->checkpoint_control&(1u<<20)) && /* V_IGN_TPR */
        ((trace->checkpoint_control>>32)&0xff)==IrqVector &&
        ((trace->checkpoint_control>>16)&15)==IrqPriority &&
        (trace->checkpoint_control&15)==IrqPriority &&
#else
        !(trace->checkpoint_flags&(1u<<9)) && (trace->checkpoint_control&VIrq) &&
#endif
        trace->checkpoint_shadow==0 && trace->checkpoint_rsp==LongInitialRsp &&
        trace->checkpoint_phase==1 && trace->checkpoint_count==0 &&
        e->return_ip==0 && e->captured_phase==0;
    /* The first VMRUN must stop at VMMCALL with the single queued V_IRQ still pending. */
    if (!trace->checkpoint_ok) return;
#ifdef HV_IRQ_PRIORITY
    /* Keep the original request: change only the threshold nibble. */
    FIELD(UINT8,IntControl)=(UINT8)((trace->checkpoint_control&0xf0)|(IrqPriority-1));
    FIELD(UINT64,Rip)=irq_ip(IrqLayout.after);
#else
    FIELD(UINT64,Rip)=irq_ip(IrqLayout.sti);
#endif
    FIELD(UINT8,TlbControl)=0;
    FIELD(UINT64,ExitCode)=~0ull;
    trace->vmruns=2;
    __svm_vmrun((UINTN)vmcb);
#else
    __svm_vmrun((UINTN)vmcb);
    trace->vmruns=1;
    trace->checkpoint_ip=FIELD(UINT64,Rip);
    if (FIELD(UINT64,ExitCode)!=0x78 || trace->checkpoint_ip!=irq_ip(IrqLayout.wait) ||
        !(FIELD(UINT64,Flags)&(1u<<9)) || (FIELD(UINT64,IntShadow)&1) ||
        (FIELD(UINT32,IntControl)&VIrq) || FIELD(UINT64,Rsp)!=LongInitialRsp ||
        e->count || e->return_ip || e->progress) return;

    /* Model only our intercepted one-byte HLT; the CPU chooses the IDT handler. */
    FIELD(UINT64,Rip)=irq_ip(IrqLayout.after);
    FIELD(UINT64,IntControl)=(1ull<<24)|VIrq|(IrqPriority<<16)|((UINT64)IrqVector<<32);
    FIELD(UINT8,TlbControl)=0;
    FIELD(UINT64,ExitCode)=~0ull;
    trace->vmruns=2;
    __svm_vmrun((UINTN)vmcb);
#endif
}

#ifdef HV_IRQ_PRIORITY
/* Keep both snapshots visible on one 80x25 firmware console. */
static void irq_pair(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *out, CHAR16 *label, UINT64 a, UINT64 b)
{
    CHAR16 buf[18];
    UINTN n=sizeof(buf)/sizeof(buf[0]);
    buf[--n]=0;
    do { buf[--n]=L"0123456789ABCDEF"[a&15]; a>>=4; } while (a);
    out->OutputString(out,label);
    out->OutputString(out,buf+n);
    hex(out,L" / ",b);
}
#endif

static void irq_report(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *out, UINT8 *vmcb, const IrqTrace *trace)
{
    volatile IrqEvidence *e=(volatile IrqEvidence *)(vmcb+GuestStack*EFI_PAGE_SIZE);
#ifdef HV_IRQ_PRIORITY
    UINT64 control=FIELD(UINT64,IntControl);
    irq_pair(out,L"Checkpoint RIP / V_TPR: ",trace->checkpoint_ip,trace->checkpoint_control&15);
    irq_pair(out,L"Queued vector / priority: ",(trace->checkpoint_control>>32)&0xff,(trace->checkpoint_control>>16)&15);
    irq_pair(out,L"Equal pending / V_IGN_TPR: ",!!(trace->checkpoint_control&VIrq),(trace->checkpoint_control>>20)&1);
    irq_pair(out,L"Equal IF / shadow: ",(trace->checkpoint_flags>>9)&1,trace->checkpoint_shadow);
    irq_pair(out,L"Equal phase / count: ",trace->checkpoint_phase,trace->checkpoint_count);
    irq_pair(out,L"Final V_TPR / pending: ",control&15,!!(control&VIrq));
    irq_pair(out,L"Final IF / shadow: ",(FIELD(UINT64,Flags)>>9)&1,FIELD(UINT64,IntShadow));
    irq_pair(out,L"Handler phase / count: ",e->captured_phase,e->count);
    irq_pair(out,L"Stacked RIP / final phase: ",e->return_ip,e->progress);
    irq_pair(out,L"Final RIP / RSP: ",FIELD(UINT64,Rip),FIELD(UINT64,Rsp));
    irq_pair(out,L"Final exit / RAX: ",FIELD(UINT64,ExitCode),FIELD(UINT64,Rax));
    if (trace->checkpoint_ok && trace->vmruns==2 && FIELD(UINT64,ExitCode)==0x78 &&
        FIELD(UINT64,Rip)==irq_ip(IrqLayout.pass) && FIELD(UINT64,Rax)==0x50415353 &&
        !(control&VIrq) && (control&15)==IrqPriority-1 &&
        FIELD(UINT64,Rsp)==LongInitialRsp && (FIELD(UINT64,Flags)&(1u<<9)) &&
        FIELD(UINT64,IntShadow)==0 && e->count==1 && e->captured_phase==1 &&
        e->return_ip==irq_ip(IrqLayout.after) && e->progress==2) {
        out->OutputString(out,L"PASS: equal V_TPR blocked; lower V_TPR delivered once.\r\n");
        return;
    }
    out->OutputString(out,L"FAIL: priority checkpoint or delivery mismatch.\r\n");
    hex(out,L"VMRUN count: ",trace->vmruns);
    hex(out,L"EXITCODE: ",FIELD(UINT64,ExitCode));
#elif defined(HV_IRQ_SHADOW)
    hex(out,L"Checkpoint RIP: ",trace->checkpoint_ip);
    hex(out,L"Masked IF: ",(trace->checkpoint_flags>>9)&1);
    hex(out,L"Masked V_IRQ pending: ",!!(trace->checkpoint_control&VIrq));
    hex(out,L"Masked phase: ",trace->checkpoint_phase);
    hex(out,L"Masked handler count: ",trace->checkpoint_count);
    hex(out,L"Handler phase: ",e->captured_phase);
    hex(out,L"Final IF: ",(FIELD(UINT64,Flags)>>9)&1);
    hex(out,L"Final V_IRQ pending: ",!!(FIELD(UINT32,IntControl)&VIrq));
    hex(out,L"Final RSP: ",FIELD(UINT64,Rsp));
    hex(out,L"Final RIP: ",FIELD(UINT64,Rip));
    hex(out,L"Final RAX: ",FIELD(UINT64,Rax));
    hex(out,L"Handler count: ",e->count);
    hex(out,L"Stacked return RIP: ",e->return_ip);
    hex(out,L"Final phase: ",e->progress);
    if (trace->vmruns==2 && trace->checkpoint_ok && FIELD(UINT64,ExitCode)==0x78 &&
        FIELD(UINT64,Rip)==irq_ip(IrqLayout.pass) && FIELD(UINT64,Rax)==0x50415353 &&
        !(FIELD(UINT32,IntControl)&VIrq) && FIELD(UINT64,Rsp)==LongInitialRsp &&
        (FIELD(UINT64,Flags)&(1u<<9)) && e->count==1 &&
        e->return_ip==irq_ip(IrqLayout.afterShadow) && e->progress==3 && e->captured_phase==2) {
        out->OutputString(out,L"PASS: masked IRQ deferred through STI shadow and consumed once.\r\n");
        return;
    }
    out->OutputString(out,L"FAIL: masked IRQ checkpoint or shadow delivery mismatch.\r\n");
    hex(out,L"VMRUN count: ",trace->vmruns);
    hex(out,L"EXITCODE: ",FIELD(UINT64,ExitCode));
    hex(out,L"EXITINFO1: ",FIELD(UINT64,Info1));
    hex(out,L"EXITINFO2: ",FIELD(UINT64,Info2));
    hex(out,L"EXITINTINFO: ",FIELD(UINT64,0x088));
    hex(out,L"Interrupt shadow: ",FIELD(UINT64,IntShadow));
#else
    hex(out,L"Wait RIP: ",trace->checkpoint_ip);
    if (trace->vmruns==2) {
        out->OutputString(out,L"Wait HLT: IF=1, shadow=0, count=0, progress=0\r\n");
        out->OutputString(out,L"VMRUN 2: V_IRQ vector=41h, priority=4\r\n");
    }
    hex(out,L"Handler count: ",e->count);
    hex(out,L"Stacked return RIP: ",e->return_ip);
    hex(out,L"Continuation progress: ",e->progress);
    hex(out,L"Final RIP: ",FIELD(UINT64,Rip));
    hex(out,L"Final RAX: ",FIELD(UINT64,Rax));
    if (trace->vmruns==2 && FIELD(UINT64,ExitCode)==0x78 && FIELD(UINT64,Rip)==irq_ip(IrqLayout.pass) &&
        FIELD(UINT64,Rax)==0x50415353 && !(FIELD(UINT32,IntControl)&VIrq) &&
        FIELD(UINT64,Rsp)==LongInitialRsp && (FIELD(UINT64,Flags)&(1u<<9)) &&
        e->count==1 && e->return_ip==irq_ip(IrqLayout.after) && e->progress==1) {
        out->OutputString(out,L"PASS: virtual IRQ consumed; IRETQ resumed after HLT.\r\n");
        return;
    }
    out->OutputString(out,L"FAIL: unexpected exit, interrupt delivery or continuation.\r\n");
    hex(out,L"VMRUN count: ",trace->vmruns);
    hex(out,L"EXITCODE: ",FIELD(UINT64,ExitCode));
    hex(out,L"EXITINFO1: ",FIELD(UINT64,Info1));
    hex(out,L"EXITINFO2: ",FIELD(UINT64,Info2));
    hex(out,L"EXITINTINFO: ",FIELD(UINT64,0x088));
    hex(out,L"Interrupt control: ",FIELD(UINT64,IntControl));
    hex(out,L"Interrupt shadow: ",FIELD(UINT64,IntShadow));
    hex(out,L"RFLAGS: ",FIELD(UINT64,Flags));
    hex(out,L"RSP: ",FIELD(UINT64,Rsp));
#endif
}

#endif
