#ifndef HV_LINUX_ENTRY_GUARD_H
#define HV_LINUX_ENTRY_GUARD_H
#include "linux_layout.h"
#include "fetch_walk.h"

/* NX diagnostic only. vmcb and ram are known private L1 allocations; state
 * from either page selects no host pointer. The caller separately authenticates
 * the staged input and checks the complete all-NX NPT with linux_tree_ok(). */
enum LinuxEntryFault {
    LinuxEntryOK, LinuxEntryStorage, LinuxEntryCR0, LinuxEntryCR3, LinuxEntryCR4,
    LinuxEntryEFER, LinuxEntryRIP, LinuxEntryRSP, LinuxEntryRSI,
    LinuxEntryFlags, LinuxEntryDebugTrap, LinuxEntryTLB, LinuxEntryGDTR,
    LinuxEntryCS, LinuxEntryDS, LinuxEntryES, LinuxEntrySS, LinuxEntrySegments,
    LinuxEntryGDT, LinuxEntryBootParams, LinuxEntryWork, LinuxEntryInitrd,
    LinuxEntryCmdline, LinuxEntryE820, LinuxEntryPaging, LinuxEntryFetch
};

/* Artifact geometry from the pinned 6.18.60-0-virt image, not a layout return
 * value or an input-selected entry. Exact comparisons prove containment before
 * any guest extent is inspected, including split 64-bit boot protocol fields. */
enum {
    EntryKernel = 0x1000000, EntryKernelBytes = 0xc0c400, EntryWorkBytes = 0x260a000,
    EntryIP = EntryKernel + 0x200, EntryRd = 0x7e97000, EntryRdBytes = 0x168964,
    EntryCmdBytes = 123
};
_Static_assert((unsigned)EntryKernel >= LinuxHighRam && EntryKernelBytes <= EntryWorkBytes &&
    EntryWorkBytes <= EntryRd - EntryKernel && EntryRdBytes <= LinuxRamBytes - EntryRd &&
    (unsigned)EntryCmdBytes <= LinuxPage, "pinned guest extents must be disjoint and contained");

/* AMD APM Vol. 2 Appendix B. Read the raw VMCB actually passed to VMRUN,
 * including offsets that fixed_vmcb.h doesn't otherwise name. No packed ABI. */
enum {
    EntryExcOff=0x008, EntryTlbOff=0x05c, EntryCleanOff=0x0c0,
    EntryEsOff=0x400, EntryCsOff=0x410, EntrySsOff=0x420, EntryDsOff=0x430,
    EntryFsOff=0x440, EntryGsOff=0x450, EntryGdtrOff=0x460, EntryLdtrOff=0x470,
    EntryIdtrOff=0x480, EntryTrOff=0x490, EntryCplOff=0x4cb, EntryEferOff=0x4d0,
    EntryCr4Off=0x548, EntryCr3Off=0x550, EntryCr0Off=0x558, EntryFlagsOff=0x570,
    EntryRipOff=0x578, EntryRspOff=0x5d8
};
#ifdef HV_FIXED_VMCB_H
#define ENTRY_OFFSET_MATCH(a,b) ((unsigned)(a)==(unsigned)(b))
_Static_assert(ENTRY_OFFSET_MATCH(EntryExcOff,Exceptions) && ENTRY_OFFSET_MATCH(EntryTlbOff,TlbControl) &&
    ENTRY_OFFSET_MATCH(EntryEsOff,Es) && ENTRY_OFFSET_MATCH(EntryCsOff,Cs) && ENTRY_OFFSET_MATCH(EntrySsOff,Ss) &&
    ENTRY_OFFSET_MATCH(EntryDsOff,Ds) && ENTRY_OFFSET_MATCH(EntryEferOff,GuestEfer) && ENTRY_OFFSET_MATCH(EntryCr4Off,Cr4) &&
    ENTRY_OFFSET_MATCH(EntryCr3Off,Cr3) && ENTRY_OFFSET_MATCH(EntryCr0Off,Cr0) && ENTRY_OFFSET_MATCH(EntryFlagsOff,Flags) &&
    ENTRY_OFFSET_MATCH(EntryRipOff,Rip) && ENTRY_OFFSET_MATCH(EntryRspOff,Rsp), "entry guard and VMCB writer disagree");
#undef ENTRY_OFFSET_MATCH
#endif

static inline int linux_entry_segment(const uint8_t *s, uint16_t sel, uint16_t attr)
{
    return linux_le16(s)==sel && linux_le16(s+2)==attr &&
        linux_le32(s+4)==UINT32_MAX && linux_le64(s+8)==0;
}

/* Check the materialized fixed guest tree, not the nested tree. RW supervisor
 * identity maps cover the entire workspace/stack, boot_params, GDT, command
 * and initrd; the reserved hole has no translation. Inspect known table pages
 * without following possibly corrupted links. These are fresh, pre-run PTEs. */
static inline int linux_entry_paging(const uint8_t *ram)
{
    for (unsigned i=0; i<512; ++i) {
        uint64_t pde=0, pte=0;
        if (!i) pde=LinuxPt|3;
        else if (i<LinuxRamBytes/LinuxLargePage) pde=(uint64_t)i*LinuxLargePage|0x83;
        if (i<LinuxHoleStart/LinuxPage || i>=LinuxHighRam/LinuxPage) pte=(uint64_t)i*LinuxPage|3;
        if (linux_le64(ram+LinuxPml4+8*i)!=(i ? 0u : LinuxPdpt|3u) ||
            linux_le64(ram+LinuxPdpt+8*i)!=(i ? 0u : LinuxPd|3u) ||
            linux_le64(ram+LinuxPd+8*i)!=pde || linux_le64(ram+LinuxPt+8*i)!=pte) return 0;
    }
    return 1;
}

/* TF is not a Linux boot protocol requirement. It and intercepted #DB are
 * diagnostic containment for this one NX fetch, never executable-entry policy. */
static inline enum LinuxEntryFault linux_entry_nx_debug(uint64_t flags, uint32_t exceptions)
{
    if (!(flags & 0x100) || !(exceptions & (1u<<1))) return LinuxEntryDebugTrap;
    return LinuxEntryOK;
}

static inline uint64_t linux_entry_split(const uint8_t *bp, unsigned lo, unsigned hi)
{
    return linux_le32(bp+lo) | ((uint64_t)linux_le32(bp+hi)<<32);
}

static inline enum LinuxEntryFault linux_entry_guard(const uint8_t *vmcb, size_t vmcb_bytes,
    uint64_t actual_rsi, const uint8_t *ram, size_t ram_bytes)
{
    if (!vmcb || vmcb_bytes!=LinuxPage || !ram || ram_bytes!=LinuxRamBytes) return LinuxEntryStorage;
    uint64_t cr3=linux_le64(vmcb+EntryCr3Off), rip=linux_le64(vmcb+EntryRipOff);
    if (linux_le64(vmcb+EntryCr0Off)!=UINT32_C(0x80010031)) return LinuxEntryCR0;
    if (cr3!=LinuxPml4) return LinuxEntryCR3;
    if (linux_le64(vmcb+EntryCr4Off)!=0x20) return LinuxEntryCR4;
    if (linux_le64(vmcb+EntryEferOff)!=0x1d00) return LinuxEntryEFER; /* SVME/NXE/LMA/LME */
    if (rip!=EntryIP) return LinuxEntryRIP;
    if (linux_le64(vmcb+EntryRspOff)!=LinuxStackTop) return LinuxEntryRSP;
    if (actual_rsi!=LinuxParams) return LinuxEntryRSI;
    uint64_t flags=linux_le64(vmcb+EntryFlagsOff);
    if ((flags & ~UINT64_C(0x100))!=2) return LinuxEntryFlags; /* IF/DF/VM/IOPL clear */
    enum LinuxEntryFault diagnostic=linux_entry_nx_debug(flags,linux_le32(vmcb+EntryExcOff));
    if (diagnostic!=LinuxEntryOK) return diagnostic;
    if (vmcb[EntryTlbOff]!=1 || linux_le32(vmcb+EntryCleanOff)!=0) return LinuxEntryTLB;
    if (linux_le64(vmcb+EntryGdtrOff+8)!=LinuxGdt ||
        linux_le32(vmcb+EntryGdtrOff+4)!=LinuxGdtLimit) return LinuxEntryGDTR;
    if (!linux_entry_segment(vmcb+EntryCsOff,LinuxCs,0xa9b)) return LinuxEntryCS;
    if (!linux_entry_segment(vmcb+EntryDsOff,LinuxDs,0xc93)) return LinuxEntryDS;
    if (!linux_entry_segment(vmcb+EntryEsOff,LinuxDs,0xc93)) return LinuxEntryES;
    if (!linux_entry_segment(vmcb+EntrySsOff,LinuxDs,0xc93)) return LinuxEntrySS;
    static const uint16_t unused[]={EntryFsOff,EntryGsOff,EntryLdtrOff,EntryIdtrOff,EntryTrOff};
    if (vmcb[EntryCplOff]!=0) return LinuxEntrySegments;
    for (unsigned i=0; i<sizeof unused/sizeof unused[0]; ++i)
        if (linux_le64(vmcb+unused[i]) || linux_le64(vmcb+unused[i]+8)) return LinuxEntrySegments;
    const uint8_t *gdt=ram+LinuxGdt;
    if (linux_le64(gdt) || linux_le64(gdt+8) ||
        linux_le64(gdt+LinuxCs)!=UINT64_C(0x00af9b000000ffff) ||
        linux_le64(gdt+LinuxDs)!=UINT64_C(0x00cf93000000ffff)) return LinuxEntryGDT;

    const uint8_t *bp=ram+LinuxParams;
    if (linux_le32(bp+0x202)!=UINT32_C(0x53726448) || linux_le16(bp+0x206)!=0x020f ||
        bp[0x210]!=0xff || bp[0x211]!=1 || linux_le32(bp+0x208) || linux_le32(bp+0x224) ||
        linux_le32(bp+0x23c) || linux_le64(bp+0x240) || linux_le64(bp+0x250)) return LinuxEntryBootParams;
    /* No ACPI RSDP, confidential-computing blob or EFI handoff in the fixed PC. */
    if (linux_le64(bp+0x70) || linux_le32(bp+0x13c)) return LinuxEntryBootParams;
    for (unsigned i=0x1c0; i<0x1e0; i+=8)
        if (linux_le64(bp+i)) return LinuxEntryBootParams;
    if (linux_le32(bp+0x214)!=EntryKernel || linux_le64(bp+0x258)!=EntryKernel ||
        linux_le32(bp+0x1f4)!=EntryKernelBytes/16 || linux_le32(bp+0x260)!=EntryWorkBytes)
        return LinuxEntryWork;
    if (linux_entry_split(bp,0x218,0xc0)!=EntryRd ||
        linux_entry_split(bp,0x21c,0xc4)!=EntryRdBytes || linux_le32(bp+0x22c)!=0x7fffffffu)
        return LinuxEntryInitrd;
    if (linux_entry_split(bp,0x228,0xc8)!=LinuxCmdline || linux_le32(bp+0x238)!=0x7ff ||
        ram[LinuxCmdline+EntryCmdBytes-1]!=0) return LinuxEntryCmdline;
    for (unsigned i=0; i<EntryCmdBytes-1; ++i) if (!ram[LinuxCmdline+i]) return LinuxEntryCmdline;
    if (bp[0x1e8]!=3) return LinuxEntryE820;
    static const uint32_t edges[]={0,LinuxHoleStart,LinuxHighRam,LinuxRamBytes};
    for (unsigned i=0; i<3; ++i) {
        const uint8_t *row=bp+0x2d0+20*i;
        if (linux_le64(row)!=edges[i] || linux_le64(row+8)!=edges[i+1]-edges[i] ||
            linux_le32(row+16)!=(i==1 ? 2u : 1u)) return LinuxEntryE820;
    }
    if (!linux_entry_paging(ram)) return LinuxEntryPaging;
    uint64_t walked;
    if (!fetch_walk(ram,ram_bytes,cr3,rip,&walked) || walked!=EntryIP || ram[EntryIP]!=0xfc)
        return LinuxEntryFetch;
    return LinuxEntryOK;
}
#endif
