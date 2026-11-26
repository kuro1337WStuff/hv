// Cold, uncompressed variant of OVMF SEC's MemFd path. Walk the original
// FFS object and both PI FV_IMAGE sections in guest code; never execute the
// flash copy as an alternative entry. Payloads have the fixed MEMFD sizes.
STATIC EFI_STATUS
W02CopyMemFvs (IN OUT EFI_FIRMWARE_VOLUME_HEADER **Fv)
{
  EFI_FIRMWARE_VOLUME_HEADER *Outer = *Fv;
  UINTN End = (UINTN)Outer + (UINTN)Outer->FvLength;
  UINTN Cursor = ALIGN_VALUE ((UINTN)Outer + Outer->HeaderLength, 8);
  UINT32 Bases[2] = { PcdGet32 (PcdOvmfPeiMemFvBase), PcdGet32 (PcdOvmfDxeMemFvBase) };
  UINT32 Sizes[2] = { PcdGet32 (PcdOvmfPeiMemFvSize), PcdGet32 (PcdOvmfDxeMemFvSize) };

  while (Cursor < End && End - Cursor >= sizeof (EFI_FFS_FILE_HEADER)) {
    EFI_FFS_FILE_HEADER *File = (EFI_FFS_FILE_HEADER *)Cursor;
    UINT32 Size = FFS_FILE_SIZE (File);
    UINTN Idx;

    if (Size < sizeof (*File) || Size > End - Cursor || Size == 0xffffff) {
      return EFI_VOLUME_CORRUPTED;
    }
    if (File->Type == EFI_FV_FILETYPE_FIRMWARE_VOLUME_IMAGE) {
      for (Idx = 0; Idx < 2; ++Idx) {
        EFI_COMMON_SECTION_HEADER *Section;
        EFI_FIRMWARE_VOLUME_HEADER *Source;
        EFI_STATUS Status = FindFfsSectionInstance (File + 1, Size - sizeof (*File),
          EFI_SECTION_FIRMWARE_VOLUME_IMAGE, Idx, &Section);
        if (EFI_ERROR (Status) || SECTION_SIZE (Section) != Sizes[Idx] + sizeof (*Section)) {
          return EFI_VOLUME_CORRUPTED;
        }
        Source = (EFI_FIRMWARE_VOLUME_HEADER *)(Section + 1);
        if (Source->Signature != EFI_FVH_SIGNATURE || Source->FvLength != Sizes[Idx]) {
          return EFI_VOLUME_CORRUPTED;
        }
        CopyMem ((VOID *)(UINTN)Bases[Idx], Source, Sizes[Idx]);
      }
      *Fv = (EFI_FIRMWARE_VOLUME_HEADER *)(UINTN)Bases[0];
      return EFI_SUCCESS;
    }
    Cursor = ALIGN_VALUE (Cursor + Size, 8);
  }
  return EFI_NOT_FOUND;
}
