#ifndef HV_LINUX_MEDIA_H
#define HV_LINUX_MEDIA_H

#include <Uefi.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleFileSystem.h>

/* Byte offsets within private, page-backed staging, NOT guest addresses. */
enum {
    LinuxImageOff = 0, LinuxImageLen = 12649472,
    LinuxRdOff = LinuxImageOff + LinuxImageLen, LinuxRdLen = 1476964,
    LinuxCmdOff = LinuxRdOff + LinuxRdLen, LinuxCmdLen = 123,
    LinuxMediaBytes = LinuxCmdOff + LinuxCmdLen,
    LinuxMediaPages = (LinuxMediaBytes + EFI_PAGE_SIZE - 1) / EFI_PAGE_SIZE
};

static const char *linux_read_file(EFI_FILE_PROTOCOL *root, CHAR16 *name,
                                   UINT8 *dest, UINTN bytes)
{
    EFI_FILE_PROTOCOL *file;
    if (root->Open(root, &file, name, EFI_FILE_MODE_READ, 0) != EFI_SUCCESS)
        return "MEDIA OPEN";

    const char *error = NULL;
    UINTN count = bytes;
    if (file->Read(file, &count, dest) != EFI_SUCCESS || count != bytes) {
        error = "MEDIA READ";
    } else {
        UINT8 extra;
        count = 1;
        if (file->Read(file, &count, &extra) != EFI_SUCCESS || count != 0)
            error = "MEDIA EOF";
    }
    if (file->Close(file) != EFI_SUCCESS && !error) error = "MEDIA CLOSE";
    return error;
}

/* Boot-services phase: stage exact file lengths. linux_verified_admit hashes
 * the retained bytes again after handoff, before any guest entry. */
static const char *linux_read_media(EFI_HANDLE image, EFI_SYSTEM_TABLE *st, UINT8 *stage)
{
    EFI_GUID image_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_GUID fs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_LOADED_IMAGE_PROTOCOL *loaded;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs;
    EFI_FILE_PROTOCOL *root;
    EFI_BOOT_SERVICES *bs = st->BootServices;
    if (bs->HandleProtocol(image, &image_guid, (void **)&loaded) != EFI_SUCCESS ||
        !loaded || !loaded->DeviceHandle) return "MEDIA IMAGE";
    if (bs->HandleProtocol(loaded->DeviceHandle, &fs_guid, (void **)&fs) != EFI_SUCCESS ||
        !fs) return "MEDIA FS";
    if (fs->OpenVolume(fs, &root) != EFI_SUCCESS) return "MEDIA VOLUME";

    const char *error = linux_read_file(root, L"VMLINUZ.BIN", stage + LinuxImageOff, LinuxImageLen);
    if (!error) error = linux_read_file(root, L"INITRD.BIN", stage + LinuxRdOff, LinuxRdLen);
    if (!error) error = linux_read_file(root, L"CMDLINE.BIN", stage + LinuxCmdOff, LinuxCmdLen);
    if (root->Close(root) != EFI_SUCCESS && !error) error = "MEDIA CLOSE";
    return error;
}

#endif
