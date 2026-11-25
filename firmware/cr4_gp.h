/** @file cr4_gp.h
 * Deliver a general-protection fault for one intercepted write in a fixed L2.
 */
#ifndef HV_CR4_GP_H
#define HV_CR4_GP_H
extern void RunGprGuest(UINTN vmcb_pa, UINT64 frame[14]);
extern volatile unsigned linux_vmrun_count;
extern const UINT8 GpStart[], GpWrite[], GpContinue[], GpHlt[], GpHandler[], GpEnd[];

typedef struct { UINT64 before, after, count, error, ip, sp, cookie; } GpWitness;
static UINT64 gp_ip(const UINT8 *label) { return FixedGuestGpa+((UINTN)label-(UINTN)GpStart); }

/** @param[in,out] vmcb Aligned private BSP arena with an installed host-save page.
 * @return NULL for both terminal controls or a static diagnostic; no unknown exit resumes.
 * AMD APM volume 2, event injection and appendix B: exception vector 13 requires
 * EV=1 and an error code of zero. Preserve the faulting RIP on injection; the L2
 * handler checks the hardware frame, removes the error slot and executes IRETQ.
 */
static const char *cr4_gp_run(UINT8 *vmcb)
{
    UINTN size=(UINTN)GpEnd-(UINTN)GpStart;
    if (size>EFI_PAGE_SIZE || gp_ip(GpContinue)!=gp_ip(GpWrite)+3 ||
        GpWrite[0]!=0x0f || GpWrite[1]!=0x22 || GpWrite[2]!=0xe0 || *GpHlt!=0xf4 ||
        gp_ip(GpHandler)>=FixedGuestGpa+size) return "GP FIXTURE LAYOUT";
    const UINT64 caller_cr4=__readcr4();
    for (unsigned inject=0;inject<=1;++inject) {
        UINT64 frame[14];
        __stosb((UINT8 *)frame,0,sizeof(frame));
        frame[1]=LongStackGpa; frame[2]=inject;
        pkg_reset(vmcb);
        long_guest_state(vmcb,(UINTN)vmcb);
        long_guest_gate(vmcb,(UINTN)vmcb,13,gp_ip(GpHandler));
        FIELD(UINT16,2)=1u<<4;
        __movsb(vmcb+Code*EFI_PAGE_SIZE,GpStart,size);
        volatile GpWitness *w=(volatile GpWitness *)(vmcb+GuestStack*EFI_PAGE_SIZE);
        w->after=w->error=~0ull;
        unsigned before=linux_vmrun_count;
        RunGprGuest((UINTN)vmcb,frame);
        if (inject) {
            pkg_observe("WR EXIT ",FIELD(UINT64,ExitCode)); pkg_observe("WR RIP ",FIELD(UINT64,Rip));
            if (FIELD(UINT64,ExitCode)!=0x14 || FIELD(UINT64,Rip)!=gp_ip(GpWrite) ||
                FIELD(UINT64,Rax)!=0x40020 || FIELD(UINT64,Cr4)!=0x20 || __readcr4()!=caller_cr4 ||
                FIELD(UINT64,Rsp)!=LongInitialRsp || w->before!=0x20 || w->after!=~0ull ||
                w->count || w->cookie || linux_vmrun_count-before!=1) return "GP PRE EFFECT";
            FIELD(UINT64,0xa8)=0x80000b0du; /* VALID | EV | exception type | vector */
            FIELD(UINT8,TlbControl)=0;
            RunGprGuest((UINTN)vmcb,frame);
        }
        pkg_observe(inject ? "GP EXIT " : "RD EXIT ",FIELD(UINT64,ExitCode));
        pkg_observe(inject ? "GP RIP " : "RD RIP ",FIELD(UINT64,Rip));
        pkg_observe(inject ? "GP COUNT " : "RD COUNT ",w->count);
        pkg_observe(inject ? "GP ERROR " : "RD ERROR ",w->error);
        if (inject) { pkg_observe("STACK IP ",w->ip); pkg_observe("STACK SP ",w->sp); }
        pkg_observe(inject ? "GP CR4 " : "RD CR4 ",w->after);
        pkg_observe(inject ? "GP COOKIE " : "RD COOKIE ",w->cookie);
        pkg_observe(inject ? "GP ENTRIES " : "RD ENTRIES ",linux_vmrun_count-before);
        if (w->count!=inject || w->error!=(inject ? 0 : ~0ull) || w->ip!=(inject ? gp_ip(GpWrite) : 0) ||
            w->sp!=(inject ? LongInitialRsp-48u : 0) || w->cookie!=0x47505254u ||
            w->before!=0x20 || w->after!=0x20 || FIELD(UINT64,Cr4)!=0x20 || __readcr4()!=caller_cr4 ||
            FIELD(UINT64,ExitCode)!=0x78 || FIELD(UINT64,Rip)!=gp_ip(GpHlt) ||
            FIELD(UINT64,Rsp)!=LongInitialRsp || linux_vmrun_count-before!=1+inject) return "GP RETURN WITNESS";
    }
    return NULL;
}
#endif
