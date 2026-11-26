#ifndef HV_W04_BOOT_H
#define HV_W04_BOOT_H
/* No target-selection interface. The only initial RIP is the one left by
 * w03_entry on this BSP. No guest instruction or call result is emulated. */
enum { W04StepLimit=4096 };
#ifdef HV_W04_RETURN
#include "w04_return_edge.h"
enum { W04HandleCallRva=0x31243, W04HandleReturnRva=0x31245, W04HandleDxeRva=0x5822 };
_Static_assert(offsetof(EFI_BOOT_SERVICES,HandleProtocol)==0x98,"pinned x64 EFI service ABI");
_Static_assert(W04HandleCallRva>=W03_TEXT_RVA && W04HandleReturnRva<W03_TEXT_RVA+W03_TEXT_LEN &&
               W04HandleDxeRva>=W02_DXE_TEXT_RVA && W04HandleDxeRva<W02_DXE_TEXT_RVA+W02_DXE_TEXT_LEN,
               "witness PCs must lie in the measured text extents");
#endif

static const EFI_BOOT_SERVICES *w04_services(UINT8 *vmcb, const W02State *s, UINT64 *address)
{
    const EFI_SYSTEM_TABLE *st=(const EFI_SYSTEM_TABLE *)w03_object(vmcb,s->boot_table,sizeof(*st));
    if (!st || st->Hdr.Signature!=EFI_SYSTEM_TABLE_SIGNATURE || st->Hdr.HeaderSize!=sizeof(*st)) return NULL;
    *address=(UINTN)st->BootServices;
    const EFI_BOOT_SERVICES *bs=(const EFI_BOOT_SERVICES *)w03_object(vmcb,*address,sizeof(*bs));
    if (!bs || bs->Hdr.Signature!=EFI_BOOT_SERVICES_SIGNATURE || bs->Hdr.HeaderSize!=sizeof(*bs) ||
        (UINTN)bs->StartImage!=s->start_service || !w03_dxe_pc(vmcb,s,(UINTN)bs->LoadImage)) return NULL;
    return bs;
}
static void w04_bind_services(UINT8 *vmcb, W02State *s)
{
    /* Sample at the real DXE ready exit, before any Windows code executes.
     * Keep only the private guest address and digest, never a guest pointer. */
    UINT64 address=0;
    const EFI_BOOT_SERVICES *bs=w04_services(vmcb,s,&address);
    if (bs) { hv_sha256((const UINT8 *)bs,sizeof(*bs),s->w04_bs_digest); s->w04_bs=(UINT32)address; }
}

static BOOLEAN w04_ram_leaf(UINT8 *vmcb, UINT32 page, UINT64 permissions)
{
    UINT64 leaf=*w02_leaf(vmcb,page);
    return (leaf&0xffffffffff000ull)==(UINTN)w02_back(vmcb,page,1) && (leaf&(W01_NX|7))==permissions;
}
static const char *w04_seal(UINT8 *vmcb, W02State *s, UINT32 text, UINT32 end)
{
    /* The source was hashed before reset and has never had a writable NPT
     * alias. Recheck backing and permissions; the loaded copy must still
     * match the pin modulo precisely W03's reviewed DIR64 relocations. */
    for (UINT32 q=W03FileGpa;q<W03FileGpa+W03_FILE_SIZE;q+=W01Page)
        if (!w04_ram_leaf(vmcb,q,W01_NX|5)) return "W04 ORIGINAL PAGE";
    if (!w03_loaded_text(vmcb,s->boot_base)) return "W04 RELOCATED TEXT";
    UINT32 entry_page=s->boot_pc&~0xfffu;
    for (UINT32 q=text;q<end;q+=W01Page)
        if (!w04_ram_leaf(vmcb,q,W01_NX|(q==entry_page?5u:7u))) return "W04 LOADED PAGE";
    /* All initialized pages are now authenticated. Seal the entire extent,
     * so a real branch between its pages does not incur an artificial NPF.
     * Only TF-safe instructions may run; w01_enter flushes the guest ASID. */
    w02_seal(vmcb,text,end-text);
    return NULL;
}
static BOOLEAN w04_one_page(UINT64 a, UINTN size)
{
    return (a&(W01Page-1))<=W01Page-size;
}
#ifdef HV_W04_RETURN
/* Do not discover or follow a new stack. Admit only aligned words on the
 * same writable, private NX page as W03's observed Windows entry frame. */
static BOOLEAN w04_stack_page(UINT8 *vmcb, const W02State *s)
{
    UINT32 page=s->boot_rsp&~0xfffu;
    return page>=W02Permanent && w02_ram(page,W01Page) && !w02_inside(page,s->boot_base,W03_IMAGE_SIZE) &&
           w02_identity(vmcb,page,TRUE,FALSE) && w04_ram_leaf(vmcb,page,W01_NX|7);
}
static BOOLEAN w04_stack_word(const W02State *s, UINT64 a)
{
    return !(a&7) && a<=s->boot_rsp && (a>>12)==(s->boot_rsp>>12);
}
#endif
static const char *w04_operands(UINT8 *vmcb, UINT64 *g, W02Insn d, const W02State *s, UINT64 frame)
{
    /* Consume only the existing W02 decoder's native scalar forms. Reject
     * exotic prefixes and bit-string memory indexing. Do not resolve data
     * faults, decode new instructions, or touch a guest operand. For an
     * explicit scalar operand, conservatively bound its maximum eight bytes. */
    if (d.k!=W02Native || d.rep || d.op16 || d.a32) return "W04 UNSUPPORTED INSN";
    UINT8 op=d.op, sub=(d.modrm>>3)&7;
    BOOLEAN mem=FALSE;
    if (op==0x0f) {
        UINT8 op2=d.op2;
        if ((op2>=0x80 && op2<=0x8f) || (op2>=0xc8 && op2<=0xcf) || op2==0x1f) mem=FALSE;
        else if (d.modrm<0xc0 && (op2==0xa3 || op2==0xab || op2==0xb3 || op2==0xbb || op2==0xba))
            return "W04 UNSUPPORTED INSN";
        else mem=d.modrm<0xc0;
    } else if ((op<0x40 && (op&7)<=3) || op==0x63 || op==0x69 || op==0x6b ||
               (op>=0x80 && op<=0x8c) || op==0xc0 || op==0xc1 || op==0xc6 || op==0xc7 ||
               (op>=0xd0 && op<=0xd3) || op==0xf6 || op==0xf7 || op==0xfe || op==0xff) mem=d.modrm<0xc0;
    if (mem) {
        UINT64 ea=w02_ea(vmcb,g,d);
        if (!w04_one_page(ea,8)) return "W04 DATA PAGE EDGE";
#ifdef HV_W04_RETURN
        /* The fixed witness does not need aliases, MMIO or wide addresses. */
        if (!w02_ram(ea,8) || !w02_identity(vmcb,ea,FALSE,FALSE) ||
            (*w02_leaf(vmcb,ea)&0xffffffffff001ull)!=((UINTN)w02_back(vmcb,ea&~0xfffull,1)|1ull))
            return "W04 UNOWNED OPERAND";
#endif
        if ((op==0x86 || op==0x87) && (ea&7)) return "W04 UNALIGNED XCHG";
    }
    UINT64 rsp=FIELD(UINT64,Rsp);
    if ((op>=0x50 && op<=0x57) || op==0x68 || op==0x6a || op==0xe8 || (op==0xff && (sub==2 || sub==6))) rsp-=8;
    else if (op==0xc9) rsp=w02_reg(vmcb,g,5);
    else if (op!=0xc3 && !(op>=0x58 && op<=0x5f)) return NULL;
#ifdef HV_W04_RETURN
    if (!w04_stack_word(s,rsp) || (frame && (rsp>frame || (rsp==frame && op!=0xc3))))
        return "W04 UNOWNED STACK";
#else
    (void)s; (void)frame;
#endif
    return w04_one_page(rsp,8)?NULL:"W04 STACK PAGE EDGE";
}

static UINT64 w04_slot(UINT8 *vmcb, W02State *s, UINT64 operand)
{
    if (operand<s->w04_bs) return 0;
    UINT64 off=operand-s->w04_bs;
    if ((off&7) || off<sizeof(EFI_TABLE_HEADER) || off>sizeof(EFI_BOOT_SERVICES)-8 ||
        off==offsetof(EFI_BOOT_SERVICES,Reserved)) return 0;
    UINT64 table=0;
    const EFI_BOOT_SERVICES *bs=w04_services(vmcb,s,&table);
    if (!bs || table!=s->w04_bs || !w01_digest((const UINT8 *)bs,sizeof(*bs),s->w04_bs_digest)) return 0;
    return w03_le64((const UINT8 *)bs+(UINTN)off);
}
#ifndef HV_W04_RETURN
static const char *w04_transfer(UINT8 *vmcb, W02State *s, W02Insn d, UINT64 operand, UINT64 expected, UINT64 old_rsp)
{
    /* A jump, return, direct call or register call can land on a service
     * address without ever reading its slot. Never infer a slot from RIP.
     * expected was read from the authenticated identity mapping BEFORE the
     * CPU read the operand and pushed its return address. */
    if (!operand || !w03_dxe_pc(vmcb,s,s->w04_target)) return "W04 UNCLASSIFIED TRANSFER";
    if (!expected || expected!=s->w04_target) return "W04 UNVERIFIED SERVICE SLOT";
    UINT64 rsp=FIELD(UINT64,Rsp), ret=(UINT64)s->w04_pc+d.n;
    const UINT8 *frame=w03_object(vmcb,rsp,40);
    if (!frame || !w04_one_page(rsp,40) || (rsp&15)!=8 || rsp+8!=old_rsp || old_rsp>s->boot_rsp ||
        w02_inside(rsp,s->boot_base,W03_IMAGE_SIZE) || w02_inside(rsp+39,s->boot_base,W03_IMAGE_SIZE) ||
        !w02_inside(ret,s->boot_base+W03_TEXT_RVA,W03_TEXT_LEN) || w03_le64(frame)!=ret) return "W04 UNVERIFIED CALL FRAME";
    s->w04_service=(UINT32)(operand-s->w04_bs);
    return "W04 VERIFIED EFI CALL"; /* stopped before the service, not kernel entry */
}
#else
/* Check BEFORE CALL; matching these values does not imply that the
 * Windows instruction itself read the slot. No other call site is armed. */
static const char *w04_handle_call(UINT8 *vmcb, W02State *s, const UINT8 *p, W02Insn d, UINT64 pre_rsp)
{
    if (d.n!=2 || p[0]!=0xff || p[1]!=0xd0) return "W04 HANDLE CALL BYTES";
    UINT64 target=(UINT64)s->relocated[W02Peims]+W04HandleDxeRva;
    if (!w03_dxe_pc(vmcb,s,target) || !w04_ram_leaf(vmcb,(UINT32)target&~0xfffu,5) ||
        FIELD(UINT64,Rax)!=target ||
        w04_slot(vmcb,s,(UINT64)s->w04_bs+offsetof(EFI_BOOT_SERVICES,HandleProtocol))!=target)
        return "W04 HANDLE SLOT MISMATCH";
    if (!w04_stack_word(s,pre_rsp-8) || (pre_rsp&15) || pre_rsp+32>s->boot_rsp ||
        !w04_one_page(pre_rsp-8,40)) return "W04 HANDLE CALL STACK";
    return NULL;
}
#endif

static const char *w04_continue(UINT8 *vmcb, UINT64 *g, W01Evidence *e, W02State *s, UINT32 text, UINT32 end)
{
#ifdef HV_W04_RETURN
    const UINT64 call_pc=(UINT64)s->boot_base+W04HandleCallRva, return_pc=(UINT64)s->boot_base+W04HandleReturnRva;
    const UINT64 entry=(UINT64)s->relocated[W02Peims]+W04HandleDxeRva;
    UINT64 frame=0; /* nonzero only after the observed CALL and checked push */
#endif
    for (;;) {
        if (s->w04_steps>=W04StepLimit || e->entries>=4000000 || s->ops>=8000000) return "W04 STEP BUDGET";
        if (__readmsr(0x40000020)-s->start>=440000000ull) return "W04 TIME BUDGET";
        UINT64 ip=FIELD(UINT64,Rip);
        if (!w02_inside(ip,text,end-text)) return "W04 UNCLASSIFIED TRANSFER";
        UINT32 pc=(UINT32)ip, page=pc&~0xfffu; e->before=s->w04_pc=pc; s->w04_target=0;
        if (FIELD(UINT64,Cs+8) || (FIELD(UINT16,Cs+2)&0x600)!=0x200 || FIELD(UINT8,0x4cb) ||
            (FIELD(UINT64,Flags)&0x36100)!=0x100 || (FIELD(UINT64,0x068)&1) ||
            FIELD(UINT32,Exceptions)!=0xffffffffu || FIELD(UINT64,NpEnable)!=1 ||
            FIELD(UINT64,NestedCr3)!=(UINTN)w01_page(vmcb,Pml4) || !w02_identity(vmcb,pc,FALSE,TRUE) ||
            !w04_ram_leaf(vmcb,page,5)) return "W04 EXEC INVARIANT";
        const UINT8 *p=w02_back(vmcb,pc,15); /* decoder requires 15 bytes of owned backing */
        if (!p) return "W04 DECODE EXTENT";
        W02Insn d=w02_decode(p,end-pc);
        if (!d.n || d.n>end-pc || !w04_one_page(pc,d.n)) return "W04 FETCH EXTENT";
        UINT64 old_rsp=FIELD(UINT64,Rsp);
#ifdef HV_W04_RETURN
        UINT64 stacked=0;
        if (!w04_stack_word(s,old_rsp) || !w04_stack_page(vmcb,s)) return "W04 UNOWNED RSP";
        if (frame) {
            if (old_rsp>frame) return "W04 SERVICE STACK";
            stacked=w03_le64(w02_back(vmcb,frame,8));
            if (stacked!=return_pc) return "W04 CHANGED RETURN SLOT";
        } else if (pc==return_pc) return "W04 PREMATURE RETURN PC";
        const char *error=w04_operands(vmcb,g,d,s,frame);
        if (!error && pc==call_pc) error=w04_handle_call(vmcb,s,p,d,old_rsp);
#else
        const char *error=w04_operands(vmcb,g,d,s,0);
#endif
        if (error) return error;
#ifndef HV_W04_RETURN
        UINT64 operand=(d.op==0xff && ((d.modrm>>3)&7)==2 && d.modrm<0xc0)?w02_ea(vmcb,g,d):0;
        UINT64 expected=operand?w04_slot(vmcb,s,operand):0;
#endif
        ++s->ops;
        w01_enter(vmcb,g,e);
        if (FIELD(UINT64,ExitCode)!=0x41 || (FIELD(UINT64,Dr6)&0xe00f)!=0x4000 ||
            !(FIELD(UINT64,Flags)&0x100)) return "W04 UNSUPPORTED EXIT";
        ++e->steps; ++s->w04_steps;
        UINT64 next=FIELD(UINT64,Rip);
        if (next>0xffffffffu) return "W04 WIDE TRANSFER";
        s->w04_target=(UINT32)next;
#ifdef HV_W04_RETURN
        UINT64 rsp=FIELD(UINT64,Rsp);
        if (frame) {
            if (next==return_pc) {
                if (!w04_handle_return_edge(frame,return_pc,old_rsp,stacked,d.n==1 && p[0]==0xc3,next,rsp) ||
                    !w02_identity(vmcb,next,FALSE,TRUE) || !w04_ram_leaf(vmcb,(UINT32)next&~0xfffu,5))
                    return "W04 UNVERIFIED RETURN";
                if (__readmsr(0x40000020)-s->start>=440000000ull) return "W04 TIME BUDGET";
                s->w04_efi_status=FIELD(UINT64,Rax); s->w04_returned=TRUE;
                if (s->w04_efi_status>>63) return "W04 HANDLE RETURNED WITH ERROR";
                return s->w04_efi_status?"W04 HANDLE RETURNED WARNING":"W04 HANDLE RETURNED SUCCESS";
            }
            if (next==entry || !w02_inside(next,text,end-text)) return "W04 SERVICE ESCAPE";
            if (rsp>frame || !w04_stack_word(s,rsp)) return "W04 SERVICE STACK";
        } else if (pc==call_pc) {
            if (next!=entry) return "W04 HANDLE TARGET MISMATCH";
            if (rsp!=old_rsp-8 || !w04_stack_page(vmcb,s)) return "W04 HANDLE FRAME MISMATCH";
            UINT64 pushed=w03_le64(w02_back(vmcb,rsp,8));
            if (pushed!=return_pc) return "W04 HANDLE FRAME MISMATCH";
            frame=rsp; s->w04_service=offsetof(EFI_BOOT_SERVICES,HandleProtocol);
            s->w04_pre_rsp=(UINT32)old_rsp; s->w04_callee=(UINT32)next; s->w04_pushed_pc=(UINT32)pushed;
            /* Only change the range this loop accepts. DXE text was already
             * measured and sealed by W02; never grant a new RX permission. */
            text=s->relocated[W02Peims]+w02_dxe.rva; end=text+w02_dxe.len;
        } else if (!w02_inside(next,text,end-text)) return "W04 UNCLASSIFIED TRANSFER";
#else
        if (!w02_inside(next,text,W03_TEXT_LEN)) return w04_transfer(vmcb,s,d,operand,expected,old_rsp);
#endif
    }
}
static const char *w04_boot(UINT8 *vmcb, UINT64 *g, W01Evidence *e, W02State *s)
{
    e->bytes=0; /* Never report a proprietary instruction or data operand. */
    if (s->phase!=5 || s->boot_steps!=1 || !s->saw_start || !s->w04_bs ||
        !w02_ram(s->boot_base,W03_IMAGE_SIZE) || s->boot_pc!=s->boot_base+W03_ENTRY_RVA ||
        s->boot_next!=s->boot_pc+4 || FIELD(UINT64,Rip)!=s->boot_next) return "W04 ENTRY INVARIANT";
    UINT32 text=s->boot_base+W03_TEXT_RVA, end=text+W03_TEXT_LEN;
    const char *error=w04_seal(vmcb,s,text,end); if (error) return error;
    error=w04_continue(vmcb,g,e,s,text,end);
    for (UINT32 q=text;q<end;q+=W01Page) *w02_leaf(vmcb,q)|=W01_NX;
    return error;
}
#endif
