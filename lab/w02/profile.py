"""Materialize the fixed cold W02 profile, only against build.sh's pinned edk2 tree."""
from pathlib import Path
import shutil

HERE = Path(__file__).resolve().parent
BOARD = Path('OvmfPkg/W02')
BOARD.mkdir()
for name in ('BoardPei.c', 'BoardPei.inf'):
    shutil.copyfile(HERE / name, BOARD / name)


def once(text, needle, replacement):
    if text.count(needle) != 1:
        raise ValueError(f'upstream anchor not unique: {needle!r}')
    return text.replace(needle, replacement)


def between(text, start, stop, replacement):
    if text.count(start) != 1 or text.count(stop) != 1:
        raise ValueError(f'upstream region not unique: {start!r} .. {stop!r}')
    a, b = text.index(start), text.index(stop)
    if b <= a:
        raise ValueError('reversed region')
    return text[:a] + replacement + text[b:]


def install(path, transform):
    path = Path(path)
    data = path.read_bytes()
    eol = '\r\n' if b'\r\n' in data else '\n'
    text = transform(data.decode().replace('\r\n', '\n'))
    path.write_bytes(text.replace('\n', eol).encode())


# Uncompressed, native EFI_SECTION_FIRMWARE_VOLUME_IMAGE pairs: no LZMA,
# no S3/prepopulated memory. Core/PCD/board/IPL are the whole PEI dispatch
# list; there are deliberately no DXE drivers, disks, boot manager or OS.
def fdf(text):
    text = between(text, '#\n#  PEI Phase modules\n#', '[FV.DXEFV]', '''INF MdeModulePkg/Core/Pei/PeiMain.inf
INF MdeModulePkg/Universal/PCD/Pei/Pcd.inf
INF OvmfPkg/W02/BoardPei.inf
INF MdeModulePkg/Core/DxeIplPeim/DxeIpl.inf

''')
    text = between(text, 'APRIORI DXE {', '[FV.FVMAIN_COMPACT]', 'INF MdeModulePkg/Core/Dxe/DxeMain.inf\n\n')
    # GenFds regenerates directly visible child FVs using their *flash*
    # addresses. They execute after copying, so specify the final RAM base.
    # Fix the block count even when the child is built outside [FD.MEMFD].
    for vol, base in (('PEIFV', 0x830000), ('DXEFV', 0x900000)):
        header = f'[FV.{vol}]\n'
        text = once(text, header, header + f'FvBaseAddress = 0x{base:x}\n')
    block = 'BlockSize          = 0x10000\n'
    if text.count(block) != 2:
        raise ValueError('unexpected nested FV block definitions')
    text = text.replace(block, block + 'NumBlocks          = 4\n')
    return between(text, 'FILE FV_IMAGE =', '!include OvmfPkg/Include/Fdf/FvmainCompactScratchEnd.fdf.inc', '''FILE FV_IMAGE = 9E21FD93-9C72-4c15-8C4B-E77F1DB2D792 {
  SECTION FV_IMAGE = PEIFV
  SECTION FV_IMAGE = DXEFV
}

''')


install('OvmfPkg/OvmfPkgX64.fdf', fdf)
install('OvmfPkg/Include/Fdf/MemFd.fdf.inc', lambda s: once(once(s,
        '0x030000|0x0D0000', '0x030000|0x040000'), '0x100000|0xE80000', '0x100000|0x040000'))


def dsc(text):
    # Each PEI executable section gets its own RO/X pages, independent of
    # mutable data. Preserve reset and SEC's original XIP linker settings.
    text = once(text, '[BuildOptions.common.EDKII.DXE_RUNTIME_DRIVER,', '''[BuildOptions.common.EDKII.PEI_CORE, BuildOptions.common.EDKII.PEIM]
  GCC:*_*_X64_DLINK_XIPFLAGS = -z common-page-size=0x1000

[BuildOptions.common.EDKII.DXE_RUNTIME_DRIVER,''')
    text = once(text, '[PcdsFeatureFlag]', '''[PcdsFeatureFlag]
  gEfiMdeModulePkgTokenSpaceGuid.PcdDxeIplBuildPageTables|FALSE''')
    return once(text, '  OvmfPkg/PlatformPei/PlatformPei.inf {', '  OvmfPkg/W02/BoardPei.inf {')


install('OvmfPkg/OvmfPkgX64.dsc', dsc)
# Use the library's upstream non-absolute IDT variant. No relocations in
# hash-admitted DXE text; the OUT terminates the experiment before IDT setup.
install('UefiCpuPkg/Library/CpuExceptionHandlerLib/DxeCpuExceptionHandlerLib.inf',
        lambda s: once(s, '  XCODE:*_*_X64_NASM_FLAGS', '  GCC:*_*_X64_NASM_FLAGS = -D SMM_DXE_NO_ABSOLUTE_RELOCS_IN_TEXT\n  XCODE:*_*_X64_NASM_FLAGS'))


def sec(text):
    # Child FVs now occur verbatim in flash. A page-aligned child header is
    # not FVMAIN: the outer volume must abut the boot SECFV, by FDF design.
    text = once(text, 'if ((UINTN)Fv->FvLength > Distance)',
                'if (Fv->FvLength != Distance - (*BootFv)->FvLength)')
    text = between(text, '/**\n  Locates the compressed main firmware volume', '/**\n  Locates the PEI Core entry point address\n\n  @param[in]  Fv ', (HERE / 'ColdFv.c.inc').read_text() + '\n')
    # Cold-only. Never accept a CMOS resume request or trust stale RAM FVs.
    text = between(text, '/**\n  Reads 8-bits of CMOS data.', '/**\n  Find core image base.', '''VOID
FindPeiCoreImageBase (
  IN OUT EFI_FIRMWARE_VOLUME_HEADER **BootFv,
  OUT EFI_PHYSICAL_ADDRESS *PeiCoreImageBase
  )
{
  if (EFI_ERROR (FindMainFv (BootFv)) || EFI_ERROR (W02CopyMemFvs (BootFv)) ||
      EFI_ERROR (FindPeiCoreImageBaseInFv (*BootFv, PeiCoreImageBase))) {
    CpuDeadLoop ();
  }
}

''')
    return text


install('OvmfPkg/Sec/SecMain.c', sec)
pei = '    Status = PeiServicesInstallPpi (&mMemoryDiscoveredPpi);'
install('MdeModulePkg/Core/Pei/PeiMain/PeiMain.c', lambda s: once(s, pei, pei + '''
    if (EFI_ERROR (Status) || !PrivateData.PeiMemoryInstalled) {
      CpuDeadLoop ();
    }
    {
      EFI_PHYSICAL_ADDRESS Page;
      volatile UINT32 *Words;
      Status = PeiServicesAllocatePages (EfiBootServicesData, 1, &Page);
      if (EFI_ERROR (Status) || (Page & EFI_PAGE_MASK) != 0 ||
          Page < SIZE_32MB || Page > SIZE_128MB - EFI_PAGE_SIZE) {
        CpuDeadLoop ();
      }
      Words = (volatile UINT32 *)(UINTN)Page;
      Words[0] = 0x32494550U;
      Words[EFI_PAGE_SIZE / sizeof (*Words) - 1] = ~(UINT32)Page;
      __asm__ __volatile__ ("movl $0x32494550, %%r10d; outl %%eax, %%dx"
        : : "a" ((UINT32)Page), "d" ((UINT16)0x505) : "r10", "memory");
    }
'''))
dxe = '  //\n  // Setup the default exception handlers'
install('MdeModulePkg/Core/Dxe/DxeMain/DxeMain.c', lambda s: once(s, dxe, '''  __asm__ __volatile__ ("movl $0x32455844, %%r10d; outl %%eax, %%dx"
    : : "a" ((UINT32)0x32455844), "d" ((UINT16)0x505) : "r10", "memory");

''' + dxe))
