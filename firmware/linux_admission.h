#ifndef HV_LINUX_ADMISSION_H
#define HV_LINUX_ADMISSION_H
#include "fixed_vmcb.h"
#include "linux_layout.h"
#include "linux_media.h"
#include "linux_input_gate.h"
#include "fetch_walk.h"
#include "linux_entry_guard.h"

/* Reservation indices are L1 pages, not GPAs. Only LinuxRamFirst..LinuxStageFirst-1
 * can supply leaf frames. The caller owns the adjacent stack separately. */
enum {
    LinuxIoFirst=HostSave+1, LinuxMsrFirst=LinuxIoFirst+3,
    LinuxNptFirst=LinuxMsrFirst+2, LinuxPtCount=LinuxRamBytes/LinuxLargePage,
    LinuxNptCount=3+LinuxPtCount, LinuxGuard=LinuxNptFirst+LinuxNptCount,
    LinuxRamFirst=LinuxGuard+1, LinuxStageFirst=LinuxRamFirst+LinuxRamBytes/LinuxPage,
    StandaloneGuestPages=LinuxStageFirst+LinuxMediaPages
};
_Static_assert(Vmcb==0 && HostSave==1 && LinuxPage==EFI_PAGE_SIZE && LinuxNptCount==67,
               "admission requires a distinct VMCB/HSAVE and 67 four-level NPT pages");
static void pkg_observe(const char *label, UINT64 value);
extern void RunGprGuest(UINTN vmcb_pa, UINT64 frame[14]);

static UINT8 *linux_stage(UINT8 *base)
{
    return base+LinuxStageFirst*LinuxPage;
}

/* Pre-set A on links and A/D on leaves: the entire tree can be compared
 * bit-for-bit before and after VMRUN, without masking any hardware changes. */
#define LINUX_LINK_FLAGS 0x27ull
#define LINUX_LEAF_FLAGS ((1ull<<63)|0x67ull)
/* In the opt-in ACPI map, the immutable descriptor page is read-only/NX;
 * the dedicated FACS page can hold guest-owned writable NVS, still NX. */
static UINT64 linux_npt_rights(UINTN gpa)
{
#ifdef HV_ACPI_MAP
    if (gpa==PowerRsdpGpa) return LINUX_LEAF_FLAGS & ~2ull;
#else
    (void)gpa;
#endif
    return LINUX_LEAF_FLAGS;
}
static void linux_build_npt(UINT8 *base)
{
    UINT64 (*npt)[512]=(UINT64 (*)[512])(base+LinuxNptFirst*LinuxPage);
    UINT8 *ram=base+LinuxRamFirst*LinuxPage;
    __stosb((UINT8 *)npt,0,LinuxNptCount*LinuxPage);
    npt[0][0]=(UINTN)npt[1]|LINUX_LINK_FLAGS;
    npt[1][0]=(UINTN)npt[2]|LINUX_LINK_FLAGS;
    for (UINTN i=0; i<LinuxPtCount; ++i) npt[2][i]=(UINTN)npt[3+i]|LINUX_LINK_FLAGS;
    for (UINTN gpa=0; gpa<LinuxRamBytes; gpa+=LinuxPage) {
        if (gpa>=LinuxHoleStart && gpa<LinuxHighRam) continue;
        npt[3+gpa/LinuxLargePage][gpa/LinuxPage%512]=(UINTN)(ram+gpa)|linux_npt_rights(gpa);
    }
}

static BOOLEAN linux_tree_ok(UINT8 *vmcb)
{
    volatile UINT64 (*npt)[512]=(volatile UINT64 (*)[512])(vmcb+LinuxNptFirst*LinuxPage);
    if (FIELD(UINT64,NpEnable)!=1 || FIELD(UINT64,NestedCr3)!=(UINTN)npt) return 0;
    /* Check every possible link, including unused root and PDPT slots. Never
     * dereference a PA from a PTE, even when reporting a corrupted tree. */
    for (UINTN page=0; page<LinuxNptCount; ++page) {
        for (UINTN slot=0; slot<512; ++slot) {
            UINT64 want=0;
            if ((page<2 && slot==0) || (page==2 && slot<LinuxPtCount))
                want=(UINTN)npt[page+1+slot]|LINUX_LINK_FLAGS;
            else if (page>=3) {
                UINTN pfn=(page-3)*512+slot;
                if (pfn<LinuxHoleStart/LinuxPage || pfn>=LinuxHighRam/LinuxPage)
                    want=(UINTN)(vmcb+(LinuxRamFirst+pfn)*LinuxPage)|linux_npt_rights(pfn*LinuxPage);
            }
            if (npt[page][slot]!=want) return 0;
        }
    }
    return 1;
}

#define LINUX_GUARD_WORD UINT64_C(0x50354e5847554152)
static BOOLEAN linux_guard_ok(UINT8 *base)
{
    volatile UINT64 *guard=(volatile UINT64 *)(base+LinuxGuard*LinuxPage);
    for (UINTN i=0; i<LinuxPage/sizeof(*guard); ++i)
        if (guard[i]!=LINUX_GUARD_WORD) return FALSE;
    return TRUE;
}

static void linux_fetch_vmcb(UINT8 *vmcb, UINT64 entry)
{
    enum { Gdtr=0x460 };
    __stosb(vmcb,0,LinuxPage); /* Never clear the active HSAVE page. */
    fixed_vmcb_defaults(vmcb,(UINTN)vmcb);
    FIELD(UINT64,IopmPa)=(UINTN)(vmcb+LinuxIoFirst*LinuxPage);
    FIELD(UINT64,MsrpmPa)=(UINTN)(vmcb+LinuxMsrFirst*LinuxPage);
    FIELD(UINT64,NestedCr3)=(UINTN)(vmcb+LinuxNptFirst*LinuxPage);
    for (UINTN seg=Es; seg<=Ds; seg+=16) {
        FIELD(UINT16,seg)=seg==Cs ? LinuxCs : LinuxDs;
        FIELD(UINT16,seg+2)=seg==Cs ? 0xa9b : 0xc93;
        FIELD(UINT32,seg+4)=0xffffffffu;
    }
    FIELD(UINT32,Gdtr+4)=LinuxGdtLimit;
    FIELD(UINT64,Gdtr+8)=LinuxGdt;
    FIELD(UINT64,GuestEfer)=SVME|(1u<<11)|(1u<<8)|(1u<<10); /* NXE/LME/LMA */
    FIELD(UINT64,Cr4)=1u<<5; /* PAE, four-level guest paging */
    FIELD(UINT64,Cr3)=LinuxPml4;
    FIELD(UINT64,Cr0)=0x80010031u; /* PG/WP/NE/ET/PE */
    FIELD(UINT64,Rsp)=LinuxStackTop;
    FIELD(UINT64,Rip)=entry;
    FIELD(UINT64,Flags)=0x102; /* IF clear, TF set. #DB is intercepted, never success. */
}

#include "fetch_fixture.h"

#if defined(HV_INPUT_TEST) || defined(HV_ENTRY_TEST) || defined(HV_ACPI_MAP)
extern volatile unsigned linux_vmrun_count; /* assembly increments just before VMRUN */
#endif

static enum LinuxEntryFault linux_checked_fetch(UINT8 *vmcb, UINT64 frame[14], const UINT8 *ram)
{
    enum LinuxEntryFault fault=linux_entry_guard(vmcb,LinuxPage,frame[4],ram,LinuxRamBytes);
    if (fault==LinuxEntryOK) RunGprGuest((UINTN)vmcb,frame);
    return fault;
}

static const char *linux_verified_admit(UINT8 *base, enum LinuxInput *bad)
{
    enum { RsiSlot=4 };
    UINT8 *vmcb=base, *ram=base+LinuxRamFirst*LinuxPage, *media=linux_stage(base);
    volatile UINT64 *guard=(volatile UINT64 *)(base+LinuxGuard*LinuxPage);
    UINT64 frame[14];
    LinuxLayout layout;
    int cpu[4], precision;
    *bad=linux_bad_input(media);
    if (*bad!=LinuxInputOK) return "STAGED INPUT SHA256";
    __cpuid(cpu,(int)0x80000001u);
    if (!(cpu[3]&(1u<<20)) || !(__readmsr(MSR_EFER)&(1u<<11))) return "NX REQUIRED";
    __stosb(base+LinuxIoFirst*LinuxPage,0xff,5*LinuxPage);
    for (UINTN i=0; i<LinuxPage/sizeof(*guard); ++i) guard[i]=LINUX_GUARD_WORD;
    const char *error=run_fetch_fixture(base,&precision);
    if (error) return error;

    error=build_linux_layout(ram,LinuxRamBytes,media+LinuxImageOff,LinuxImageLen,
        media+LinuxRdOff,LinuxRdLen,media+LinuxCmdOff,LinuxCmdLen,&layout);
    if (error) return error;
#ifdef HV_ACPI_MAP
    if (!linux_acpi_publish(ram,LinuxRamBytes)) return "ACPI PUBLICATION";
#endif
    linux_fetch_vmcb(vmcb,layout.entry);
    uint64_t walked;
    if (!fetch_walk(ram,LinuxRamBytes,FIELD(UINT64,Cr3),layout.entry,&walked) ||
        walked!=layout.entry || ram[walked]!=0xfc) return "ADMIT ENTRY MAPPING";
    __stosb((UINT8 *)frame,0,sizeof(frame));
    frame[RsiSlot]=LinuxParams;
    if (!linux_tree_ok(vmcb)) return "ADMIT NPT SETUP";
    if (!linux_guard_ok(base)) return "ADMIT PRIVATE GUARD";
#ifdef HV_ACPI_MAP
    UINT8 *bp=ram+LinuxParams, *nvs_type=bp+AcpiMap+2*AcpiRow+16;
    unsigned attempts=linux_vmrun_count;
    linux_put64(bp+AcpiRootField,LinuxHoleStart);
    enum LinuxEntryFault fault=linux_checked_fetch(vmcb,frame,ram);
    pkg_observe("BAD ROOT ",fault);
    pkg_observe("ROOT ENTRIES ",linux_vmrun_count-attempts);
    if (fault!=LinuxEntryBootParams || linux_vmrun_count!=attempts) return "ACPI BAD ROOT";
    linux_put64(bp+AcpiRootField,PowerRsdpGpa);
    linux_put32(nvs_type,1);
    fault=linux_checked_fetch(vmcb,frame,ram);
    pkg_observe("BAD NVS ",fault);
    pkg_observe("NVS ENTRIES ",linux_vmrun_count-attempts);
    if (fault!=LinuxEntryE820 || linux_vmrun_count!=attempts) return "ACPI BAD NVS";
    linux_put32(nvs_type,4);
    pkg_observe("RSDP GPA ",linux_le64(bp+AcpiRootField));
    pkg_observe("E820 COUNT ",bp[AcpiMapCount]);
    pkg_observe("FACS GPA ",linux_le64(ram+PowerRsdpGpa+PowerFadtOffset+132));
#endif
#ifdef HV_ENTRY_TEST
    unsigned before=linux_vmrun_count;
    frame[RsiSlot]=~0ull;
    enum LinuxEntryFault fault=linux_checked_fetch(vmcb,frame,ram);
    frame[RsiSlot]=LinuxParams;
    pkg_observe("RSI FAULT ",fault);
    pkg_observe("RSI ENTRIES ",linux_vmrun_count-before);
    if (fault!=LinuxEntryRSI || linux_vmrun_count!=before) return "BAD RSI ADMITTED";
    FIELD(UINT64,Cr3)=LinuxRamBytes;
    fault=linux_checked_fetch(vmcb,frame,ram);
    FIELD(UINT64,Cr3)=LinuxPml4;
    pkg_observe("CR3 FAULT ",fault);
    pkg_observe("CR3 ENTRIES ",linux_vmrun_count-before);
    if (fault!=LinuxEntryCR3 || linux_vmrun_count!=before) return "BAD CR3 ADMITTED";
    pkg_observe("ENTRY CR0 ",FIELD(UINT64,Cr0));
    pkg_observe("ENTRY CR4 ",FIELD(UINT64,Cr4));
    pkg_observe("ENTRY EFER ",FIELD(UINT64,GuestEfer));
    pkg_observe("ENTRY RSP ",FIELD(UINT64,Rsp));
    pkg_observe("ENTRY GDT ",FIELD(UINT64,0x468));
#endif
    if (linux_checked_fetch(vmcb,frame,ram)!=LinuxEntryOK) return "NX ENTRY CONTRACT";
#ifdef HV_ACPI_MAP
    pkg_observe("NX ENTRIES ",linux_vmrun_count-attempts);
    if (linux_vmrun_count-attempts!=1 || !linux_acpi_valid(ram,LinuxRamBytes))
        return "ACPI RESTORED MAP";
#endif
#ifdef HV_ENTRY_TEST
    pkg_observe("NX ENTRIES ",linux_vmrun_count-before);
    if (linux_vmrun_count-before!=1) return "NX ENTRY COUNT";
#endif

    UINT64 exit=FIELD(UINT64,ExitCode), info=FIELD(UINT64,Info1), gpa=FIELD(UINT64,Info2);
    UINT64 rip=FIELD(UINT64,Rip), cr3=FIELD(UINT64,Cr3), guarded=guard[0];
    UINT32 rd=linux_le32(ram+LinuxParams+0x218);
    pkg_observe("P5 EXIT ",exit);
    pkg_observe("P5 INFO1 ",info);
    pkg_observe("P5 GPA ",gpa);
    pkg_observe("P5 RIP ",rip);
    pkg_observe("P5 RSI ",frame[RsiSlot]);
    pkg_observe("P5 WALK ",walked);
    pkg_observe("P5 CR3 ",cr3);
    pkg_observe("P5 INITRD ",rd);
    pkg_observe("P5 GUARD ",guarded);
    if (!linux_tree_ok(vmcb)) return "ADMIT NPT CHANGED";
    if (!linux_guard_ok(base)) return "ADMIT PRIVATE GUARD";
    /* APM vol. 2 15.25.7: final GPA (not a guest walk), protection fault,
     * instruction fetch. Nested U/S is not guest CPL; allow either value. */
    if (exit!=0x400 || (info & ~4ull)!=((1ull<<32)|0x11) ||
        rip!=layout.entry || fetch_precision(walked,gpa)!=precision) return "ADMIT FETCH NPF";
    if (cr3!=LinuxPml4 || rd!=layout.initrd || linux_le32(ram+LinuxParams+0x21c)!=LinuxRdLen ||
        FIELD(UINT64,Rsp)!=LinuxStackTop || FIELD(UINT64,Rax)!=0) return "ADMIT ENTRY STATE";
    for (UINTN i=0; i<14; ++i)
        if (frame[i]!=(UINT64)(i==RsiSlot ? LinuxParams : 0)) return "ADMIT GPR STATE";
    return NULL;
}

#ifdef HV_INPUT_TEST
#include "linux_input_trial.h"
#endif

static const char *linux_admit(UINT8 *base)
{
#ifdef HV_INPUT_TEST
    return linux_input_trial(base);
#else
    enum LinuxInput bad;
    return linux_verified_admit(base,&bad);
#endif
}
#endif
