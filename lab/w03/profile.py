"""Layer the fixed W03 buffer handoff on the complete, unchanged W02 cold build."""
from pathlib import Path
import runpy

HERE = Path(__file__).resolve().parent
cold = runpy.run_path(str(HERE.parent / 'w02' / 'profile.py'))
once = cold['once']


def handoff(text):
    include = '#include "DxeMain.h"'
    text = once(text, include, include + '\n\n' + (HERE / 'BootManager.c.inc').read_text())
    # At this point DxeMain has built its own tables, installed its own
    # loaded-image handle, run library constructors and initialized events,
    # debug image info and memory protection. No BDS or disk is needed.
    anchor = '  CoreInitializeMemoryProtection ();'
    return once(text, anchor, anchor + '''

  W03StartBootManager ();
  // Rejection, failed StartImage, or an unexpected return: never retry,
  // continue to BDS, or synthesize success. The monitor has a finite budget.
  for (;;) {
    CpuDeadLoop ();
  }
''')


cold['install']('MdeModulePkg/Core/Dxe/DxeMain/DxeMain.c', handoff)


def immutable_vectors(text):
    # Apply only to the DXE library selected below; SEC/PEI use the same NASM
    # source. The old Xcode-style option fills MOV immediates in live .text.
    # Both ends of each direct jump relocate together, so no fixup is needed.
    anchor = '%ifdef SMM_DXE_NO_ABSOLUTE_RELOCS_IN_TEXT\n    mov     rax, strict qword 0    ;'
    text = once(text, anchor, '''%ifdef W03_IMMUTABLE_DXE_VECTORS
    jmp     near ASM_PFX(CommonInterruptEntry)
    ; The C hook-after copy uses the ordinary vector stride as its size.
    ; Keep all 18 bytes (push imm32, push rax, rel32 jump, seven NOPs).
    times 7 nop
%else
''' + anchor)
    text = once(text, '    jmp     rax\n%assign Vector Vector+1',
                '    jmp     rax\n%endif\n%assign Vector Vector+1')
    # The hook template is a source for CopyMem, not an IDT entry. Keep the
    # original absolute target so the template still works after copying it.
    # The normal PE loader applies this DIR64 relocation to data, not code.
    return once(text, '\nHookAfterStubHeaderBegin:', '''
%ifdef W03_IMMUTABLE_DXE_VECTORS
SECTION .data
ALIGN 8
%endif
HookAfterStubHeaderBegin:''')


exception_lib = 'UefiCpuPkg/Library/CpuExceptionHandlerLib/'
cold['install'](exception_lib + 'X64/ExceptionHandlerAsm.nasm', immutable_vectors)
cold['install'](exception_lib + 'DxeCpuExceptionHandlerLib.inf', lambda s: once(
    s, 'GCC:*_*_X64_NASM_FLAGS = -D SMM_DXE_NO_ABSOLUTE_RELOCS_IN_TEXT',
    'GCC:*_*_X64_NASM_FLAGS = -D W03_IMMUTABLE_DXE_VECTORS'))
