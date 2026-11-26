"""Inventory the pinned relocated decompressor's half-open BSS clear, offline."""
import argparse
import hashlib
from pathlib import Path
import struct

if __package__:
    from . import verify_relocation as relocation
else:  # Direct CLI invocation.
    import verify_relocation as relocation


BSS_START = 0x35cf400
BSS_END = 0x35e88e8
PAGE = relocation.PAGE
WINDOW_START = BSS_START & -PAGE
WINDOW_END = (BSS_END + PAGE - 1) & -PAGE
POISON = 0xa5
# startup_64: last source and destination qwords, byte count, SHR/STD/REP MOVSQ/CLD.
COPY_STUB = bytes.fromhex('488d35 66c1c000 488dbb f8c3c000 b9 00c4c000 c1e903 fd f348a5 fc')
# Relocated entry: XOR EAX,EAX; two RIP-relative LEAs; SUB RCX,RDI;
# SHR RCX,3; REP STOSQ. The LEA targets are decoded below, not assumed.
CLEAR_STUB = bytes.fromhex('31c0 488d3d f7e30000 488d0d d8780200 4829f9 48c1e903 f348ab')


def layout(image):
    """Derive the byte range from the authenticated copy and clear instructions."""
    if (len(image) != relocation.IMAGE_BYTES or
            hashlib.sha256(image).hexdigest() != relocation.IMAGE_HASH):
        raise ValueError('unreviewed image')
    setup = (image[0x1f1] + 1) * 512
    copied = struct.unpack_from('<I', image, 0x1f4)[0] * 16
    site = 0x28b
    stub = image[setup + site:setup + site + len(COPY_STUB)]
    if stub != COPY_STUB:
        raise ValueError('unexpected backward copy sequence')
    source_last = site + 7 + struct.unpack_from('<i', stub, 3)[0]
    dest_last = struct.unpack_from('<i', stub, 10)[0]
    count = struct.unpack_from('<I', stub, 15)[0]
    if not (setup + copied == len(image) and copied % 8 == 0 and
            source_last + 8 == dest_last + 8 == count == copied):
        raise ValueError('unexpected backward copy extent')

    entry = setup + relocation.ENTRY - relocation.COPY_BASE
    stub = image[entry:entry + len(CLEAR_STUB)]
    if stub != CLEAR_STUB:
        raise ValueError('unexpected BSS clear sequence')
    start = relocation.ENTRY + 9 + struct.unpack_from('<i', stub, 5)[0]
    end = relocation.ENTRY + 16 + struct.unpack_from('<i', stub, 12)[0]
    if (start != relocation.COPY_BASE + copied or (start, end) != (BSS_START, BSS_END)
            or (end - start) % 8):
        raise ValueError('unexpected BSS clear extent')
    first = setup + WINDOW_START - relocation.COPY_BASE
    return start, end, first, image[first:setup + copied]


def model(prefix, start, end):
    """Model one clear in a bounded window; never write to the source image."""
    if not WINDOW_START <= start <= end <= WINDOW_END:
        raise ValueError('clear outside model window')
    if len(prefix) != BSS_START - WINDOW_START:
        raise ValueError('incomplete file-backed prefix')
    window = bytearray([POISON]) * (WINDOW_END - WINDOW_START)
    window[:len(prefix)] = prefix
    window[start - WINDOW_START:end - WINDOW_START] = bytes(end - start)
    return window


def check(window, prefix):
    lo, hi = BSS_START - WINDOW_START, BSS_END - WINDOW_START
    if len(window) != WINDOW_END - WINDOW_START:
        raise ValueError('incomplete window')
    # Compare every byte; the file-backed byte just before BSS can already be zero.
    if window[:lo] != prefix:
        raise ValueError('file-backed prefix changed')
    if window[lo]:
        raise ValueError('first BSS byte uncleared')
    if window[hi - 1]:
        raise ValueError('last BSS byte uncleared')
    if window.count(0, lo, hi) != hi - lo:
        raise ValueError('interior BSS byte uncleared')
    if window.count(POISON, hi) != len(window) - hi:
        raise ValueError('suffix guard changed')


def witness(image):
    start, end, first, prefix = layout(image)
    window = model(prefix, start, end)
    check(window, prefix)
    lo, hi = start - WINDOW_START, end - WINDOW_START
    trailing = len(prefix) - len(prefix.rstrip(b'\0'))
    print(f'copy=[{relocation.COPY_BASE:#x},{start:#x}) BSS=[{start:#x},{end:#x}) '
          f'zero_bytes={window.count(0, lo, hi)} qwords={(hi - lo) // 8}')
    print(f'prefix file=[{first:#x},{first + len(prefix):#x}) bytes={lo} '
          f'nonzero={lo - prefix.count(0)} trailing_zeros={trailing}')
    print(f'window=[{WINDOW_START:#x},{WINDOW_END:#x}) pages={len(window) // PAGE} poison={POISON:#x} '
          f'suffix_guard_bytes={window.count(POISON, hi)} SHA256={hashlib.sha256(window).hexdigest()}')
    for label, left, right, reason in (
            ('omit start byte', start + 1, end, 'first BSS byte uncleared'),
            ('omit end byte', start, end - 1, 'last BSS byte uncleared'),
            ('round start down', WINDOW_START, end, 'file-backed prefix changed'),
            ('round end up', start, WINDOW_END, 'suffix guard changed')):
        relocation.require_refusal(label, reason, check, model(prefix, left, right), prefix)
    # Corrupt a byte outside the examined slices, without mutating caller-owned data.
    damaged = bytearray(image)
    damaged[0] ^= 1
    relocation.require_refusal('changed image', 'unreviewed image', layout, damaged)
    print('Offline byte model only; no guest execution or permission changes.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('vmlinuz', type=Path)
    args = parser.parse_args()
    try:
        with args.vmlinuz.open('rb') as source:
            witness(source.read(relocation.IMAGE_BYTES + 1))
    except (OSError, ValueError) as err:
        parser.exit(1, f'verify_relocation_bss: {err}\n')
