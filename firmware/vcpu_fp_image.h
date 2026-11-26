#ifndef HV_VCPU_FP_IMAGE_H
#define HV_VCPU_FP_IMAGE_H

#include <stddef.h>
#ifdef _MSC_VER
#include <intrin.h>
#else
#include <string.h>
#endif

/* FXSAVE64 memory ABI, using the caller's EFI fixed-width integer types. */
typedef struct { UINT64 sig; UINT16 se; UINT8 reserved[6]; } FpSt;
typedef struct {
    UINT16 cw, sw;
    UINT8 tw, reserved1;
    UINT16 op;
    UINT64 ip, dp;
    UINT32 mx, mask;
    FpSt st[8];                    /* Logical ST(i); FTW instead uses physical Rn. */
    UINT64 xmm[32];                /* Two QWORDs per XMM, low first. */
    UINT8 reserved2[96];
} FpImage;
typedef struct { _Alignas(16) FpImage fx; UINT64 gpr[14]; } FpBank;

_Static_assert(sizeof(FpImage)==512 && offsetof(FpImage,tw)==4 && offsetof(FpImage,mx)==24 &&
               offsetof(FpImage,st)==32 && offsetof(FpImage,xmm)==160 && sizeof(FpSt)==16 &&
               offsetof(FpBank,fx)==0 && offsetof(FpBank,gpr)==512 && sizeof(FpBank)==0x270 &&
               _Alignof(FpBank)==16, "legacy FP image/bank ABI mismatch");

/* Construct in private memory before first use, not in the live switch or
 * on a saved resume context. FCW/empty FTW model FINIT, not processor RESET.
 * Zeroing ST/XMM payloads, pointers, tail and mask is policy, not a host
 * capability observation. The freestanding MSVC build links no CRT. */
static inline void fp_cold_image(FpImage *fx)
{
#ifdef _MSC_VER
    __stosb((unsigned char *)fx, 0, sizeof *fx);
#else
    memset(fx, 0, sizeof *fx);
#endif
    fx->cw = 0x037f;
    fx->mx = 0x1f80;
}

#endif
