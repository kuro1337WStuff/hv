#ifndef HV_W03_ENTRY_H
#define HV_W03_ENTRY_H
/* No firmware pointer is ever dereferenced as an L1 address. All checks
 * below operate on the stopped single guest, through its private backing. */
_Static_assert(W03FileGpa+W03FileCapacity==W02Permanent &&
               (UINT64)W02_PEI_FV_BASE+W02_PEI_FV_SIZE<=W03DescriptorGpa &&
               (UINT64)W02_DXE_FV_BASE+W02_DXE_FV_SIZE<=W03DescriptorGpa,"disjoint memory windows");
#include "w03_reloc.h"
static BOOLEAN w03_dxe_pc(UINT8 *vmcb, const W02State *s, UINT64 pc)
{
    UINT32 b=s->relocated[W02Peims];
    return b && w02_inside(pc,b+w02_dxe.rva,w02_dxe.len) &&
           w02_identity(vmcb,pc,FALSE,TRUE) && (*w02_leaf(vmcb,pc)&(W01_NX|7))==5;
}
static UINT8 *w03_object(UINT8 *vmcb, UINT64 a, UINTN n)
{
    return a>=W02Permanent && !(a&7) && w02_ram(a,n) ? w02_span(vmcb,a,n,FALSE):NULL;
}
static const char *w03_ready(UINT8 *vmcb, UINT64 *g, W02State *s, UINT32 pc, UINT32 ax)
{
    static const UINT8 window[]=W03_READY_OUT_WINDOW;
    _Static_assert(sizeof(window)==32,"ready window size");
    UINT32 dxe=s->relocated[W02Peims];
    if (s->phase!=3 || !s->fvs || !s->pei_pc || !s->dxe_pc || !dxe || ax!=0x33594452u ||
        g[8]!=ax || pc!=(UINT64)dxe+W03_READY_OUT_RVA || pc<24 ||
        !w03_dxe_pc(vmcb,s,pc-24) || !w03_dxe_pc(vmcb,s,pc+7)) return "READY ORIGIN";
    const UINT8 *p=w02_span(vmcb,pc-24,sizeof(window),FALSE);
    if (!p || !w03_equal(p,window,sizeof(window))) return "READY WINDOW";
    /* R8/R9/R11, bound by the real IOIO from the measured DXE site. */
    UINT64 base=g[6], handle=g[7], table=g[9];
    if (base<W02Permanent || (base&(W03_SECTION_ALIGN-1)) || !w02_ram(base,W03_IMAGE_SIZE)) return "LOADED EXTENT";
    for (UINT32 off=0;off<W03_IMAGE_SIZE;off+=W01Page)
        if ((*w02_leaf(vmcb,base+off)&(W01_NX|7))!=(W01_NX|7)) return "IMAGE PAGE ALIAS";
    const EFI_SYSTEM_TABLE *st=(const EFI_SYSTEM_TABLE *)w03_object(vmcb,table,sizeof(*st));
    if (!st || st->Hdr.Signature!=EFI_SYSTEM_TABLE_SIGNATURE || st->Hdr.HeaderSize!=sizeof(*st) ||
        !w03_object(vmcb,handle,sizeof(UINT64)) || w02_inside(table,(UINT32)base,W03_IMAGE_SIZE) ||
        w02_inside(handle,(UINT32)base,W03_IMAGE_SIZE)) return "EFI ARGUMENTS";
    const EFI_BOOT_SERVICES *bs=(const EFI_BOOT_SERVICES *)w03_object(vmcb,(UINTN)st->BootServices,sizeof(*bs));
    if (!bs || bs->Hdr.Signature!=EFI_BOOT_SERVICES_SIGNATURE || bs->Hdr.HeaderSize!=sizeof(*bs) ||
        !w03_dxe_pc(vmcb,s,(UINTN)bs->LoadImage) || !w03_dxe_pc(vmcb,s,(UINTN)bs->StartImage)) return "EFI SERVICES";
    s->ready_pc=pc; s->boot_base=(UINT32)base; s->boot_handle=(UINT32)handle;
    s->boot_table=(UINT32)table; s->start_service=(UINT32)(UINTN)bs->StartImage; s->phase=4;
    return NULL;
}
/* Independently bind the copied, relocated executable extent byte-for-byte
 * to the file admitted before reset. The offline pin contains only exact
 * DIR64 target RVAs from the authenticated relocation directory. */
static BOOLEAN w03_loaded_text(UINT8 *vmcb, UINT32 base)
{
    const UINT8 *file=w02_back(vmcb,W03FileGpa,W03_FILE_SIZE);
    const UINT8 *loaded=w02_span(vmcb,base+W03_TEXT_RVA,W03_TEXT_LEN,FALSE);
    return file && loaded && w03_text_matches(file,loaded,base);
}
/* Observe the actual transfer out of pinned DXE, never write guest RIP.
 * The CALL itself can stop on #DB or a denied target fetch; neither one
 * witnesses a retired instruction at the target. */
static const char *w03_transfer(UINT8 *vmcb, UINT64 *g, W02State *s, W02Insn d, UINT32 from, UINT64 to)
{
    UINT32 entry=s->boot_base+W03_ENTRY_RVA;
    if (to!=s->start_service && to!=entry) return NULL;
    if (!w03_dxe_pc(vmcb,s,from) || d.k!=W02Native || d.op16 || d.rep ||
        !(d.op==0xe8 || (d.op==0xff && ((d.modrm>>3)&7)==2))) return "DXE CALL ORIGIN";
    UINT64 rsp=FIELD(UINT64,Rsp);
    const UINT64 *ret=(const UINT64 *)w03_object(vmcb,rsp,40);
    if (!ret || (rsp&15)!=8 || *ret!=from+d.n || !w03_dxe_pc(vmcb,s,*ret) ||
        g[1]!=s->boot_handle) return "DXE CALL FRAME";
    if (to==s->start_service) {
        if (s->saw_start) return "REPEATED START";
        s->saw_start=TRUE;
    } else {
        if (!s->saw_start || g[2]!=s->boot_table || rsp<W02Permanent+40 ||
            !w02_span(vmcb,rsp-40,80,TRUE) || w02_inside(rsp-40,s->boot_base,W03_IMAGE_SIZE) ||
            w02_inside(rsp+39,s->boot_base,W03_IMAGE_SIZE)) return "ENTRY ABI";
        s->boot_rsp=(UINT32)rsp; s->boot_pc=entry;
    }
    return NULL;
}
static const char *w03_entry(UINT8 *vmcb, UINT64 *g, W01Evidence *e, W02State *s, UINT32 pc)
{
    if (pc!=s->boot_base+W03_ENTRY_RVA || s->boot_pc!=pc || !s->saw_start || s->boot_steps ||
        g[1]!=s->boot_handle || g[2]!=s->boot_table || FIELD(UINT64,Rsp)!=s->boot_rsp) return "ENTRY ORIGIN";
    if (!w03_loaded_text(vmcb,s->boot_base)) return "RELOCATED BOOT TEXT";
    /* Admit just the independently identified register-only SUB RSP, imm8
     * prologue. No control-transfer instruction or generic Windows decoder. */
    const UINT8 *p=w02_back(vmcb,pc,4);
    if (!p) return "ENTRY RANGE";
    if (!w03_entry_prologue(p)) return "ENTRY INSTRUCTION";
    UINT64 *leaf=w02_leaf(vmcb,pc);
    if ((*leaf&(W01_NX|7))!=(W01_NX|7)) return "ENTRY PAGE STATE";
    if (__readmsr(0x40000020)-s->start>=440000000ull) return "W03 TIME BUDGET";
    *leaf&=~(W01_NX|2ull); /* one page, one bounded VMRUN; no writable alias */
    FIELD(UINT64,Flags)|=0x100;
    w01_enter(vmcb,g,e);
    *leaf|=W01_NX; /* re-deny; no further guest entry */
    if (FIELD(UINT64,ExitCode)!=0x41 || (FIELD(UINT64,Dr6)&0xe00f)!=0x4000 ||
        !(FIELD(UINT64,Flags)&0x100) || FIELD(UINT64,Rip)!=pc+4 || FIELD(UINT64,Rsp)!=s->boot_rsp-40 ||
        g[1]!=s->boot_handle || g[2]!=s->boot_table) return "ENTRY NOT RETIRED";
    ++e->steps; s->boot_steps=1; s->boot_next=pc+4; s->phase=5;
    return NULL;
}
#endif
