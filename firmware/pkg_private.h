#ifndef HV_PKG_PRIVATE_H
#define HV_PKG_PRIVATE_H

enum { PrivateDataGpa=0x2000, PrivateHoleGpa=PrivateDataGpa+NptPageBytes };

/* Check the active root and every slot in each of the four known table pages.
 * No pointer sourced from a PTE is dereferenced. An unexpected subtree, large
 * page, writable code alias, or nonzero unused slot fails the same allowlist. */
static BOOLEAN pkg_private_tree(UINT8 *vmcb, BOOLEAN after_entry)
{
    volatile FixedNpt *npt=(volatile FixedNpt *)(vmcb+Pml4*EFI_PAGE_SIZE);
    if (FIELD(UINT64,NpEnable)!=1 || FIELD(UINT64,NestedCr3)!=(UINTN)npt) return 0;
    for (UINTN level=0; level<NptTableCount; ++level) {
        for (UINTN slot=0; slot<NptPageBytes/sizeof(UINT64); ++slot) {
            UINT64 expected=0, mutable_bits=0;
            if (level+1<NptTableCount && slot==0) {
                expected=(UINTN)&npt->table[level+1][0]|7;
                mutable_bits=0x20; /* accessed */
            } else if (level==NptTableCount-1 && slot==FixedGuestGpa/NptPageBytes) {
                expected=(UINTN)(vmcb+Code*EFI_PAGE_SIZE)|5;
                mutable_bits=0x20;
            } else if (level==NptTableCount-1 && slot==PrivateDataGpa/NptPageBytes) {
                /* Real mode: the otherwise idle descriptor frame is guest RAM
                 * for this fixture only. All other frames remain non-writable. */
                expected=(UINTN)(vmcb+GuestDescriptors*EFI_PAGE_SIZE)|7;
                mutable_bits=0x60; /* accessed, dirty */
            }
            UINT64 actual=npt->table[level][slot];
            if ((actual & ~(after_entry ? mutable_bits : 0))!=expected) return 0;
        }
    }
    return 1;
}

static const char *pkg_private(UINT8 *vmcb)
{
    enum { Tail=PrivateHoleGpa-1, OldByte=0x5a, NewByte=0xa5 };
    static const UINT8 stores[]={
        0xc6,0x06,Tail & 0xff,Tail >> 8,NewByte, /* mov byte [2fffh],a5h */
        0xc6,0x06,PrivateHoleGpa & 0xff,PrivateHoleGpa >> 8,0x19,
        0xf4 /* fail closed if the second store unexpectedly completes */
    };
    const UINT64 canary=0xf1358b76d94a20ceull;
    UINT8 *data=vmcb+GuestDescriptors*EFI_PAGE_SIZE;
    volatile UINT8 *control=data+NptPageBytes-1;
    volatile UINT64 *private_word=(volatile UINT64 *)(vmcb+FpScratch*EFI_PAGE_SIZE);
    FixedNpt *npt=(FixedNpt *)(vmcb+Pml4*EFI_PAGE_SIZE);
    _Static_assert(FpScratch==GuestDescriptors+1,"physical guard must follow guest RAM");
    pkg_reset(vmcb);
    __movsb(vmcb+Code*EFI_PAGE_SIZE,stores,sizeof(stores));
    npt->table[3][PrivateDataGpa/NptPageBytes]=(UINTN)data|7;
    *control=OldByte;
    *private_word=canary;
    if (*control!=OldByte || *private_word!=canary) return "PRIVATE INITIAL";
    if (!pkg_private_tree(vmcb,0)) return "PRIVATE NPT SETUP";

    __svm_vmrun((UINTN)vmcb); /* exactly one entry; no resume after the NPF */
    UINT64 exit=FIELD(UINT64,ExitCode), rip=FIELD(UINT64,Rip);
    UINT64 info=FIELD(UINT64,Info1), gpa=FIELD(UINT64,Info2);
    UINT8 committed=*control;
    UINT64 guarded=*private_word;
    pkg_observe("P3 EXIT ",exit);
    pkg_observe("P3 RIP ",rip);
    pkg_observe("P3 INFO1 ",info);
    pkg_observe("P3 GPA ",gpa);
    pkg_observe("P3 CONTROL ",committed);
    pkg_observe("P3 PRIVATE ",guarded);
    if (!pkg_private_tree(vmcb,1)) return "PRIVATE NPT CHANGED";
    if (committed!=NewByte || guarded!=canary) return "PRIVATE BACKING";
    /* U/S is not the L2 CPL. All other bits must describe a nonpresent,
     * final-GPA data write, not a guest walk, fetch, or protection failure. */
    if (exit!=0x400 || rip!=FixedGuestGpa+5 || gpa!=PrivateHoleGpa ||
        (info & ~4ull)!=((1ull<<32)|2)) return "PRIVATE NPF";
    return NULL;
}
#endif
