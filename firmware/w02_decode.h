#ifndef HV_W02_DECODE_H
#define HV_W02_DECODE_H
/* This is an allowlist, not a permissive x86 length disassembler. Prefix
 * groups, opcode extensions and lengths must match before a native step.
 * No segment loads, interrupt/debug shadows, SYSCALL, IRET, debug registers,
 * lock, EVEX/VEX/XOP, SIMD or x87 native execution. */
enum W02Kind { W02Bad,W02Native,W02Cpuid,W02Msr,W02Io,W02Tsc,W02String,
               W02Flags,W02Cr,W02Fp,W02Desc,W02JumpXmm };
typedef struct {
    UINT8 n, op, rex, op16, a32, rep, modrm, sib, op2;
    INT32 disp;
    enum W02Kind k;
} W02Insn;
static W02Insn w02_decode(const UINT8 *p, UINT32 avail)
{
    W02Insn d={0}; unsigned i=0, imm=0; BOOLEAN mod=FALSE, pair=FALSE;
    /* The reader has at least 15 physically owned bytes. avail limits the
     * final instruction to one measured executable extent. */
    while (i<4) {
        UINT8 b=p[i];
        if (b==0x66 && !d.op16) d.op16=1;
        else if (b==0x67 && !d.a32) d.a32=1;
        else if ((b==0xf2 || b==0xf3) && !d.rep) d.rep=b;
        else break;
        ++i;
    }
    if ((p[i]&0xf0)==0x40) d.rex=p[i++];
    UINT8 op=d.op=p[i++]; d.k=W02Native;
    if (op==0x0f) { pair=TRUE; d.op2=op=p[i++]; }
    unsigned width=(d.op16 && !(d.rex&8)) ? 2 : 4;
    if (!pair) {
        if (op<0x40 && (op&7)<=5) {
            if ((op&7)<=3) mod=TRUE;
            else imm=(op&7)==4?1:width;
        } else if ((op>=0x50 && op<=0x5f) || op==0x90 || op==0x98 || op==0x99 ||
                   op==0xc3 || op==0xc9 || op==0xfc || op==0xfd) imm=0;
        else if (op==0x68 || op==0x6a) imm=op==0x68?width:1;
        else if (op==0x69 || op==0x6b) { mod=TRUE; imm=op==0x69?width:1; }
        else if (op==0x63 || (op>=0x80 && op<=0x8d && op!=0x82)) {
            mod=TRUE;
            if (op==0x80 || op==0x83) imm=1; else if (op==0x81) imm=width;
        } else if ((op>=0x70 && op<=0x7f) || (op>=0xe0 && op<=0xe3) || op==0xeb) imm=1;
        else if (op==0xe8 || op==0xe9) imm=4;
        else if (op>=0xb0 && op<=0xbf) imm=op<0xb8?1:(d.rex&8)?8:width;
        else if (op==0xa8 || op==0xa9) imm=op==0xa8?1:width;
        else if ((op>=0x91 && op<=0x97) || op==0xf8 || op==0xf9 || op==0xf5) imm=0;
        else if (op==0xc0 || op==0xc1 || op==0xc6 || op==0xc7 || (op>=0xd0 && op<=0xd3) ||
                 op==0xf6 || op==0xf7 || op==0xfe || op==0xff) {
            mod=TRUE; imm=op==0xc0||op==0xc1||op==0xc6?1:op==0xc7?width:0;
        } else if (op==0xa4 || op==0xa5 || op==0xaa || op==0xab) d.k=W02String;
        else if (op==0x9c || op==0x9d || op==0xfa || op==0xfb) d.k=W02Flags;
        else if (op==0xec || op==0xed || op==0xee || op==0xef || (op>=0xe4 && op<=0xe7)) {
            d.k=W02Io; if (op<0xec) imm=1;
        } else if (op==0x9b) d.k=W02Fp;
        else if (op==0xdb || op==0xd9) { mod=TRUE; d.k=W02Fp; }
        else d.k=W02Bad;
    } else {
        if (op==0xa2) d.k=W02Cpuid;
        else if (op==0x30 || op==0x32) d.k=W02Msr;
        else if (op==0x31) d.k=W02Tsc;
        else if (op==0x20 || op==0x22) { mod=TRUE; d.k=W02Cr; }
        else if (op==0x01) { mod=TRUE; d.k=W02Desc; }
        else if (op==0xae) { mod=TRUE; d.k=W02Fp; }
        else if ((op==0x6f || op==0x7f) && d.rep==0xf3 && !d.op16) { mod=TRUE; d.k=W02JumpXmm; }
        else if (op>=0x80 && op<=0x8f) imm=4;
        else if ((op>=0x40 && op<=0x4f) || (op>=0x90 && op<=0x9f) ||
                 op==0xb6 || op==0xb7 || op==0xbe || op==0xbf || op==0xaf ||
                 op==0xa3 || op==0xab || op==0xb3 || op==0xbb || op==0xba ||
                 op==0xbc || op==0xbd || op==0xa4 || op==0xa5 || op==0xac || op==0xad || op==0x1f) {
            mod=TRUE; imm=op==0xba || op==0xa4 || op==0xac ? 1:0;
        } else if (op>=0xc8 && op<=0xcf) imm=0;
        else d.k=W02Bad;
    }
    if (d.rep && !(d.rep==0xf3 && (d.k==W02String || d.k==W02JumpXmm || (!pair && op==0x90)))) d.k=W02Bad;
    if (mod) {
        UINT8 m=d.modrm=p[i++], reg=(m>>3)&7, rm=m&7; unsigned len=0;
        if ((!pair && ((op==0xff && (reg==3 || reg==5 || reg==7)) ||
                       (op==0xfe && reg>1) || ((op==0xc6||op==0xc7) && reg) ||
                       (op==0x8c && reg>5) || (op==0x8d && m>=0xc0) ||
                       ((op==0xf6||op==0xf7) && reg==1))) ||
            (pair && ((op==0x1f && reg) || (op==0xba && reg<4)))) d.k=W02Bad;
        if (!pair && (op==0xf6 || op==0xf7) && !reg) imm=op==0xf6?1:width;
        if (m<0xc0) {
            if (rm==4) { d.sib=p[i++]; rm=d.sib&7; }
            len=m>=0x80?4:m>=0x40?1:rm==5?4:0;
            if (len==4) d.disp=*(const INT32 *)(p+i); else if (len) d.disp=(INT8)p[i];
            i+=len;
        }
    }
    if (d.k!=W02Native && (d.rep || d.a32 || d.op16 || d.rex) &&
        d.k!=W02String && d.k!=W02Io && d.k!=W02Desc && d.k!=W02Fp && d.k!=W02Cr && d.k!=W02JumpXmm) d.k=W02Bad;
    if (d.k==W02Io && (d.rep || d.a32 || d.rex)) d.k=W02Bad;
    if (d.k==W02Cr && (d.rep||d.op16||d.a32||d.modrm<0xc0)) d.k=W02Bad;
    if (d.k==W02Desc && ((d.modrm>>3&7)>3 || d.modrm>=0xc0 || d.rep || d.op16)) d.k=W02Bad;
    if (d.k==W02Fp && !((!pair && (op==0x9b || (op==0xdb && d.modrm==0xe3) ||
        (op==0xd9 && d.modrm<0xc0 && ((d.modrm>>3&7)==5 || (d.modrm>>3&7)==7)))) ||
        (pair && op==0xae && d.modrm<0xc0 && ((d.modrm>>3&7)==2 || (d.modrm>>3&7)==3)))) d.k=W02Bad;
    i+=imm;
    if (i>15 || i>avail) d.k=W02Bad;
    d.n=(UINT8)i; return d;
}
static UINT64 w02_reg(UINT8 *vmcb, UINT64 *g, unsigned r)
{
    if (!r) return FIELD(UINT64,Rax); if (r==4) return FIELD(UINT64,Rsp);
    return g[r==3?0:r<3?r:r-2];
}
static void w02_set(UINT8 *vmcb, UINT64 *g, unsigned r, UINT64 v)
{
    if (!r) FIELD(UINT64,Rax)=v; else if (r==4) FIELD(UINT64,Rsp)=v;
    else g[r==3?0:r<3?r:r-2]=v;
}
static UINT64 w02_ea(UINT8 *vmcb, UINT64 *g, W02Insn d)
{
    UINT8 m=d.modrm, rm=m&7; UINT64 a=(INT64)d.disp;
    if (rm==4) {
        unsigned idx=(d.sib>>3)&7; rm=d.sib&7;
        if (idx!=4 || (d.rex&2)) a+=w02_reg(vmcb,g,idx+((d.rex&2)?8:0))<<(d.sib>>6);
    } else if (m<0x40 && rm==5) a+=FIELD(UINT64,Rip)+d.n;
    if (!(m<0x40 && rm==5)) a+=w02_reg(vmcb,g,rm+((d.rex&1)?8:0));
    return d.a32 ? (UINT32)a : a;
}
#endif
