// W02: only the RAM aperture backed privately by this one board is advertised.
// No CMOS sizing, S3, PCI enumeration, DMA, storage or OS boot contract.
#include <PiPei.h>
#include <Ppi/MasterBootMode.h>
#include <Library/HobLib.h>
#include <Library/PcdLib.h>
#include <Library/PeiServicesLib.h>

STATIC CONST EFI_PEI_PPI_DESCRIPTOR mBootMode = {
  EFI_PEI_PPI_DESCRIPTOR_PPI | EFI_PEI_PPI_DESCRIPTOR_TERMINATE_LIST,
  &gEfiPeiMasterBootModePpiGuid, NULL
};

EFI_STATUS EFIAPI BoardPei (IN EFI_PEI_FILE_HANDLE File, IN CONST EFI_PEI_SERVICES **Services)
{
  EFI_STATUS Status;
  UINTN DxeBase = PcdGet32 (PcdOvmfDxeMemFvBase);
  UINTN DxeSize = PcdGet32 (PcdOvmfDxeMemFvSize);
  UINT64 Base = SIZE_32MB;
  UINT64 Size = SIZE_128MB - SIZE_32MB;

  Status = PeiServicesSetBootMode (BOOT_WITH_FULL_CONFIGURATION);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  BuildResourceDescriptorHob (EFI_RESOURCE_SYSTEM_MEMORY,
    EFI_RESOURCE_ATTRIBUTE_PRESENT | EFI_RESOURCE_ATTRIBUTE_INITIALIZED |
    EFI_RESOURCE_ATTRIBUTE_TESTED | EFI_RESOURCE_ATTRIBUTE_WRITE_BACK_CACHEABLE,
    Base, Size);
  BuildCpuHob (32, 16);
  BuildMemoryAllocationHob (PcdGet32 (PcdOvmfPeiMemFvBase), PcdGet32 (PcdOvmfPeiMemFvSize), EfiBootServicesData);
  BuildMemoryAllocationHob (DxeBase, DxeSize, EfiBootServicesData);
  BuildFvHob (DxeBase, DxeSize);
  PeiServicesInstallFvInfoPpi (NULL, (VOID *)DxeBase, DxeSize, NULL, NULL);
  Status = PeiServicesInstallPpi (&mBootMode);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  // PEI Core, not this PEIM or L1, migrates the temporary heap and stack.
  return PeiServicesInstallPeiMemory (Base, Size);
}
