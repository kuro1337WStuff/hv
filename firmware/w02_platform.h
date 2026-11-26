#ifndef HV_W02_PLATFORM_H
#define HV_W02_PLATFORM_H
static BOOLEAN w02_marker(UINT8 *vmcb, UINT64 *g, W02State *s, UINT32 pc, UINT32 ax)
{
    static const UINT8 pei[32]=W02_PEI_OUT_WINDOW, dxe[32]=W02_DXE_OUT_WINDOW;
    BOOLEAN first=s->phase==1; UINT32 tag=first?0x32494550u:0x32455844u;
    if (!s->fvs || (!first && s->phase!=2) || g[8]!=tag || pc<24) return FALSE;
    UINT32 rva=first?W02_PEI_OUT_RVA:W02_DXE_OUT_RVA;
    if (pc<rva) return FALSE;
    UINT32 base=pc-rva, orig=first?W02_PEI_FV_IMAGE_BASE:W02_DXE_FV_IMAGE_BASE;
    BOOLEAN bound=FALSE;
    for (unsigned i=0;i<=W02Peims;++i) {
        const W02Image *m=w02_image(i);
        if (m->origin==orig && w02_inside(pc,base+m->rva,m->len) &&
            ((first && base==orig) || (s->relocated[i] && base==s->relocated[i]))) bound=TRUE;
    }
    if (!bound) return FALSE;
    const UINT8 *p=w02_span(vmcb,pc-24,32,FALSE), *w=first?pei:dxe;
    if (!p || (*w02_leaf(vmcb,pc)&(3ull|W01_NX))!=1) return FALSE;
    for (unsigned i=0;i<32;++i) if (p[i]!=w[i]) return FALSE;
    if (first) {
        if ((ax&4095) || ax<W02Permanent) return FALSE;
        p=w02_span(vmcb,ax,W01Page,TRUE);
        if (!p || !(*w02_leaf(vmcb,ax)&W01_NX) || *(const UINT32 *)p!=tag ||
            *(const UINT32 *)(p+4092)!=~ax) return FALSE;
        s->pei_pc=pc; s->pei_page=ax;
    } else {
        if (ax!=tag) return FALSE;
        s->dxe_pc=pc;
    }
    ++s->phase; return TRUE;
}
static const char *w02_io(UINT8 *vmcb, UINT64 *g, W02State *s, W02Insn d, const UINT8 *p)
{
    UINT32 pc=(UINT32)FIELD(UINT64,Rip), ax=(UINT32)FIELD(UINT64,Rax), val=0;
    UINT16 port=d.op>=0xec?(UINT16)g[2]:p[d.n-1];
    unsigned width=(d.op&1)?(d.op16?2:4):1; BOOLEAN in=(d.op&2)==0;
    UINT64 info=FIELD(UINT64,Info1), next=pc+d.n;
    /* EXITINFO1's segment/address-size fields are irrelevant for scalar I/O;
     * require no string/REP and exactly one operand width, port and direction. */
    if (FIELD(UINT64,ExitCode)!=0x7b || FIELD(UINT64,Info2)!=next ||
        (info&~0x1f80ull)!=(((UINT64)port<<16)|(width<<4)|in)) return "IO RECEIPT";
    ++s->io;
    if (port==0x505 && !in && width==4 && d.op==0xef && d.n==1) {
        if (!w02_marker(vmcb,g,s,pc,ax)) return "WITNESS ORIGIN";
    } else if (port==0xcf8 && width==4) {
        if (in) val=s->pci; else s->pci=ax;
    } else if (port>=0xcfc && port<=0xcff && width<=0xd00u-port && (s->pci&0x80000000u)) {
        UINT32 addr=(s->pci&0x00fffffc)+(port-0xcfc), dev=addr&~3u;
        if (in) {
            switch (dev) {
            case 0: val=0x29c08086; break;           /* Q35 MCH */
            case 0xf800: val=0x29188086; break;      /* ICH9 LPC */
            case 0xf840: val=0x601; break;           /* guest PMBASE */
            case 0xf844: val=s->pm_enable; break;    /* ACPI_CNTL */
            default: val=0xffffffffu; break;         /* no other functions */
            }
            val>>=8*(addr&3);
        } else if (addr==0xf844 && width==1 && (UINT8)ax==0x80) s->pm_enable=0x80;
        else if (!(addr==0xf840 && width==4 && (ax&~1u)==0x600)) return "PCI REGISTER";
    } else if (port==0x70 && !in && width==1) s->cmos=(UINT8)ax&127;
    else if (port==0x71 && in && width==1) {
        /* Normal boot, 128 MiB contiguous below 4G (CMOS 34h/35h uses
         * 64 KiB units above 16 MiB). No RTC/time services. */
        switch (s->cmos) {
        case 0x0f: case 0x34: case 0x5b: case 0x5c: case 0x5d: val=0; break;
        case 0x35: val=7; break;
        case 0x5f: val=0; break; /* one BSP */
        default: return "CMOS REGISTER";
        }
    } else if ((port==0x21 || port==0xa1) && width==1) {
        unsigned n=port==0xa1;
        if (in) val=s->pic[n]; else if ((UINT8)ax==0xff) s->pic[n]=0xff; else return "PIC MASK";
    } else if (port==0x608 && in && width==4 && (s->pm_enable&0x80)) val=(UINT32)(s->timer+=1024)&0xffffff;
    else if (port==0x402 && width==1) { if (in) val=0xe9; } /* debugcon sink */
    else if (port==0x510 && !in && width==2) s->fwsel=(UINT16)ax;
    else if (port==0x511 && in && width==1) {
        /* No fw_cfg device/DMA transport on this fixed board. */
        if (s->fwsel!=0) return "FW CFG SELECT";
        val=0xff;
    } else return "UNHANDLED PORT";
    if (in) {
        UINT32 mask=width==1?255:width==2?65535:0xffffffffu;
        FIELD(UINT64,Rax)=(width==4?0:FIELD(UINT64,Rax)&~(UINT64)mask)|(val&mask);
    }
    return NULL;
}
static const char *w02_sys(UINT8 *vmcb, UINT64 *g, W02State *s, W02Insn d)
{
    UINT32 op=(UINT32)FIELD(UINT64,Rax), cx=(UINT32)g[1];
    UINT64 x=FIELD(UINT64,ExitCode), v=0, in=((UINT64)(UINT32)g[2]<<32)|op;
    if (FIELD(UINT64,NextRip)!=FIELD(UINT64,Rip)+d.n) return "SYSTEM NRIP";
    if (d.k==W02Cpuid && x==0x72) {
        UINT32 a=0,b=0,c=0,z=0;
        switch (op) {
        case 0: a=1; b=0x68747541; c=0x444d4163; z=0x69746e65; break;
        case 1: a=0x00020f32; b=1u<<16; z=0x07008371u; break;
        case 0x80000000: a=0x80000001; break;
        case 0x80000001: a=0x00020f32; z=0x21108371; break;
        default: break; /* above maximum standard or extended leaf */
        }
        FIELD(UINT64,Rax)=a; g[0]=b; g[1]=c; g[2]=z; return NULL;
    }
    if (d.k==W02Msr && x==0x7c) {
        if (d.op2==0x32 && FIELD(UINT64,Info1)==0) {
            switch (cx) {
            case 0x1b: v=0xfee00900; break;             /* BSP, xAPIC */
            case MSR_EFER: v=FIELD(UINT64,GuestEfer)&~SVME; break;
            case 0xc0010131u: v=0; break;              /* no SEV */
            case 0x277: v=FIELD(UINT64,Pat); break;
            default: return "UNHANDLED RDMSR";
            }
        } else if (d.op2==0x30 && FIELD(UINT64,Info1)==1) {
            if (cx!=MSR_EFER || (in&~0x800ull)!=(FIELD(UINT64,GuestEfer)&~(SVME|0x800ull))) return "UNHANDLED WRMSR";
            FIELD(UINT64,GuestEfer)=in|SVME; return NULL;
        } else return "MSR RECEIPT";
    } else if (d.k==W02Tsc && x==0x6e) v=(s->timer+=1024);
    else return "SYSTEM RECEIPT";
    FIELD(UINT64,Rax)=(UINT32)v; g[2]=v>>32; return NULL;
}
/* No xAPIC page mapped in NPT. Accept only the four dword registers used
 * by InitializeApicTimer/DisableApicTimerInterrupt on the single BSP. */
static BOOLEAN w02_apic(UINT8 *vmcb, UINT64 *g, W02State *s, W02Insn d)
{
    if (d.op2 || (d.op!=0x8b && d.op!=0x89) || d.modrm>=0xc0 || d.op16 || (d.rex&8)) return FALSE;
    UINT64 a=w02_ea(vmcb,g,d); BOOLEAN wr=d.op==0x89;
    unsigned r=((d.modrm>>3)&7)+((d.rex&4)?8:0), idx;
    UINT64 raw=FIELD(UINT64,Info2);
    if ((raw!=a && raw!=(a&~0xfffull)) || !w02_identity(vmcb,a,wr,FALSE) ||
        (FIELD(UINT64,Info1)&~4ull)!=((1ull<<32)|(wr?2:0))) return FALSE;
    switch (a) {
    case 0xfee000f0: idx=0; break; case 0xfee00320: idx=1; break;
    case 0xfee00380: idx=2; break; case 0xfee003e0: idx=3; break;
    case 0xfee00390: if (wr) return FALSE; w02_set(vmcb,g,r,s->apic[2]-(UINT32)(s->timer+=1024)); return TRUE;
    default: return FALSE;
    }
    if (wr) s->apic[idx]=(UINT32)w02_reg(vmcb,g,r); else w02_set(vmcb,g,r,s->apic[idx]);
    return TRUE;
}
#endif
