"""Offline E06 discriminator: can ELF load flags alone authorize first execution?

Only inspects the pinned diskless kernel. It never generates NPT permissions.
"""
import argparse
import gzip
import hashlib
from pathlib import Path
import struct


KERNEL = '1e8ac7aebfab3f0fcb443eae94ad320efbbda51be91b928c32ca1740b44ebe9d'
PHDR = struct.Struct('<IIQQQQQQ')
SHDR = struct.Struct('<IIQQQQIIQQ')


def inspect(image):
    if len(image) != 12649472 or hashlib.sha256(image).hexdigest() != KERNEL:
        raise ValueError('unreviewed kernel; no executable layout decision')
    setup = (image[0x1f1] + 1) * 512
    off, size = struct.unpack_from('<II', image, 0x248)
    elf = gzip.decompress(image[setup + off:setup + off + size])
    if elf[:6] != b'\x7fELF\x02\x01' or struct.unpack_from('<H', elf, 18)[0] != 62:
        raise ValueError('expected x86-64 little-endian ELF')
    gpa, phoff, shoff = struct.unpack_from('<QQQ', elf, 24)
    phsize, phnum, shsize, shnum, names = struct.unpack_from('<5H', elf, 54)
    if phsize != PHDR.size or shsize != SHDR.size:
        raise ValueError('unexpected ELF entry sizes')
    loads = [p for i in range(phnum) if (p := PHDR.unpack_from(elf, phoff + i*phsize))[0] == 1]
    sections = [SHDR.unpack_from(elf, shoff + i*shsize) for i in range(shnum)]
    strings = sections[names]
    strs = elf[strings[4]:strings[4]+strings[5]]
    # This pinned x86 kernel uses a physical e_entry; section addresses are linked high.
    candidates = [p for p in loads if p[4] <= gpa < p[4]+p[5]]
    if len(candidates) != 1:
        raise ValueError('entry has no unique file-backed physical segment')
    p = candidates[0]
    delta = gpa - p[4]
    entry_va = p[3] + delta
    owners = [s for s in sections if s[2] & 2 and s[3] <= entry_va < s[3]+s[5]]
    if len(owners) != 1:
        raise ValueError('entry has no unique allocated section')
    s = owners[0]
    name = strs[s[0]:strs.index(b'\0', s[0])].decode('ascii')
    first = elf[p[2]+delta:p[2]+delta+3]
    print(f'ELF entry GPA={gpa:#x} PT_LOAD flags={p[1]:#x} section={name} flags={s[2]:#x} bytes={first.hex()}')
    if (gpa, p[1], name, bool(s[2] & 4), first) != (0x30047c0, 6, '.init.text', True, bytes.fromhex('4989f7')):
        raise ValueError('pinned E06 counterexample not established')
    print('No-go: PT_LOAD lacks execute at the real entry. Making the RW segment executable would grant W+X. Keep Linux NX.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('vmlinuz', type=Path)
    args = parser.parse_args()
    try:
        inspect(args.vmlinuz.read_bytes())
    except (ValueError, OSError) as err:
        parser.exit(1, f'inspect_exec_layout: {err}\n')
