// W03 has no disk controller. Only this guest-private, immutable source
// buffer is an image input. The allocator sees [32 MiB,128 MiB), not the file.
STATIC VOID
W03StartBootManager (VOID)
{
  typedef struct {
    UINT32 Magic;
    UINT32 Length;
  } W03_FILE_DESCRIPTOR;
  CONST W03_FILE_DESCRIPTOR *Descriptor = (CONST VOID *)(UINTN)0x00fff000;
  EFI_BOOT_SERVICES *Boot = gDxeCoreST->BootServices;
  EFI_HANDLE Child = NULL;
  EFI_LOADED_IMAGE_PROTOCOL *Loaded = NULL;
  EFI_STATUS Status;
  UINTN Size = Descriptor->Length;

  if ((Descriptor->Magic != 0x33424d57U) || (Size == 0) || (Size > SIZE_16MB)) {
    return;
  }

  Status = Boot->LoadImage (FALSE, gDxeCoreImageHandle, NULL,
                  (VOID *)(UINTN)SIZE_16MB, Size, &Child);
  if ((Status != EFI_SUCCESS) || (Child == NULL)) {
    return;
  }
  Status = Boot->HandleProtocol (Child, &gEfiLoadedImageProtocolGuid, (VOID **)&Loaded);
  if ((Status != EFI_SUCCESS) || (Loaded == NULL) ||
      (Loaded->ParentHandle != gDxeCoreImageHandle) || (Loaded->SystemTable != gDxeCoreST) ||
      (Loaded->ImageBase == NULL) || (Loaded->ImageSize == 0)) {
    return;
  }

  // These registers describe the actual guest service result at the measured
  // OUT. No register supplies an entry address; only CoreStartImage knows it.
  {
    register VOID *Base __asm__ ("r8") = Loaded->ImageBase;
    register EFI_HANDLE Handle __asm__ ("r9") = Child;
    register EFI_SYSTEM_TABLE *SystemTable __asm__ ("r11") = Loaded->SystemTable;
    __asm__ __volatile__ ("movl $0x33594452, %%r10d; outl %%eax, %%dx"
      : : "a" ((UINT32)0x33594452), "d" ((UINT16)0x505),
          "r" (Base), "r" (Handle), "r" (SystemTable) : "r10", "memory");
  }
  Boot->StartImage (Child, NULL, NULL);
}
