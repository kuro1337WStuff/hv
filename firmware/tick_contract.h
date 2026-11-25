#ifndef HV_TICK_CONTRACT_H
#define HV_TICK_CONTRACT_H
#include "pit_pic.h"

extern void RunGprGuest(UINTN vmcb_pa, UINT64 frame[14]);
extern const UINT8 TickStart[], TickEnd[], TickIn[], TickOut[], TickUnmask[], TickAfterUnmask[],
                   TickWait[], TickPass[], TickHandler[], TickBadEoi[], TickDeny[];

enum { TickHlt=0x78, TickIoio=0x7b, TickIf=1u<<9, TickVIrq=1u<<8, TickShadow=0x068,
       TickExitBudget=100, TickFirst=32, TickSecond=64 };
typedef struct { UINT32 count, woke, cookie, reserved; UINT64 stacked[2]; } TickEvidence;
_Static_assert(sizeof(TickEvidence)==32 && LongStackGpa==0x7000 && LongInitialRsp==0x7ff0,
               "tick guest evidence and interrupt stack ABI");
typedef struct { PitPic pic; UINT64 frame[14]; unsigned entries, eois; } TickRun;

static UINTN tick_offset(const UINT8 *site) { return (UINTN)site-(UINTN)TickStart; }
static UINT64 tick_ip(const UINT8 *site) { return FixedGuestGpa+tick_offset(site); }

static BOOLEAN tick_layout(UINTN size)
{
    UINTN neg=tick_offset(TickBadEoi), deny=tick_offset(TickDeny);
    return size<=EFI_PAGE_SIZE && neg<deny && deny<size &&
           tick_offset(TickHandler)<neg && tick_offset(TickIn)<neg && tick_offset(TickOut)<neg &&
           tick_offset(TickUnmask)<tick_offset(TickAfterUnmask) &&
           tick_offset(TickAfterUnmask)==tick_offset(TickUnmask)+1 &&
           tick_offset(TickAfterUnmask)<tick_offset(TickWait) && tick_offset(TickWait)+1<tick_offset(TickPass) &&
           tick_offset(TickWait)<neg && tick_offset(TickPass)<neg &&
           TickStart[0]==0xfb && TickIn[0]==0xec && TickOut[0]==0xee && TickUnmask[0]==0xee &&
           TickWait[0]==0xf4 && TickPass[0]==0xf4 && TickDeny[0]==0xee;
}

static void tick_setup(UINT8 *vmcb, TickRun *t, UINTN size, const UINT8 *start)
{
    pkg_reset(vmcb); /* fresh VMCB, zeroed evidence and all-deny IOPM */
    __movsb(vmcb+Code*EFI_PAGE_SIZE,TickStart,size);
    long_guest_state(vmcb,(UINTN)vmcb);
    long_guest_gate(vmcb,(UINTN)vmcb,PIT_PIC_VECTOR,tick_ip(TickHandler));
    FIELD(UINT64,Rip)=tick_ip(start);
    __stosb((UINT8 *)t->frame,0,sizeof(t->frame));
    pit_pic_init(&t->pic);
}

static BOOLEAN tick_exit(UINT8 *vmcb, TickRun *t, UINT64 code, const UINT8 *site)
{
    if (t->entries>=TickExitBudget) return FALSE;
    ++t->entries;
    FIELD(UINT64,ExitCode)=~0ull;
    FIELD(UINT64,Info1)=FIELD(UINT64,Info2)=0;
    RunGprGuest((UINTN)vmcb,t->frame);
    FIELD(UINT8,TlbControl)=0;
    return FIELD(UINT64,ExitCode)==code && FIELD(UINT64,Rip)==tick_ip(site);
}

/* An expected label, byte-sized scalar opcode, DX and EXITINFO2 must all agree.
 * ASIZE/SEG do not select any operand for IN AL,DX or OUT DX,AL. No physical I/O. */
static BOOLEAN tick_io_exit(UINT8 *vmcb, TickRun *t, const UINT8 *site, UINT16 port)
{
    if (!tick_exit(vmcb,t,TickIoio,site)) return FALSE;
    UINT64 info=FIELD(UINT64,Info1);
    return (UINT16)t->frame[2]==port && (UINT16)(info>>16)==port &&
           (info&0x7d)==(UINT64)(0x10|(site==TickIn)) && FIELD(UINT64,Info2)==tick_ip(site)+1;
}

static BOOLEAN tick_apply_out(UINT8 *vmcb, TickRun *t, const UINT8 *site, UINT8 expected)
{
    UINT8 byte=(UINT8)FIELD(UINT64,Rax);
    if (byte!=expected || !pit_pic_write(&t->pic,(UINT16)t->frame[2],1,byte)) return FALSE;
    FIELD(UINT64,Rip)=tick_ip(site)+1;
    return TRUE;
}

static BOOLEAN tick_out(UINT8 *vmcb, TickRun *t, UINT16 port, UINT8 expected)
{
    return tick_io_exit(vmcb,t,TickOut,port) && tick_apply_out(vmcb,t,TickOut,expected);
}

static BOOLEAN tick_program(UINT8 *vmcb, TickRun *t, UINT8 reload)
{
    return tick_out(vmcb,t,0x43,0x38) && !t->pic.armed &&
           tick_out(vmcb,t,0x40,reload) && !t->pic.armed && tick_out(vmcb,t,0x40,0) && t->pic.armed;
}

/* Virtual PIT input ticks only. No TSC polling, wall-time claim or unbounded
 * wait. Check the independently expected deadline, not a time derived from
 * guest registers. The HLT arm cannot wake even one tick early. */
static BOOLEAN tick_expire(PitPic *p, BOOLEAN sleeping)
{
    UINT64 start=sleeping ? TickFirst : 0, due=start+(sleeping ? TickSecond : TickFirst);
    if (p->now!=start || p->deadline!=due || !p->armed || p->pending || p->in_service ||
        !pit_pic_advance(p,due-1)) return FALSE;
    pkg_observe(sleeping ? "WAKE PRE " : "MASK PRE ",p->now);
    if (p->now!=due-1 || !p->armed || p->pending || p->in_service) return FALSE;
    if (sleeping) {
        if (pit_pic_wake(p,due-1) || p->now!=due-1 || !p->armed || p->pending ||
            !pit_pic_wake(p,due)) return FALSE;
    } else if (!pit_pic_advance(p,due)) return FALSE;
    pkg_observe(sleeping ? "WAKE DUE " : "MASK DUE ",p->now);
    return p->now==due && !p->armed && p->pending==1 && !p->in_service;
}

static BOOLEAN tick_ready(UINT8 *vmcb)
{
    return FIELD(UINT64,Rsp)==LongInitialRsp && (FIELD(UINT64,Flags)&TickIf) &&
           !(FIELD(UINT64,TickShadow)&1) && !(FIELD(UINT64,IntControl)&TickVIrq);
}

/* Let hardware deliver V_IRQ through the IDT; never fabricate the interrupt
 * frame. At the ISR's first IN, independently inspect the five 64-bit slots
 * pushed by the CPU at our known aligned ring-0 RSP, not a guest-supplied GPA. */
static BOOLEAN tick_irq(UINT8 *vmcb, TickRun *t, volatile TickEvidence *e, UINT64 return_ip)
{
    PitPic *p=&t->pic;
    unsigned n=t->eois;
    if (n>=2 || FIELD(UINT64,Rip)!=return_ip || !tick_ready(vmcb) || p->imr!=0xfe ||
        !pit_pic_take(p) || p->pending || p->in_service!=1) return FALSE;
    FIELD(UINT64,IntControl)=((UINT64)PIT_PIC_VECTOR<<32)|(1u<<24)|TickVIrq|((PIT_PIC_VECTOR>>4)<<16);
    if (!n) pkg_observe("IRQ VECTOR ",(FIELD(UINT64,IntControl)>>32)&0xff);
    if (!tick_io_exit(vmcb,t,TickIn,0x21)) return FALSE;
    const volatile UINT64 *stack=(const volatile UINT64 *)(vmcb+GuestStack*EFI_PAGE_SIZE+
        (LongInitialRsp-LongStackGpa)-5*sizeof(UINT64));
    pkg_observe(n ? "ISR2 RIP " : "ISR1 RIP ",stack[0]);
    if ((FIELD(UINT64,IntControl)&TickVIrq) || (FIELD(UINT64,Flags)&TickIf) ||
        stack[0]!=return_ip || (UINT16)stack[1]!=8 || !(stack[2]&TickIf) ||
        stack[3]!=LongInitialRsp || (UINT16)stack[4]!=16 ||
        e->count!=n+1 || e->stacked[n]!=return_ip || e->woke || e->cookie) return FALSE;
    UINT8 byte;
    if (!pit_pic_read(p,(UINT16)t->frame[2],1,&byte) || byte!=0xfe) return FALSE;
    FIELD(UINT64,Rax)=(FIELD(UINT64,Rax)&~0xffull)|byte;
    FIELD(UINT64,Rip)=tick_ip(TickIn)+1;
    if (!tick_out(vmcb,t,0x21,0xff) || p->imr!=0xff || p->in_service!=1 ||
        !tick_out(vmcb,t,0x20,0x60) || p->in_service || p->pending) return FALSE;
    ++t->eois;
    return tick_out(vmcb,t,0x21,0xfe) && p->imr==0xfe;
}

static const char *tick_contract_run(UINT8 *vmcb)
{
    UINTN size=tick_offset(TickEnd);
    if (!tick_layout(size)) return "PIT IMAGE LAYOUT";
    TickRun t;
    t.entries=t.eois=0;
    tick_setup(vmcb,&t,size,TickStart);
    PitPic *p=&t.pic;
    volatile TickEvidence *e=(volatile TickEvidence *)(vmcb+GuestStack*EFI_PAGE_SIZE);
    if (!tick_out(vmcb,&t,0x21,0xff) || !tick_program(vmcb,&t,TickFirst) ||
        !tick_io_exit(vmcb,&t,TickUnmask,0x21)) return "MASKED PIT PROGRAM";
    if (!tick_ready(vmcb) || e->count || e->woke || e->cookie || e->stacked[0] || e->stacked[1] ||
        p->imr!=0xff || !tick_expire(p,FALSE)) return "MASKED PIT DEADLINE";
    pkg_observe("PIC IMR ",p->imr);
    pkg_observe("PIC IRR ",p->pending);
    if (pit_pic_take(p) || p->pending!=1 || p->in_service ||
        !tick_apply_out(vmcb,&t,TickUnmask,0xfe) || !tick_irq(vmcb,&t,e,tick_ip(TickAfterUnmask)))
        return "UNMASK IRQ0";

    if (!tick_program(vmcb,&t,TickSecond) || !tick_exit(vmcb,&t,TickHlt,TickWait)) return "REARM OR WAIT";
    pkg_observe("SLEEP RIP ",FIELD(UINT64,Rip));
    if (!tick_ready(vmcb) || t.eois!=1 || e->count!=1 || e->stacked[0]!=tick_ip(TickAfterUnmask) ||
        e->stacked[1] || e->woke || e->cookie || p->imr!=0xfe || !tick_expire(p,TRUE))
        return "HALTED PIT DEADLINE";
    FIELD(UINT64,Rip)=tick_ip(TickWait)+1; /* retire only the decoded intercepted HLT */
    if (!tick_irq(vmcb,&t,e,tick_ip(TickWait)+1)) return "WAKE IRQ0";
    if (!tick_exit(vmcb,&t,TickHlt,TickPass)) return "PIT GUEST STOP";
    pkg_observe("PIC EOIS ",t.eois);
    pkg_observe("L2 IRQS ",e->count);
    pkg_observe("L2 WOKE ",e->woke);
    pkg_observe("L2 COOKIE ",e->cookie);
    pkg_observe("STOP RIP ",FIELD(UINT64,Rip));
    pkg_observe("PASS RAX ",FIELD(UINT64,Rax));
    if (FIELD(UINT64,Rax)!=0x50415353 || !tick_ready(vmcb) || t.eois!=2 || e->count!=2 ||
        e->woke!=1 || e->cookie!=0x04871001 || e->stacked[0]!=tick_ip(TickAfterUnmask) ||
        e->stacked[1]!=tick_ip(TickWait)+1 || p->armed || p->pending || p->in_service ||
        p->imr!=0xfe || p->now!=TickFirst+TickSecond) return "PIT FINAL STATE";

    /* A new VMCB and model: this is refusal by the narrow IRQ0 contract, not
     * a predicted physical 8259 exception. Never advance RIP or resume. */
    tick_setup(vmcb,&t,size,TickBadEoi);
    if (!tick_io_exit(vmcb,&t,TickDeny,0x20)) return "DENY EXIT DECODE";
    UINT8 byte=(UINT8)FIELD(UINT64,Rax);
    UINT16 dx=(UINT16)t.frame[2];
    BOOLEAN refused=byte==0x61 && !pit_pic_write(p,dx,1,byte);
    pkg_observe("DENY RIP ",FIELD(UINT64,Rip));
    pkg_observe("DENY PORT ",dx);
    pkg_observe("DENY BYTE ",byte);
    pkg_observe("REFUSED ",refused);
    pkg_observe("ENTRIES ",t.entries);
    if (!refused || FIELD(UINT64,Rip)!=tick_ip(TickDeny) || p->imr!=0xff || p->armed ||
        p->pending || p->in_service || p->now || e->count || e->woke || e->cookie) return "EOI NOT DENIED";
    return NULL;
}
#endif
