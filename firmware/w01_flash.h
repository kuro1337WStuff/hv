#ifndef HV_W01_FLASH_H
#define HV_W01_FLASH_H
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleFileSystem.h>
#include "hv_sha256.h"
#ifdef HV_W03
#include "../build/w03_pin.h"
#include "w03_layout.h"
#elif defined(HV_W02)
#include "../build/w02_pin.h"
#else
#include "../build/w01_pin.h"
#endif

#define W01_FLASH_GPA 0xffc00000u
#define W01_CODE_GPA  0xffc84000u
#define W01_RESET_GPA 0xfffffff0u
#define W01_NX (1ull<<63)
/* edk2-stable202611 OvmfPkg/Include/Fdf/{OvmfPkgDefines,MemFd}.fdf.inc:
 * 0x800000-0x806000 early page tables, 0x80b000 work area, 0x814000-
 * 0x820000 SEC/PEI temporary RAM. No identity mapping to these host frames. */
enum { W01Page=4096, W01RamGpa=0x800000, W01RamPages=0x20,
       W01VarsBytes=0x84000, W01CodeBytes=0x37c000,
       W01HostGdt=Pml4+1, W01ExtHost, W01Pdpt, /* use spare fixed-layout pages */
       W01PdRam=Pages, W01PtRam, W01PdFlash,
       W01PtFlash,
#ifdef HV_W02
       W02PtRam=W01PtFlash+2, W02Ram=W02PtRam+64, W02RamBytes=128*1024*1024,
       W01Ram=W02Ram+W01RamGpa/W01Page, W01Flash=W02Ram+W02RamBytes/W01Page,
#else
       W01Ram=W01PtFlash+2, W01Flash=W01Ram+W01RamPages,
#endif
       StandaloneGuestPages=W01Flash+(W01VarsBytes+W01CodeBytes)/W01Page };
_Static_assert(W01_SEC_GPA >= W01_CODE_GPA && W01_SEC_GPA < W01_RESET_GPA &&
               W01_SEC_GPA / W01Page != W01_RESET_GPA / W01Page,
               "SEC must be in a separate page of pinned XIP CODE");
_Static_assert(W01CodeBytes+W01VarsBytes==0x400000 && EFI_PAGE_SIZE==W01Page,
               "split flash geometry");
static UINT8 *w01_page(UINT8 *base, UINTN n) { return base+n*W01Page; }

static const char *w01_read_fd(EFI_FILE_PROTOCOL *root, CHAR16 *name, UINT8 *dest, UINTN bytes)
{
    EFI_FILE_PROTOCOL *fd;
    if (root->Open(root,&fd,name,EFI_FILE_MODE_READ,0)!=EFI_SUCCESS) return "FLASH OPEN";
    UINTN n=bytes;
    UINT8 extra;
    const char *e=NULL;
    if (fd->Read(fd,&n,dest)!=EFI_SUCCESS || n!=bytes) e="FLASH SHORT";
    else {
        n=1;
        if (fd->Read(fd,&n,&extra)!=EFI_SUCCESS || n!=0) e="FLASH EOF";
    }
    if (fd->Close(fd)!=EFI_SUCCESS && !e) e="FLASH CLOSE";
    return e;
}
static const char *w01_read_media(EFI_HANDLE image, EFI_SYSTEM_TABLE *st, UINT8 *vmcb)
{
    EFI_GUID image_guid=EFI_LOADED_IMAGE_PROTOCOL_GUID, fs_guid=EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_LOADED_IMAGE_PROTOCOL *loaded;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs;
    EFI_FILE_PROTOCOL *root;
    EFI_BOOT_SERVICES *bs=st->BootServices;
    if (bs->HandleProtocol(image,&image_guid,(void **)&loaded)!=EFI_SUCCESS || !loaded ||
        !loaded->DeviceHandle) return "MEDIA IMAGE";
    if (bs->HandleProtocol(loaded->DeviceHandle,&fs_guid,(void **)&fs)!=EFI_SUCCESS || !fs ||
        fs->OpenVolume(fs,&root)!=EFI_SUCCESS) return "MEDIA VOLUME";
    UINT8 *flash=w01_page(vmcb,W01Flash);
#ifdef HV_W03
    const char *e=w01_read_fd(root,L"\\W03CODE.FD",flash+W01VarsBytes,W01CodeBytes);
    if (!e) e=w01_read_fd(root,L"\\W03VARS.FD",flash,W01VarsBytes);
    if (!e) e=w01_read_fd(root,L"\\WINBOOT.EFI",w01_page(vmcb,W02Ram)+W03FileGpa,W03_FILE_SIZE);
#elif defined(HV_W02)
    const char *e=w01_read_fd(root,L"\\W02CODE.FD",flash+W01VarsBytes,W01CodeBytes);
    if (!e) e=w01_read_fd(root,L"\\W02VARS.FD",flash,W01VarsBytes);
#else
    const char *e=w01_read_fd(root,L"\\W01CODE.FD",flash+W01VarsBytes,W01CodeBytes);
    if (!e) e=w01_read_fd(root,L"\\W01VARS.FD",flash,W01VarsBytes);
#endif
    if (root->Close(root)!=EFI_SUCCESS && !e) e="VOLUME CLOSE";
    return e;
}
static BOOLEAN w01_digest(const UINT8 *p, UINTN n, const UINT8 pin[32])
{
    UINT8 actual[32];
    hv_sha256(p,n,actual);
    for (unsigned i=0;i<32;++i) if (actual[i]!=pin[i]) return FALSE;
    return TRUE;
}
#endif
