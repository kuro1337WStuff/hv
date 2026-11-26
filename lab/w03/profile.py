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
