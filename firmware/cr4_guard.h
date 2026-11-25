/** @file cr4_guard.h
 * Fixed AMD SVM control-register interception fixture for the nested lab.
 */
#ifndef HV_CR4_GUARD_H
#define HV_CR4_GUARD_H

extern void RunGprGuest(UINTN vmcb_pa, UINT64 frame[14]);
extern volatile unsigned linux_vmrun_count;
extern const UINT8 CrGuardStart[], CrGuardWrite[], CrGuardHlt[], CrGuardEnd[];

/** Verify that refusing a guest CR4 write leaves guest state unchanged.
 * @param[in,out] vmcb Page-aligned L1-owned standalone arena, starting at its VMCB.
 * @pre The caller owns the BSP SVM context and has installed its private HSAVE page.
 * @return NULL if both arms match; otherwise a static diagnostic for the L1 terminal.
 *
 * APM vol. 2 appendices B and C: bit 4 at VMCB offset 002h intercepts
 * a write to CR4 with exit 014h. Reset the fixture for each arm. The write
 * arm stops at the faulting RIP: no emulation, RIP advance, re-entry or exception injection.
 * RCX addresses the guest witness through an identity-mapped linear address;
 * it is not an L1 pointer. No guest value is written to a host control register.
 */
static const char *cr4_guard_run(UINT8 *vmcb)
{
    const UINTN bytes=(UINTN)CrGuardEnd-(UINTN)CrGuardStart;
    if (bytes!=22 || (UINTN)CrGuardWrite-(UINTN)CrGuardStart!=14 ||
        (UINTN)CrGuardHlt-(UINTN)CrGuardStart!=21) return "CR4 FIXED SITES";
    const UINT64 caller_cr4=__readcr4();
    for (unsigned write_arm=0;write_arm<=1;++write_arm) {
        pkg_reset(vmcb);
        long_guest_state(vmcb,(UINTN)vmcb);
        FIELD(UINT16,2)=1u<<4;
        UINT64 frame[14];
        __stosb((UINT8 *)frame,0,sizeof(frame));
        frame[1]=LongStackGpa; /* RCX */
        frame[2]=write_arm;   /* RDX */
        __movsb(vmcb+Code*EFI_PAGE_SIZE,CrGuardStart,bytes);
        volatile UINT32 *seen=(volatile UINT32 *)(vmcb+GuestStack*EFI_PAGE_SIZE);
        volatile UINT8 *marker=(volatile UINT8 *)(seen+1);
        *seen=~0u; *marker=0x5a;
        if (FIELD(UINT64,Cr4)!=0x20 || FIELD(UINT16,2)!=(1u<<4)) return "CR4 PRECONDITION";
        unsigned before=linux_vmrun_count;
        RunGprGuest((UINTN)vmcb,frame);
        UINT64 code=FIELD(UINT64,ExitCode), ip=FIELD(UINT64,Rip), ax=FIELD(UINT64,Rax);
        unsigned entries=linux_vmrun_count-before;
        pkg_observe(write_arm ? "WR EXIT " : "RD EXIT ",code);
        pkg_observe(write_arm ? "WR RIP " : "RD RIP ",ip);
        pkg_observe(write_arm ? "WR RAX " : "RD RAX ",ax);
        pkg_observe(write_arm ? "WR SEEN " : "RD SEEN ",*seen);
        pkg_observe(write_arm ? "WR CR4 " : "RD CR4 ",FIELD(UINT64,Cr4));
        pkg_observe(write_arm ? "WR MARK " : "RD MARK ",*marker);
        pkg_observe(write_arm ? "WR ENTRIES " : "RD ENTRIES ",entries);
        if (__readcr4()!=caller_cr4 || FIELD(UINT64,Cr4)!=0x20 || *seen!=0x20 || entries!=1)
            return "CR4 STATE OR COUNT";
        if (code!=(write_arm ? 0x14u : 0x78u) || ip!=FixedGuestGpa+(write_arm ? 14u : 21u) ||
            ax!=(write_arm ? 0x40020u : 0x20u) || *marker!=(write_arm ? 0x5a : 0xb6))
            return "CR4 EARLY OR LATE";
    }
    return NULL;
}
#endif
