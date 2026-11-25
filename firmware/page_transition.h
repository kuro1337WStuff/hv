#ifndef HV_PAGE_TRANSITION_H
#define HV_PAGE_TRANSITION_H
/** @file Fixed single-vCPU write/seal/execute fixture; no executable input interface. */

extern void RunGprGuest(UINTN vmcb_pa, UINT64 frame[14]);
extern volatile unsigned linux_vmrun_count; /* HV_COUNT_ENTRIES: increment at VMRUN */
/** Relocation-free setup code, never a source of expected bytes for admission. */
extern const UINT8 TransitionSetup[], TransitionSetupEnd[];

enum { TrGpa=0x8000, TrAliasGpa=0x1ff000, TrMarker=0x5452414e };
/* Use the spare descriptor and FP workspace frames as candidate and guard. */
enum { TrFrame=(int)GuestDescriptors, TrGuard=(int)FpScratch };
_Static_assert(EFI_PAGE_SIZE==4096 && GuestTables+4==GuestStack && GuestStack+1==(int)TrFrame &&
               TrFrame+1==TrGuard && (int)TrGuard<(int)StandaloneGuestPages,"disjoint owned frames");
#define TR_NX (1ull<<63)
#define TR_ADDR 0x000ffffffffff000ull
static const UINT8 tr_setup[]={0xbf,0,0x80,0,0, 0xc7,7,0xb8,0x4e,0x41,0x52,
                               0x66,0xc7,0x47,4,0x54,0xf4, 0xf4};
static const UINT8 tr_payload[]={0xb8,0x4e,0x41,0x52,0x54,0xf4}; /* rest of page MUST be zero */
enum TrMode { TrWrite, TrSealed, TrExecute };
enum TrArm { TrGood, TrCorrupt, TrAlias };
enum TrVerdict { TrAllow, TrBytes, TrWriter, TrMap };

/* Guest VA = GPA for the two linear mappings only. The nested table pages
 * used by the guest walker are not addressable through any guest PTE. */
static UINT64 tr_pte(enum TrMode mode, BOOLEAN nested)
{
    return (nested ? 0x65ull : 0x61ull) | (mode==TrWrite ? 2ull : 0) |
           (mode==TrExecute ? 0 : TR_NX);
}

static UINT64 tr_nested(UINT8 *vmcb, unsigned slot, enum TrMode mode)
{
    unsigned gpa=slot*EFI_PAGE_SIZE;
    if (gpa==FixedGuestGpa) return (UINTN)(vmcb+Code*EFI_PAGE_SIZE)|0x65;
    if (gpa==TrGpa) return (UINTN)(vmcb+TrFrame*EFI_PAGE_SIZE)|tr_pte(mode,TRUE);
    if (gpa>=LongRootGpa && gpa<LongRootGpa+sizeof(FixedNpt))
        return (UINTN)(vmcb+GuestTables*EFI_PAGE_SIZE+(gpa-LongRootGpa))|TR_NX|0x67;
    return 0;
}

static UINT64 tr_guest(unsigned slot, enum TrMode mode)
{
    if (slot==FixedGuestGpa/EFI_PAGE_SIZE) return FixedGuestGpa|0x61;
    if (slot==TrGpa/EFI_PAGE_SIZE) return TrGpa|tr_pte(mode,FALSE);
    return 0;
}

/** Prove there is exactly one reachable PT per tree, no large leaves and no
 * other subtrees. Never follow a guest-supplied physical address. */
static BOOLEAN tr_roots(UINT8 *vmcb)
{
    volatile FixedNpt *n=(volatile FixedNpt *)(vmcb+Pml4*EFI_PAGE_SIZE);
    volatile FixedNpt *g=(volatile FixedNpt *)(vmcb+GuestTables*EFI_PAGE_SIZE);
    if (FIELD(UINT64,NpEnable)!=1 || FIELD(UINT64,NestedCr3)!=(UINTN)n ||
        FIELD(UINT64,Cr3)!=LongRootGpa) return FALSE;
    for (unsigned l=0;l<3;++l) {
        if (n->table[l][0]!=((UINTN)&n->table[l+1][0]|0x27) ||
            g->table[l][0]!=((LongRootGpa+(l+1)*EFI_PAGE_SIZE)|0x23)) return FALSE;
        for (unsigned s=1;s<512;++s)
            if (n->table[l][s] || g->table[l][s]) return FALSE;
    }
    return TRUE;
}

/* Check all actual leaves, not just the candidate slot. Optional alias is
 * ONLY for matching the injected control; the admission gate never allows it. */
static BOOLEAN tr_leaves(UINT8 *vmcb, enum TrMode mode, BOOLEAN alias, unsigned *writers)
{
    volatile FixedNpt *n=(volatile FixedNpt *)(vmcb+Pml4*EFI_PAGE_SIZE);
    volatile FixedNpt *g=(volatile FixedNpt *)(vmcb+GuestTables*EFI_PAGE_SIZE);
    UINTN target=(UINTN)(vmcb+TrFrame*EFI_PAGE_SIZE);
    BOOLEAN equal=TRUE;
    *writers=0;
    for (unsigned s=0;s<512;++s) {
        UINT64 actual=n->table[3][s];
        if ((actual&3)==3 && (actual&TR_ADDR)==target) ++*writers;
        UINT64 expected=(alias && s==TrAliasGpa/EFI_PAGE_SIZE) ? target|TR_NX|0x67 : tr_nested(vmcb,s,mode);
        if (actual!=expected || g->table[3][s]!=tr_guest(s,mode)) equal=FALSE;
    }
    return equal;
}

static unsigned tr_bad_bytes(UINT8 *vmcb, BOOLEAN populated)
{
    volatile UINT8 *candidate=vmcb+TrFrame*EFI_PAGE_SIZE;
    unsigned bad=0;
    for (unsigned i=0;i<EFI_PAGE_SIZE;++i)
        bad+=candidate[i]!=(populated && i<sizeof(tr_payload) ? tr_payload[i] : 0);
    return bad;
}

static BOOLEAN tr_private_ok(UINT8 *vmcb)
{
    volatile UINT8 *setup=vmcb+Code*EFI_PAGE_SIZE, *guard=vmcb+TrGuard*EFI_PAGE_SIZE;
    for (unsigned i=0;i<EFI_PAGE_SIZE;++i)
        if (guard[i]!=0xa7 || setup[i]!=(i<sizeof(tr_setup) ? tr_setup[i] : 0xf4)) return FALSE;
    return TRUE;
}

/** Read-only gate while the candidate is RO/NX and L2 is stopped. No case
 * selector, alias exception, byte input, or entry pointer is accepted. */
static enum TrVerdict tr_admit(UINT8 *vmcb, unsigned *writers, unsigned *bad)
{
    if (!tr_roots(vmcb)) return TrMap;
    BOOLEAN exact=tr_leaves(vmcb,TrSealed,FALSE,writers);
    *bad=tr_bad_bytes(vmcb,TRUE);
    if (*writers) return TrWriter;
    if (!exact || !tr_private_ok(vmcb)) return TrMap;
    return *bad ? TrBytes : TrAllow;
}

static BOOLEAN tr_entry_ok(UINT8 *vmcb)
{
    return FIELD(UINT64,Cr0)==0x80010031 && FIELD(UINT64,Cr4)==0x20 && FIELD(UINT64,GuestEfer)==0x1d00 &&
           FIELD(UINT64,Flags)==2 && FIELD(UINT32,Exceptions)==0xffffffffu &&
           (FIELD(UINT32,Intercepts)&(1u<<24)) && FIELD(UINT32,Asid)==1 && FIELD(UINT8,TlbControl)==1 &&
           FIELD(UINT32,0xc0)==0; /* clean bits clear: reload controls */
}

static const char *tr_case(UINT8 *vmcb, enum TrArm arm)
{
    UINT64 frame[14];
    unsigned before=linux_vmrun_count, writers=0, bad=0;
    volatile FixedNpt *n=(volatile FixedNpt *)(vmcb+Pml4*EFI_PAGE_SIZE);
    volatile FixedNpt *g=(volatile FixedNpt *)(vmcb+GuestTables*EFI_PAGE_SIZE);
    volatile UINT8 *candidate=vmcb+TrFrame*EFI_PAGE_SIZE;
    pkg_reset(vmcb);
    /* Cover the whole nested tree with NX before long_guest_state creates
     * its default writable walk/stack leaves. Drop that cover only AFTER
     * installing the final leaves; the unused guest stack is not mapped. */
    n->table[0][0]|=TR_NX;
    long_guest_state(vmcb,(UINTN)vmcb);
    FIELD(UINT64,GuestEfer)|=1u<<11;
    for (unsigned s=0;s<512;++s) {
        n->table[3][s]=tr_nested(vmcb,s,TrWrite);
        g->table[3][s]=tr_guest(s,TrWrite);
    }
    for (unsigned l=0;l<3;++l) n->table[l][0]=(UINTN)&n->table[l+1][0]|0x27;
    __stosb((UINT8 *)frame,0,sizeof(frame));
    __stosb(vmcb+TrGuard*EFI_PAGE_SIZE,0xa7,EFI_PAGE_SIZE);
    __stosb(vmcb+Code*EFI_PAGE_SIZE,0xf4,EFI_PAGE_SIZE);
    __movsb(vmcb+Code*EFI_PAGE_SIZE,TransitionSetup,sizeof(tr_setup));
    if (!tr_roots(vmcb) || !tr_leaves(vmcb,TrWrite,FALSE,&writers) || writers!=1 ||
        tr_bad_bytes(vmcb,FALSE) || !tr_private_ok(vmcb) || !tr_entry_ok(vmcb) ||
        FIELD(UINT64,Rip)!=FixedGuestGpa || FIELD(UINT64,Rax)!=0) return "TRANSITION PRE";
    RunGprGuest((UINTN)vmcb,frame);
    UINT64 checkpoint=FIELD(UINT64,Rip);
    static const char *const check_label[]={"A CHECK RIP ","B CHECK RIP ","C CHECK RIP "};
    pkg_observe(check_label[arm],checkpoint);
    if (linux_vmrun_count-before!=1 || FIELD(UINT64,ExitCode)!=0x78 ||
        checkpoint!=FixedGuestGpa+sizeof(tr_setup)-1 || frame[5]!=TrGpa || FIELD(UINT64,Rax)!=0 ||
        !tr_roots(vmcb) || !tr_leaves(vmcb,TrWrite,FALSE,&writers) || writers!=1 ||
        tr_bad_bytes(vmcb,TRUE) || !tr_private_ok(vmcb)) return "TRANSITION CHECKPOINT";

    n->table[3][TrGpa/EFI_PAGE_SIZE]=tr_nested(vmcb,TrGpa/EFI_PAGE_SIZE,TrSealed);
    g->table[3][TrGpa/EFI_PAGE_SIZE]=tr_guest(TrGpa/EFI_PAGE_SIZE,TrSealed);
    if (arm==TrCorrupt) candidate[EFI_PAGE_SIZE-1]=1; /* host-only perturbation while NX */
    if (arm==TrAlias) n->table[3][TrAliasGpa/EFI_PAGE_SIZE]=(UINTN)candidate|TR_NX|0x67;
    enum TrVerdict verdict=tr_admit(vmcb,&writers,&bad);
    static const char *const write_label[]={"A WRITERS ","B WRITERS ","C WRITERS "};
    static const char *const bytes_label[]={"A BAD BYTES ","B BAD BYTES ","C BAD BYTES "};
    static const char *const gate_label[]={"A GATE ","B GATE ","C GATE "};
    pkg_observe(write_label[arm],writers);
    pkg_observe(bytes_label[arm],bad);
    pkg_observe(gate_label[arm],verdict);
    if (verdict!=(arm==TrCorrupt ? TrBytes : arm==TrAlias ? TrWriter : TrAllow) ||
        writers!=(unsigned)(arm==TrAlias) || bad!=(unsigned)(arm==TrCorrupt) ||
        candidate[EFI_PAGE_SIZE-1]!=(UINT8)(arm==TrCorrupt) || !tr_private_ok(vmcb) ||
        !tr_roots(vmcb) || !tr_leaves(vmcb,TrSealed,arm==TrAlias,&writers)) return "TRANSITION SEAL";
    if (verdict==TrAllow) {
        n->table[3][TrGpa/EFI_PAGE_SIZE]=tr_nested(vmcb,TrGpa/EFI_PAGE_SIZE,TrExecute);
        g->table[3][TrGpa/EFI_PAGE_SIZE]=tr_guest(TrGpa/EFI_PAGE_SIZE,TrExecute);
        FIELD(UINT64,Rip)=TrGpa;
        FIELD(UINT64,ExitCode)=~0ull;
        FIELD(UINT8,TlbControl)=1; /* AMD APM vol. 2, TLB_CONTROL: full local flush on VMRUN */
        pkg_observe("A TLB CTRL ",FIELD(UINT8,TlbControl));
        if (!tr_entry_ok(vmcb) || FIELD(UINT64,Rip)!=TrGpa || !tr_roots(vmcb) ||
            !tr_leaves(vmcb,TrExecute,FALSE,&writers) || writers) return "TRANSITION ACTIVATE";
        RunGprGuest((UINTN)vmcb,frame);
        pkg_observe("A EXIT ",FIELD(UINT64,ExitCode));
        pkg_observe("A FINAL RIP ",FIELD(UINT64,Rip));
        pkg_observe("A MARK ",FIELD(UINT64,Rax));
        if (FIELD(UINT64,ExitCode)!=0x78 || FIELD(UINT64,Rip)!=TrGpa+sizeof(tr_payload)-1 ||
            FIELD(UINT64,Rax)!=TrMarker || !tr_roots(vmcb) || !tr_leaves(vmcb,TrExecute,FALSE,&writers) ||
            writers || tr_bad_bytes(vmcb,TRUE) || !tr_private_ok(vmcb)) return "TRANSITION FINAL";
    }
    static const char *const run_label[]={"A RUNS ","B RUNS ","C RUNS "};
    unsigned entries=linux_vmrun_count-before;
    pkg_observe(run_label[arm],entries);
    return entries==(arm==TrGood ? 2u : 1u) ? NULL : "TRANSITION COUNT";
}

/** Run on a stopped BSP-owned vCPU in the identity-mapped standalone arena.
 * Requires nested paging, host NXE, ASID 1, and the counted GPR wrapper. AMD
 * APM vol. 2 ch. 15 (Nested Paging, TLB Control). Oracle: three fresh setup
 * HLTs at 1011h; good: zero writers/mismatches, flush 1, HLT at 8005h with
 * EAX=5452414Eh; bad final page byte: one mismatch; extra writable nested
 * leaf: one writer. Both negatives remain NX. Exactly 2/1/1 VMRUNs.
 * Returns NULL only if all observations and the full private guard agree;
 * on unexpected state, returns an error without resuming the failing guest. */
static const char *transition_run(UINT8 *vmcb)
{
    if ((UINTN)TransitionSetupEnd-(UINTN)TransitionSetup!=sizeof(tr_setup)) return "TRANSITION SETUP SIZE";
    unsigned before=linux_vmrun_count;
    for (unsigned a=TrGood;a<=TrAlias;++a) {
        const char *error=tr_case(vmcb,(enum TrArm)a);
        if (error) return error;
    }
    unsigned entries=linux_vmrun_count-before;
    pkg_observe("ENTRIES ",entries);
    return entries==4 ? NULL : "TRANSITION TOTAL";
}
#undef TR_ADDR
#undef TR_NX
#endif
