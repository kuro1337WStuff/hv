#ifndef HV_W01_MEMORY_H
#define HV_W01_MEMORY_H
#include "w01_flash.h"

static UINT64 *w01_table(UINT8 *base, UINTN n) { return (UINT64 *)w01_page(base,n); }
static void w01_link(UINT8 *base, UINTN parent, UINTN i, UINTN child)
{
    w01_table(base,parent)[i]=(UINTN)w01_page(base,child)|7;
}
static void w01_memory(UINT8 *base)
{
    /* One PML4. Only private RAM and split flash below it. In particular
     * no large leaves, no low ROM alias and no mapping of an L1 structure. */
    w01_link(base,Pml4,0,W01Pdpt);
    w01_link(base,W01Pdpt,0,W01PdRam);
    w01_link(base,W01PdRam,W01RamGpa>>21,W01PtRam);
    for (UINTN i=0;i<W01RamPages;++i)
        w01_table(base,W01PtRam)[i]=(UINTN)w01_page(base,W01Ram+i)|7|W01_NX;
    w01_link(base,W01Pdpt,3,W01PdFlash);
    for (UINTN j=0;j<2;++j) {
        w01_link(base,W01PdFlash,510+j,W01PtFlash+j);
        for (UINTN i=0;i<512;++i) {
            UINTN k=512*j+i;
            /* VARS is per-run, volatile and NX; CODE is permanently RO. */
            w01_table(base,W01PtFlash+j)[i]=(UINTN)w01_page(base,W01Flash+k)|
                (k<W01VarsBytes/W01Page ? 7ull : 5ull)|W01_NX;
        }
    }
}
static volatile UINT64 *w01_code_leaf(UINT8 *base, UINT32 pc)
{
    UINT32 index=(pc-W01_FLASH_GPA)/W01Page;
    return &w01_table(base,W01PtFlash+index/512)[index%512];
}
/* Independent of EXITINFO2. Admit only the pin's non-CoCo four-level
 * identity mapping with 2 MiB leaves, not an arbitrary page-table walker.
 * Do not dereference a guest-provided frame as a host address. */
static BOOLEAN w01_identity(UINT8 *vmcb, UINT64 va)
{
    if (va>0xffffffffull) return FALSE;
    if (!(FIELD(UINT64,Cr0)&(1ull<<31))) return TRUE;
    if (!(FIELD(UINT64,Cr0)&1) || !(FIELD(UINT64,GuestEfer)&(1u<<10)) ||
        (FIELD(UINT64,Cr4)&((1u<<12)|(1u<<5)))!=(1u<<5) || FIELD(UINT64,Cr3)!=W01RamGpa)
        return FALSE;
    UINTN gig=(UINTN)(va>>30);
    UINT64 root=w01_table(vmcb,W01Ram)[0];
    UINT64 pdpt=w01_table(vmcb,W01Ram+1)[gig];
    UINT64 pd=w01_table(vmcb,W01Ram+2+gig)[(va>>21)&511];
    return (root&~0x60ull)==((W01RamGpa+0x1000u)|3u) &&
           (pdpt&~0x60ull)==((W01RamGpa+(2+gig)*W01Page)|3u) &&
           (pd&~0x60ull)==((va&0xffe00000ull)|0x83u);
}
static BOOLEAN w01_pc(UINT8 *vmcb, UINT32 *pc)
{
    UINT64 ip=FIELD(UINT64,Rip), cs=FIELD(UINT64,Cs+8);
    if (ip>0xffffffffull || cs>0xffffffffull || !w01_identity(vmcb,cs+ip) ||
        cs+ip<W01_CODE_GPA) return FALSE;
    *pc=(UINT32)(cs+ip);
    return TRUE;
}
static BOOLEAN w01_fetch(UINT8 *vmcb, UINT32 expected)
{
    UINT32 actual;
    UINT64 raw=FIELD(UINT64,Info2);
    return FIELD(UINT64,ExitCode)==0x400 &&
           (FIELD(UINT64,Info1)&~4ull)==((1ull<<32)|0x11) && w01_pc(vmcb,&actual) &&
           actual==expected && (raw==expected || raw==(expected&~0xfffu)) &&
           (*w01_code_leaf(vmcb,expected)&(W01_NX|7))==(W01_NX|5);
}
#endif
