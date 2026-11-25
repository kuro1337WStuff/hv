#ifndef HV_UART_THRE_H
#define HV_UART_THRE_H
#include "uart16450.h"

extern void RunGprGuest(UINTN vmcb_pa, UINT64 frame[14]);
extern const UINT8 ThreStart[],ThreEnd[],ThreIn[],ThreOut[],ThrePass[],ThreNegative[],ThreDeny[];
static UINTN thre_offset(const UINT8 *site) { return (UINTN)site-(UINTN)ThreStart; }
static UINT64 thre_ip(const UINT8 *site) { return FixedGuestGpa+thre_offset(site); }
static void thre_setup(UINT8 *vmcb, UINT64 frame[14], const UINT8 *start)
{
    pkg_reset(vmcb); /* All-deny IOPM and fresh guest evidence page. */
    __movsb(vmcb+Code*EFI_PAGE_SIZE,ThreStart,thre_offset(ThreEnd));
    long_guest_state(vmcb,(UINTN)vmcb);
    FIELD(UINT64,Rip)=thre_ip(start);
    __stosb((UINT8 *)frame,0,14*sizeof(*frame));
}
static void thre_enter(UINT8 *vmcb, UINT64 frame[14])
{
    FIELD(UINT64,ExitCode)=~0ull;
    FIELD(UINT64,Info1)=FIELD(UINT64,Info2)=0;
    RunGprGuest((UINTN)vmcb,frame);
    FIELD(UINT8,TlbControl)=0;
}
/* Only one-byte scalar DX opcodes from the immutable fixed image. Never
 * trust EXITINFO2 as a resumption address, even for the negative arm. */
static BOOLEAN thre_decode(UINT8 *vmcb, UINT64 dx, UINT16 port, const UINT8 *site, BOOLEAN input)
{
    UINT64 info=FIELD(UINT64,Info1),ip=thre_ip(site);
    return dx==port && FIELD(UINT64,ExitCode)==0x7b && FIELD(UINT64,Rip)==ip &&
           FIELD(UINT64,Info2)==ip+1 && (UINT16)(info>>16)==port && (info&0x7d)==(0x10u|input);
}

static const char *uart_thre_run(UINT8 *vmcb)
{
    /* Low three bits select COM1 register; bit 3 denotes IN. Index also
     * identifies which transaction caused each modeled IRQ4 sample. */
    static const UINT8 steps[]={9,1,9,1,9,1,9,2,10,7,15,7,15,1,4,4,4,10,10,0,10,13,13,10,10};
    const UINT32 expected_lines=(1u<<14)|(1u<<16)|(1u<<21)|(1u<<22);
    UINTN size=thre_offset(ThreEnd);
    if (size>EFI_PAGE_SIZE || thre_offset(ThreDeny)>=size || thre_offset(ThreNegative)>=thre_offset(ThreDeny) ||
        thre_offset(ThrePass)>=thre_offset(ThreNegative) || thre_offset(ThreIn)>=thre_offset(ThreNegative) ||
        thre_offset(ThreOut)>=thre_offset(ThreNegative) || *ThrePass!=0xf4 || *ThreIn!=0xec ||
        *ThreOut!=0xee || *ThreDeny!=0xee) return "THRE BLOB LAYOUT";
    Uart16450 u;
    __stosb((UINT8 *)&u,0,sizeof(u));
    UINT64 frame[14];
    thre_setup(vmcb,frame,ThreStart);
    UINT32 lines=0;
    for (unsigned i=0;i<sizeof(steps);++i) {
        thre_enter(vmcb,frame);
        BOOLEAN input=(steps[i]&8)!=0;
        UINT16 port=(UINT16)(Uart8250Base+(steps[i]&7));
        const UINT8 *site=input ? ThreIn : ThreOut;
        if (!thre_decode(vmcb,frame[2],port,site,input)) {
            pkg_observe("IO STEP ",i+1);
            pkg_observe("IO RIP ",FIELD(UINT64,Rip));
            return "THRE IO DECODE";
        }
        UINT64 ax=FIELD(UINT64,Rax);
        UINT8 byte=(UINT8)ax;
        if (!uart16450_access(&u,port,input,&byte)) return "THRE IO REFUSED";
        if (uart16450_irq4(&u)) lines|=1u<<i;
        if (input) FIELD(UINT64,Rax)=(ax&~0xffull)|byte;
        FIELD(UINT64,Rip)=thre_ip(site)+1;
    }
    thre_enter(vmcb,frame);
    volatile UINT8 *receipt=vmcb+GuestStack*EFI_PAGE_SIZE;
    _Static_assert(LongStackGpa==0x7000,"UART guest receipt address");
    const UINT32 cookie=*(volatile UINT32 *)(receipt+16);
    UINT64 probe=0,tx=0;
    for (unsigned i=0;i<7;++i) {
        probe=(probe<<8)|receipt[i];
        tx=(tx<<8)|receipt[7+i];
    }
    pkg_observe("L2 PROBE ",probe);
    pkg_observe("L2 CAUSES ",tx);
    pkg_observe("L2 COOKIE ",cookie);
    pkg_observe("L1 LINES ",lines);
    pkg_observe("L1 BYTE ",u.base.tx[0]);
    pkg_observe("L1 COUNT ",u.base.count);
    pkg_observe("L1 BUSY ",u.base.polls_busy);
    if (FIELD(UINT64,ExitCode)!=0x78 || FIELD(UINT64,Rip)!=thre_ip(ThrePass) || FIELD(UINT64,Rsp)!=LongInitialRsp ||
        cookie!=0x54485245 || probe!=0x00000f000155aaull || tx!=0x02010100600201ull) return "THRE L2 RECEIPT";
    if (lines!=expected_lines || u.base.count!=1 || u.base.tx[0]!=0x54 || u.base.polls_busy!=1 || u.base.busy ||
        u.thre_pending || u.base.ier!=2 || u.base.mcr!=8 || u.base.scratch!=0xaa) return "THRE DEVICE STATE";

    /* Same completed device, fresh guest. Compare every device byte (including
     * unused capture capacity), not just the readback of the targeted latch. */
    UINT8 before[sizeof(u)];
    __movsb(before,(const UINT8 *)&u,sizeof(u));
    thre_setup(vmcb,frame,ThreNegative);
    thre_enter(vmcb,frame);
    if (!thre_decode(vmcb,frame[2],Uart8250Base+1,ThreDeny,FALSE) || FIELD(UINT64,Rax)!=0x82) return "THRE DENY DECODE";
    UINT8 byte=(UINT8)FIELD(UINT64,Rax);
    BOOLEAN refused=!uart16450_access(&u,Uart8250Base+1,FALSE,&byte);
    BOOLEAN unchanged=TRUE;
    for (unsigned i=0;i<sizeof(u);++i) if (((const UINT8 *)&u)[i]!=before[i]) unchanged=FALSE;
    pkg_observe("DENY BYTE ",byte);
    pkg_observe("REFUSED ",refused);
    pkg_observe("UNCHANGED ",unchanged);
    pkg_observe("DENY COOKIE ",*(volatile UINT32 *)(receipt+16));
    if (!refused || !unchanged || byte!=0x82 || FIELD(UINT64,Rip)!=thre_ip(ThreDeny) ||
        *(volatile UINT32 *)(receipt+16)!=0) return "THRE DENY STATE";
    return NULL; /* No advance or re-entry after refusal. */
}
#endif
