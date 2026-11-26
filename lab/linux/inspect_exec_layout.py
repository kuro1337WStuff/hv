"""Offline E06 discriminator: can ELF load flags alone authorize first execution?

Usage: python lab/linux/inspect_exec_layout.py PATH_TO_PINNED_VMLINUZ
Ranges are half-open; file offsets refer to the inflated ELF. The report concerns
the linked, uncompressed entry, not the relocated decompressor or runtime memory.
It never generates NPT permissions.
"""
import argparse
import gzip
import hashlib
from pathlib import Path
import struct


KERNEL = '1e8ac7aebfab3f0fcb443eae94ad320efbbda51be91b928c32ca1740b44ebe9d'
PHDR = struct.Struct('<IIQQQQQQ')
SHDR = struct.Struct('<IIQQQQIIQQ')
PAGE = 4096
# Linux v6.18.60 arch/x86/include/asm/alternative.h: packed alt_instr and
# ALTINSTR_ENTRY. The signed first field is relative to itself. instrlen
# includes old-site padding; a zero label difference describes no bytes.
ALT = struct.Struct('<i8xB1x')
VA_LIMIT = 1 << 64


def _report_alternatives(elf, sections, strs, page):
    end = page + PAGE
    if page < 0 or page % PAGE or end > VA_LIMIT:
        raise ValueError('invalid linked entry page')
    tables = [s for s in sections if strs[s[0]:strs.index(b'\0', s[0])] == b'.altinstructions']
    if len(tables) != 1:
        raise ValueError('expected one .altinstructions section')
    _, kind, flags, addr, off, size, _, _, _, entsize = tables[0]
    if (kind != 1 or not flags & 2 or flags & 0x800 or not size or
            entsize not in (0, ALT.size) or size % ALT.size or
            off + size > len(elf) or addr + size > VA_LIMIT):
        raise ValueError('invalid .altinstructions section')
    hits = []
    for pos in range(0, size, ALT.size):
        disp, length = ALT.unpack_from(elf, off + pos)
        start = addr + pos + disp
        stop = start + length
        if not 0 <= start < VA_LIMIT or stop > VA_LIMIT:
            raise ValueError('invalid alternative original VA span')
        lo, hi = max(page, start), min(end, stop)
        if lo < hi:
            hits.append((lo, hi, start, stop, pos // ALT.size))
    cursor, covered = page, 0
    for lo, hi, start, stop, index in sorted(hits):
        covered += max(0, hi - max(lo, cursor))
        cursor = max(cursor, hi)
        print(f'  alt record={index} original VA=[{start:#x},{stop:#x}) '
              f'intersection VA=[{lo:#x},{hi:#x})')
    print(f'.altinstructions linked VA page=[{page:#x},{end:#x}) '
          f'records={size // ALT.size} intersecting={len(hits)} '
          f'unique_original_starts={len({hit[2] for hit in hits})} unique_page_bytes={covered}')


def _report_sections(elf, sections, strs, start, end):
    cursor = start
    for s in sorted(sections, key=lambda s: s[3]):
        index, kind, flags, addr, offset, size, *_ = s
        lo, hi = max(start, addr), min(end, addr + size)
        if not flags & 2 or lo >= hi:
            continue
        if cursor < lo:
            print(f'  section gap VA=[{cursor:#x},{lo:#x})')
        name = strs[index:strs.index(b'\0', index)].decode('ascii')
        # gABI "Sections": sh_offset is independent of PT_LOAD.p_offset;
        # SHT_NOBITS has no bytes in the file. Never use sections for the hash.
        source = 'SHT_NOBITS (no file bytes)'
        if kind != 8:
            if offset + size > len(elf):
                raise ValueError('truncated allocated section file span')
            off = offset + lo - addr
            source = f'section file=[{off:#x},{off + hi - lo:#x})'
        print(f'  section={name} flags={flags:#x} VA=[{lo:#x},{hi:#x}) {source}')
        cursor = max(cursor, hi)
    if cursor < end:
        print(f'  section gap VA=[{cursor:#x},{end:#x})')


def _report_page(elf, entry, loads, sections, strs):
    start = entry & -PAGE
    end = start + PAGE
    print(f'Linked uncompressed entry page GPA=[{start:#x},{end:#x})')
    # System V gABI, "Program Loading" / PT_LOAD:
    # https://gabi.xinuos.com/elf/07-pheader.html
    # Only [p_paddr+p_filesz, p_paddr+p_memsz) is segment zero-fill here.
    page = bytearray()
    cursor = start
    for p in sorted(loads, key=lambda p: p[4]):
        _, flags, off, va, pa, filesz, memsz, _ = p
        lo, hi = max(start, pa), min(end, pa + memsz)
        if lo >= hi:
            continue
        if lo < cursor:
            raise ValueError(f'ambiguous overlapping PT_LOAD at GPA={lo:#x}')
        if cursor < lo:
            print(f'  unmapped GPA=[{cursor:#x},{lo:#x})')
        if filesz > memsz or (filesz and off + filesz > len(elf)):
            raise ValueError('invalid or truncated PT_LOAD file span')
        print(f'PT_LOAD flags={flags:#x} GPA=[{pa:#x},{pa + memsz:#x}) '
              f'VA=[{va:#x},{va + memsz:#x}) file_offset={off:#x} '
              f'filesz={filesz:#x} memsz={memsz:#x}')
        print(f'  page intersection GPA=[{lo:#x},{hi:#x}) '
              f'VA=[{va + lo - pa:#x},{va + hi - pa:#x})')
        split = max(lo, min(hi, pa + filesz))
        if lo < split:
            offset = off + lo - pa
            length = split - lo
            print(f'  file-backed GPA=[{lo:#x},{split:#x}) '
                  f'file=[{offset:#x},{offset + length:#x}) length={length:#x}')
            page.extend(elf[offset:offset + length])
        if split < hi:
            print(f'  ELF zero-fill GPA=[{split:#x},{hi:#x}) length={hi - split:#x}')
            page.extend(bytes(hi - split))
        _report_sections(elf, sections, strs, va + lo - pa, va + hi - pa)
        cursor = hi
    if cursor < end:
        print(f'  unmapped GPA=[{cursor:#x},{end:#x})')
    if len(page) != PAGE:
        raise ValueError('entry page not completely covered by PT_LOAD')
    print(f'Linked page SHA256={hashlib.sha256(page).hexdigest()} length={len(page)}')


IMAGE_BYTES = 12649472


def extract_pinned_elf(image):
    """Authenticate the complete bzImage before inflating its one known payload."""
    if len(image) != IMAGE_BYTES or hashlib.sha256(image).hexdigest() != KERNEL:
        raise ValueError('unreviewed kernel; no executable layout decision')
    setup = (image[0x1f1] + 1) * 512
    off, size = struct.unpack_from('<II', image, 0x248)
    if not size or setup + off + size > len(image):
        raise ValueError('truncated compressed payload')
    elf = gzip.decompress(memoryview(image)[setup + off:setup + off + size])
    if elf[:6] != b'\x7fELF\x02\x01' or struct.unpack_from('<H', elf, 18)[0] != 62:
        raise ValueError('expected x86-64 little-endian ELF')
    return elf


def inspect(image):
    elf = extract_pinned_elf(image)
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
    _report_page(elf, gpa, loads, sections, strs)
    _report_alternatives(elf, sections, strs, entry_va & -PAGE)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('vmlinuz', type=Path)
    args = parser.parse_args()
    try:
        inspect(args.vmlinuz.read_bytes())
    except (ValueError, OSError) as err:
        parser.exit(1, f'inspect_exec_layout: {err}\n')
