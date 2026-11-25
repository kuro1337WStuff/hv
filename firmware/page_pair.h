#ifndef HV_PAGE_PAIR_H
#define HV_PAGE_PAIR_H

extern void RunGprGuest(UINTN vmcb_pa, UINT64 frame[14]);
extern volatile unsigned linux_vmrun_count; /* counted at VMRUN, not at the C call */
extern const UINT8 PairStart[], PairEnd[];

enum PairArm { PairPositive, PairGuestNx, PairNestedNx, PairWrite, PairReadOnly };
enum { PairRxVa=0x18000, PairNxVa=0x19000, PairDataVa=0x1a000,
       PairRxGpa=0x8000, PairNxGpa=0x9000, PairDataGpa=0xa000,
       PairOffset=0x123, PairStoreArm=12, PairStoreInsn=17, PairStoreHlt=20,
       PairDataFrame=FpScratch+1, PairMarker=0x50353038 };
#define PAIR_NX (1ull<<63)
/* Third-party bytes, even if the same length, are not an executable fixture. */
static const UINT8 pair_bytes[]={
    0xb8,0x40,0xa0,1,0, 0xc7,0,0x38,0x30,0x35,0x50, 0xf4,
    0xb8,0,0xa0,1,0, 0xc6,0,0xb6, 0xf4
};
_Static_assert((int)PairDataFrame<(int)StandaloneGuestPages && (int)Code<(int)GuestTables &&
               GuestTables+4==GuestStack && GuestStack+1==GuestDescriptors &&
               GuestDescriptors+1==FpScratch,"owned disjoint page frames");

/* One row per allowed nested leaf. va=0 means that the guest page walker
 * can use the GPA but no guest linear mapping of that backing page exists.
 * Pre-set guest and nested A/D so the tree checks need no ignored bits. */
static const struct { UINT32 va, gpa, frame; UINT64 guest, nested; } pair_pages[]={
    {0,LongRootGpa,GuestTables,0,PAIR_NX|0x67},
    {0,LongRootGpa+0x1000,GuestTables+1,0,PAIR_NX|0x67},
    {0,LongRootGpa+0x2000,GuestTables+2,0,PAIR_NX|0x67},
    {0,LongRootGpa+0x3000,GuestTables+3,0,PAIR_NX|0x67},
    {LongStackGpa,LongStackGpa,GuestStack,PAIR_NX|0x63,PAIR_NX|0x67},
    {PairRxVa,PairRxGpa,Code,0x61,0x65},
    {PairNxVa,PairNxGpa,GuestDescriptors,0x61,PAIR_NX|0x65},
    {PairDataVa,PairDataGpa,PairDataFrame,PAIR_NX|0x63,PAIR_NX|0x67}
};
#define PAIR_PAGES (sizeof(pair_pages)/sizeof(pair_pages[0]))

/* Only descriptors in pair_pages ever supply a frame. Check every unused
 * slot as well: no extra link or leaf (e.g. a writable alias to code). */
static BOOLEAN pair_tables(UINT8 *vmcb, enum PairArm arm, BOOLEAN install)
{
    volatile FixedNpt *g=(volatile FixedNpt *)(vmcb+GuestTables*EFI_PAGE_SIZE);
    volatile FixedNpt *n=(volatile FixedNpt *)(vmcb+Pml4*EFI_PAGE_SIZE);
    if (FIELD(UINT64,Cr3)!=LongRootGpa || FIELD(UINT64,NpEnable)!=1 ||
        FIELD(UINT64,NestedCr3)!=(UINTN)n) return FALSE;
    if (install) { /* remove long_guest_state's default identity code mapping */
        g->table[3][FixedGuestGpa/EFI_PAGE_SIZE]=0;
        n->table[3][FixedGuestGpa/EFI_PAGE_SIZE]=0;
    }
    for (unsigned l=0;l<3;++l) {
        UINT64 gl=(LongRootGpa+(l+1)*EFI_PAGE_SIZE)|0x23;
        UINT64 nl=(UINTN)&n->table[l+1][0]|0x27;
        if (install) { g->table[l][0]=gl; n->table[l][0]=nl; }
        else {
            if (g->table[l][0]!=gl || n->table[l][0]!=nl) return FALSE;
            for (unsigned s=1;s<512;++s)
                if (g->table[l][s] || n->table[l][s]) return FALSE;
        }
    }
    unsigned guest_leaves=0;
    for (unsigned i=0;i<PAIR_PAGES;++i) {
        UINT32 va=pair_pages[i].va, gpa=pair_pages[i].gpa;
        UINT64 nl=(UINTN)(vmcb+pair_pages[i].frame*EFI_PAGE_SIZE)|pair_pages[i].nested;
        if (gpa==PairDataGpa && arm==PairReadOnly) nl&=~2ull;
        if (install) n->table[3][gpa>>12]=nl;
        else if (n->table[3][gpa>>12]!=nl) return FALSE;
        if (va) {
            UINT64 gl=gpa|pair_pages[i].guest;
            if (va==PairRxVa && arm==PairGuestNx) gl|=PAIR_NX;
            if (install) g->table[3][va>>12]=gl;
            else if (g->table[3][va>>12]!=gl) return FALSE;
            ++guest_leaves;
        }
    }
    if (!install) {
        unsigned gc=0,nc=0;
        for (unsigned s=0;s<512;++s) {
            gc+=g->table[3][s]!=0;
            nc+=n->table[3][s]!=0;
        }
        if (gc!=guest_leaves || nc!=PAIR_PAGES) return FALSE;
    }
    return TRUE;
}

/* Independently decode the actual four guest entries from known frame
 * addresses only. Return the present mapping even when NX denies fetch. */
static BOOLEAN pair_walk(UINT8 *vmcb, UINT32 va, UINT64 *gpa, BOOLEAN nx, BOOLEAN rw)
{
    volatile FixedNpt *g=(volatile FixedNpt *)(vmcb+GuestTables*EFI_PAGE_SIZE);
    UINT64 table=FIELD(UINT64,Cr3), xd=0, write=2;
    for (unsigned l=0;l<4;++l) {
        if (table!=LongRootGpa+l*EFI_PAGE_SIZE) return FALSE;
        UINT64 e=g->table[l][((UINT64)va>>(39-9*l))&511];
        if (!(e&1) || (e&0x80)) return FALSE;
        xd|=e&PAIR_NX; write&=e;
        table=e&0x000ffffffffff000ull;
    }
    *gpa=table+(va&0xfff);
    return (xd!=0)==nx && (write!=0)==rw;
}

static BOOLEAN pair_memory(UINT8 *vmcb, UINT32 mark, UINT8 byte)
{
    volatile UINT8 *data=vmcb+PairDataFrame*EFI_PAGE_SIZE, *guard=vmcb+FpScratch*EFI_PAGE_SIZE;
    volatile UINT8 *rx=vmcb+Code*EFI_PAGE_SIZE, *denied=vmcb+GuestDescriptors*EFI_PAGE_SIZE;
    for (unsigned i=0;i<EFI_PAGE_SIZE;++i) {
        UINT8 insn=(i>=PairOffset && i<PairOffset+sizeof(pair_bytes)) ? pair_bytes[i-PairOffset] : 0xf4;
        UINT8 value=i==0 ? byte : (i>=0x40 && i<0x44) ? (UINT8)(mark>>(8*(i-0x40))) : 0;
        if (guard[i]!=0xa7 || data[i]!=value || rx[i]!=insn || denied[i]!=insn) return FALSE;
    }
    return TRUE;
}

static const char *pair_case(UINT8 *vmcb, enum PairArm arm)
{
    UINT64 frame[14], walked=0, data_gpa=0;
    const BOOLEAN store=arm==PairWrite || arm==PairReadOnly;
    const UINT32 base=(arm==PairNestedNx ? PairNxVa : PairRxVa)+PairOffset;
    const UINT32 entry=base+(store ? PairStoreArm : 0);
    pkg_reset(vmcb);
    long_guest_state(vmcb,(UINTN)vmcb);
    FIELD(UINT64,GuestEfer)|=1u<<11; /* guest NXE */
    FIELD(UINT64,Rip)=entry;
    __stosb((UINT8 *)frame,0,sizeof(frame));
    __stosb(vmcb+FpScratch*EFI_PAGE_SIZE,0xa7,EFI_PAGE_SIZE);
    UINT8 *data=vmcb+PairDataFrame*EFI_PAGE_SIZE;
    data[0]=0x5a;
    UINT8 *rx=vmcb+Code*EFI_PAGE_SIZE, *denied=vmcb+GuestDescriptors*EFI_PAGE_SIZE;
    __stosb(rx,0xf4,EFI_PAGE_SIZE);
    __movsb(rx+PairOffset,PairStart,sizeof(pair_bytes));
    __movsb(denied,rx,EFI_PAGE_SIZE);
    if (!pair_tables(vmcb,arm,TRUE) || !pair_tables(vmcb,arm,FALSE) || !pair_memory(vmcb,0,0x5a))
        return "PAIR PRE ISOLATION";
    if (!pair_walk(vmcb,entry,&walked,arm==PairGuestNx,FALSE) ||
        walked!=(arm==PairNestedNx ? PairNxGpa : PairRxGpa)+(entry&0xfff) ||
        !pair_walk(vmcb,PairDataVa,&data_gpa,TRUE,TRUE) || data_gpa!=PairDataGpa)
        return "PAIR GUEST WALK";
    if (FIELD(UINT64,Flags)!=2 || FIELD(UINT64,GuestEfer)!=0x1d00 ||
        FIELD(UINT64,Cr0)!=0x80010031 || FIELD(UINT64,Cr4)!=0x20 ||
        FIELD(UINT32,Exceptions)!=0xffffffffu || !(FIELD(UINT32,Intercepts)&(1u<<24)) ||
        FIELD(UINT32,Asid)!=1 || FIELD(UINT8,TlbControl)!=1 || FIELD(UINT32,0xc0)!=0)
        return "PAIR ENTRY CONTROL";
    unsigned before=linux_vmrun_count;
    RunGprGuest((UINTN)vmcb,frame); /* fresh entry, one terminal exit, never resume */
    UINT64 code=FIELD(UINT64,ExitCode), rip=FIELD(UINT64,Rip), info=FIELD(UINT64,Info1), raw=FIELD(UINT64,Info2);
    UINT32 mark=*(volatile UINT32 *)(data+0x40);
    UINT8 byte=*(volatile UINT8 *)data;
    static const char *const exit_label[]={"A EXIT ","G EXIT ","N EXIT ","W EXIT ","R EXIT "};
    static const char *const rip_label[]={"A RIP ","G RIP ","N RIP ","W RIP ","R RIP "};
    pkg_observe(exit_label[arm],code);
    pkg_observe(rip_label[arm],rip);
    if (arm==PairPositive) pkg_observe("A MARK ",mark);
    else if (arm==PairWrite) pkg_observe("W BYTE ",byte);
    else {
        pkg_observe(arm==PairGuestNx ? "G INFO1 " : arm==PairNestedNx ? "N INFO1 " : "R INFO1 ",info);
        pkg_observe(arm==PairGuestNx ? "G RAW " : arm==PairNestedNx ? "N RAW " : "R RAW ",raw);
        if (arm==PairReadOnly) pkg_observe("R BYTE ",byte);
        else pkg_observe(arm==PairGuestNx ? "G WALK " : "N WALK ",walked);
    }
    if (linux_vmrun_count-before!=1) return "PAIR ENTRY COUNT";
    if (!pair_tables(vmcb,arm,FALSE) || !pair_memory(vmcb,arm==PairPositive ? PairMarker : 0,
                                                     arm==PairWrite ? 0xb6 : 0x5a)) return "PAIR POST ISOLATION";
    switch (arm) {
    case PairPositive: case PairWrite:
        if (code!=0x78 || rip!=base+(store ? PairStoreHlt : PairStoreArm-1)) return "PAIR EXPECT HLT";
        break;
    case PairGuestNx: /* Intercepted #PF: linear address from INFO2, not CR2. */
        if (code!=0x4e || rip!=entry || info!=0x11 || raw!=entry) return "PAIR GUEST FETCH PF";
        break;
    case PairNestedNx: /* 044 calibrated raw fetch GPA is the page base on this platform. */
        if (code!=0x400 || rip!=entry || (info&~4ull)!=0x100000011ull || raw!=walked-(walked&0xfff))
            return "PAIR NESTED FETCH";
        break;
    case PairReadOnly: /* Aligned witness, compare raw unrounded. No claim about subpage write precision. */
        if (code!=0x400 || rip!=base+PairStoreInsn || (info&~4ull)!=0x100000003ull || raw!=data_gpa)
            return "PAIR NESTED STORE";
        break;
    }
    return NULL;
}

static const char *page_pair_run(UINT8 *vmcb)
{
    if ((UINTN)PairEnd-(UINTN)PairStart!=sizeof(pair_bytes)) return "PAIR BLOB SIZE";
    unsigned before=linux_vmrun_count;
    for (unsigned a=PairPositive;a<=PairReadOnly;++a) {
        const char *error=pair_case(vmcb,(enum PairArm)a);
        if (error) return error;
    }
    unsigned entries=linux_vmrun_count-before;
    pkg_observe("ENTRIES ",entries);
    return entries==5 ? NULL : "PAIR TOTAL ENTRIES";
}
#undef PAIR_PAGES
#undef PAIR_NX
#endif
