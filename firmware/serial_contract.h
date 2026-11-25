#ifndef HV_SERIAL_CONTRACT_H
#define HV_SERIAL_CONTRACT_H
#include "uart8250.h"

extern void RunGprGuest(UINTN vmcb_pa, UINT64 frame[14]);
extern const UINT8 UartStart[], UartEnd[], UartIn[], UartOut[], UartPass[], UartNegStart[], UartDeny[];
enum { SerialHlt=0x78, SerialIoio=0x7b, SerialBudget=64 };
typedef struct { UINT32 cookie, lsr, busy; } SerialEvidence;
_Static_assert(sizeof(SerialEvidence)==12 && LongStackGpa==0x7000,"serial evidence ABI");

static UINTN serial_offset(const UINT8 *site) { return (UINTN)site-(UINTN)UartStart; }
static UINT64 serial_ip(const UINT8 *site) { return FixedGuestGpa+serial_offset(site); }

static BOOLEAN serial_layout(UINTN size)
{
    UINTN neg=serial_offset(UartNegStart), deny=serial_offset(UartDeny);
    return size<=EFI_PAGE_SIZE && neg<deny && deny<size && serial_offset(UartPass)<neg &&
           serial_offset(UartIn)<neg && serial_offset(UartOut)<neg && UartIn[0]==0xec &&
           UartOut[0]==0xee && UartPass[0]==0xf4 && UartDeny[0]==0xee;
}

static void serial_setup(UINT8 *vmcb, UINT64 frame[14], UINTN size, const UINT8 *start)
{
    pkg_reset(vmcb); /* fresh VMCB and all-deny IOPM; no guest I/O can pass through */
    __movsb(vmcb+Code*EFI_PAGE_SIZE,UartStart,size);
    long_guest_state(vmcb,(UINTN)vmcb);
    FIELD(UINT64,Rip)=serial_ip(start);
    __stosb((UINT8 *)frame,0,14*sizeof(*frame));
}

static void serial_enter(UINT8 *vmcb, UINT64 frame[14])
{
    FIELD(UINT64,ExitCode)=~0ull;
    FIELD(UINT64,Info1)=FIELD(UINT64,Info2)=0;
    RunGprGuest((UINTN)vmcb,frame);
    FIELD(UINT8,TlbControl)=0;
}

/* Only exact, unprefixed IN AL,DX / OUT DX,AL at known sites in the fixed
 * read-only guest image. For these non-string opcodes there is no memory
 * operand: ASIZE and SEG don't select one. EXITINFO2 must independently agree
 * with our expected next IP; never dispatch, index memory, or resume from it. */
static BOOLEAN serial_decode(UINT8 *vmcb, UINT16 dx, const UINT8 *site, BOOLEAN input)
{
    UINT64 ip=serial_ip(site), info=FIELD(UINT64,Info1);
    return FIELD(UINT64,ExitCode)==SerialIoio && FIELD(UINT64,Rip)==ip &&
           FIELD(UINT64,Info2)==ip+1 && (UINT16)(info>>16)==dx &&
           (info&0x7d)==(UINT64)(0x10|input); /* SZ8 only, no STR/REP, direction from opcode */
}

/* Display the actual bytes in wire order, including LF, not a printable copy
 * from the guest image or L1's expected string. Count is checked separately. */
static UINT64 serial_capture(const Uart8250 *u)
{
    UINT64 bytes=0;
    for (unsigned i=0; i<8; ++i) bytes=(bytes<<8)|u->tx[i];
    return bytes;
}

static const char *serial_contract_run(UINT8 *vmcb)
{
    const UINT64 expected=0x4c323a383235300aull; /* L2:8250 then LF */
    UINTN size=serial_offset(UartEnd);
    if (!serial_layout(size)) {
        pkg_observe("IMAGE SIZE ",size);
        return "UART BLOB LAYOUT";
    }
    Uart8250 u;
    UINT64 frame[14]; /* slot 2 is RDX; guest RAX lives in the VMCB */
    __stosb((UINT8 *)&u,0,sizeof(u));
    serial_setup(vmcb,frame,size,UartStart);
    volatile SerialEvidence *e=(volatile SerialEvidence *)(vmcb+GuestStack*EFI_PAGE_SIZE);
    const char *error=NULL;
    unsigned entries;
    for (entries=1;; ++entries) {
        serial_enter(vmcb,frame);
        UINT64 rip=FIELD(UINT64,Rip), ax=FIELD(UINT64,Rax);
        if (FIELD(UINT64,ExitCode)==SerialHlt) {
            if (rip!=serial_ip(UartPass) || ax!=0x50415353 || FIELD(UINT64,Rsp)!=LongInitialRsp)
                error="UART GUEST HLT";
            break;
        }
        if (entries==SerialBudget) { error="UART EXIT BUDGET"; break; }
        BOOLEAN input=(rip==serial_ip(UartIn));
        const UINT8 *site=input ? UartIn : UartOut;
        UINT16 dx=(UINT16)frame[2];
        if (!serial_decode(vmcb,dx,site,input)) { error="UART IO DECODE"; break; }
        UINT8 byte=(UINT8)ax;
        if (!uart8250_access(&u,dx,input,&byte)) { error="UART IO REFUSED"; break; }
        if (input) FIELD(UINT64,Rax)=(ax&~0xffull)|byte;
        FIELD(UINT64,Rip)=serial_ip(site)+1;
    }
    UINT64 bytes=serial_capture(&u);
    UINT32 divisor=((UINT32)u.dlm<<8)|u.dll;
    UINT32 regs=((UINT32)u.lcr<<24)|((UINT32)u.ier<<16)|((UINT32)u.mcr<<8)|u.scratch;
    pkg_observe("L2 COOKIE ",e->cookie);
    pkg_observe("L2 LSR ",e->lsr);
    pkg_observe("L2 BUSY ",e->busy);
    pkg_observe("L1 CAPTURE ",bytes);
    pkg_observe("L1 COUNT ",u.count);
    pkg_observe("L1 BUSY ",u.polls_busy);
    pkg_observe("L1 DIVISOR ",divisor);
    pkg_observe("L1 REGS ",regs);
    pkg_observe("L1 ENTRIES ",entries);
    pkg_observe("L1 EXIT ",FIELD(UINT64,ExitCode));
    pkg_observe("L1 RIP ",FIELD(UINT64,Rip));
    pkg_observe("L1 RAX ",FIELD(UINT64,Rax));
    if (error) {
        pkg_observe("L1 INFO1 ",FIELD(UINT64,Info1));
        pkg_observe("L1 NEXT IP ",FIELD(UINT64,Info2));
        pkg_observe("L1 RDX ",frame[2]);
        return error;
    }
    if (e->cookie!=0x82504701u || e->lsr!=Uart8250Ready || e->busy!=8) return "UART L2 EVIDENCE";
    if (u.count!=8 || bytes!=expected) return "UART TX MISMATCH";
    if (u.busy || u.polls_busy!=8 || divisor!=1 || regs!=0x0300035a) return "UART FINAL STATE";

    /* One fresh guest entry with the same completed UART instance. Refusal is
     * required from the model, not inferred merely from seeing another exit. */
    serial_setup(vmcb,frame,size,UartNegStart);
    serial_enter(vmcb,frame);
    UINT16 dx=(UINT16)frame[2];
    UINT8 byte=(UINT8)FIELD(UINT64,Rax);
    BOOLEAN decoded=serial_decode(vmcb,dx,UartDeny,FALSE) && dx==0x2f8 && byte==0xa5;
    BOOLEAN refused=decoded && !uart8250_access(&u,dx,FALSE,&byte);
    UINT64 after=serial_capture(&u);
    pkg_observe("DENY EXIT ",FIELD(UINT64,ExitCode));
    pkg_observe("DENY RIP ",FIELD(UINT64,Rip));
    pkg_observe("DENY INFO1 ",FIELD(UINT64,Info1));
    pkg_observe("DENY NEXT ",FIELD(UINT64,Info2));
    pkg_observe("DENY RDX ",frame[2]);
    pkg_observe("DENY RAX ",FIELD(UINT64,Rax));
    pkg_observe("REFUSED ",refused);
    pkg_observe("END COUNT ",u.count);
    pkg_observe("END CAPTURE ",after);
    if (!decoded) return "COM2 EXIT DECODE";
    if (!refused || FIELD(UINT64,Rip)!=serial_ip(UartDeny)) return "COM2 NOT DENIED";
    if (u.count!=8 || after!=bytes) return "COM2 CHANGED TX";
    return NULL; /* stopped at the denied OUT; no advance, forwarding, or re-entry */
}
#endif
