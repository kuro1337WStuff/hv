/* Host-only guard checks; no privileged instructions or hardware access.
 * From an x64 VS prompt at repository root:
 * cl /nologo /std:c11 /O2 /W4 /WX lab\tests\entry_guard_test.c /Fobuild\entry_guard_test.obj /Febuild\entry_guard_test.exe
 * build\entry_guard_test.exe build\linux-inputs\vmlinuz-virt build\linux-inputs\initramfs.cpio build\linux-inputs\cmdline.bin
 */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include "../../firmware/linux_entry_guard.h"

static uint8_t ram[LinuxRamBytes], vmcb[LinuxPage];
static uint8_t image[0xc10400], rd[0x168964], cmd[123];

static int load_file(const char *path, uint8_t *bytes, size_t length)
{
    FILE *f=fopen(path,"rb");
    if (!f) { perror(path); return 0; }
    int ok=fread(bytes,1,length,f)==length && fgetc(f)==EOF && !ferror(f);
    if (fclose(f)!=0) ok=0;
    if (!ok) fprintf(stderr,"wrong length or read error: %s\n",path);
    return ok;
}

/* Supply VMCB bytes using APM offsets, independently of the guard's readers. */
static void nx_vmcb(uint32_t entry)
{
    for (unsigned s=0x400; s<=0x430; s+=16) {
        linux_put32(vmcb+s,s==0x410 ? 0x0a9b0010 : 0x0c930018);
        linux_put32(vmcb+s+4,UINT32_MAX);
    }
    linux_put32(vmcb+0x464,0x1f); linux_put64(vmcb+0x468,0x12000);
    linux_put32(vmcb+0x008,UINT32_MAX); vmcb[0x05c]=1;
    linux_put64(vmcb+0x4d0,0x1d00); linux_put64(vmcb+0x548,0x20);
    linux_put64(vmcb+0x550,0x13000); linux_put64(vmcb+0x558,0x80010031u);
    linux_put64(vmcb+0x570,0x102); linux_put64(vmcb+0x578,entry); linux_put64(vmcb+0x5d8,0x18000);
}

static enum LinuxEntryFault inspect(uint64_t rsi)
{
    return linux_entry_guard(vmcb,sizeof vmcb,rsi,ram,sizeof ram);
}

typedef struct { unsigned offset; uint8_t bits; enum LinuxEntryFault fault; } Poison;

static int poisons(uint8_t *bytes, const Poison *cases, size_t count)
{
    for (size_t i=0; i<count; ++i) {
        bytes[cases[i].offset]^=cases[i].bits;
        enum LinuxEntryFault got=inspect(0x10000);
        bytes[cases[i].offset]^=cases[i].bits;
        if (got!=cases[i].fault) {
            fprintf(stderr,"offset %#x xor %#x: expected field %d, got %d\n",
                cases[i].offset,(unsigned)cases[i].bits,(int)cases[i].fault,(int)got);
            return 0;
        }
    }
    return 1;
}

#define CHECK(expr) do { if (!(expr)) { fprintf(stderr,"line %d: %s\n",__LINE__,#expr); return 1; } } while (0)
int main(int argc, char **argv)
{
    if (argc!=4) { fputs("usage: entry_guard_test vmlinuz-virt initramfs.cpio cmdline.bin\n",stderr); return 2; }
    CHECK(load_file(argv[1],image,sizeof image)); CHECK(load_file(argv[2],rd,sizeof rd));
    CHECK(load_file(argv[3],cmd,sizeof cmd));
    LinuxLayout layout;
    const char *err=build_linux_layout(ram,sizeof ram,image,sizeof image,rd,sizeof rd,cmd,sizeof cmd,&layout);
    if (err) { fprintf(stderr,"layout: %s\n",err); return 1; }
    nx_vmcb(layout.entry);
    CHECK(inspect(0x10000)==LinuxEntryOK);
    CHECK(linux_entry_guard(vmcb,sizeof vmcb-1,0x10000,ram,sizeof ram)==LinuxEntryStorage);
    CHECK(linux_entry_guard(vmcb,sizeof vmcb,0x10000,ram,sizeof ram-1)==LinuxEntryStorage);
    CHECK(inspect(0x10001)==LinuxEntryRSI && inspect(UINT64_MAX)==LinuxEntryRSI);
    static const Poison state[]={
        {0x55b,0x80,LinuxEntryCR0}, {0x551,0x10,LinuxEntryCR3}, {0x548,0x20,LinuxEntryCR4},
        {0x4d1,8,LinuxEntryEFER}, {0x578,1,LinuxEntryRIP}, {0x5d8,8,LinuxEntryRSP},
        {0x571,2,LinuxEntryFlags}, /* IF */ {0x571,1,LinuxEntryDebugTrap}, /* TF */
        {0x008,2,LinuxEntryDebugTrap}, {0x05c,1,LinuxEntryTLB}, {0x0c0,1,LinuxEntryTLB},
        {0x464,1,LinuxEntryGDTR}, {0x46f,0x80,LinuxEntryGDTR},
        {0x410,8,LinuxEntryCS}, {0x41f,0x80,LinuxEntryCS}, {0x433,4,LinuxEntryDS},
        {0x400,8,LinuxEntryES}, {0x424,1,LinuxEntrySS}, {0x44f,1,LinuxEntrySegments}
    };
    CHECK(poisons(vmcb,state,sizeof state/sizeof state[0]));
    static const Poison guest[]={
        {LinuxGdt+0x17,1,LinuxEntryGDT}, {LinuxParams+0x250,1,LinuxEntryBootParams},
        {LinuxParams+0x70,1,LinuxEntryBootParams}, {LinuxParams+0x1df,1,LinuxEntryBootParams},
        {LinuxParams+0x214,1,LinuxEntryWork}, {LinuxParams+0x263,0x80,LinuxEntryWork},
        {LinuxParams+0xc0,1,LinuxEntryInitrd}, {LinuxParams+0xc4,0x80,LinuxEntryInitrd},
        {LinuxParams+0x21f,0x80,LinuxEntryInitrd}, {LinuxParams+0xc8,1,LinuxEntryCmdline},
        {LinuxCmdline,'n',LinuxEntryCmdline}, /* early NUL */ {LinuxCmdline+122,1,LinuxEntryCmdline},
        {LinuxParams+0x2d0+40+15,0x80,LinuxEntryE820},
        {LinuxPml4+7,0x80,LinuxEntryPaging}, /* guest NX above leaf */
        {LinuxPd+8*8+7,0x80,LinuxEntryPaging}, /* entry PDE NX */
        {LinuxPd+8*8+2,0x20,LinuxEntryPaging}, /* present but wrong 2 MiB frame */
        {LinuxPt+8*0x17,2,LinuxEntryPaging}, /* stack not writable */
        {LinuxPt+8*0x10,4,LinuxEntryPaging}, /* user boot_params leaf */
        {0x1000200,1,LinuxEntryFetch}
    };
    CHECK(poisons(ram,guest,sizeof guest/sizeof guest[0]));
    CHECK(inspect(0x10000)==LinuxEntryOK);
    puts("NX entry guard: pinned layout accepted; register, pointer, range and guest permission controls rejected.");
    return 0;
}
