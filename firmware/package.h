#ifndef HV_PACKAGE_H
#define HV_PACKAGE_H
/* All fixtures use the same BSP-owned arena, never the adjacent L1 stack. */
enum { GuestTables=Pages, GuestStack=GuestTables+NptTableCount,
       GuestDescriptors=GuestStack+1, FpScratch=GuestDescriptors+1,
       StandaloneGuestPages=20 };
_Static_assert(FpScratch<StandaloneGuestPages,"private FX workspace must fit");
typedef struct { unsigned completed; } StandaloneSample;
static void pkg_observe(const char *label, UINT64 value);
#ifndef HV_CPU_CONTRACT
static void pkg_text(const char *text);
#endif

static void pkg_reset(UINT8 *vmcb)
{
    UINTN base=(UINTN)vmcb;
    __stosb(vmcb,0,EFI_PAGE_SIZE);
    /* Leave the active VM_HSAVE_PA page alone. Overwrite the entire IOPM/MSRPM. */
    __stosb(vmcb+Pml4*EFI_PAGE_SIZE,0,(Iopm-Pml4)*EFI_PAGE_SIZE);
    __stosb(vmcb+Iopm*EFI_PAGE_SIZE,0xff,(Pages-Iopm)*EFI_PAGE_SIZE);
    __stosb(vmcb+Pages*EFI_PAGE_SIZE,0,(StandaloneGuestPages-Pages)*EFI_PAGE_SIZE);
    build_fixed_npt((FixedNpt *)(vmcb+Pml4*EFI_PAGE_SIZE),base+Pml4*EFI_PAGE_SIZE,base+Code*EFI_PAGE_SIZE);
    fixed_vmcb_defaults(vmcb,base);
    fixed_real_mode_state(vmcb);
}

#ifdef HV_CPU_CONTRACT
#include "long_guest.h"
#include "vcpu_fpstate.h"
#include "cpu_contract.h"
#else
#define HV_GUEST_IDT
#include "long_guest.h"
#include "pkg_context.h"
#include "pkg_exits.h"
#include "pkg_private.h"
#include "pkg_devices.h"

static const char *pkg_run(UINT8 *vmcb, StandaloneSample *s)
{
    const char *reason=pkg_context(vmcb);
    if (reason) return reason;
    ++s->completed;
    reason=pkg_exits(vmcb);
    if (reason) return reason;
    ++s->completed;
    reason=pkg_private(vmcb);
    if (reason) return reason;
    ++s->completed;
    reason=pkg_devices(vmcb);
    if (!reason) ++s->completed;
    return reason;
}
#endif /* HV_CPU_CONTRACT */
#endif
