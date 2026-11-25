#include <Uefi.h>
#include <intrin.h>
#include "fixed_npt.h"

/* AMD64 APM Vol. 2, Appendix B: all offsets here are from the VMCB page base. */
enum {
    Exceptions=0x008, Intercepts=0x00c, SvmIntercepts=0x010, IopmPa=0x040,
    MsrpmPa=0x048, Asid=0x058, TlbControl=0x05c, IntControl=0x060,
    ExitCode=0x070, Info1=0x078, Info2=0x080, NpEnable=0x090, NestedCr3=0x0b0, NextRip=0x0c8,
    Es=0x400, Cs=0x410, Ss=0x420, Ds=0x430, GuestEfer=0x4d0, Cr0=0x558,
    Dr7=0x560, Dr6=0x568, Flags=0x570, Rip=0x578, Rsp=0x5d8, Rax=0x5f8, Pat=0x668
};
enum { Vmcb, HostSave, Pml4, Code=Pml4+NptTableCount, Iopm, Msrpm=Iopm+3, Pages=Msrpm+2 };
#ifdef HV_NPT_WRITE
enum { DataGpa=0x2000, InitialByte=0x5a, StoredByte=0xa5, Data=Pages, AllocPages=Pages+1 };
#elif defined(HV_NPT_REMAP)
enum { DataGpa=0x2000, FrameA=Pages, FrameB, AllocPages, MarkerA=0xa11a, MarkerB=0xb22b };
typedef struct { UINT64 exit, ip, ax; } ReadSample;
#else
enum { AllocPages=Pages };
#endif
_Static_assert(sizeof(FixedNpt)==NptTableCount*EFI_PAGE_SIZE, "NPT allocation size mismatch");
#define MSR_EFER 0xc0000080u
#define MSR_VM_CR 0xc0010114u
#define MSR_HSAVE 0xc0010117u
#define SVME (1ull << 12)
#define MARKER 0x4c32u
#define FIELD(t, offset) (*(volatile t *)(vmcb + (offset)))

#ifdef HV_CPUID_RESUME
/* Register order and symbol addresses are shared with cpuid_resume.asm. */
typedef struct { UINT64 bx, cx, dx; } CpuidFrame;
extern void RunCpuidGuest(UINTN vmcb_pa, CpuidFrame *frame);
extern const UINT8 L2Start[], L2Cpuid[], L2AfterCpuid[], L2PassHlt[], L2End[];
static UINT64 guest_ip(const UINT8 *label)
{
    return FixedGuestGpa + ((UINTN)label - (UINTN)L2Start);
}
#endif

static void hex(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *out, CHAR16 *label, UINT64 v)
{
    CHAR16 buf[] = L"0x0000000000000000\r\n";
    for (UINTN i=18; i>2; v>>=4) buf[--i]=L"0123456789ABCDEF"[v & 15];
    out->OutputString(out,label);
    out->OutputString(out,buf);
}

#ifdef HV_FORENSIC_WRITE
/* AMD APM Vol. 2 §15.25.7. U/S is not the guest CPL. This non-SNP fixture
 * uses no shadow stack or verify-paging-write features. */
static BOOLEAN final_write_fault(UINT64 exit, UINT64 info)
{
    const UINT64 required=(1ull<<32)|3;
    const UINT64 excluded=(1ull<<38)|(1ull<<37)|(1ull<<33)|0x58;
    return exit==0x400 && (info & (required|excluded))==required;
}

/* Only decode C6 /0 [disp16],imm8 from our immutable real-mode payload.
 * CS.base and DS.base are zero. Never use the reported GPA as a host pointer. */
static BOOLEAN forensic_report(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *out, const UINT8 *code,
                               UINTN length, UINT64 exit, UINT64 info, UINT64 gpa,
                               UINT64 ip, UINT8 before, UINT8 after)
{
    hex(out,L"EXITCODE: ",exit);
    hex(out,L"EXITINFO1: ",info);
    hex(out,L"Fault GPA (EXITINFO2): ",gpa);
    hex(out,L"Saved RIP: ",ip);
    hex(out,L"Backing byte before: ",before);
    hex(out,L"Backing byte after:  ",after);
    if (!final_write_fault(exit,info)) return FALSE;
    out->OutputString(out,L"Access: final data write, present-page protection fault\r\n");
    if (length<5 || ip<FixedGuestGpa || ip-FixedGuestGpa>length-5) return FALSE;
    const UINT8 *p=code+(UINTN)(ip-FixedGuestGpa);
    if (p[0]!=0xc6 || p[1]!=0x06) return FALSE;
    UINT16 dest=(UINT16)(p[2] | ((UINT16)p[3]<<8));
    hex(out,L"MOV byte [disp16], imm8 | destination: ",dest);
    hex(out,L"Attempted byte from instruction: ",p[4]);
    return ip==0x1000 && gpa==0x2000 && dest==gpa && p[4]==0xa5 &&
           before==0x5a && after==before;
}
#endif

static void smoke(EFI_SYSTEM_TABLE *st)
{
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *out=st->ConOut;
    EFI_BOOT_SERVICES *bs=st->BootServices;
    int r[4];
    __cpuid(r,0);
    if (r[1]!=0x68747541 || r[3]!=0x69746e65 || r[2]!=0x444d4163) {
        out->OutputString(out,L"SKIP: AuthenticAMD required.\r\n"); return;
    }
    __cpuid(r,1);
    if (!((UINT32)r[2] & (1u<<31))) { out->OutputString(out,L"SKIP: VM-only test.\r\n"); return; }
    __cpuid(r,0x40000000);
    if (r[1]!=0x7263694d || r[2]!=0x666f736f || r[3]!=0x76482074) {
        out->OutputString(out,L"SKIP: Hyper-V lab required.\r\n"); return;
    }
    __cpuid(r,(int)0x80000000u);
    if ((UINT32)r[0]<0x8000000au) { out->OutputString(out,L"SKIP: no SVM feature leaf.\r\n"); return; }
    __cpuid(r,(int)0x80000001u);
    if (!(r[2]&4)) { out->OutputString(out,L"SKIP: SVM not exposed.\r\n"); return; }
    __cpuid(r,(int)0x8000000au);
#ifdef HV_CPUID_RESUME
    BOOLEAN nrips=(r[3] & (1u<<3))!=0;
    CpuidFrame frame={0,0,0};
    UINT64 first_exit, first_ip, leaf, subleaf, resume_ip=0;
    BOOLEAN resumed=FALSE;
    UINTN length=(UINTN)L2End-(UINTN)L2Start;
    if (length>EFI_PAGE_SIZE || guest_ip(L2AfterCpuid)!=guest_ip(L2Cpuid)+2) {
        out->OutputString(out,L"FAIL: invalid fixed CPUID guest layout.\r\n"); return;
    }
#endif
    if (!(r[3]&1) || (UINT32)r[1]<2 || (__readcr4() & (1u<<12))) {
        out->OutputString(out,L"SKIP: need nested paging, guest ASID 1 and four-level host paging.\r\n"); return;
    }
    if (__readmsr(MSR_VM_CR) & (1u<<4)) { out->OutputString(out,L"SKIP: VM_CR disables SVM.\r\n"); return; }
    UINT64 efer=__readmsr(MSR_EFER);
    if (efer & SVME) { out->OutputString(out,L"SKIP: SVM already enabled; leaving existing state alone.\r\n"); return; }

    EFI_PHYSICAL_ADDRESS base=0;
    if (EFI_ERROR(bs->AllocatePages(AllocateAnyPages,EfiLoaderData,AllocPages,&base))) {
        out->OutputString(out,L"FAIL: AllocatePages.\r\n"); return;
    }
    /* UEFI x64 boot services use identity mappings. All paging structures are private to L1. */
    UINT8 *vmcb=(UINT8 *)(UINTN)base;
    bs->SetMem(vmcb, EFI_PAGES_TO_SIZE(AllocPages), 0);
    bs->SetMem(vmcb+Iopm*EFI_PAGE_SIZE, 5*EFI_PAGE_SIZE, 0xff);
    build_fixed_npt((FixedNpt *)(vmcb+Pml4*EFI_PAGE_SIZE),
                    base+Pml4*EFI_PAGE_SIZE, base+Code*EFI_PAGE_SIZE);
#ifdef HV_CPUID_RESUME
    bs->CopyMem(vmcb+Code*EFI_PAGE_SIZE,(VOID *)L2Start,length);
#elif defined(HV_NPT_WRITE)
    /* 16-bit MOV byte ptr DS:[2000h], A5h; HLT. Code stays read-only in both arms. */
    const UINT8 payload[]={0xc6,0x06,DataGpa & 0xff,DataGpa >> 8,StoredByte,0xf4};
    bs->CopyMem(vmcb+Code*EFI_PAGE_SIZE,(VOID *)payload,sizeof(payload));
    volatile UINT8 *target=vmcb+Data*EFI_PAGE_SIZE;
    volatile UINT64 *leaf=&((FixedNpt *)(vmcb+Pml4*EFI_PAGE_SIZE))->table[3][DataGpa/NptPageBytes];
    *target=InitialByte;
    *leaf=(base+Data*EFI_PAGE_SIZE)|7;
    UINT64 control_exit, control_ip;
    UINT8 control_byte;
    BOOLEAN control_ok;
#ifdef HV_FORENSIC_WRITE
    UINT8 before=0;
#endif
#elif defined(HV_NPT_REMAP)
    /* MOV AX, word ptr DS:[2000h]; HLT. Neither backing frame changes between reads. */
    const UINT8 payload[]={0xa1,DataGpa & 0xff,DataGpa >> 8,0xf4};
    bs->CopyMem(vmcb+Code*EFI_PAGE_SIZE,(VOID *)payload,sizeof(payload));
    *(volatile UINT16 *)(vmcb+FrameA*EFI_PAGE_SIZE)=MarkerA;
    *(volatile UINT16 *)(vmcb+FrameB*EFI_PAGE_SIZE)=MarkerB;
    volatile UINT64 *leaf=&((FixedNpt *)(vmcb+Pml4*EFI_PAGE_SIZE))->table[3][DataGpa/NptPageBytes];
    ReadSample samples[2];
    UINTN reads=0;
    BOOLEAN matched=FALSE;
#else
    /* One read-only guest page, no guest I/O or arbitrary image loader: MOV AX,imm16; HLT. */
    const UINT8 payload[]={0xb8, MARKER & 0xff, MARKER >> 8, 0xf4};
    bs->CopyMem(vmcb+Code*EFI_PAGE_SIZE,(VOID *)payload,sizeof(payload));
#endif
    FIELD(UINT32,Exceptions)=0xffffffffu;
    FIELD(UINT32,Intercepts)=0x0fu | (1u<<24) | (1u<<27) | (1u<<28) | (1u<<31);
#ifdef HV_CPUID_RESUME
    FIELD(UINT32,Intercepts)|=1u<<18;
#endif
    FIELD(UINT32,SvmIntercepts)=0x7f; /* includes mandatory VMRUN intercept */
    FIELD(UINT64,IopmPa)=base+Iopm*EFI_PAGE_SIZE;
    FIELD(UINT64,MsrpmPa)=base+Msrpm*EFI_PAGE_SIZE;
    FIELD(UINT32,Asid)=1;
    FIELD(UINT8,TlbControl)=1; /* flush before ASID reuse */
    FIELD(UINT32,IntControl)=1u<<24;
    FIELD(UINT64,NpEnable)=1;
    FIELD(UINT64,NestedCr3)=base+Pml4*EFI_PAGE_SIZE;
    for (UINTN seg=Es; seg<=Ds; seg+=16) {
        FIELD(UINT16,seg+2)=(seg==Cs)?0x9b:0x93;
        FIELD(UINT32,seg+4)=0xffff;
    }
    FIELD(UINT64,GuestEfer)=SVME; /* required even for this real-mode guest */
    FIELD(UINT64,Cr0)=0x30;
    FIELD(UINT64,Dr6)=0xffff0ff0u;
    FIELD(UINT64,Dr7)=0x400;
    FIELD(UINT64,Flags)=2;
    FIELD(UINT64,Rip)=FixedGuestGpa;
    FIELD(UINT64,Rsp)=FixedGuestGpa+EFI_PAGE_SIZE-16; /* no stack accesses in this payload */
    FIELD(UINT64,Pat)=__readmsr(0x277);
    FIELD(UINT64,ExitCode)=~0ull; /* never confuse unmodified memory with success */

#ifdef HV_CPUID_RESUME
    out->OutputString(out,L"Entering L2: CPUID -> validate 4 replies -> HLT\r\n");
#elif defined(HV_FORENSIC_WRITE)
    out->OutputString(out,L"Forensic NPT test | fixed MOV byte [2000h], A5h\r\n");
#elif defined(HV_NPT_WRITE)
    out->OutputString(out,L"NPT store to GPA 2000: writable control, then read-only\r\n");
#elif defined(HV_NPT_REMAP)
    out->OutputString(out,L"NPT remap: read GPA 2000, first frame A then frame B\r\n");
#else
    out->OutputString(out,L"Entering fixed L2 guest: MOV AX,4C32h; HLT\r\n");
#endif
    EFI_TPL tpl=bs->RaiseTPL(TPL_HIGH_LEVEL);
    UINT64 cr2=__readcr2(), dr6=__readdr(6), dr7=__readdr(7);
    __writemsr(MSR_EFER,efer|SVME);
    UINT64 hsave=__readmsr(MSR_HSAVE);
    __svm_clgi();
    __writemsr(MSR_HSAVE,base+HostSave*EFI_PAGE_SIZE);
#ifdef HV_CPUID_RESUME
    RunCpuidGuest((UINTN)base,&frame);
    first_exit=FIELD(UINT64,ExitCode);
    first_ip=FIELD(UINT64,Rip);
    leaf=FIELD(UINT64,Rax);
    subleaf=frame.cx;
    if (first_exit==0x72 && first_ip==guest_ip(L2Cpuid) && leaf==0x400000fe && subleaf==7) {
        /* No general instruction decoder: the only accepted site is our known 0F A2. */
        resume_ip=nrips ? FIELD(UINT64,NextRip) : first_ip+2;
        if (resume_ip==guest_ip(L2AfterCpuid)) {
            FIELD(UINT64,Rax)=0x11223344u;
            frame.bx=0x55667788u; frame.cx=0x99aabbccu; frame.dx=0xddeeff00u;
            FIELD(UINT64,Rip)=resume_ip;
            FIELD(UINT8,TlbControl)=0; /* same ASID and unmodified mappings */
            FIELD(UINT64,ExitCode)=~0ull;
            resumed=TRUE;
            RunCpuidGuest((UINTN)base,&frame);
        }
    }
#elif defined(HV_NPT_WRITE)
    __svm_vmrun((UINTN)base);
    control_exit=FIELD(UINT64,ExitCode);
    control_ip=FIELD(UINT64,Rip);
    control_byte=*target;
    control_ok=control_exit==0x78 && control_ip==FixedGuestGpa+sizeof(payload)-1 && control_byte==StoredByte;
#ifdef HV_FORENSIC_WRITE
    control_ok=control_ok && !final_write_fault(control_exit,FIELD(UINT64,Info1));
#endif
    if (control_ok) {
        *target=InitialByte;
#ifdef HV_FORENSIC_WRITE
        before=*target;
#endif
        *leaf &= ~2ull; /* retain backing frame and all non-write attributes */
        FIELD(UINT64,Rip)=FixedGuestGpa;
        FIELD(UINT8,TlbControl)=1; /* revoke the cached writable translation before re-entry */
        FIELD(UINT64,ExitCode)=~0ull;
        FIELD(UINT64,Info1)=FIELD(UINT64,Info2)=0;
        __svm_vmrun((UINTN)base);
    }
#elif defined(HV_NPT_REMAP)
    for (UINTN arm=0; arm<2; ++arm) {
        *leaf=(base+(arm ? FrameB : FrameA)*EFI_PAGE_SIZE)|5; /* identical read-only permissions */
        FIELD(UINT8,TlbControl)=1; /* full flush at VMRUN; retain ASID and N_CR3 */
        FIELD(UINT64,Rip)=FixedGuestGpa;
        FIELD(UINT64,Rax)=0; /* a missing load cannot pass either arm */
        FIELD(UINT64,ExitCode)=~0ull;
        __svm_vmrun((UINTN)base);
        ReadSample *s=&samples[reads++];
        s->exit=FIELD(UINT64,ExitCode);
        s->ip=FIELD(UINT64,Rip);
        s->ax=FIELD(UINT64,Rax);
        matched=s->exit==0x78 && s->ip==FixedGuestGpa+sizeof(payload)-1 &&
                s->ax==(arm ? MarkerB : MarkerA);
        if (!matched) break;
    }
#else
    /* The fixed guest touches only AX. VMRUN saves host RAX and writes guest RAX to the VMCB. */
    __svm_vmrun((UINTN)base);
#endif
    __writecr2(cr2);
    __writedr(6,dr6);
    __writedr(7,dr7);
    __writemsr(MSR_HSAVE,hsave);
    __svm_stgi();
    __writemsr(MSR_EFER,efer);
    bs->RestoreTPL(tpl);

#ifdef HV_NPT_REMAP
    for (UINTN i=0; i<reads; ++i) {
        out->OutputString(out,i ? L"Frame B:\r\n" : L"Frame A:\r\n");
        hex(out,L"  EXITCODE: ",samples[i].exit);
        hex(out,L"  RIP: ",samples[i].ip);
        hex(out,L"  Guest RAX: ",samples[i].ax);
    }
    out->OutputString(out,reads==2 && matched
        ? L"PASS: same GPA read A then B after remap.\r\n"
        : L"FAIL: unexpected halt or backing-frame marker.\r\n");
#else
    UINT64 exit=FIELD(UINT64,ExitCode), ip=FIELD(UINT64,Rip);
#ifdef HV_NPT_WRITE
    hex(out,L"RW EXITCODE: ",control_exit);
    hex(out,L"RW RIP: ",control_ip);
    hex(out,L"RW byte (expected A5): ",control_byte);
    if (!control_ok) {
        out->OutputString(out,L"FAIL: writable control; protected arm not entered.\r\n");
    } else {
        UINT64 info=FIELD(UINT64,Info1), gpa=FIELD(UINT64,Info2);
        UINT8 protected_byte=*target;
#ifdef HV_FORENSIC_WRITE
        BOOLEAN matched=forensic_report(out,vmcb+Code*EFI_PAGE_SIZE,sizeof(payload),
                                        exit,info,gpa,ip,before,protected_byte);
        out->OutputString(out,matched
            ? L"PASS: attempted write identified; no byte committed.\r\n"
            : L"FAIL: forensic event or backing byte differs from fixture.\r\n");
#else
        hex(out,L"RO EXITCODE: ",exit);
        hex(out,L"RO RIP: ",ip);
        hex(out,L"RO EXITINFO1: ",info);
        hex(out,L"RO fault GPA: ",gpa);
        hex(out,L"RO byte (expected 5A): ",protected_byte);
        /* P=1, W/R=1, RSVD=0, I/D=0; final data access, not a table walk. */
        if (exit==0x400 && ip==FixedGuestGpa && gpa==DataGpa &&
            (info & 0x30000001bull)==0x100000003ull && protected_byte==InitialByte)
            out->OutputString(out,L"PASS: writable store retired; read-only store faulted.\r\n");
        else
            out->OutputString(out,L"FAIL: wrong nested fault, RIP, GPA or target byte.\r\n");
#endif
    }
#else
    UINT64 ax=FIELD(UINT64,Rax);
#ifdef HV_CPUID_RESUME
    hex(out,L"First EXITCODE: ",first_exit);
    hex(out,L"CPUID RIP: ",first_ip);
    hex(out,L"Leaf: ",leaf);
    hex(out,L"Subleaf: ",subleaf);
    hex(out,nrips ? L"Resume RIP (nRIP): " : L"Resume RIP (fixed length): ",resume_ip);
    hex(out,L"VMRUN count: ",resumed ? 2 : 1);
#endif
    hex(out,L"EXITCODE: ",exit);
    hex(out,L"Guest RAX: ",ax);
    hex(out,L"Guest RIP: ",ip);
#ifdef HV_CPUID_RESUME
    if (resumed && exit==0x78 && ax==0x50415353 && ip==guest_ip(L2PassHlt))
        out->OutputString(out,L"PASS: L2 validated EAX/EBX/ECX/EDX after resume.\r\n");
#else
    if (exit==0x78 && ax==MARKER && ip==FixedGuestGpa+sizeof(payload)-1)
        out->OutputString(out,L"PASS: L2 marker executed; HLT intercepted; returned to L1.\r\n");
#endif
    else {
        out->OutputString(out,L"FAIL: unexpected guest exit or state.\r\n");
        hex(out,L"EXITINFO1: ",FIELD(UINT64,Info1));
        hex(out,L"EXITINFO2: ",FIELD(UINT64,Info2));
    }
#endif
#endif
    bs->FreePages(base,AllocPages);
}

EFI_STATUS EFIAPI EfiMain(EFI_HANDLE image, EFI_SYSTEM_TABLE *st)
{
    (void)image;
    st->ConOut->ClearScreen(st->ConOut);
    st->ConOut->OutputString(st->ConOut,L"HV | AMD SVM nested guest smoke test\r\n\r\n");
    smoke(st);
    st->ConOut->OutputString(st->ConOut,L"\r\nEsc returns to firmware.\r\n");
    EFI_STATUS s=st->BootServices->SetWatchdogTimer(0,0,0,NULL);
    while (!EFI_ERROR(s)) {
        UINTN index;
        EFI_INPUT_KEY key;
        s=st->BootServices->WaitForEvent(1,&st->ConIn->WaitForKey,&index);
        if (EFI_ERROR(s)) break;
        s=st->ConIn->ReadKeyStroke(st->ConIn,&key);
        if (s==EFI_NOT_READY) s=EFI_SUCCESS;
        else if (!EFI_ERROR(s) && key.ScanCode==SCAN_ESC) break;
    }
    return s;
}
