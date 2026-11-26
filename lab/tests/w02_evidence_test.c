/* Host control for the W02 witness verifier, not a guest boot. Use the actual
 * authenticated PEI and DXE FVs from this build, not placeholder code bytes.
 * CLI: w02_evidence_test.exe build\\w02\\W02CODE.FD. */
#include <Uefi.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <string.h>
#include <intrin.h>
#define HV_W02 1
#include "../../firmware/fixed_vmcb.h"
#include "../../firmware/w01_memory.h"
#include "../../firmware/w02_memory.h"
#include "../../firmware/w02_decode.h"
#include "../../firmware/w02_scalar.h"
#include "../../firmware/w02_platform.h"

#define CHECK(expr) do { if (!(expr)) { fprintf(stderr,"failed at %d: %s\n",__LINE__,#expr); return 1; } } while (0)

static int load_volume(const char *path, UINT32 offset, UINT8 *dst, UINTN size)
{
    FILE *fp=NULL;
    if (fopen_s(&fp,path,"rb")) return 0;
    int exact=fseek(fp,(long)offset,SEEK_SET)==0 && fread(dst,1,size,fp)==size;
    return fclose(fp)==0 && exact;
}

int main(int argc, char **argv)
{
    UINT8 *vmcb=_aligned_malloc((size_t)StandaloneGuestPages*W01Page,W01Page);
    CHECK(argc==2 && vmcb);
    memset(vmcb,0,(size_t)StandaloneGuestPages*W01Page);
    /* Four-level guest paging covering low memory with 2 MiB identity leaves. */
    FIELD(UINT64,Cr0)=0x80010001; FIELD(UINT64,GuestEfer)=0x400;
    FIELD(UINT64,Cr4)=0x20; FIELD(UINT64,Cr3)=W01RamGpa;
    UINT64 *pt=(UINT64 *)w01_page(vmcb,W01Ram);
    pt[0]=(W01RamGpa+0x1000)|3; pt[512]=(W01RamGpa+0x2000)|3;
    for (unsigned i=0;i<64;++i) pt[1024+i]=((UINT64)i<<21)|0x83;
    w02_map(vmcb);
    CHECK(load_volume(argv[1],W02_PEI_FV_CODE_OFFSET,w02_back(vmcb,W02_PEI_FV_BASE,W02_PEI_FV_SIZE),W02_PEI_FV_SIZE));
    CHECK(load_volume(argv[1],W02_DXE_FV_CODE_OFFSET,w02_back(vmcb,W02_DXE_FV_BASE,W02_DXE_FV_SIZE),W02_DXE_FV_SIZE));
    const UINT32 page=40u<<20, dxe_base=48u<<20;
    UINT32 pei_pc=W02_PEI_FV_IMAGE_BASE+W02_PEI_OUT_RVA, dxe_pc=dxe_base+W02_DXE_OUT_RVA, end;
    W02State state={0}; state.phase=1;
    UINT64 g[14]={0};
    CHECK(w02_text(vmcb,&state,pei_pc,&end)==NULL && state.fvs);
    /* Relocate the exact immutable DXE .text only; never run those bytes here. */
    memcpy(w02_back(vmcb,dxe_base+W02_DXE_TEXT_RVA,W02_DXE_TEXT_LEN),
           w02_back(vmcb,W02_DXE_FV_IMAGE_BASE+W02_DXE_TEXT_RVA,W02_DXE_TEXT_LEN),W02_DXE_TEXT_LEN);
    CHECK(w02_text(vmcb,&state,dxe_pc,&end)==NULL);
    g[8]=0x32455844;
    CHECK(!w02_marker(vmcb,g,&state,dxe_pc,0x32455844) && state.phase==1);
    UINT32 *words=(UINT32 *)w02_back(vmcb,page,W01Page);
    g[8]=words[0]=0x32494550;
    CHECK(!w02_marker(vmcb,g,&state,pei_pc,page) && state.phase==1);
    words[1023]=~page;
    CHECK(!w02_marker(vmcb,g,&state,pei_pc+1,page) && state.phase==1);
    CHECK(w02_marker(vmcb,g,&state,pei_pc,page) && state.phase==2 && state.pei_page==page);
    CHECK(!w02_marker(vmcb,g,&state,pei_pc,page) && state.phase==2);
    g[8]=0x32455844;
    CHECK(!w02_marker(vmcb,g,&state,dxe_pc+1,0x32455844) && state.phase==2);
    CHECK(w02_marker(vmcb,g,&state,dxe_pc,0x32455844) && state.phase==3 && state.dxe_pc==dxe_pc);
    /* 65 pages exceed one dispatch. Retiring REP early loses the last page. */
    const UINT8 rep_movsb[15]={0xf3,0xa4};
    W02Insn copy=w02_decode(rep_movsb,2);
    const UINT32 src=64u<<20, dst=68u<<20, size=65u*4096;
    UINT8 *in=w02_back(vmcb,src,size), *out=w02_back(vmcb,dst,size);
    for (UINT32 i=0;i<size;++i) in[i]=(UINT8)(1+(i%251));
    g[4]=src; g[5]=dst; g[1]=size; FIELD(UINT64,Rip)=0x1234;
    for (unsigned visit=0;visit<4 && FIELD(UINT64,Rip)==0x1234;++visit)
        CHECK(w02_soft_step(vmcb,g,&state,copy)==NULL);
    CHECK(FIELD(UINT64,Rip)==0x1236 && !g[1] && g[4]==src+size && g[5]==dst+size);
    CHECK(memcmp(in,out,size)==0);
    _aligned_free(vmcb);
    puts("W02 host controls: measured sites in order; five false markers refused; complete 65-page REP copy.");
    return 0;
}
