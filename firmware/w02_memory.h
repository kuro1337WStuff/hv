#ifndef HV_W02_MEMORY_H
#define HV_W02_MEMORY_H
/* All translations terminate in this reservation. No guest value, including
 * EXITINFO2 or a PTE, is ever interpreted as an L1 address. */
enum { W02Permanent=32*1024*1024 };
typedef struct { UINT32 origin, rva, len; UINT8 digest[32]; } W02Image;
static const W02Image w02_peims[]=W02_PEI_IMAGES;
enum { W02Peims=sizeof(w02_peims)/sizeof(w02_peims[0]) };
static const W02Image w02_dxe={W02_DXE_FV_IMAGE_BASE,W02_DXE_TEXT_RVA,W02_DXE_TEXT_LEN,W02_DXE_TEXT_SHA256};
typedef struct {
    UINT32 relocated[W02Peims+1];
    UINT32 pci, apic[4], mxcsr, phase, pei_page, pei_pc, dxe_pc;
    UINT16 fcw, fwsel; UINT8 cmos, pm_enable, pic[2];
    UINT64 io, ops, sec_pc, start, timer;
    UINT8 jump_xmm[10][16]; /* PEI BaseLib's nonvolatile XMM6..15 jump buffer */
    BOOLEAN fvs;
#ifdef HV_W03
    UINT32 ready_pc, boot_base, boot_handle, boot_table, boot_rsp, boot_pc, boot_next, boot_steps;
    UINT32 start_service; BOOLEAN saw_start;
#ifdef HV_W04
    UINT32 w04_steps, w04_pc, w04_target, w04_service;
    UINT32 w04_bs; UINT8 w04_bs_digest[32]; /* measured DXE's ready-time table */
#endif
#endif
} W02State;
static BOOLEAN w02_ram(UINT64 a, UINTN n)
{
    return n && a<W02RamBytes && n<=W02RamBytes-a &&
           (a+n<=0xa0000 || a>=0x100000);
}
static UINT8 *w02_back(UINT8 *vmcb, UINT64 a, UINTN n)
{
    if (w02_ram(a,n)) return w01_page(vmcb,W02Ram)+(UINTN)a;
    if (a>=W01_FLASH_GPA && a<0x100000000ull && n && n<=0x100000000ull-a)
        return w01_page(vmcb,W01Flash)+(UINTN)(a-W01_FLASH_GPA);
    return NULL;
}
static UINT64 *w02_leaf(UINT8 *vmcb, UINT64 a)
{
    if (a<W02RamBytes) return &w01_table(vmcb,W02PtRam+(UINTN)(a>>21))[(a>>12)&511];
    return (UINT64 *)w01_code_leaf(vmcb,(UINT32)a); /* caller has checked flash */
}
static void w02_map(UINT8 *vmcb)
{
    for (UINTN pt=0;pt<W02RamBytes/(512*W01Page);++pt) {
        for (UINTN i=0;i<512;++i) {
            UINTN a=(pt*512+i)*W01Page;
            if (w02_ram(a,1)) w01_table(vmcb,W02PtRam+pt)[i]=
                (UINTN)(w01_page(vmcb,W02Ram)+a)|7|W01_NX;
        }
        w01_link(vmcb,W01PdRam,pt,W02PtRam+pt);
    }
#ifdef HV_W03
    /* Reserved gap, descriptor through the entire 16 MiB file window.
     * w02_span also requires the actual W leaf on every emulated store. */
    for (UINT32 a=W03DescriptorGpa;a<W02Permanent;a+=W01Page) *w02_leaf(vmcb,a)&=~2ull;
#endif
    *w01_code_leaf(vmcb,W01_RESET_GPA)|=W01_NX;
}
/* Below 4 GiB, four levels, supervisor long mode only. Follow private guest
 * page tables, checking effective R/W and NX, then require identity. This
 * also permits DXE IPL's replacement CR3 without trusting its provenance. */
static BOOLEAN w02_identity(UINT8 *vmcb, UINT64 a, BOOLEAN wr, BOOLEAN ex)
{
    if (a>=0x100000000ull || !(FIELD(UINT64,Cr0)&(1ull<<31)) ||
        !(FIELD(UINT64,GuestEfer)&0x400) || (FIELD(UINT64,Cr4)&0x1020)!=0x20) return FALSE;
    UINT64 tab=FIELD(UINT64,Cr3)&~0xfffull;
    for (int level=3;level>=0;--level) {
        if (!w02_ram(tab,W01Page)) return FALSE;
        unsigned shift=12+9*level;
        UINT64 p=((UINT64 *)w02_back(vmcb,tab,W01Page))[(a>>shift)&511];
        if (!(p&1) || (wr && !(p&2) && (FIELD(UINT64,Cr0)&0x10000)) ||
            (ex && (p&W01_NX)) || (p&0x7ff0000000000000ull) ||
            ((p&W01_NX) && !(FIELD(UINT64,GuestEfer)&0x800))) return FALSE;
        tab=p&0xffffffffff000ull;
        if (!level || (p&128)) {
            if (level==3) return FALSE;
            UINT64 mask=(1ull<<shift)-1;
            return (!level || !(tab&mask&~0x1000ull)) && (tab&~mask)==(a&~mask);
        }
    }
    return FALSE;
}
static UINT8 *w02_span(UINT8 *vmcb, UINT64 a, UINTN n, BOOLEAN wr)
{
    UINT8 *p=w02_back(vmcb,a,n);
    if (!p || (wr && !w02_ram(a,n))) return NULL;
    for (UINT64 q=a&~0xfffull;q<a+n;q+=W01Page)
        if (!w02_identity(vmcb,q,wr,FALSE) || ((*w02_leaf(vmcb,q)&(wr?3:1))!=(wr?3u:1u))) return NULL;
    return p;
}
static BOOLEAN w02_inside(UINT64 pc, UINT32 base, UINT32 size)
{
    return pc>=base && pc-base<size;
}
static void w02_seal(UINT8 *vmcb, UINT32 a, UINT32 n)
{
    for (UINT64 q=a&~0xfffull;q<(UINT64)a+n;q+=W01Page)
        *w02_leaf(vmcb,q)&=~(W01_NX|2ull);
}
static BOOLEAN w02_volume(UINT8 *vmcb, UINT32 a, UINT32 size, const UINT8 hash[32])
{
    return w02_ram(a,size) && w01_digest(w02_back(vmcb,a,size),size,hash);
}
static const W02Image *w02_image(unsigned i)
{
    return i==W02Peims ? &w02_dxe : &w02_peims[i];
}
/* Called at fetch boundaries, never for a data fault. Permanently seal only
 * measured text. No write/execute transition for arbitrary scratch/stack. */
static const char *w02_text(UINT8 *vmcb, W02State *s, UINT32 pc, UINT32 *end)
{
    if (w02_inside(pc,W02_SEC_TEXT_BASE,W02_SEC_TEXT_LEN)) {
        *end=W02_SEC_TEXT_BASE+W02_SEC_TEXT_LEN;
        return NULL;
    }
    if (!w02_ram(pc,1)) return "OUTSIDE CODE";
    if (!s->fvs) {
        static const UINT8 pei[]=W02_PEI_FV_SHA256, dxe[]=W02_DXE_FV_SHA256;
        if (!w02_volume(vmcb,W02_PEI_FV_BASE,W02_PEI_FV_SIZE,pei) ||
            !w02_volume(vmcb,W02_DXE_FV_BASE,W02_DXE_FV_SIZE,dxe)) return "DECODED FV HASH";
        s->fvs=TRUE;
        for (unsigned i=0;i<W02Peims;++i) {
            const W02Image *m=w02_image(i); UINT32 text=m->origin+m->rva;
            if (!m->len || text<m->origin || !w02_inside(text,W02_PEI_FV_BASE,W02_PEI_FV_SIZE) ||
                m->len>W02_PEI_FV_BASE+W02_PEI_FV_SIZE-text ||
                !w02_volume(vmcb,text,m->len,m->digest)) return "PEIM TEXT HASH";
            w02_seal(vmcb,text,m->len);
        }
    }
    for (unsigned i=0;i<=W02Peims;++i) {
        const W02Image *m=w02_image(i); UINT32 text=m->origin+m->rva;
        if (i!=W02Peims && w02_inside(pc,text,m->len)) { *end=text+m->len; return NULL; }
        if (s->relocated[i]) {
            text=s->relocated[i]+m->rva;
            if (w02_inside(pc,text,m->len)) { *end=text+m->len; return NULL; }
        }
    }
    if (pc<W02Permanent) return "UNPINNED XIP";
    for (unsigned i=0;i<=W02Peims;++i) {
        const W02Image *m=w02_image(i);
        if (s->relocated[i] || pc<m->rva || !m->len || m->len>W02RamBytes-W02Permanent) continue;
        for (UINT32 b=(pc-m->rva)&~0xfffu;b>=W02Permanent && pc-b-m->rva<m->len;b-=W01Page) {
            UINT32 text=b+m->rva;
            if (!w02_ram(text,m->len) || !w01_digest(w02_back(vmcb,text,m->len),m->len,m->digest)) continue;
            w02_seal(vmcb,text,m->len); s->relocated[i]=b; *end=text+m->len; return NULL;
        }
    }
    return "RELOC TEXT HASH";
}
#endif
