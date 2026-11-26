#ifndef HV_W02_SCALAR_H
#define HV_W02_SCALAR_H
static const char *w02_string(UINT8 *vmcb, UINT64 *g, W02Insn d)
{
    BOOLEAN mov=d.op==0xa4 || d.op==0xa5, rev=(FIELD(UINT64,Flags)&0x400)!=0;
    if (d.a32) return "STRING ADDRSIZE";
    UINTN unit=!(d.op&1)?1:(d.rex&8)?8:d.op16?2:4;
    UINT64 count=d.rep?g[1]:1;
    if (count>W02RamBytes/unit) return "STRING COUNT";
    /* No host DF changes. Revalidate at least once per page and update
     * architectural registers after each completed chunk. Under replacement
     * page tables take one element at a time; never use a cached arbitrary
     * guest translation to write through a page-table change. */
    UINT64 scalar=FIELD(UINT64,Rax);
    for (unsigned chunks=0;count && chunks<64;++chunks) {
        UINTN n=1;
        if (FIELD(UINT64,Cr3)==W01RamGpa && !(g[5]&(unit-1)) && (!mov || !(g[4]&(unit-1)))) {
            UINTN dl=rev?(g[5]&4095)+unit:4096-(g[5]&4095);
            UINTN sl=mov?(rev?(g[4]&4095)+unit:4096-(g[4]&4095)):dl;
            n=(dl<sl?dl:sl)/unit; if (n>count) n=(UINTN)count;
            if (!w01_identity(vmcb,g[5]) || (mov && !w01_identity(vmcb,g[4])) ||
                (g[5]>=W01RamGpa && g[5]<W01RamGpa+6*W01Page)) n=1;
        }
        UINTN bytes=n*unit, back=rev?bytes-unit:0;
        UINT8 *dst=w02_span(vmcb,g[5]-back,bytes,TRUE), *src=mov?w02_span(vmcb,g[4]-back,bytes,FALSE):NULL;
        if (!dst || (mov && !src)) return "STRING RANGE";
        if (!rev && (!mov || g[4]+bytes<=g[5] || g[5]+bytes<=g[4])) {
            if (mov) __movsb(dst,src,bytes);
            else if (unit==8) __stosq((UINT64 *)dst,scalar,n);
            else if (unit==4) __stosd((unsigned long *)dst,(UINT32)scalar,n);
            else if (unit==2) __stosw((UINT16 *)dst,(UINT16)scalar,n);
            else __stosb(dst,(UINT8)scalar,n);
        } else {
            for (UINTN j=0;j<n;++j) {
                UINTN off=(rev?n-1-j:j)*unit;
                const UINT8 *from=mov?src+off:(const UINT8 *)&scalar;
                UINT64 v=0; __movsb((UINT8 *)&v,from,unit); __movsb(dst+off,(UINT8 *)&v,unit);
            }
        }
        UINT64 delta=rev?(0ull-bytes):bytes;
        g[5]+=delta; if (mov) g[4]+=delta; count-=n; if (d.rep) g[1]=count;
    }
    return NULL;
}
static const char *w02_soft(UINT8 *vmcb, UINT64 *g, W02State *s, W02Insn d)
{
    UINT64 flags=FIELD(UINT64,Flags)&~0x100ull, rsp=FIELD(UINT64,Rsp), v; UINT8 *p;
    unsigned sub=(d.modrm>>3)&7;
    switch (d.k) {
    case W02String: return w02_string(vmcb,g,d);
    case W02JumpXmm: {
        /* SetJump/LongJump use only MOVDQU memory forms for XMM6..15.
         * Never execute SSE or copy the caller's real floating-point bank. */
        unsigned r=sub+((d.rex&4)?8:0); BOOLEAN store=d.op2==0x7f;
        if (d.modrm>=0xc0 || r<6 || (FIELD(UINT64,Cr0)&0xc) || !(FIELD(UINT64,Cr4)&0x200))
            return "JUMP XMM FORM";
        p=w02_span(vmcb,w02_ea(vmcb,g,d),16,store);
        if (!p) return "JUMP XMM RANGE";
        if (store) __movsb(p,s->jump_xmm[r-6],16);
        else __movsb(s->jump_xmm[r-6],p,16);
        return NULL;
    }
    case W02Flags:
        if (d.op==0xfa || d.op==0xfb) {
            if (d.op==0xfb) flags|=0x200; else flags&=~0x200ull;
            FIELD(UINT64,Flags)=flags; /* no injected interrupts on this BSP */
        } else if (d.op==0x9c) {
            p=w02_span(vmcb,rsp-8,8,TRUE); if (!p) return "PUSHF RANGE";
            *(UINT64 *)p=flags&~0x10000ull; FIELD(UINT64,Rsp)=rsp-8;
        } else {
            p=w02_span(vmcb,rsp,8,FALSE); if (!p) return "POPF RANGE";
            v=*(UINT64 *)p;
            if ((v&~0x00200ed7ull) || !(v&2)) return "POPF BITS";
            FIELD(UINT64,Flags)=v; FIELD(UINT64,Rsp)=rsp+8;
        }
        return NULL;
    case W02Cr: {
        unsigned cr=sub+(d.rex&4?8:0), reg=(d.modrm&7)+(d.rex&1?8:0);
        UINTN field=cr==0?Cr0:cr==3?Cr3:cr==4?Cr4:0;
        if (!field) return "CONTROL REGISTER";
        v=w02_reg(vmcb,g,reg);
        if (d.op2==0x20) w02_set(vmcb,g,reg,FIELD(UINT64,field));
        else {
            if ((cr==0 && ((v^FIELD(UINT64,Cr0))&~0x10000ull)) ||
                (cr==3 && ((v&4095) || !w02_ram(v,4096))) ||
                (cr==4 && ((v^FIELD(UINT64,Cr4))&~0x780ull))) return "CONTROL VALUE";
            FIELD(UINT64,field)=v;
        }
        return NULL;
    }
    case W02Desc: {
        UINTN off=(sub&1)?0x480:0x460;
        p=w02_span(vmcb,w02_ea(vmcb,g,d),10,sub<2); if (!p) return "DT OPERAND";
        if (sub<2) { *(UINT16 *)p=(UINT16)FIELD(UINT32,off+4); *(UINT64 *)(p+2)=FIELD(UINT64,off+8); }
        else {
            UINT16 limit=*(const UINT16 *)p; v=*(const UINT64 *)(p+2);
            if (!w02_span(vmcb,v,(UINTN)limit+1,FALSE)) return "DT RANGE";
            FIELD(UINT32,off+4)=limit; FIELD(UINT64,off+8)=v;
        }
        return NULL;
    }
    case W02Fp:
        /* The private virtual x87 environment starts empty with no pending
         * exceptions. No arithmetic, SIMD data or FP instructions run on L1. */
        if (d.op==0x9b) return NULL;
        if (d.op==0xdb) { s->fcw=0x037f; return NULL; }
        p=w02_span(vmcb,w02_ea(vmcb,g,d),d.op==0xd9?2:4,sub==3||sub==7);
        if (!p) return "FP OPERAND";
        if (d.op==0xd9) { if (sub==7) *(UINT16 *)p=s->fcw; else s->fcw=*(const UINT16 *)p; }
        else if (sub==3) *(UINT32 *)p=s->mxcsr;
        else { v=*(const UINT32 *)p; if (v>0xffff || (v&0x40)) return "MXCSR BITS"; s->mxcsr=(UINT32)v; }
        return NULL;
    default: return "UNREVIEWED OPCODE";
    }
}
/* All stopped instructions advance only after their effects have completed.
 * A partial REP keeps RIP and its remaining count for the next bounded slice. */
static const char *w02_soft_step(UINT8 *vmcb, UINT64 *g, W02State *s, W02Insn d)
{
    const char *error=w02_soft(vmcb,g,s,d);
    if (!error && (d.k!=W02String || !d.rep || !g[1])) FIELD(UINT64,Rip)+=d.n;
    return error;
}
#endif
