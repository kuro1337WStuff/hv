#ifndef HV_POWER_FIXTURE_H
#define HV_POWER_FIXTURE_H
#include "power_request.h"
#include "uart8250.h"

/* Decoder fixture only. The fixed L2 knows the synthetic PM1a address/type;
 * it does not discover ACPI, execute AML, or run a Linux shutdown path. */
extern void RunGprGuest(UINTN vmcb_pa, UINT64 frame[14]);
extern const UINT8 PowerStart[], PowerNegative[], PowerPoll[], PowerTx[],
    PowerSelect[], PowerFinal[], PowerWait[], PowerFail[], PowerEnd[];

typedef struct { UINT32 cookie, tx_count; } PowerEvidence;
_Static_assert(LongStackGpa==0x7000 && sizeof(PowerEvidence)==8,"fixed guest evidence ABI");
_Static_assert(PowerPm1aControl==0x604 && PowerS5Select==0x1401 && PowerS5Request==0x3401,
               "guest PM1a ABI");
enum { PowerIoExit=0x7b, PowerHltExit=0x78, PowerMarkerLen=6, PowerBudget=22 };

static UINT64 power_ip(const UINT8 *site)
{
    return FixedGuestGpa+((UINTN)site-(UINTN)PowerStart);
}

static BOOLEAN power_layout(UINTN size)
{
    const UINT8 *const sites[]={PowerStart,PowerNegative,PowerPoll,PowerTx,PowerSelect,
                                PowerFinal,PowerWait,PowerFail,PowerEnd};
    if (size>EFI_PAGE_SIZE) return FALSE;
    for (UINTN i=1;i<sizeof(sites)/sizeof(*sites);++i)
        if ((UINTN)sites[i]<=(UINTN)sites[i-1]) return FALSE;
    return (UINTN)PowerSelect+2<=(UINTN)PowerFinal && (UINTN)PowerFinal+2<=(UINTN)PowerWait &&
           PowerPoll[0]==0xec && PowerTx[0]==0xee && PowerSelect[0]==0x66 && PowerSelect[1]==0xef &&
           PowerFinal[0]==0x66 && PowerFinal[1]==0xef && PowerWait[0]==0xf4 && PowerFail[0]==0xf4;
}

/* For the approved scalar DX sites the instruction length equals the operand
 * size (EC/EE or 66 EF). EXITINFO2 is corroboration, never a resume target. */
static BOOLEAN power_io_exit(UINT8 *vmcb, UINT64 dx, UINT16 port, const UINT8 *site,
                             unsigned bytes, BOOLEAN input)
{
    UINT64 info=FIELD(UINT64,Info1), ip=power_ip(site);
    return FIELD(UINT64,ExitCode)==PowerIoExit && FIELD(UINT64,Rip)==ip &&
           FIELD(UINT64,Info2)==ip+bytes && dx==port && (UINT16)(info>>16)==port &&
           (info&0x7d)==((UINT64)(bytes<<4)|input); /* direction, STR, REP, operand size */
}

static const char *power_case(UINT8 *vmcb, UINTN size, BOOLEAN positive)
{
    Uart8250 uart;
    PowerPm1 pm={PowerRunning,0};
    UINT64 frame[14]; /* slot 2 = RDX; RAX is in VMCB */
    __stosb((UINT8 *)&uart,0,sizeof(uart));
    __stosb((UINT8 *)frame,0,sizeof(frame));
    pkg_reset(vmcb); /* all I/O intercepted; no forwarding */
    __movsb(vmcb+Code*EFI_PAGE_SIZE,PowerStart,size);
    long_guest_state(vmcb,(UINTN)vmcb);
    FIELD(UINT64,Rip)=power_ip(positive ? PowerStart : PowerNegative);
    volatile PowerEvidence *e=(volatile PowerEvidence *)(vmcb+GuestStack*EFI_PAGE_SIZE);
    unsigned words=0, entries=0;
    uint32_t last=0;
    const char *error=NULL;
    while (pm.lifecycle==PowerRunning) {
        if (entries==PowerBudget) { error="POWER EXIT BUDGET"; break; }
        ++entries;
        FIELD(UINT64,ExitCode)=~0ull;
        FIELD(UINT64,Info1)=FIELD(UINT64,Info2)=0;
        RunGprGuest((UINTN)vmcb,frame);
        FIELD(UINT8,TlbControl)=0;
        UINT64 rip=FIELD(UINT64,Rip), ax=FIELD(UINT64,Rax);
        if (FIELD(UINT64,ExitCode)==PowerHltExit) {
            if (rip!=power_ip(PowerWait) || !(FIELD(UINT64,Flags)&0x200) || positive || words!=2) {
                error="POWER BAD HLT"; break;
            }
            pm.lifecycle=PowerWaitingForIrq;
            break;
        }
        if (rip==power_ip(PowerPoll) || rip==power_ip(PowerTx)) {
            BOOLEAN input=rip==power_ip(PowerPoll);
            const UINT8 *site=input ? PowerPoll : PowerTx;
            UINT16 port=input ? Uart8250Base+5 : Uart8250Base;
            UINT8 byte=(UINT8)ax;
            if (words || !power_io_exit(vmcb,frame[2],port,site,1,input) ||
                !uart8250_access(&uart,port,input,&byte)) { error="POWER UART EXIT"; break; }
            if (input) FIELD(UINT64,Rax)=(ax&~0xffull)|byte;
            FIELD(UINT64,Rip)=power_ip(site)+1;
            continue;
        }
        const UINT8 *site=words ? PowerFinal : PowerSelect;
        if (words>=2 || !power_io_exit(vmcb,frame[2],PowerPm1aControl,site,2,FALSE)) {
            error="POWER PM1A EXIT"; break;
        }
        last=(UINT16)ax;
        PowerIoResult result=power_pm1_io(&pm,PowerScalarOut,(uint16_t)frame[2],2,&last);
        ++words;
        BOOLEAN request=positive && words==2;
        if (last!=(request ? 0x3401u : 0x1401u) ||
            result!=(request ? PowerIoShutdown : PowerIoHandled) ||
            pm.lifecycle!=(request ? PowerStopped : PowerRunning) || pm.sleep_type_bits!=0x1400) {
            error="POWER PM1A RESULT"; break;
        }
        if (request) break; /* leave RIP on the intercepted OUT: no final re-entry */
        FIELD(UINT64,Rip)=power_ip(site)+2;
    }
    UINT64 capture=0;
    for (unsigned i=0;i<uart.count && i<8;++i) capture=(capture<<8)|uart.tx[i];
    if (!error && (e->cookie!=0x53354701u || e->tx_count!=PowerMarkerLen ||
                   FIELD(UINT64,Rsp)!=LongInitialRsp)) error="POWER L2 EVIDENCE";
    if (!error && (capture!=0x4c323a53350aull || uart.count!=PowerMarkerLen ||
                   uart.polls_busy!=PowerMarkerLen || uart.busy)) error="POWER TX EVIDENCE";
    if (!error && (words!=2 || entries!=(positive ? 21u : 22u) ||
                   pm.lifecycle!=(positive ? PowerStopped : PowerWaitingForIrq))) error="POWER STOP";
    if (error) pm.lifecycle=PowerFailed;
    pkg_observe(positive ? "S5 L2 TX " : "NEG L2 TX ",((UINT64)e->cookie<<32)|e->tx_count);
    pkg_observe(positive ? "S5 CAPTURE " : "NEG CAPTURE ",capture);
    pkg_observe(positive ? "S5 COUNT " : "NEG COUNT ",uart.count);
    pkg_observe(positive ? "S5 PM1A " : "NEG PM1A ",((UINT64)words<<32)|last);
    pkg_observe(positive ? "S5 ENTRIES " : "NEG ENTRIES ",entries);
    pkg_observe(positive ? "S5 EXIT " : "NEG EXIT ",FIELD(UINT64,ExitCode));
    pkg_observe(positive ? "S5 RIP " : "NEG RIP ",FIELD(UINT64,Rip));
    pkg_observe(positive ? "S5 STATE " : "NEG STATE ",pm.lifecycle);
    return error;
}

static const char *power_fixture_run(UINT8 *vmcb)
{
    UINTN size=(UINTN)PowerEnd-(UINTN)PowerStart;
    if (!power_layout(size)) return "POWER BLOB LAYOUT";
    const char *error=power_case(vmcb,size,FALSE);
    return error ? error : power_case(vmcb,size,TRUE);
}
#endif
