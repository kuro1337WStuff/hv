#ifndef HV_VCPU_GPR_H
#define HV_VCPU_GPR_H

/* Included only by l2_smoke.c. The packed guest image and the software-frame
 * order are shared with vcpu_gpr.asm; RAX/RSP/RIP live in the VMCB. */
extern void RunGprGuest(UINTN vmcb_pa, UINT64 frame[14]);
extern const UINT8 GprStart[], GprCpuid1[], GprAfter1[], GprCpuid2[], GprAfter2[],
                   GprPassHlt[], GprFailHlt[], GprEnd[];

enum { GprRootGpa=0x2000, GprStackGpa=0x7000, GprEntries=3 };
static const UINT8 *const gpr_sites[]={GprCpuid1,GprCpuid2};
static const UINT8 *const gpr_after[]={GprAfter1,GprAfter2};

static UINT64 gpr_ip(const UINT8 *label)
{
    return FixedGuestGpa + ((UINTN)label-(UINTN)GprStart);
}

static BOOLEAN gpr_layout_ok(UINTN length)
{
    if (!length || length>EFI_PAGE_SIZE) return FALSE;
    for (UINTN i=0; i<2; ++i) {
        UINTN offset=(UINTN)gpr_sites[i]-(UINTN)GprStart;
        if (offset>=length || length-offset<=2 || gpr_after[i]!=gpr_sites[i]+2 ||
            gpr_sites[i][0]!=0x0f || gpr_sites[i][1]!=0xa2) return FALSE;
    }
    return (UINTN)GprAfter1<(UINTN)GprCpuid2 &&
        (UINTN)GprPassHlt-(UINTN)GprStart<length &&
        (UINTN)GprFailHlt-(UINTN)GprStart<length && GprPassHlt[0]==0xf4 && GprFailHlt[0]==0xf4;
}

static void gpr_guest_state(UINT8 *vmcb, EFI_PHYSICAL_ADDRESS base)
{
    /* Only code and stack have guest-linear mappings. CR3 and its links are
     * GPAs, never L1 addresses. The walk frames also need NPT translations. */
    FixedNpt *guest=(FixedNpt *)(vmcb+GuestTables*EFI_PAGE_SIZE);
    FixedNpt *npt=(FixedNpt *)(vmcb+Pml4*EFI_PAGE_SIZE);
    for (UINTN level=0; level<NptTableCount; ++level) {
        UINTN gpa=GprRootGpa+level*EFI_PAGE_SIZE;
        npt->table[3][gpa/EFI_PAGE_SIZE]=(base+(GuestTables+level)*EFI_PAGE_SIZE)|7;
        if (level+1<NptTableCount) guest->table[level][0]=(gpa+EFI_PAGE_SIZE)|0x23; /* P/RW/A */
    }
    guest->table[3][FixedGuestGpa/EFI_PAGE_SIZE]=FixedGuestGpa|0x21; /* P/A, read only */
    guest->table[3][GprStackGpa/EFI_PAGE_SIZE]=GprStackGpa|0x63; /* P/RW/A/D */
    npt->table[3][GprStackGpa/EFI_PAGE_SIZE]=(base+GuestStack*EFI_PAGE_SIZE)|7;

    /* AMD APM vol. 2 §15.5.1: CS.L=1, CS.D=0 with LME, PAE, PE and PG. No
     * segment reloads or event delivery: these are cached flat descriptors. */
    for (UINTN seg=Es; seg<=Ds; seg+=16) {
        FIELD(UINT16,seg)=(seg==Cs)?8:16;
        FIELD(UINT16,seg+2)=(seg==Cs)?0xa9b:0xc93;
        FIELD(UINT32,seg+4)=0xffffffffu;
    }
    FIELD(UINT64,GuestEfer)=SVME|(1u<<8)|(1u<<10); /* LME, LMA */
    FIELD(UINT64,Cr4)=1u<<5; /* PAE */
    FIELD(UINT64,Cr3)=GprRootGpa;
    FIELD(UINT64,Cr0)=0x80010031u; /* PG/WP/NE/ET/PE */
    FIELD(UINT64,Rsp)=0x7ff0;
}

typedef struct {
    struct { UINT64 code, ip; } exits[GprEntries];
    UINTN count, bad_round, bad_reg;
    UINT64 actual, expected;
} GprTrace;

static BOOLEAN gpr_capture_ok(UINT8 *vmcb, const UINT64 frame[14], UINTN round, GprTrace *trace)
{
    /* Map conventional x86 register numbers to wrapper slots. The two VMCB
     * registers bypass this array. Compare all 64 bits, including CPUID inputs. */
    static const UINT8 slot[]={0,1,2,0,0,3,4,5,6,7,8,9,10,11,12,13};
    for (UINTN reg=0; reg<16; ++reg) {
        UINT64 actual, expected;
        if (reg==0) { actual=FIELD(UINT64,Rax); expected=0x400000fe; }
        else if (reg==4) { actual=FIELD(UINT64,Rsp); expected=round==1?0x7ff0:0x7fc0; }
        else {
            actual=frame[slot[reg]];
            expected=reg==1 ? round : 0xa50000005a000000ull + (round<<48) +
                                      (reg<<32) + (round<<8) + reg;
        }
        if (actual!=expected) {
            trace->bad_round=round; trace->bad_reg=reg;
            trace->actual=actual; trace->expected=expected;
            return FALSE;
        }
    }
    return TRUE;
}

static void gpr_run(UINT8 *vmcb, UINT64 frame[14], BOOLEAN nrips, GprTrace *trace)
{
    trace->count=trace->bad_round=0;
    for (UINTN i=0; i<GprEntries; ++i) {
        FIELD(UINT64,ExitCode)=~0ull;
        RunGprGuest((UINTN)vmcb,frame);
        trace->exits[i].code=FIELD(UINT64,ExitCode);
        trace->exits[i].ip=FIELD(UINT64,Rip);
        ++trace->count;
        if (i==2 || trace->exits[i].code!=0x72 || trace->exits[i].ip!=gpr_ip(gpr_sites[i])) break;
        UINTN round=i+1;
        if (!gpr_capture_ok(vmcb,frame,round,trace)) break;
        UINT64 resume_ip=nrips ? FIELD(UINT64,NextRip) : trace->exits[i].ip+2;
        if (resume_ip!=gpr_ip(gpr_after[i])) break;
        /* Emulate only this leaf. All other software-owned slots and RSP are
         * returned unmodified, not regenerated from the expected values. */
        FIELD(UINT64,Rax)=0xa1000000u+round;
        frame[0]=0xb2000000u+round;
        frame[1]=0xc3000000u+round;
        frame[2]=0xd4000000u+round;
        FIELD(UINT64,Rip)=resume_ip;
        FIELD(UINT8,TlbControl)=0;
    }
}

static void gpr_report(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *out, UINT8 *vmcb, const GprTrace *trace)
{
    static CHAR16 *const names[]={L"RAX",L"RCX",L"RDX",L"RBX",L"RSP",L"RBP",L"RSI",L"RDI",
                                  L"R8",L"R9",L"R10",L"R11",L"R12",L"R13",L"R14",L"R15"};
    hex(out,L"VMRUN count: ",trace->count);
    for (UINTN i=0; i<trace->count; ++i) {
        hex(out,L"  EXITCODE: ",trace->exits[i].code);
        hex(out,L"  Guest RIP: ",trace->exits[i].ip);
    }
    UINT64 code=FIELD(UINT64,ExitCode), ip=FIELD(UINT64,Rip), ax=FIELD(UINT64,Rax);
    hex(out,L"Final RAX: ",ax);
    if (trace->count==3 && code==0x78 && ip==gpr_ip(GprPassHlt) && ax==0x50415353) {
        out->OutputString(out,L"PASS: integer GPRs survived both CPUID resumes.\r\n");
        return;
    }
    UINTN round=trace->bad_round, reg;
    if (round) {
        out->OutputString(out,L"FAIL: captured CPUID input state.\r\n");
        reg=trace->bad_reg;
        hex(out,L"  Actual: ",trace->actual);
        hex(out,L"  Expected: ",trace->expected);
    } else if (code==0x78 && ip==gpr_ip(GprFailHlt) &&
               ((ax>>8)==1 || (ax>>8)==2) && (ax&255)<16) {
        out->OutputString(out,L"FAIL: guest detected wrong restored state.\r\n");
        round=(UINTN)(ax>>8); reg=(UINTN)(ax&255);
    } else {
        out->OutputString(out,L"FAIL: unexpected exit, RIP, resume target or marker.\r\n");
        hex(out,L"  nRIP: ",FIELD(UINT64,NextRip));
        hex(out,L"  EXITINFO1: ",FIELD(UINT64,Info1));
        hex(out,L"  EXITINFO2: ",FIELD(UINT64,Info2));
        return;
    }
    hex(out,L"  Round: ",round);
    out->OutputString(out,L"  Register: "); out->OutputString(out,names[reg]); out->OutputString(out,L"\r\n");
}
#endif
