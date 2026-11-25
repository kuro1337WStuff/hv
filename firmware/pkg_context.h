#ifndef HV_PKG_CONTEXT_H
#define HV_PKG_CONTEXT_H
#include "long_guest.h"
#include "vcpu_fpstate.h"

static const char *pkg_context(UINT8 *vmcb)
{
    int cpu[4];
    const UINT32 required=1u|(1u<<24)|(1u<<25)|(1u<<26); /* x87, FXSR, SSE, SSE2 */
    __cpuid(cpu,1);
    if (((UINT32)cpu[3]&required)!=required) return "FP FEATURES";
    if ((__readcr0()&0xcu) || !(__readcr4()&(1u<<9))) return "FP DISABLED";
    if (__readmsr(MSR_EFER)&(1ull<<14)) return "FFXSR ACTIVE";
    if (!fp_layout_ok(FpLayout.size)) return "FP LAYOUT";

    pkg_reset(vmcb);
    __movsb(vmcb+Code*EFI_PAGE_SIZE,FpStart,FpLayout.size);
    long_guest_state(vmcb,(UINTN)vmcb);
    FIELD(UINT64,Cr4)|=1u<<9; /* guest OSFXSR */

    FpTrace t;
    fp_run(vmcb,&t);
    pkg_observe("FP ENTRIES ",t.count);
    pkg_observe("FP EXIT A ",t.exit[0].code);
    pkg_observe("FP CALL RIP ",t.exit[0].ip);
    pkg_observe("FP CALL AX ",t.exit[0].ax);
    if (t.count==2) {
        pkg_observe("FP EXIT B ",t.exit[1].code);
        pkg_observe("FP HLT RIP ",t.exit[1].ip);
        pkg_observe("FP MARKER ",t.exit[1].ax);
        pkg_observe("SSE:X87 ",((UINT64)t.sse_result<<32)|t.x87_result);
    }
    return t.checked==2 ? NULL : "FP MISMATCH";
}
#endif
