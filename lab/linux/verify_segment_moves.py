"""Offline witness for Linux v6.18.60 arch/x86/boot/compressed/misc.c:parse_elf.

For the pinned relocatable x86-64 vmlinuz-virt only. Snapshot the headers, then
move each PT_LOAD from output + p_offset to output + (p_paddr - LOAD_PHYSICAL_ADDR)
in table order. This step copies p_filesz, not p_memsz. Coordinates in the JSON
report are half-open byte offsets from output unless explicitly marked physical.
No execution, address randomization, subsequent relocations, BSS clear, or NPT.
"""
import argparse
from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import struct

if __package__:
    from . import inspect_exec_layout as layout
else:
    import inspect_exec_layout as layout


LOAD_PHYSICAL_ADDR = 0x1000000  # Pinned CONFIG_PHYSICAL_START and CONFIG_PHYSICAL_ALIGN.
ENTRY = 0x30047c0
# Trial 102: independent .init.text file offset and linked (not decompressor) page.
PAGE_FILE = 0x2204000
PAGE_HASH = 'b62d37075a019e3a4d75779cab2f5903ad736ce73d2a8b57d53c716cb8de2d52'
GUARD = 0xa5


def digest(data):
    return hashlib.sha256(data).hexdigest()


@dataclass(frozen=True)
class Move:
    index: int
    src: int
    dst: int
    filesz: int
    memsz: int


def snapshot(elf):
    entry, phoff = struct.unpack_from('<QQ', elf, 24)
    stride, count = struct.unpack_from('<HH', elf, 54)
    if entry != ENTRY or stride != layout.PHDR.size or phoff + stride * count > len(elf):
        raise ValueError('unexpected pinned ELF header or program header extent')
    moves = []
    for i in range(count):
        kind, _, src, _, pa, filesz, memsz, _ = layout.PHDR.unpack_from(elf, phoff + i * stride)
        if kind != 1:
            continue
        dst = pa - LOAD_PHYSICAL_ADDR
        if not (0 <= dst < src < dst + filesz and filesz <= memsz and
                src + filesz <= len(elf) and dst + memsz <= len(elf)):
            raise ValueError('expected bounded leftward overlapping PT_LOAD move')
        moves.append(Move(i, src, dst, filesz, memsz))
    if len(moves) != 2:
        raise ValueError('expected two PT_LOAD records')
    a, b = moves
    page = (entry & -layout.PAGE) - LOAD_PHYSICAL_ADDR
    if not (a.dst == 0 and a.filesz == a.memsz and a.dst + a.filesz <= b.dst and
            b.filesz < b.memsz and b.src + b.filesz < len(elf) < b.src + b.memsz and
            b.dst <= page and page + layout.PAGE <= b.dst + b.filesz and
            b.src + page - b.dst == PAGE_FILE):
        raise ValueError('unexpected pinned segment, tail or linked entry layout')
    return moves


def copy_blocks(buf, move, length, descending):
    src, dst = move.src, move.dst
    if not (0 <= dst < src and 0 < length <= len(buf) - src and length <= len(buf) - dst):
        raise ValueError('copy outside modeled buffer')
    # Each individual block is disjoint; across blocks only ascending order
    # preserves unread bytes in these two overlapping leftward moves.
    step = min(65536, src - dst)
    offsets = range(0, length, step)
    for pos in reversed(offsets) if descending else offsets:
        end = min(pos + step, length)
        buf[dst + pos:dst + end] = buf[src + pos:src + end]


def model(elf, moves, case):
    original = memoryview(elf)
    buf = bytearray(elf)
    last = moves[1]
    if case == 'p_memsz':
        # Only the bad read needs bytes beyond the inflated ELF. Represent its
        # unknown suffix explicitly; establish tail damage on real bytes alone below.
        buf.extend(bytes([GUARD]) * (last.src + last.memsz - len(elf)))
    work = memoryview(buf)
    copied = []
    for m in moves:
        length = m.memsz if case == 'p_memsz' and m == last else m.filesz
        descending = case == 'descending_first' and m == moves[0]
        copy_blocks(work, m, length, descending)
        copied.append({'phdr': m.index, 'order': 'descending' if descending else 'ascending',
                       'source': [m.src, m.src + length], 'destination': [m.dst, m.dst + length],
                       'overlap_bytes': m.dst + length - m.src, 'copied_bytes': length})

    # Compare final destinations after BOTH moves, not the bytes just after each copy.
    failures = []
    for m, record in zip(moves, copied):
        expect = digest(original[m.src:m.src + m.filesz])
        got = digest(work[m.dst:m.dst + m.filesz])
        if got != expect:
            failures.append(f'phdr_{m.index}_file_bytes_changed')
        record.update(p_filesz=m.filesz, p_memsz=m.memsz, file_source_sha256=expect,
                      final_file_destination_sha256=got)

    tail = last.dst + last.filesz
    end = last.dst + last.memsz
    before, after = original[tail:end], work[tail:end]
    nonzero = len(before) - elf.count(0, tail, end)
    if not nonzero:
        raise ValueError('expected nonzero preexisting tail evidence')
    if after != before:
        failures.append('memsz_only_tail_changed')
    tail_report = {'range': [tail, end], 'before_nonzero_bytes': nonzero,
                   'before_sha256': digest(before), 'after_sha256': digest(after)}
    if case == 'p_memsz':
        extra = last.src + last.filesz
        real = len(elf) - extra
        if after[:real] != original[extra:] or after[:real] == before[:real]:
            raise ValueError('missing tail corruption from real inflated bytes')
        if buf.count(GUARD, tail + real, end) != end - (tail + real):
            raise ValueError('incomplete modeled suffix copy')
        copied[-1]['final_memsz_destination_sha256'] = digest(work[last.dst:end])
        tail_report.update(extra_inflated_source=[extra, len(elf)],
                           corrupted_prefix_compared=[tail, tail + real],
                           synthetic_source=[len(elf), len(work)], synthetic_fill=GUARD)

    page = (ENTRY & -layout.PAGE) - LOAD_PHYSICAL_ADDR
    entry_page = work[page:page + layout.PAGE]
    page_hash = digest(entry_page)
    if page_hash != PAGE_HASH or entry_page != original[PAGE_FILE:PAGE_FILE + layout.PAGE]:
        failures.append('linked_entry_page_changed')
    expected = {'p_filesz': [], 'descending_first': [f'phdr_{moves[0].index}_file_bytes_changed'],
                'p_memsz': ['memsz_only_tail_changed']}[case]
    if failures != expected:
        raise ValueError(f'{case}: expected {expected}, observed {failures}')
    return {'case': case, 'moves': copied, 'memsz_only_tail': tail_report,
            'final_entry_page_sha256': page_hash, 'mismatches': failures}


def witness(image):
    elf = layout.extract_pinned_elf(image)
    moves = snapshot(elf)
    cases = [model(elf, moves, case) for case in ('p_filesz', 'descending_first', 'p_memsz')]
    damaged = bytearray(image)
    damaged[0] ^= 1
    try:
        layout.extract_pinned_elf(damaged)
    except ValueError as exc:
        if str(exc) != 'unreviewed kernel; no executable layout decision':
            raise
        refusal = str(exc)
    else:
        raise ValueError('changed image was not refused')
    page = ENTRY & -layout.PAGE
    return {'scope': 'Linux v6.18.60 parse_elf PT_LOAD moves only; offline, no guest execution',
            'vmlinuz_sha256': layout.KERNEL, 'inflated_bytes': len(elf),
            'linked_physical_base': LOAD_PHYSICAL_ADDR, 'linked_physical_entry': ENTRY,
            'linked_physical_page': [page, page + layout.PAGE],
            'linked_page_file': [PAGE_FILE, PAGE_FILE + layout.PAGE],
            'expected_entry_page_sha256': PAGE_HASH, 'cases': cases, 'changed_image_refusal': refusal}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('vmlinuz', type=Path)
    args = parser.parse_args()
    try:
        with args.vmlinuz.open('rb') as source:
            image = source.read(layout.IMAGE_BYTES + 1)
        print(json.dumps(witness(image), sort_keys=True))
    except (OSError, ValueError) as err:
        parser.exit(1, f'verify_segment_moves: {err}\n')
