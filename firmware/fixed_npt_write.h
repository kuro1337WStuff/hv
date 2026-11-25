#ifndef HV_FIXED_NPT_WRITE_H
#define HV_FIXED_NPT_WRITE_H
#include "fixed_vmcb.h"

enum { DataGpa=0x2000, InitialByte=0x5a, StoredByte=0xa5, Data=Pages };
/* Real-mode MOV byte ptr DS:[2000h], A5h; HLT. */
static const UINT8 fixed_store_hlt[]={0xc6,0x06,DataGpa & 0xff,DataGpa >> 8,StoredByte,0xf4};

/* AMD APM Vol. 2 §15.25.7. U/S is not the guest CPL. This non-SNP fixture
 * uses no shadow stack or verify-paging-write features. */
static inline BOOLEAN final_write_fault(UINT64 exit, UINT64 info)
{
    const UINT64 required=(1ull<<32)|3;
    const UINT64 excluded=(1ull<<38)|(1ull<<37)|(1ull<<33)|0x58;
    return exit==0x400 && (info & (required|excluded))==required;
}
#endif
