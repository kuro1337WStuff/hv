#ifndef HV_W01_H
#define HV_W01_H
#include "w01_step.h"

extern void W01Switch(UINTN pa, UINT64 gpr[14], UINTN host_ext_pa);
extern int W01PrepareHostGdt(UINT8 *page, UINT8 gdtrs[20]);
extern void W01LoadGdtr(const UINT8 *gdtr);

typedef struct {
    UINT64 entries, steps, reset_pc, reset_raw, reset_info, reset_exit, before, bytes;
    UINT64 cpuid, efer_io, reached32, reached64;
} W01Evidence;

static void w01_reset(UINT8 *vmcb)
{
#ifdef HV_W03
    /* Only original admitted file bytes survive reset, not descriptor/slack. */
    UINT8 *ram=w01_page(vmcb,W02Ram), *file=ram+W03FileGpa;
    __stosb(vmcb,0,(UINTN)(file-vmcb));
    __stosb(file+W03_FILE_SIZE,0,W02RamBytes-W03FileGpa-W03_FILE_SIZE);
    ((UINT32 *)(ram+W03DescriptorGpa))[0]=W03DescriptorTag;
    ((UINT32 *)(ram+W03DescriptorGpa))[1]=W03_FILE_SIZE;
#else
    __stosb(vmcb,0,W01Flash*W01Page); /* retained flash is above this region */
#endif
    __stosb(w01_page(vmcb,Iopm),0xff,5*W01Page); /* complete IOPM and MSRPM */
    w01_memory(vmcb);
    fixed_vmcb_defaults(vmcb,(UINTN)vmcb);
    FIELD(UINT32,0x004)=0xffffffffu; /* all DR reads/writes */
    FIELD(UINT32,Intercepts)|=(1u<<18)|(1u<<22)|(1u<<26)|(1u<<29)|(1u<<30);
    FIELD(UINT32,SvmIntercepts)=0x3fffu; /* also WBINVD, MONITOR/MWAIT, XSETBV */
    FIELD(UINT64,Pat)=0x0007040600070406ull;
    for (UINTN seg=Es;seg<=0x450;seg+=16) { /* ES..GS */
        FIELD(UINT16,seg+2)=seg==Cs ? 0x9b : 0x93;
        FIELD(UINT32,seg+4)=0xffff;
    }
    FIELD(UINT16,Cs)=0xf000;
    FIELD(UINT64,Cs+8)=0xffff0000ull;
    FIELD(UINT32,0x464)=FIELD(UINT32,0x484)=0xffff; /* GDTR, IDTR */
    FIELD(UINT16,0x472)=0x82; FIELD(UINT16,0x492)=0x8b; /* LDTR, TR */
    FIELD(UINT32,0x474)=FIELD(UINT32,0x494)=0xffff;
    FIELD(UINT64,Rip)=0xfff0;
    FIELD(UINT64,Cr0)=0x60000010u;
    FIELD(UINT64,GuestEfer)=SVME; /* required internally by AMD VMRUN */
    FIELD(UINT64,Flags)=2; /* no TF until after actual denied reset fetch */
}
static void w01_enter(UINT8 *vmcb, UINT64 *gpr, W01Evidence *e)
{
    FIELD(UINT8,TlbControl)=1;
    FIELD(UINT32,0xc0)=0; /* no cached VMCB fields */
    FIELD(UINT64,ExitCode)=~0ull;
    FIELD(UINT64,Info1)=FIELD(UINT64,Info2)=0;
    ++e->entries;
    FIELD(UINT64,Dr6)=0xffff0ff0u; /* fresh BS evidence for each #DB */
    W01Switch((UINTN)vmcb,gpr,(UINTN)w01_page(vmcb,W01ExtHost));
}
static const char *w01_intercept(UINT8 *vmcb, UINT64 *gpr, const UINT8 *code, enum W01Kind k,
                                W01Evidence *e)
{
    /* Bind both bytes, unprefixed, and the hardware nRIP. Only VTF's
     * fixed non-CoCo requests, no host-derived CPUID/MSR data or accesses. */
    UINT64 exitcode=FIELD(UINT64,ExitCode), ip=FIELD(UINT64,Rip);
    if (code[0]!=0x0f || FIELD(UINT64,NextRip)!=ip+2) return "INTERCEPT NRIP";
    if (k==W01Cpuid && exitcode==0x72 && code[1]==0xa2) {
        /* floor_contract.h: the two non-indexed identification leaves used
         * to exclude TDX and SEV; ECX input ignored by this CPU contract. */
        UINT32 leaf=(UINT32)FIELD(UINT64,Rax);
        if (leaf!=(ip==0xfffff93du ? 0u : 0x80000000u) || e->cpuid!=(leaf ? 1u : 0u))
            return "CPUID LEAF";
        FIELD(UINT64,Rax)=leaf ? 0x80000001u : 1u;
        gpr[0]=leaf ? 0 : 0x68747541u; gpr[1]=leaf ? 0 : 0x444d4163u; gpr[2]=leaf ? 0 : 0x69746e65u;
        ++e->cpuid;
    } else if ((k==W01ReadEfer || k==W01WriteEfer) && exitcode==0x7c &&
               (UINT32)gpr[1]==MSR_EFER && FIELD(UINT64,GuestEfer)==SVME) {
        if (k==W01ReadEfer && code[1]==0x32 && FIELD(UINT64,Info1)==0 && !e->efer_io) {
            FIELD(UINT64,Rax)=gpr[2]=0; /* guest-visible reset EFER */
        } else if (k==W01WriteEfer && code[1]==0x30 && FIELD(UINT64,Info1)==1 && e->efer_io==1 &&
                   (UINT32)FIELD(UINT64,Rax)==0x100 && (UINT32)gpr[2]==0 &&
                   !(FIELD(UINT64,Cr0)&(1ull<<31)) && (FIELD(UINT64,Cr4)&0x20)) {
            FIELD(UINT64,GuestEfer)=SVME|0x100u; /* guest-only LME */
        } else return "EFER OPERATION";
        ++e->efer_io;
    } else return "UNHANDLED EXIT";
    FIELD(UINT64,Rip)=ip+2;
    return NULL;
}
static const char *w01_trace(UINT8 *vmcb, UINT64 *gpr, W01Evidence *e)
{
    e->before=W01_RESET_GPA;
    w01_enter(vmcb,gpr,e);
    e->reset_exit=FIELD(UINT64,ExitCode); e->reset_raw=FIELD(UINT64,Info2); e->reset_info=FIELD(UINT64,Info1);
    e->reset_pc=FIELD(UINT64,Cs+8)+FIELD(UINT64,Rip);
    if (FIELD(UINT16,Cs)!=0xf000 || FIELD(UINT64,Cs+8)!=0xffff0000u || FIELD(UINT64,Rip)!=0xfff0 ||
        (FIELD(UINT64,Cr0)&((1ull<<31)|1)) || !w01_fetch(vmcb,W01_RESET_GPA)) return "RESET FETCH";
    *w01_code_leaf(vmcb,W01_RESET_GPA)&=~W01_NX; /* only VTF. SEC stays RO/NX. */
    BOOLEAN jumped=FALSE;
    /* Bound every VMRUN as well as the overall count. At most one scalar
     * instruction (two for MOV SS/direct JMP), or an intercepted fetch/op.
     * The 2,048-entry fill and 6,144-dword clear are genuine guest loops. */
    while (e->entries<131072) {
        UINT32 pc;
        if (!w01_pc(vmcb,&pc)) return "CODE TRANSLATION";
        e->before=pc;
        if (pc/W01Page==W01_SEC_GPA/W01Page) {
            w01_enter(vmcb,gpr,e); /* always NX, regardless of byte offset */
            return jumped && e->reached64 && pc==W01_SEC_GPA && w01_fetch(vmcb,W01_SEC_GPA)
                ? NULL : "SEC FETCH";
        }
        const UINT8 *code=w01_page(vmcb,W01Flash)+(pc-W01_FLASH_GPA);
        if ((FIELD(UINT64,0x068)&1) || (FIELD(UINT64,Flags)&(1u<<17))) return "DEBUG SHADOW";
        enum W01Kind k=w01_kind(vmcb,pc,gpr);
        if (k==W01Reject) return "UNREVIEWED INSN";
        FIELD(UINT64,Flags)|=0x100u;
        UINT64 before_ip=FIELD(UINT64,Rip);
        w01_enter(vmcb,gpr,e);
        if (FIELD(UINT16,Cs)==0x10 && (FIELD(UINT64,Cr0)&1) && !e->reached32) e->reached32=FIELD(UINT64,Rip);
        if (FIELD(UINT16,Cs)==0x38 && (FIELD(UINT16,Cs+2)&0x200) && (FIELD(UINT64,GuestEfer)&0x400) &&
            !e->reached64) e->reached64=FIELD(UINT64,Rip);
        if (k==W01JumpSec) jumped=TRUE;
        UINT64 x=FIELD(UINT64,ExitCode);
        if (x==0x400) {
            if (k==W01JumpSec && e->reached64 && w01_fetch(vmcb,W01_SEC_GPA)) return NULL;
            return "UNEXPECTED NPF";
        }
        if (k==W01Scalar || k==W01JumpSec) {
            if (x!=0x41 || !(FIELD(UINT64,Flags)&0x100) ||
                (FIELD(UINT64,Dr6)&0xe00fu)!=0x4000) return "EXPECTED STEP";
            ++e->steps;
        } else {
            if (FIELD(UINT64,Rip)!=before_ip) return "INTERCEPT RIP";
            const char *error=w01_intercept(vmcb,gpr,code,k,e);
            if (error) return error;
        }
    }
    return "ENTRY BUDGET";
}
#ifdef HV_W02
#include "w02_run.h"
#endif
static const char *run_standalone_l2(void *reservation)
{
    static const UINT8 code_pin[]=W01_CODE_SHA256, vars_pin[]=W01_VARS_SHA256;
    _Static_assert(sizeof(code_pin)==32 && sizeof(vars_pin)==32,"digest length");
    UINT8 *vmcb=(UINT8 *)reservation, *flash=w01_page(vmcb,W01Flash);
    if (!w01_digest(flash+W01VarsBytes,W01CodeBytes,code_pin)) return "CODE HASH";
    if (!w01_digest(flash,W01VarsBytes,vars_pin)) return "VARS HASH";
#ifdef HV_W03
    static const UINT8 file_pin[]=W03_FILE_SHA256;
    _Static_assert(sizeof(file_pin)==32,"WinBoot digest length");
    if (!w01_digest(w01_page(vmcb,W02Ram)+W03FileGpa,W03_FILE_SIZE,file_pin)) return "BOOT FILE HASH";
#endif
    pkg_observe("SEC PIN ",W01_SEC_GPA);
    int r[4];
    __cpuid(r,0);
    if (r[0]<1 || r[1]!=0x68747541 || r[3]!=0x69746e65 || r[2]!=0x444d4163) return "AMD REQUIRED";
    __cpuid(r,1);
    if (!((UINT32)r[2]&(1u<<31)) || !(r[3]&(1u<<16)) || !(r[3]&(1u<<5))) return "VM PAT MSR REQUIRED";
    if (!(r[3]&(1u<<9)) || !(__readmsr(0x1b)&(1u<<8))) return "BSP REQUIRED";
    __cpuid(r,0x40000000);
    if (r[1]!=0x7263694d || r[2]!=0x666f736f || r[3]!=0x76482074) return "HYPER V REQUIRED";
#ifdef HV_W02
    __cpuid(r,0x40000003);
    if (!(r[0]&2)) return "REFERENCE CLOCK";
    W02State w02; __stosb((UINT8 *)&w02,0,sizeof(w02));
    w02.start=__readmsr(0x40000020); /* Hyper-V partition reference time in 100 ns */
#endif
    __cpuid(r,(int)0x80000000u);
    if ((UINT32)r[0]>=0x80000022u) {
        int ext[4]; __cpuid(ext,(int)0x80000022u);
        if ((ext[0]&2) && (__readmsr(0xc000010fu)&0x40)) return "HOST LBR STACK";
    }
    if ((UINT32)r[0]<0x8000000au) return "NO SVM LEAF";
    __cpuid(r,(int)0x80000001u);
    if (!(r[2]&4) || !(r[3]&(1u<<20))) return "SVM NX REQUIRED";
    __cpuid(r,(int)0x8000000au);
    if ((r[3]&9)!=9 || (UINT32)r[1]<2) return "NPT NRIP ASID";
    if ((__readcr4()&((1u<<12)|(1u<<23))) || (__readmsr(MSR_VM_CR)&(1u<<4))) return "HOST SVM MODE";
    UINT64 efer=__readmsr(MSR_EFER);
    if ((efer&(SVME|0x800))!=0x800) return "HOST EFER";
    if ((__readeflags()&(0x100u|0x200u)) || __readmsr(0x1d9)!=0) return "HOST DEBUG OR IF";
    w01_reset(vmcb);
    if (!w01_review(vmcb)) return "VTF REVIEW HASH";
    UINT64 gpr[14]; __stosb((UINT8 *)gpr,0,sizeof(gpr));
    gpr[2]=0x00020f32; /* existing fixed AMD CPU floor reset signature */
    W01Evidence ev; __stosb((UINT8 *)&ev,0,sizeof(ev));
    UINT8 gdtrs[20];
    if (!W01PrepareHostGdt(w01_page(vmcb,W01HostGdt),gdtrs)) return "HOST GDT";
    UINT64 cr2=__readcr2(), dr6=__readdr(6), dr7=__readdr(7);
    __writemsr(MSR_EFER,efer|SVME);
    UINT64 old_hsave=__readmsr(MSR_HSAVE);
    __svm_clgi();
    W01LoadGdtr(gdtrs+10);
    __writemsr(MSR_HSAVE,(UINTN)w01_page(vmcb,HostSave));
    const char *reason=w01_trace(vmcb,gpr,&ev);
#ifdef HV_W02
    if (!reason) reason=w02_run(vmcb,gpr,&ev,&w02);
#endif
    __writecr2(cr2); __writedr(6,dr6); __writedr(7,dr7);
    __writemsr(MSR_HSAVE,old_hsave); W01LoadGdtr(gdtrs);
    __svm_stgi(); __writemsr(MSR_EFER,efer);
#ifdef HV_W04
    /* Absolute guest PCs and exit metadata only. Do not publish image bytes. */
    pkg_observe("SEC FETCH ",w02.sec_pc); pkg_observe("PEI PAGE ",w02.pei_page);
    pkg_observe("DXE OUT PC ",w02.dxe_pc); pkg_observe("READY PC ",w02.ready_pc);
    pkg_observe("IMAGE BASE ",w02.boot_base); pkg_observe("ENTRY PC ",w02.boot_pc);
    pkg_observe("ENTRY NEXT ",w02.boot_next); pkg_observe("ENTRY STEPS ",w02.boot_steps);
    pkg_observe("EFI TABLE ",w02.boot_table); pkg_observe("PHASE ",w02.phase);
    pkg_observe("POST STEPS ",w02.w04_steps); pkg_observe("POST PC ",w02.w04_pc);
    pkg_observe("NEXT TARGET ",w02.w04_target); pkg_observe("SERVICE OFF ",w02.w04_service);
    pkg_observe("RIP ",FIELD(UINT64,Rip)); pkg_observe("RSP ",FIELD(UINT64,Rsp));
    pkg_observe("EXIT ",FIELD(UINT64,ExitCode)); pkg_observe("INFO1 ",FIELD(UINT64,Info1));
    pkg_observe("INFO2 ",FIELD(UINT64,Info2)); pkg_observe("DR6 ",FIELD(UINT64,Dr6));
#else
    if (ev.before>=W01_CODE_GPA && ev.before<=0xffffffffu) {
        const UINT8 *p=flash+(UINTN)(ev.before-W01_FLASH_GPA);
        for (unsigned i=0;i<8 && ev.before+i<=0xffffffffu;++i) ev.bytes|=(UINT64)p[i]<<(i*8);
    }
    pkg_observe("RESET EXIT ",ev.reset_exit); pkg_observe("RESET INFO ",ev.reset_info);
    pkg_observe("RESET RAW ",ev.reset_raw); pkg_observe("RESET PC ",ev.reset_pc);
    pkg_observe("PM32 RIP ",ev.reached32); pkg_observe("LONG RIP ",ev.reached64);
#ifndef HV_W03
    pkg_observe("CPUID COUNT ",ev.cpuid); pkg_observe("EFER COUNT ",ev.efer_io);
#endif
    pkg_observe("ENTRIES ",ev.entries); pkg_observe("STEPS ",ev.steps);
    pkg_observe("BEFORE PC ",ev.before); pkg_observe("BYTES LE ",ev.bytes);
    pkg_observe("EXIT ",FIELD(UINT64,ExitCode)); pkg_observe("INFO1 ",FIELD(UINT64,Info1));
#ifndef HV_W03
    pkg_observe("RAW GPA ",FIELD(UINT64,Info2)); pkg_observe("CS BASE ",FIELD(UINT64,Cs+8));
    pkg_observe("CS ",FIELD(UINT16,Cs)); pkg_observe("RIP ",FIELD(UINT64,Rip));
#else
    pkg_observe("RAW GPA ",FIELD(UINT64,Info2)); pkg_observe("RIP ",FIELD(UINT64,Rip));
#endif
    pkg_observe("CR0 ",FIELD(UINT64,Cr0)); pkg_observe("CR3 ",FIELD(UINT64,Cr3));
    pkg_observe("CR4 ",FIELD(UINT64,Cr4)); pkg_observe("RAX ",FIELD(UINT64,Rax));
    pkg_observe("RCX ",gpr[1]); pkg_observe("DR6 ",FIELD(UINT64,Dr6));
#ifdef HV_W02
    pkg_observe("SEC FETCH ",w02.sec_pc); pkg_observe("PHASE ",w02.phase);
    pkg_observe("FV HASHED ",w02.fvs); pkg_observe("PEI OUT PC ",w02.pei_pc);
    pkg_observe("PEI PAGE ",w02.pei_page); pkg_observe("DXE OUT PC ",w02.dxe_pc);
    pkg_observe("W02 OPS ",w02.ops);
    pkg_observe("IO COUNT ",w02.io); pkg_observe("RDX ",gpr[2]); pkg_observe("RSP ",FIELD(UINT64,Rsp));
    pkg_observe("ELAPSED100NS",__readmsr(0x40000020)-w02.start);
#ifdef HV_W03
    pkg_observe("READY PC ",w02.ready_pc); pkg_observe("IMAGE BASE ",w02.boot_base);
    pkg_observe("ENTRY RVA ",W03_ENTRY_RVA); pkg_observe("ENTRY PC ",w02.boot_pc);
    pkg_observe("ENTRY NEXT ",w02.boot_next); pkg_observe("EFI HANDLE ",w02.boot_handle);
    pkg_observe("EFI TABLE ",w02.boot_table); pkg_observe("ENTRY RSP ",w02.boot_rsp);
    pkg_observe("ENTRY STEPS ",w02.boot_steps);
#endif
#endif
#endif
    return reason;
}
#endif
