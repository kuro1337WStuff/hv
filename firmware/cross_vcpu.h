/* The fixed real-mode read/HLT guest and its shared NPT are prepared by l2_smoke.c.
 * Dispatch is blocking: neither guest is running while the BSP replaces the leaf. */
#include <Pi/PiMultiPhase.h>
#include <Protocol/MpService.h>

typedef struct {
    EFI_MP_SERVICES_PROTOCOL *mp;
    UINT8 *vmcb;
    UINT64 save_pa;
    UINTN expected_cpu, actual_cpu;
    UINT64 marker, exit, ip, ax;
    BOOLEAN ack;
} CrossRead;

static void EFIAPI cross_read(VOID *arg)
{
    CrossRead *r=(CrossRead *)arg;
    UINT8 *vmcb=r->vmcb;
    r->ack=FALSE;
    r->actual_cpu=(UINTN)-1;
    r->exit=~0ull; r->ip=0; r->ax=0;
    if (EFI_ERROR(r->mp->WhoAmI(r->mp,&r->actual_cpu)) || r->actual_cpu!=r->expected_cpu) return;
    int features[4];
    __cpuid(features,(int)0x80000001u);
    if (!(features[2]&4)) return;
    __cpuid(features,(int)0x8000000au);
    if (!(features[3]&1) || (UINT32)features[1]<2 || (__readcr4() & (1u<<12))) return;
    UINT64 efer=__readmsr(MSR_EFER);
    if ((efer&SVME) || (__readmsr(MSR_VM_CR)&(1u<<4))) return;
    FIELD(UINT64,Pat)=__readmsr(0x277);
    FIELD(UINT64,Rip)=FixedGuestGpa; FIELD(UINT64,Rax)=0; FIELD(UINT64,ExitCode)=~0ull;
    FIELD(UINT8,TlbControl)=1; /* full local flush at entry on THIS logical processor */
    UINT64 flags=__readeflags();
    __writeeflags(flags & ~(1ull<<9));
    UINT64 cr2=__readcr2(), dr6=__readdr(6), dr7=__readdr(7);
    __writemsr(MSR_EFER,efer|SVME);
    UINT64 hsave=__readmsr(MSR_HSAVE);
    __svm_clgi();
    __writemsr(MSR_HSAVE,r->save_pa);
    __svm_vmrun((UINTN)vmcb);
    __writecr2(cr2); __writedr(6,dr6); __writedr(7,dr7);
    __writemsr(MSR_HSAVE,hsave);
    __svm_stgi(); __writemsr(MSR_EFER,efer);
    if (flags & (1u<<9)) __writeeflags(__readeflags() | (1ull<<9));
    r->exit=FIELD(UINT64,ExitCode); r->ip=FIELD(UINT64,Rip); r->ax=FIELD(UINT64,Rax);
    r->ack=r->exit==0x78 && r->ip==FixedGuestGpa+3 && r->ax==r->marker;
}

static void cross_vcpu(EFI_SYSTEM_TABLE *st, UINT64 base, volatile UINT64 *leaf)
{
    EFI_BOOT_SERVICES *bs=st->BootServices;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *out=st->ConOut;
    EFI_GUID guid=EFI_MP_SERVICES_PROTOCOL_GUID;
    EFI_MP_SERVICES_PROTOCOL *mp;
    EFI_STATUS status=bs->LocateProtocol(&guid,NULL,(VOID **)&mp);
    if (EFI_ERROR(status)) {
        hex(out,L"LocateProtocol status: ",status);
        out->OutputString(out,L"SKIP: UEFI MP Services unavailable; no remote guest entry.\r\n"); return;
    }
    UINTN count, enabled, bsp, ap;
    if (EFI_ERROR(mp->GetNumberOfProcessors(mp,&count,&enabled)) || EFI_ERROR(mp->WhoAmI(mp,&bsp))) {
        out->OutputString(out,L"FAIL: cannot enumerate firmware processors.\r\n"); return;
    }
    hex(out,L"Enabled firmware processors: ",enabled);
    EFI_PROCESSOR_INFORMATION pi;
    for (ap=0; ap<count; ++ap) {
        if (ap!=bsp && !EFI_ERROR(mp->GetProcessorInfo(mp,ap,&pi)) &&
            (pi.StatusFlag & 7)==(PROCESSOR_ENABLED_BIT|PROCESSOR_HEALTH_STATUS_BIT)) break;
    }
    if (enabled<2 || ap==count) { out->OutputString(out,L"SKIP: no enabled healthy AP.\r\n"); return; }
    CrossRead cpu[2];
    cpu[0].expected_cpu=bsp; cpu[1].expected_cpu=ap;
    cpu[0].mp=cpu[1].mp=mp;
    cpu[0].vmcb=(UINT8 *)(UINTN)base; cpu[1].vmcb=cpu[0].vmcb+ApVmcb*EFI_PAGE_SIZE;
    cpu[0].save_pa=base+HostSave*EFI_PAGE_SIZE; cpu[1].save_pa=base+ApSave*EFI_PAGE_SIZE;
    bs->CopyMem(cpu[1].vmcb,cpu[0].vmcb,EFI_PAGE_SIZE); /* private VMCBs, identical N_CR3 */
    for (UINTN epoch=0; epoch<2; ++epoch) {
        out->OutputString(out,epoch ? L"Remap B, require both acknowledgements:\r\n" : L"Prime A on both processors:\r\n");
        _InterlockedExchange64((volatile __int64 *)leaf,(__int64)((base+(epoch?FrameB:FrameA)*EFI_PAGE_SIZE)|5));
        for (UINTN i=0; i<2; ++i) {
            CrossRead *r=&cpu[i]; r->ack=FALSE; r->marker=epoch ? MarkerB : MarkerA;
            if (i) {
                /* No firmware timeout may terminate a callback while it owns SVM MSRs. */
                status=mp->StartupThisAP(mp,cross_read,ap,NULL,0,r,NULL);
                if (EFI_ERROR(status)) { hex(out,L"FAIL: AP dispatch status: ",status); return; }
            } else cross_read(r);
            hex(out,L"  Processor: ",r->actual_cpu); hex(out,L"  Guest RAX: ",r->ax);
            if (!r->ack) {
                hex(out,L"  EXITCODE: ",r->exit); hex(out,L"  RIP: ",r->ip);
                out->OutputString(out,L"FAIL: missing processor/HLT/marker acknowledgement.\r\n"); return;
            }
        }
    }
    out->OutputString(out,L"PASS: both processors read A then B after local flushes.\r\n");
}
