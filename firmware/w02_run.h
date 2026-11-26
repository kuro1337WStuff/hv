#ifndef HV_W02_RUN_H
#define HV_W02_RUN_H
#include "w02_memory.h"
#include "w02_decode.h"
#include "w02_scalar.h"
#ifdef HV_W03
#include "w03_entry.h"
#endif
#include "w02_platform.h"

static const char *w02_run(UINT8 *vmcb, UINT64 *g, W01Evidence *e, W02State *s)
{
    if (!w01_fetch(vmcb,W01_SEC_GPA) || !w02_inside(W01_SEC_GPA,W02_SEC_TEXT_BASE,W02_SEC_TEXT_LEN) ||
        W02_SEC_TEXT_BASE<W01_CODE_GPA || (UINT64)W02_SEC_TEXT_BASE+W02_SEC_TEXT_LEN>W01_RESET_GPA)
        return "SEC CONTINUATION";
    w02_map(vmcb); w02_seal(vmcb,W02_SEC_TEXT_BASE,W02_SEC_TEXT_LEN);
    s->sec_pc=W01_SEC_GPA; s->phase=1; s->mxcsr=0x1f80; s->fcw=0x37f; s->apic[0]=0xff; s->apic[1]=0x10000;
    w02_progress("SEC FETCH",s->sec_pc,e->entries);
    FIELD(UINT32,Intercepts)|=(1u<<14); /* RDTSC */
    FIELD(UINT32,0)=0xffffffffu; /* CR0..15 reads/writes, never native */
    const char *error=NULL;
    /* Every native instruction retires under TF, with #DB intercepted.
     * Forms that can disable or defer single stepping never run natively. */
    while (s->ops<8000000 && e->entries<4000000) {
        UINT64 ip=FIELD(UINT64,Rip); UINT32 pc=(UINT32)ip, end=0; ++s->ops;
        if (!(s->ops&255) && __readmsr(0x40000020)-s->start>=440000000ull) { error="W02 TIME BUDGET"; break; }
        if (ip!=pc || FIELD(UINT64,Cs+8)!=0 || !(FIELD(UINT16,Cs+2)&0x200) ||
            FIELD(UINT8,0x4cb)!=0 || (FIELD(UINT64,Flags)&0x36000) ||
            (FIELD(UINT64,0x068)&1) || !w02_identity(vmcb,pc,FALSE,TRUE)) { error="GUEST EXEC STATE"; break; }
        e->before=pc; e->bytes=0;
#ifdef HV_W03
        if (s->phase==4 && w02_inside(pc,s->boot_base,W03_IMAGE_SIZE)) {
            error=w03_entry(vmcb,g,e,s,pc);
            break; /* never run another instruction, including on refusal */
        }
#endif
        const UINT8 *p=w02_back(vmcb,pc,15);
#ifndef HV_W03
        if (p) for (unsigned j=0;j<8;++j) e->bytes|=(UINT64)p[j]<<(8*j);
#endif
        error=w02_text(vmcb,s,pc,&end); if (error) break;
#ifdef HV_W03
        /* Log diagnostic octets only from authenticated open firmware. */
        if (p && end-pc>=8) for (unsigned j=0;j<8;++j) e->bytes|=(UINT64)p[j]<<(8*j);
#endif
        if (!p || (*w02_leaf(vmcb,pc)&(W01_NX|7))!=5) { error="UNSEALED TEXT"; break; }
        W02Insn d=w02_decode(p,end-pc);
        if (d.k==W02Bad) { error="UNREVIEWED OPCODE"; break; }
        if ((pc>>12)!=((pc+d.n-1)>>12) && (!w02_identity(vmcb,pc+d.n-1,FALSE,TRUE) ||
            (*w02_leaf(vmcb,pc+d.n-1)&(W01_NX|7))!=5)) { error="CROSS PAGE FETCH"; break; }
        if ((d.op==0x86 || d.op==0x87) && d.modrm<0xc0 && (w02_ea(vmcb,g,d)&7)) { error="UNALIGNED XCHG"; break; }
        if (d.k==W02Native || d.k==W02Io || d.k==W02Msr || d.k==W02Cpuid || d.k==W02Tsc) {
            FIELD(UINT64,Flags)|=0x100;
            w01_enter(vmcb,g,e);
            UINT64 x=FIELD(UINT64,ExitCode), rip=FIELD(UINT64,Rip);
            if (d.k==W02Native) {
                if (x==0x41 && (FIELD(UINT64,Dr6)&0xe00f)==0x4000 && (FIELD(UINT64,Flags)&0x100)) ++e->steps;
                else if (x==0x400 && rip==pc && w02_apic(vmcb,g,s,d)) FIELD(UINT64,Rip)=pc+d.n;
                else if (x==0x400 && rip!=pc && rip<=0xffffffffu && w02_identity(vmcb,rip,FALSE,TRUE) &&
                    (FIELD(UINT64,Info1)&~4ull)==((1ull<<32)|0x11) &&
                    (FIELD(UINT64,Info2)==rip || FIELD(UINT64,Info2)==(rip&~0xfffull))) {
                    /* Branch retired into NX private RAM: pin the target on
                     * the next dispatch; do not grant anything on EXITINFO2. */
                } else { error="SCALAR EXIT"; break; }
#ifdef HV_W03
                if (s->phase==4) {
                    error=w03_transfer(vmcb,g,s,d,pc,rip);
                    if (error) break;
                }
#endif
            } else {
                if (rip!=pc) { error="INTERCEPT PC"; break; }
                UINT32 phase=s->phase;
                error=d.k==W02Io?w02_io(vmcb,g,s,d,p):w02_sys(vmcb,g,s,d);
                if (error) break;
                if (s->phase!=phase) {
#ifdef HV_W03
                    w02_progress(s->phase==2?"PEI MEMORY":s->phase==3?"DXE OUT":"DXE READY",pc,e->entries);
#else
                    w02_progress(s->phase==2?"PEI MEMORY":"DXE OUT",pc,e->entries);
                    if (s->phase==3) break; /* actual stopped DXE IOIO, no resume */
#endif
                }
                FIELD(UINT64,Rip)=pc+d.n;
            }
        } else {
            error=w02_soft_step(vmcb,g,s,d); if (error) break;
        }
        if (!(s->ops&0x3ffff)) w02_progress(s->phase==2?"PEI MEMORY":s->fvs?"PINNED FV":"SEC RUN",pc,e->entries);
    }
#ifdef HV_W03
    return error?error:s->phase==5 && s->boot_steps==1?NULL:"W03 ENTRY BUDGET";
#else
    return error?error:s->phase==3?NULL:"W02 ENTRY BUDGET";
#endif
}
#endif
