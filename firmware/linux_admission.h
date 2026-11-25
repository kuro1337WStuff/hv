#ifndef HV_LINUX_ADMISSION_H
#define HV_LINUX_ADMISSION_H
#include "fixed_vmcb.h"
#include "linux_layout.h"
#include "linux_media.h"

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
        npt[3+gpa/LinuxLargePage][gpa/LinuxPage%512]=(UINTN)(ram+gpa)|LINUX_LEAF_FLAGS;
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
                    want=(UINTN)(vmcb+(LinuxRamFirst+pfn)*LinuxPage)|LINUX_LEAF_FLAGS;
            }
            if (npt[page][slot]!=want) return 0;
        }
    }
    return 1;
}

static const char *linux_admit(UINT8 *base)
{
    enum { Gdtr=0x460, RsiSlot=4 };
    const UINT64 nxe=1ull<<11, canary=0x50354e5847554152ull;
    UINT8 *vmcb=base, *ram=base+LinuxRamFirst*LinuxPage, *media=linux_stage(base);
    volatile UINT64 *guard=(volatile UINT64 *)(base+LinuxGuard*LinuxPage);
    UINT64 frame[14];
    LinuxLayout layout;
    int cpu[4];
    __cpuid(cpu,(int)0x80000001u);
    if (!(cpu[3]&(1u<<20)) || !(__readmsr(MSR_EFER)&nxe)) return "NX REQUIRED";
    const char *error=build_linux_layout(ram,LinuxRamBytes,media+LinuxImageOff,LinuxImageLen,
        media+LinuxRdOff,LinuxRdLen,media+LinuxCmdOff,LinuxCmdLen,&layout);
    if (error) return error;
    /* Pin the secondary stop too: CLD is a single, non-repeating instruction
     * that cannot clear TF or establish a debug shadow. If NX is ignored, its
     * intercepted single-step is a terminal failure, never proof of admission. */
    if (ram[layout.entry]!=0xfc) return "ADMIT FIRST OPCODE";

    __stosb(vmcb,0,LinuxPage); /* Never clear the active HSAVE page. */
    __stosb(base+LinuxIoFirst*LinuxPage,0xff,5*LinuxPage);
    for (UINTN i=0; i<LinuxPage/sizeof(*guard); ++i) guard[i]=canary;
    linux_build_npt(base);
    fixed_vmcb_defaults(vmcb,(UINTN)base);
    FIELD(UINT64,IopmPa)=(UINTN)(base+LinuxIoFirst*LinuxPage);
    FIELD(UINT64,MsrpmPa)=(UINTN)(base+LinuxMsrFirst*LinuxPage);
    FIELD(UINT64,NestedCr3)=(UINTN)(base+LinuxNptFirst*LinuxPage);
    for (UINTN seg=Es; seg<=Ds; seg+=16) {
        FIELD(UINT16,seg)=seg==Cs ? LinuxCs : LinuxDs;
        FIELD(UINT16,seg+2)=seg==Cs ? 0xa9b : 0xc93;
        FIELD(UINT32,seg+4)=0xffffffffu;
    }
    FIELD(UINT32,Gdtr+4)=LinuxGdtLimit;
    FIELD(UINT64,Gdtr+8)=LinuxGdt;
    FIELD(UINT64,GuestEfer)=SVME|nxe|(1u<<8)|(1u<<10); /* LME/LMA */
    FIELD(UINT64,Cr4)=1u<<5; /* PAE, four-level guest paging */
    FIELD(UINT64,Cr3)=LinuxPml4;
    FIELD(UINT64,Cr0)=0x80010031u; /* PG/WP/NE/ET/PE */
    FIELD(UINT64,Rsp)=LinuxStackTop;
    FIELD(UINT64,Rip)=layout.entry;
    FIELD(UINT64,Flags)=0x102; /* IF clear, TF set. #DB is intercepted, never success. */
    __stosb((UINT8 *)frame,0,sizeof(frame));
    frame[RsiSlot]=LinuxParams;
    if (!linux_tree_ok(vmcb)) return "ADMIT NPT SETUP";
    RunGprGuest((UINTN)base,frame); /* one entry only, no permission upgrade or resume */

    UINT64 exit=FIELD(UINT64,ExitCode), info=FIELD(UINT64,Info1), gpa=FIELD(UINT64,Info2);
    UINT64 rip=FIELD(UINT64,Rip), cr3=FIELD(UINT64,Cr3), guarded=guard[0];
    UINT32 rd=linux_le32(ram+LinuxParams+0x218);
    pkg_observe("P5 EXIT ",exit);
    pkg_observe("P5 INFO1 ",info);
    pkg_observe("P5 GPA ",gpa);
    pkg_observe("P5 RIP ",rip);
    pkg_observe("P5 RSI ",frame[RsiSlot]);
    pkg_observe("P5 CR3 ",cr3);
    pkg_observe("P5 INITRD ",rd);
    pkg_observe("P5 GUARD ",guarded);
    if (!linux_tree_ok(vmcb)) return "ADMIT NPT CHANGED";
    for (UINTN i=0; i<LinuxPage/sizeof(*guard); ++i)
        if (guard[i]!=canary) return "ADMIT PRIVATE GUARD";
    /* APM vol. 2 15.25.7: final GPA (not a guest walk), protection fault,
     * instruction fetch. Nested U/S is not guest CPL; allow either value. */
    if (exit!=0x400 || (info & ~4ull)!=((1ull<<32)|0x11) ||
        gpa!=layout.entry || rip!=layout.entry) return "ADMIT FETCH NPF";
    if (cr3!=LinuxPml4 || rd!=layout.initrd || linux_le32(ram+LinuxParams+0x21c)!=LinuxRdLen ||
        FIELD(UINT64,Rsp)!=LinuxStackTop || FIELD(UINT64,Rax)!=0) return "ADMIT ENTRY STATE";
    for (UINTN i=0; i<14; ++i)
        if (frame[i]!=(UINT64)(i==RsiSlot ? LinuxParams : 0)) return "ADMIT GPR STATE";
    return NULL;
}
#endif
