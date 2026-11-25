"""Disassemble the reviewed CPU-floor switch in an EFI file; never execute it.

Usage: python lab/linux/audit_state_switch.py build/L1CPUFLOOR.EFI
Requires capstone 5 and pefile. A different linked .text requires a new review.
"""
import argparse
from hashlib import sha256
from pathlib import Path

import capstone
import pefile


TEXT_SHA256 = "45eeae13f6b5b7ed75d7af2543d5356acf0d44d01642889382dbd0c96eaf3415"
# RVAs from the reviewed PE unwind records + matching assembler object symbols.
FUNCTIONS = (("RunLegacyVcpu", 0x28E0, 0x2ABB),
             ("FpFixture", 0x2ABB, 0x2CA2),
             ("ObserveLegacyRun", 0x2CA2, 0x2D91))


def audit(path):
    with pefile.PE(data=path.read_bytes()) as image:
        text = next((s for s in image.sections if s.Name.rstrip(b"\0") == b".text"), None)
        if image.FILE_HEADER.Machine != 0x8664 or text is None or text.VirtualAddress != 0x1000:
            raise ValueError("not the reviewed x64 EFI code layout")
        digest = sha256(text.get_data(length=text.Misc_VirtualSize)).hexdigest()
        if digest != TEXT_SHA256:
            raise ValueError(f"unreviewed .text SHA-256 {digest}; do not reuse these RVAs")
        ranges = {(entry.struct.BeginAddress, entry.struct.EndAddress)
                  for entry in getattr(image, "DIRECTORY_ENTRY_EXCEPTION", ())}
        decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
        for name, start, end in FUNCTIONS:
            if (start, end) not in ranges:
                raise ValueError(f"missing unwind range for {name}")
            print(f"{name} [{start:04x},{end:04x})")
            cursor = start
            for ins in decoder.disasm(image.get_data(start, end - start), start):
                if ins.address != cursor:
                    raise ValueError(f"decode gap at RVA {cursor:x}")
                cursor += ins.size
                if ins.mnemonic not in {"mov", "sub", "add"}:
                    print(f"  {ins.address:04x}: {ins.mnemonic} {ins.op_str}".rstrip())
            if cursor != end:
                raise ValueError(f"incomplete decode at RVA {cursor:x}")
        print("Reviewed legacy switch only; E03 B/C state ownership is NOT established.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path)
    try:
        audit(parser.parse_args().image)
    except (OSError, ValueError, pefile.PEFormatError) as error:
        parser.exit(1, f"audit stopped: {error}\n")
