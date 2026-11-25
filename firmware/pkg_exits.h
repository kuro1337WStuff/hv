#ifndef HV_PKG_EXITS_H
#define HV_PKG_EXITS_H
#include "fixed_cpuid.h"
#include "exception_ud.h"

static const char *pkg_exits(UINT8 *vmcb)
{
    const UINT32 unknown_leaf=0x400000ffu;
    UINTN length=(UINTN)L2End-(UINTN)L2Start;
    if (!cpuid_layout_ok(length) || !ud_layout_ok(UdLayout.size)) return "EXIT GUEST LAYOUT";

    /* Use the positive fixture's real dispatcher and post-CPUID code. Skip
     * just the guest request initializer: RAX below and the zeroed RCX in
     * RunCpuidGuest's input frame make this an unsupported (leaf, subleaf). */
    pkg_reset(vmcb);
    __movsb(vmcb+Code*EFI_PAGE_SIZE,L2Start,length);
    FIELD(UINT32,Intercepts)|=1u<<18;
    FIELD(UINT64,Rip)=cpuid_ip(L2Cpuid);
    FIELD(UINT64,Rax)=unknown_leaf;
    CpuidSample s;
    s.nrips=FALSE;
    const char *reason=fixed_cpuid_run(vmcb,&s);
    UINT64 stop_ip=FIELD(UINT64,Rip), stop_ax=FIELD(UINT64,Rax);
    pkg_observe("STOP EXIT ",s.first_exit);
    pkg_observe("STOP RIP ",stop_ip);
    pkg_observe("STOP RAX ",stop_ax);
    pkg_observe("ENTRIES ",s.entries);
    if (reason!=cpuid_bad_request) return reason ? reason : "REQUEST ACCEPTED";
    if (s.entries!=1 || s.leaf!=unknown_leaf || s.subleaf!=0 ||
        stop_ip!=s.first_rip || stop_ax!=unknown_leaf) return "UNKNOWN STOP STATE";

    pkg_reset(vmcb);
    __movsb(vmcb+Code*EFI_PAGE_SIZE,UdStart,UdLayout.size);
    long_guest_state(vmcb,(UINTN)vmcb);
    long_guest_gate(vmcb,(UINTN)vmcb,6,FixedGuestGpa+UdLayout.handler);
    UdTrace trace;
    ud_run(vmcb,&trace);
    volatile UdEvidence *e=(volatile UdEvidence *)(vmcb+GuestStack*EFI_PAGE_SIZE);
    pkg_observe("FAULT EXIT ",trace.first_exit);
    pkg_observe("FAULT RIP ",trace.first_ip);
    pkg_observe("UD COUNT ",e->count);
    pkg_observe("UD PHASE ",e->phase);
    pkg_observe("UD HLT RIP ",FIELD(UINT64,Rip));
    return ud_passed(vmcb,&trace) ? NULL : "UD DELIVERY RETURN";
}
#endif
