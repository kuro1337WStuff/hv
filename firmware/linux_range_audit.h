#ifndef HV_LINUX_RANGE_AUDIT_H
#define HV_LINUX_RANGE_AUDIT_H

/* A stopped-BSP snapshot, not a permission to enter. Nothing in this profile
 * installs guest RIP or enables SVM. Only addresses within 'vmcb' are read. */
extern volatile unsigned linux_vmrun_count;
enum RangeVerdict { RangeOK, RangeData, RangeWriter, RangeBounds, RangeMap };
typedef struct {
    UINTN src_pages, dst_pages, writers, checked, last, bad;
} RangeAudit;

_Static_assert(RangeSetup+RangeBytes==LinuxImageLen && RangeSpan-RangeBytes==0xc00 &&
               (RangeSrc%LinuxPage)==0 && (RangeDst%LinuxPage)==0 &&
               RangeSrc+RangeSpan<RangeDst && RangeDst+RangeSpan<LinuxRamBytes,
               "fixed disjoint page-aligned pinned copy");

static BOOLEAN range_fits(UINTN start, UINTN size, UINTN end)
{
    return start<=end && size<=end-start;
}

static BOOLEAN range_bounds(const LinuxLayout *l, UINTN dst)
{
    /* Check rounded frame bounds as well as exact file bytes; do not form an
     * end pointer, round an untrusted size up, or subtract an unchecked base. */
    return l->setup_bytes==RangeSetup && l->kernel_bytes==RangeBytes &&
        range_fits(l->setup_bytes,l->kernel_bytes,LinuxImageLen) &&
        l->kernel==RangeSrc && range_fits(l->kernel,RangeSpan,LinuxRamBytes) &&
        !(dst%LinuxPage) && range_fits(dst,RangeSpan,LinuxRamBytes) &&
        dst>=l->kernel && range_fits(dst-l->kernel,RangeSpan,l->init_bytes) &&
        range_fits(dst,RangeSpan,l->initrd) && dst==RangeDst;
}

static enum RangeVerdict range_check(UINT8 *vmcb, const LinuxLayout *l, UINTN dst, RangeAudit *s)
{
    UINT8 *ram=vmcb+LinuxRamFirst*LinuxPage;
    volatile UINT64 (*npt)[512]=(volatile UINT64 (*)[512])(vmcb+LinuxNptFirst*LinuxPage);
    const UINT64 addr=UINT64_C(0x000ffffffffff000), nx=UINT64_C(1)<<63;
    BOOLEAN canonical=TRUE;
    __stosb((UINT8 *)s,0,sizeof(*s));
    if (!range_bounds(l,dst)) return RangeBounds;
    if (FIELD(UINT64,NpEnable)!=1 || FIELD(UINT64,NestedCr3)!=(UINTN)npt) return RangeMap;
    /* Validate every reachable link first, including unused entries. Never
     * follow a frame from a PTE. The fixed 4 KiB leaves cover all guest RAM. */
    for (UINTN page=0; page<LinuxNptCount; ++page) {
        for (UINTN slot=0; slot<512; ++slot) {
            UINT64 pte=npt[page][slot];
            if (pte!=linux_expected_slot(vmcb,page,slot)) {
                if (page<3) return RangeMap;
                canonical=FALSE;
            }
            if (page<3) continue;
            UINTN gpa=((page-3)*512+slot)*LinuxPage;
            UINTN pa=(UINTN)(pte&addr);
            if ((pte&3)==3 && pa>=(UINTN)ram && linux_range_page(pa-(UINTN)ram))
                ++s->writers;
            /* Independent of the builder's policy: count only exact owned
             * frames at their canonical GPA with P=1, RW=0, NX=1. */
            if (linux_range_page(gpa) && pa==(UINTN)(ram+gpa) && (pte&(nx|3))==(nx|1)) {
                if (gpa<dst) ++s->src_pages; else ++s->dst_pages;
            }
        }
    }
    if (s->writers) return RangeWriter;
    if (!canonical || s->src_pages!=RangePages || s->dst_pages!=RangePages ||
        !linux_guard_ok(vmcb)) return RangeMap;

    const UINT8 *file=linux_stage(vmcb)+LinuxImageOff+l->setup_bytes;
    const volatile UINT8 *actual=ram+dst;
    for (UINTN off=0; off<l->kernel_bytes; off+=s->last) {
        UINTN left=l->kernel_bytes-off;
        s->last=left<LinuxPage ? left : LinuxPage;
        for (UINTN i=0; i<s->last; ++i) s->bad+=actual[off+i]!=file[off+i];
        s->checked+=s->last;
    }
    /* These guards are outside the authenticated file interval. They must
     * survive the host copy; do not include them in its verified byte count. */
    if (ram[dst-1]!=0xa5) return RangeMap;
    for (UINTN i=l->kernel_bytes; i<RangeSpan; ++i)
        if (actual[i]!=0xa5) return RangeMap;
    return s->bad ? RangeData : RangeOK;
}

static const char *linux_range_audit(UINT8 *vmcb)
{
    UINT8 *ram=vmcb+LinuxRamFirst*LinuxPage, *media=linux_stage(vmcb);
    volatile UINT64 *guard=(volatile UINT64 *)(vmcb+LinuxGuard*LinuxPage);
    volatile UINT64 (*npt)[512]=(volatile UINT64 (*)[512])(vmcb+LinuxNptFirst*LinuxPage);
    LinuxLayout layout;
    RangeAudit s;
    if ((UINTN)vmcb>UINT64_C(0xfffffffffffff)-(StandaloneGuestPages*LinuxPage-1))
        return "FRAME WIDTH";
    if (linux_bad_input(media)!=LinuxInputOK) return "STAGED INPUT SHA256";
    const char *error=build_linux_layout(ram,LinuxRamBytes,media+LinuxImageOff,LinuxImageLen,
        media+LinuxRdOff,LinuxRdLen,media+LinuxCmdOff,LinuxCmdLen,&layout);
    if (error) return error;
    if (!range_bounds(&layout,RangeDst)) return "COPY GEOMETRY";
    for (UINTN i=0; i<LinuxPage/sizeof(*guard); ++i) guard[i]=LINUX_GUARD_WORD;
    linux_build_npt(vmcb);
    __stosb(vmcb,0,LinuxPage);
    FIELD(UINT64,NpEnable)=1;
    FIELD(UINT64,NestedCr3)=(UINTN)npt;
    /* The rounded tail and the preceding byte belong to the zero-filled
     * initialization window. Mark them before copying disjoint owned ranges. */
    ram[RangeDst-1]=0xa5;
    __stosb(ram+RangeDst+RangeBytes,0xa5,RangeSpan-RangeBytes);
    linux_copy(ram+RangeDst,ram+RangeSrc,layout.kernel_bytes);
    pkg_observe("FILE OFFSET ",layout.setup_bytes);
    pkg_observe("SRC GPA ",layout.kernel);
    pkg_observe("DST GPA ",RangeDst);
    pkg_observe("DST END ",RangeDst+layout.kernel_bytes);
    enum RangeVerdict gate=range_check(vmcb,&layout,RangeDst,&s);
    pkg_observe("SRC RO NX ",s.src_pages);
    pkg_observe("DST RO NX ",s.dst_pages);
    pkg_observe("COPY BYTES ",s.checked);
    pkg_observe("LAST BYTES ",s.last);
    pkg_observe("GOOD GATE ",gate);
    volatile UINT8 *last=ram+RangeDst+RangeBytes-1;
    volatile UINT64 *alias=&npt[LinuxNptCount-1][511];
    UINT64 saved;
    if (gate!=RangeOK || s.checked!=RangeBytes || s.last!=RangeBytes%LinuxPage) {
        error="GOOD RANGE"; goto done;
    }
    *last^=1;
    gate=range_check(vmcb,&layout,RangeDst,&s);
    *last^=1;
    pkg_observe("BYTE DIFF ",s.bad);
    pkg_observe("BYTE GATE ",gate);
    if (gate!=RangeData || s.bad!=1 || s.checked!=RangeBytes) { error="BYTE CONTROL"; goto done; }

    /* Temporarily point the last regular RAM leaf at the last destination
     * frame, still NX. Its saved mapping and both frames are arena-owned. */
    saved=*alias;
    *alias=(UINTN)(ram+RangeDst+RangeSpan-LinuxPage)|LINUX_LEAF_FLAGS;
    gate=range_check(vmcb,&layout,RangeDst,&s);
    *alias=saved;
    pkg_observe("ALIAS WRITE ",s.writers);
    pkg_observe("ALIAS GATE ",gate);
    if (gate!=RangeWriter || s.writers!=1 || s.checked!=0) { error="ALIAS CONTROL"; goto done; }
    gate=range_check(vmcb,&layout,LinuxRamBytes-LinuxPage,&s);
    pkg_observe("BOUNDS GATE ",gate);
    if (gate!=RangeBounds || s.checked!=0) { error="END CONTROL"; goto done; }
    gate=range_check(vmcb,&layout,MAX_UINTN-(LinuxPage-1),&s);
    pkg_observe("WRAP GATE ",gate);
    if (gate!=RangeBounds || s.checked!=0) { error="WRAP CONTROL"; goto done; }
    gate=range_check(vmcb,&layout,RangeDst,&s);
    pkg_observe("REST GATE ",gate);
    pkg_observe("WRITERS ",s.writers);
    pkg_observe("ENTRIES ",linux_vmrun_count);
    pkg_observe("MARKER ",FIELD(UINT64,Rax));
    if (gate!=RangeOK || s.checked!=RangeBytes || linux_vmrun_count ||
        FIELD(UINT64,Rip) || FIELD(UINT64,Rax)) error="RESTORED RANGE";
done:
    ram[RangeDst-1]=0;
    __stosb(ram+RangeDst+RangeBytes,0,RangeSpan-RangeBytes);
    return error;
}
#endif
