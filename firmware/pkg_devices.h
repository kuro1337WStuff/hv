#ifndef HV_PKG_DEVICES_H
#define HV_PKG_DEVICES_H

extern void RunGprGuest(UINTN vmcb_pa, UINT64 frame[14]);
extern const UINT8 P4Start[], P4AfterShadow[], P4Equal[], P4Wait[], P4Out[], P4Stop[],
                   P4Handler[], P4Eoi[], P4End[];

enum { DevVector=0x41, DevClass=DevVector>>4, DevPending=1<<8, DevIf=1<<9,
       DevShadow=0x068, DevHlt=0x78, DevIoio=0x7b, DevCall=0x81 };
typedef struct { UINT64 shadow, count, woke, stacked; } DevEvidence; /* GPA 7000h */
typedef struct {
    UINT64 frame[14];            /* RunGprGuest; includes the guest's string/port registers */
    unsigned eois;
    BOOLEAN awaiting_eoi;       /* No new edge can be raised until the ISR acknowledges this one. */
} DevController;

static UINT64 dev_ip(const UINT8 *site)
{
    return FixedGuestGpa + ((UINTN)site-(UINTN)P4Start);
}

static BOOLEAN dev_layout(void)
{
    UINTN size=(UINTN)P4End-(UINTN)P4Start;
    const UINT8 *calls[]={P4Start,P4AfterShadow,P4Equal,P4Eoi};
    if (size<4 || size>EFI_PAGE_SIZE) return FALSE;
    for (UINTN i=0; i<sizeof(calls)/sizeof(calls[0]); ++i) {
        const UINT8 *p=calls[i];
        if ((UINTN)p-(UINTN)P4Start>size-4 || p[0]!=0x0f || p[1]!=1 || p[2]!=0xd9) return FALSE;
    }
    return (UINTN)P4Wait-(UINTN)P4Start<size-1 && P4Wait[0]==0xf4 &&
           (UINTN)P4Stop-(UINTN)P4Start<size && P4Stop[0]==0xf4 &&
           (UINTN)P4Out-(UINTN)P4Start<size-1 && P4Out[0]==0xee &&
           (UINTN)P4Handler-(UINTN)P4Start<size && P4Start[3]==0xfb &&
           P4AfterShadow[3]==0x90 && (UINTN)P4Equal==(UINTN)P4AfterShadow+4 &&
           (UINTN)P4Wait==(UINTN)P4Equal+3;
}

/* Every call permits exactly one expected exit. Resume sites come from the
 * checked, read-only image, not an unchecked next-RIP field. */
static BOOLEAN dev_run(UINT8 *vmcb, DevController *c, UINT64 exitcode, const UINT8 *site)
{
    FIELD(UINT64,ExitCode)=~0ull;
    FIELD(UINT64,Info2)=0;
    RunGprGuest((UINTN)vmcb,c->frame);
    FIELD(UINT8,TlbControl)=0;
    return FIELD(UINT64,ExitCode)==exitcode && FIELD(UINT64,Rip)==dev_ip(site);
}

static BOOLEAN dev_unstacked(UINT8 *vmcb, unsigned interrupt_flag)
{
    return FIELD(UINT64,Rsp)==LongInitialRsp && (FIELD(UINT64,Flags)&DevIf)==interrupt_flag &&
           !(FIELD(UINT64,DevShadow)&1);
}

static UINT64 dev_request_word(unsigned tpr)
{
    return ((UINT64)DevVector<<32)|(1ull<<24)|DevPending|(DevClass<<16)|tpr;
}

static UINT64 dev_irq_fields(UINT8 *vmcb)
{
    /* Only the vector, V_INTR_MASKING, V_IGN_TPR, priority, pending and TPR
     * participate in this oracle. Do not pin unrelated control bits to zero. */
    return FIELD(UINT64,IntControl)&((0xffull<<32)|(1u<<24)|(1u<<20)|(15u<<16)|DevPending|0xff);
}

static BOOLEAN dev_raise(UINT8 *vmcb, DevController *c, unsigned tpr)
{
    if (c->awaiting_eoi || (FIELD(UINT64,IntControl)&DevPending)) return FALSE;
    c->awaiting_eoi=TRUE;
    FIELD(UINT64,IntControl)=dev_request_word(tpr);
    return TRUE;
}

/* The fixed guest controller acknowledges by VMMCALL(EAX=vector) inside its
 * IDT gate, not by host I/O or an emulated physical APIC. A duplicate, early
 * or wrong-vector EOI cannot retire the edge. */
static BOOLEAN dev_eoi(UINT8 *vmcb, DevController *c, volatile DevEvidence *e,
                       const UINT8 *return_site, const char *label)
{
    if (!dev_run(vmcb,c,DevCall,P4Eoi)) return FALSE;
    pkg_observe(label,e->stacked);
    UINT64 rsp=FIELD(UINT64,Rsp);
    if (!c->awaiting_eoi || (FIELD(UINT64,IntControl)&DevPending) || FIELD(UINT64,Rax)!=DevVector ||
        (FIELD(UINT64,Flags)&DevIf) || e->count!=c->eois+1u || e->shadow!=1 || e->woke ||
        e->stacked!=dev_ip(return_site) || (rsp&7) || rsp<LongStackGpa+sizeof(*e) ||
        rsp>LongInitialRsp-3*sizeof(UINT64)) return FALSE;
    volatile UINT64 *stack=(volatile UINT64 *)(vmcb+GuestStack*EFI_PAGE_SIZE+(UINTN)(rsp-LongStackGpa));
    if (stack[0]!=e->stacked || (UINT16)stack[1]!=8 || !(stack[2]&DevIf)) return FALSE;
    c->awaiting_eoi=FALSE;
    ++c->eois;
    FIELD(UINT64,Rip)=dev_ip(P4Eoi)+3; /* IRETQ */
    return TRUE;
}

/* One-shot in L1 TSC ticks, armed only once the guest is sleeping. The poll
 * budget is independent of the clock: a frozen or backward clock fails closed.
 * Keep raw clock values out of the repeatable terminal framebuffer. */
static UINT64 dev_clock(void)
{
    _mm_lfence();
    UINT64 t=__rdtsc();
    _mm_lfence();
    return t;
}

static BOOLEAN dev_timer_due(void)
{
    const UINT64 arm=dev_clock();
    UINT64 previous=arm;
    for (unsigned polls=0; polls<131072; ++polls) {
        UINT64 now=dev_clock();
        if (now<previous) return FALSE;
        if (now-arm>=65536) return TRUE;
        previous=now;
        _mm_pause();
    }
    return FALSE;
}

static const char *pkg_devices(UINT8 *vmcb)
{
    static const char marker[]="L2:UART:TX";
    static char captured[sizeof(marker)]; /* stable for pkg_text, never prefilled from the oracle */
    volatile DevEvidence *e=(volatile DevEvidence *)(vmcb+GuestStack*EFI_PAGE_SIZE);
    DevController c;
    __stosb((UINT8 *)&c,0,sizeof(c));
    pkg_reset(vmcb);
    if (!dev_layout()) return "DEVICE LAYOUT";
    __movsb(vmcb+Code*EFI_PAGE_SIZE,P4Start,(UINTN)P4End-(UINTN)P4Start);
    long_guest_state(vmcb,(UINTN)vmcb);
    long_guest_gate(vmcb,(UINTN)vmcb,DevVector,dev_ip(P4Handler));

    if (!dev_raise(vmcb,&c,0) || !dev_run(vmcb,&c,DevCall,P4Start)) return "MASK EXIT";
    UINT64 ctrl=dev_irq_fields(vmcb);
    pkg_observe("MASK IF ",(FIELD(UINT64,Flags)&DevIf)!=0);
    pkg_observe("MASK CTRL ",ctrl);
    if (!dev_unstacked(vmcb,0) || ctrl!=dev_request_word(0) || e->count || e->shadow || e->woke ||
        e->stacked) return "MASK STATE";
    FIELD(UINT64,Rip)=dev_ip(P4Start)+3; /* STI, then a guest-local INC */
    if (!dev_eoi(vmcb,&c,e,P4AfterShadow,"STI STACK ")) return "STI EOI";
    if (!dev_run(vmcb,&c,DevCall,P4AfterShadow) || !dev_unstacked(vmcb,DevIf) || e->count!=1)
        return "STI RETURN";

    if (!dev_raise(vmcb,&c,DevClass)) return "PRIORITY ARM";
    FIELD(UINT64,Rip)=dev_ip(P4AfterShadow)+3;
    if (!dev_run(vmcb,&c,DevCall,P4Equal)) return "EQUAL EXIT";
    ctrl=dev_irq_fields(vmcb);
    pkg_observe("EQUAL CTRL ",ctrl);
    if (!dev_unstacked(vmcb,DevIf) || ctrl!=dev_request_word(DevClass) || e->count!=1 ||
        e->stacked!=dev_ip(P4AfterShadow)) return "EQUAL STATE";
    /* Lower only V_TPR. Do not recreate the already queued request. */
    FIELD(UINT8,IntControl)=DevClass-1;
    FIELD(UINT64,Rip)=dev_ip(P4Wait);
    if (!dev_eoi(vmcb,&c,e,P4Wait,"PRIO STACK ")) return "PRIORITY EOI";
    pkg_observe("LOWER TPR ",FIELD(UINT8,IntControl));
    if (FIELD(UINT8,IntControl)!=DevClass-1 || !dev_run(vmcb,&c,DevHlt,P4Wait) ||
        !dev_unstacked(vmcb,DevIf) || (FIELD(UINT64,IntControl)&DevPending) || e->count!=2 || e->woke)
        return "SLEEP STATE";
    pkg_observe("SLEEP RIP ",FIELD(UINT64,Rip));
    BOOLEAN due=dev_timer_due();
    pkg_observe("TIMER DUE ",due);
    if (!due) return "TIMER TIMEOUT";
    if (!dev_raise(vmcb,&c,DevClass-1)) return "TIMER ARM";
    FIELD(UINT64,Rip)=dev_ip(P4Wait)+1;
    if (!dev_eoi(vmcb,&c,e,P4Wait+1,"WAKE STACK ")) return "WAKE EOI";

    UINTN bytes=0;
    while (bytes<sizeof(captured)-1) {
        if (!dev_run(vmcb,&c,DevIoio,P4Out) || !dev_unstacked(vmcb,DevIf)) return "UART EXIT";
        UINT64 info=FIELD(UINT64,Info1);
        /* EXITINFO1: port 15:0 in bits 31:16, size8, OUT, non-string, non-REP.
         * EXITINFO2 for IOIO is the following IP; never use it to select a site. */
        if (((info>>16)&0xffff)!=0x3f8 || (info&0x7d)!=0x10 || FIELD(UINT64,Info2)!=dev_ip(P4Out)+1)
            return "UART DECODE";
        captured[bytes++]=(char)(UINT8)FIELD(UINT64,Rax);
        FIELD(UINT64,Rip)=dev_ip(P4Out)+1;
    }
    captured[bytes]=0;
    for (UINTN i=0; i<bytes; ++i) if (captured[i]!=marker[i]) return "UART MARKER";
    pkg_text(captured);
    pkg_observe("UART BYTES ",bytes);
    BOOLEAN stopped=dev_run(vmcb,&c,DevHlt,P4Stop);
    pkg_observe("IRQ COUNT ",e->count);
    pkg_observe("EOI COUNT ",c.eois);
    pkg_observe("PROGRESS ",e->woke);
    pkg_observe("STOP RIP ",FIELD(UINT64,Rip));
    pkg_observe("STOP RAX ",FIELD(UINT64,Rax));
    pkg_observe("STOP EXIT ",FIELD(UINT64,ExitCode));
    if (!stopped || FIELD(UINT64,Rax)!=0x50415353 || !dev_unstacked(vmcb,DevIf) ||
        (FIELD(UINT64,IntControl)&DevPending) || c.awaiting_eoi || c.eois!=3 || e->count!=3 ||
        e->woke!=1 || e->shadow!=1 || e->stacked!=dev_ip(P4Wait)+1) return "DEVICE STOP";
    return NULL;
}
#endif
