#ifndef HV_UART_IRQ4_H
#define HV_UART_IRQ4_H
#include "uart16450.h"

extern void RunGprGuest(UINTN vmcb_pa, UINT64 frame[14]);
extern const UINT8 U4Start[], U4End[], U4Mcr[], U4Ier[], U4First[], U4Again[], U4Poll[],
                   U4Pass[], U4Handler[], U4Ack[], U4Eoi[];
enum { U4Vector=0x34, U4If=1u<<9, U4VIrq=1u<<8, U4Shadow=0x68, U4Budget=8 };
typedef struct {
    Uart16450 uart;
    UINT64 frame[14];
    unsigned entries, edges, deliveries, eois;
    BOOLEAN level, pending, service;
} U4Run;
typedef struct { UINT32 handlers, progress, cookie; UINT8 ack, poll; } U4Receipt;
_Static_assert(LongStackGpa==0x7000 && LongInitialRsp==0x7ff0,"IRQ4 guest receipt and frame");
static UINTN u4_off(const UINT8 *site) { return (UINTN)site-(UINTN)U4Start; }
static UINT64 u4_ip(const UINT8 *site) { return FixedGuestGpa+u4_off(site); }
static BOOLEAN u4_layout(UINTN size)
{
    const UINT8 *sites[]={U4Mcr,U4Ier,U4First,U4Again,U4Poll,U4Pass,U4Handler,U4Ack,U4Eoi};
    const UINT8 op[]={0xee,0xee,0xf4,0xf4,0xec,0xf4,0,0xec,0xee};
    if (!size || size>EFI_PAGE_SIZE) return FALSE;
    for (unsigned i=0;i<sizeof(op);++i)
        if (u4_off(sites[i])>=size || (i && u4_off(sites[i])<=u4_off(sites[i-1])) ||
            (op[i] && *sites[i]!=op[i])) return FALSE;
    return TRUE;
}
static BOOLEAN u4_exit(UINT8 *vmcb, U4Run *r, UINT64 code, const UINT8 *site)
{
    if (r->entries>=U4Budget) return FALSE;
    ++r->entries;
    FIELD(UINT64,ExitCode)=~0ull;
    FIELD(UINT64,Info1)=FIELD(UINT64,Info2)=0;
    RunGprGuest((UINTN)vmcb,r->frame);
    FIELD(UINT8,TlbControl)=0;
    return FIELD(UINT64,ExitCode)==code && FIELD(UINT64,Rip)==u4_ip(site);
}
static BOOLEAN u4_io(UINT8 *vmcb, U4Run *r, const UINT8 *site, UINT16 port, BOOLEAN input)
{
    if (!u4_exit(vmcb,r,0x7b,site)) return FALSE;
    UINT64 info=FIELD(UINT64,Info1);
    return r->frame[2]==port && (UINT16)(info>>16)==port && (info&0x7d)==(0x10u|input) &&
           FIELD(UINT64,Info2)==u4_ip(site)+1;
}
/* The master PIC slice has only one unmasked input. Latch a rising edge,
 * not a sustained level. Only the handler's specific EOI clears service. */
static BOOLEAN u4_access(UINT8 *vmcb, U4Run *r, const UINT8 *site, UINT16 port, BOOLEAN input, UINT8 expected)
{
    UINT64 ax=FIELD(UINT64,Rax);
    UINT8 byte=(UINT8)ax;
    if ((!input && byte!=expected) || !uart16450_access(&r->uart,port,input,&byte) || byte!=expected) return FALSE;
    if (input) FIELD(UINT64,Rax)=(ax&~0xffull)|byte;
    BOOLEAN level=uart16450_irq4(&r->uart);
    if (level && !r->level) { ++r->edges; r->pending=TRUE; }
    r->level=level;
    FIELD(UINT64,Rip)=u4_ip(site)+1;
    return TRUE;
}
static BOOLEAN u4_ready(UINT8 *vmcb)
{
    return (FIELD(UINT64,Flags)&U4If) && !(FIELD(UINT64,U4Shadow)&1) &&
           !(FIELD(UINT64,IntControl)&(U4VIrq|15)) && FIELD(UINT64,Rsp)==LongInitialRsp;
}
/* Both waits consult the same PIC arbitration. If it accepts a second edge,
 * the fixed guest must really enter the handler again (and fail the oracle). */
static void u4_resume(UINT8 *vmcb, U4Run *r, const UINT8 *site)
{
    FIELD(UINT64,Rip)=u4_ip(site)+1;
    if (!r->pending || r->service) return;
    r->pending=FALSE;
    r->service=TRUE;
    FIELD(UINT64,IntControl)=((UINT64)U4Vector<<32)|(1u<<24)|((U4Vector>>4)<<16)|U4VIrq;
}
static const char *u4_arm(UINT8 *vmcb, U4Run *r, BOOLEAN enabled)
{
    pkg_reset(vmcb);
    __movsb(vmcb+Code*EFI_PAGE_SIZE,U4Start,u4_off(U4End));
    long_guest_state(vmcb,(UINTN)vmcb);
    long_guest_gate(vmcb,(UINTN)vmcb,U4Vector,u4_ip(U4Handler));
    volatile U4Receipt *e=(volatile U4Receipt *)(vmcb+GuestStack*EFI_PAGE_SIZE);
    UINT8 mcr=enabled ? 8 : 0;
    FIELD(UINT64,Rax)=mcr;
    if (!u4_io(vmcb,r,U4Mcr,0x3fc,FALSE) || !u4_access(vmcb,r,U4Mcr,0x3fc,FALSE,mcr) ||
        !u4_io(vmcb,r,U4Ier,0x3f9,FALSE) || !u4_access(vmcb,r,U4Ier,0x3f9,FALSE,2)) return "IRQ4 UART SETUP";
    if (!u4_exit(vmcb,r,0x78,U4First) || !u4_ready(vmcb) || e->handlers || e->progress || e->cookie)
        return "IRQ4 FIRST WAIT";
    pkg_observe(enabled ? "ON CAUSE " : "OFF CAUSE ",r->uart.thre_pending);
    pkg_observe(enabled ? "ON EDGES " : "OFF EDGES ",r->edges);
    if (!r->uart.thre_pending || r->level!=enabled || r->edges!=enabled || r->pending!=enabled) return "IRQ4 EDGE STATE";
    u4_resume(vmcb,r,U4First);
    if (r->service) {
        if (!u4_io(vmcb,r,U4Ack,0x3fa,TRUE)) return "IRQ4 HANDLER IN";
        const UINTN frame=LongInitialRsp-5*sizeof(UINT64);
        const volatile UINT64 *s=(const volatile UINT64 *)(vmcb+GuestStack*EFI_PAGE_SIZE+frame-LongStackGpa);
        if (FIELD(UINT64,Rsp)!=frame || (FIELD(UINT64,Flags)&U4If) || (FIELD(UINT64,IntControl)&U4VIrq) ||
            s[0]!=u4_ip(U4First)+1 || (UINT16)s[1]!=8 || !(s[2]&U4If) || s[3]!=LongInitialRsp ||
            (UINT16)s[4]!=16 || e->handlers!=1 || e->progress || e->cookie) return "IRQ4 STACK FRAME";
        ++r->deliveries;
        if (!u4_access(vmcb,r,U4Ack,0x3fa,TRUE,2) || r->uart.thre_pending || r->level) return "IRQ4 SOURCE ACK";
        if (!u4_io(vmcb,r,U4Eoi,0x20,FALSE) || (UINT8)FIELD(UINT64,Rax)!=0x64 || e->ack!=2 ||
            !r->service || r->pending) return "IRQ4 SPECIFIC EOI";
        r->service=FALSE;
        ++r->eois;
        FIELD(UINT64,Rip)=u4_ip(U4Eoi)+1;
    }
    if (!u4_exit(vmcb,r,0x78,U4Again) || !u4_ready(vmcb) || e->progress!=1 || e->cookie ||
        r->uart.thre_pending==enabled || r->level || r->service) return "IRQ4 SECOND WAIT";
    u4_resume(vmcb,r,U4Again);
    UINT8 idle=enabled ? 1 : 2;
    if (!u4_io(vmcb,r,U4Poll,0x3fa,TRUE) || !u4_access(vmcb,r,U4Poll,0x3fa,TRUE,idle)) return "IRQ4 MAINLINE IIR";
    if (!u4_exit(vmcb,r,0x78,U4Pass)) return "IRQ4 FINAL HLT";
    pkg_observe(enabled ? "ON HANDLERS " : "OFF HANDLER ",e->handlers);
    pkg_observe(enabled ? "ON DELIVER " : "OFF DELIVER ",r->deliveries);
    pkg_observe(enabled ? "ON EOI " : "OFF EOI ",r->eois);
    pkg_observe(enabled ? "ON ISR IIR " : "OFF ISR IIR ",e->ack);
    pkg_observe(enabled ? "ON POLL IIR " : "OFF POLL ",e->poll);
    pkg_observe(enabled ? "ON COOKIE " : "OFF COOKIE ",e->cookie);
    pkg_observe(enabled ? "ON ENTRIES " : "OFF ENTRIES ",r->entries);
    if (!u4_ready(vmcb) || r->level || r->pending || r->service || r->uart.thre_pending ||
        e->handlers!=enabled || r->deliveries!=enabled || r->eois!=enabled || r->edges!=enabled ||
        e->ack!=(enabled ? 2 : 0) || e->poll!=idle || e->progress!=1 || e->cookie!=0x34515249 ||
        r->entries!=(enabled ? 8u : 6u)) return "IRQ4 FINAL RECEIPT";
    return NULL;
}
static const char *uart_irq4_run(UINT8 *vmcb)
{
    if (!u4_layout(u4_off(U4End))) return "IRQ4 BLOB LAYOUT";
    for (unsigned arm=0;arm<2;++arm) {
        U4Run r;
        __stosb((UINT8 *)&r,0,sizeof(r));
        const char *err=u4_arm(vmcb,&r,arm==0);
        if (err) {
            pkg_observe("FAIL ENTRY ",r.entries);
            pkg_observe("FAIL RIP ",FIELD(UINT64,Rip));
            pkg_observe("FAIL EXIT ",FIELD(UINT64,ExitCode));
            return err;
        }
    }
    return NULL;
}
#endif
