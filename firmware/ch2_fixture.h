/** @file ch2_fixture.h
 * Fixed L2 channel-2 scalar-I/O fixture. Pulses are scheduled by L1 at named
 * reads, not measured against a clock. No host I/O, IRQ, TSC or Linux entry.
 */
#ifndef HV_CH2_FIXTURE_H
#define HV_CH2_FIXTURE_H
#include "long_guest.h"
#include "pit_channel2.h"

extern void RunGprGuest(UINTN vmcb_pa, UINT64 frame[14]);
extern volatile unsigned linux_vmrun_count; /* vcpu_gpr.asm /DHV_COUNT_ENTRIES */
extern const UINT8 Ch2Start[], Ch2In[], Ch2Out[], Ch2Hlt[], Ch2Deny[], Ch2Long[], Ch2End[];

enum { Ch2Entries=13, Ch2Ioio=0x7b, Ch2Halt=0x78 };
typedef struct { UINT64 samples; UINT32 cookie; UINT8 resumed; } Ch2Witness;
typedef struct { PitChannel2 pit; UINT64 frame[14]; unsigned first; } Ch2Arm;
_Static_assert(FixedGuestGpa==0x1000 && LongStackGpa==0x7000 && LongInitialRsp==0x7ff0,
               "fixed CH2 guest addresses");

static UINT64 ch2_ip(const UINT8 *site) { return FixedGuestGpa+((UINTN)site-(UINTN)Ch2Start); }

/** One entry at most, charged at the instruction in vcpu_gpr.asm. No generic
 * decoder or guest-selected next RIP: only the approved one-byte sites resume. */
static BOOLEAN ch2_enter(UINT8 *vmcb, Ch2Arm *a, UINT64 code, const UINT8 *site)
{
    if (linux_vmrun_count-a->first>=Ch2Entries) return FALSE;
    FIELD(UINT64,ExitCode)=~0ull;
    FIELD(UINT64,Info1)=FIELD(UINT64,Info2)=0;
    RunGprGuest((UINTN)vmcb,a->frame);
    return FIELD(UINT64,ExitCode)==code && FIELD(UINT64,Rip)==ch2_ip(site);
}

/** AMD APM vol. 2, I/O intercepts: corroborate DX and EXITINFO2 and check
 * direction, STR, REP and operand size. ASIZE/SEG do not select any memory
 * operand for EC/EE. A mismatched exit cannot advance the virtual clock. */
static BOOLEAN ch2_io(UINT8 *vmcb, Ch2Arm *a, UINT16 port, const UINT8 *site)
{
    if (!ch2_enter(vmcb,a,Ch2Ioio,site)) return FALSE;
    UINT64 info=FIELD(UINT64,Info1);
    return a->frame[2]==port && (UINT16)(info>>16)==port &&
           (info&0x7d)==(0x10u+(site==Ch2In)) && FIELD(UINT64,Info2)==ch2_ip(site)+1 &&
           FIELD(UINT64,Rsp)==(site==Ch2Deny ? LongInitialRsp : LongInitialRsp-8);
}

static void ch2_resume(UINT8 *vmcb, const UINT8 *site)
{
    FIELD(UINT64,Rip)=ch2_ip(site)+1;
    FIELD(UINT8,TlbControl)=0;
}

static BOOLEAN ch2_out(UINT8 *vmcb, Ch2Arm *a, UINT16 port, UINT8 expected)
{
    if (!ch2_io(vmcb,a,port,Ch2Out)) return FALSE;
    UINT8 byte=(UINT8)FIELD(UINT64,Rax);
    if (byte!=expected || !pit2_write(&a->pit,port,1,byte)) return FALSE;
    ch2_resume(vmcb,Ch2Out);
    return TRUE;
}

static BOOLEAN ch2_in(UINT8 *vmcb, Ch2Arm *a, UINT16 port, UINT64 pulse)
{
    UINT8 byte;
    if (!ch2_io(vmcb,a,port,Ch2In) || !pit2_advance(&a->pit,pulse) ||
        !pit2_read(&a->pit,port,1,&byte)) return FALSE;
    FIELD(UINT64,Rax)=(FIELD(UINT64,Rax)&~0xffull)|byte;
    ch2_resume(vmcb,Ch2In);
    return TRUE;
}

/** Entire expected model state at the terminal pulse, including the byte-read
 * cursor. Checking both sides of refusal avoids comparing structure padding. */
static BOOLEAN ch2_expired(const PitChannel2 *p, UINT16 n)
{
    return p->tick==(UINT64)n+1 && p->count==0 && p->reload==n && p->phase==PIT2_COUNT &&
           p->control61==1 && p->out==1 && p->read_high==0;
}

static const char *ch2_case(UINT8 *vmcb, UINTN size, BOOLEAN deny)
{
    Ch2Arm a;
    pit2_init(&a.pit);
    __stosb((UINT8 *)a.frame,0,sizeof(a.frame));
    pkg_reset(vmcb); /* reset owned backing and intercept all I/O */
    long_guest_state(vmcb,(UINTN)vmcb);
    __movsb(vmcb+Code*EFI_PAGE_SIZE,Ch2Start,size);
    FIELD(UINT64,Rip)=ch2_ip(deny ? Ch2Long : Ch2Start);
    volatile Ch2Witness *w=(volatile Ch2Witness *)(vmcb+GuestStack*EFI_PAGE_SIZE);
    w->samples=0xccccccccccccccccull; /* zero-valued guest stores still have to happen */
    const UINT16 n=deny ? 59659 : 11931;
    const UINT64 boundary=1u+(n&0xffu); /* load at 1; live count is now xx00 */
    a.first=linux_vmrun_count;
    if (!ch2_in(vmcb,&a,0x61,0) || !ch2_out(vmcb,&a,0x61,1) ||
        !ch2_out(vmcb,&a,0x43,0xb0) || !ch2_out(vmcb,&a,0x42,(UINT8)n) ||
        !ch2_out(vmcb,&a,0x42,(UINT8)(n>>8))) return "CH2 PROGRAM";
    if (!ch2_in(vmcb,&a,0x61,1) || a.pit.count!=n ||
        !ch2_in(vmcb,&a,0x42,boundary) || !ch2_in(vmcb,&a,0x42,boundary) ||
        !ch2_in(vmcb,&a,0x42,boundary) || !ch2_in(vmcb,&a,0x42,boundary+1) ||
        !ch2_in(vmcb,&a,0x61,n) || a.pit.count!=1 || !ch2_in(vmcb,&a,0x61,(UINT64)n+1) ||
        !ch2_expired(&a.pit,n)) return "CH2 SAMPLE";
    if (deny) {
        if (!ch2_io(vmcb,&a,0x43,Ch2Deny) || FIELD(UINT64,Rax)!=0xb6 ||
            pit2_write(&a.pit,0x43,1,(UINT8)FIELD(UINT64,Rax)) || !ch2_expired(&a.pit,n) ||
            FIELD(UINT64,Rip)!=ch2_ip(Ch2Deny)) return "CH2 REFUSAL";
        pkg_observe("DENY CMD ",FIELD(UINT64,Rax));
    } else if (!ch2_enter(vmcb,&a,Ch2Halt,Ch2Hlt)) return "CH2 EXPECTED HLT";
    pkg_observe(deny ? "B SAMPLES " : "A SAMPLES ",w->samples);
    pkg_observe(deny ? "B PULSE " : "A PULSE ",a.pit.tick);
    pkg_observe(deny ? "B EXIT " : "A EXIT ",FIELD(UINT64,ExitCode));
    pkg_observe(deny ? "B RIP " : "A RIP ",FIELD(UINT64,Rip));
    pkg_observe(deny ? "B ENTRIES " : "A ENTRIES ",linux_vmrun_count-a.first);
    if (w->samples!=(deny ? 0x2101e800e9000100ull : 0x21012d002e000100ull) ||
        w->cookie!=0x43483250 || w->resumed || FIELD(UINT64,Rsp)!=LongInitialRsp ||
        linux_vmrun_count-a.first!=Ch2Entries) return "CH2 TERMINAL WITNESS";
    return NULL;
}

/** @param[in,out] vmcb Page-aligned BSP-owned package arena, host-save page installed.
 * @return NULL only for two complete 13-entry arms; otherwise static diagnostic.
 * The first arm stops at HLT; the second stops on the unsupported OUT, without
 * ticking, emulating success, changing RIP or re-entering. Never polls host time.
 */
static const char *ch2_run(UINT8 *vmcb)
{
    UINTN size=(UINTN)Ch2End-(UINTN)Ch2Start;
    if (size>EFI_PAGE_SIZE || ch2_ip(Ch2Hlt)!=0x1002 || ch2_ip(Ch2Deny)!=0x1003 ||
        !((UINTN)Ch2Deny<(UINTN)Ch2In && (UINTN)Ch2In<(UINTN)Ch2Out &&
          (UINTN)Ch2Out<(UINTN)Ch2Long && (UINTN)Ch2Long<(UINTN)Ch2End) ||
        *Ch2Hlt!=0xf4 || *Ch2Deny!=0xee || *Ch2In!=0xec || *Ch2Out!=0xee) return "CH2 LAYOUT";
    const char *error=ch2_case(vmcb,size,FALSE);
    return error ? error : ch2_case(vmcb,size,TRUE);
}
#endif
