"""Prepare W03 from authenticated guest flash and the locally identified Windows EFI file.

Only the public recipe goes in Git. The pin, image, private ISO and section map
stay under build/. Recompute every pin when packaging to catch input swaps.
"""
import argparse
from hashlib import sha256
import json
from pathlib import Path
import struct

import w02_media as cold
from w01_media import BUILD, CODE_BASE, align, le, require, span

HEADER = BUILD / 'w03_pin.h'
MAP = BUILD / 'w03-preboot-map.json'
FILES = (('W03CODE.FD', 0x37c000), ('W03VARS.FD', 0x84000), ('WINBOOT.EFI', 1557304))
WINDOWS_SHA = 'd99ca68bb147f5f30bfb96d9f16b557622ac5a86c2effa2d918badecaa6f88ef'


def windows_pe(file):
    """Bind the loaded code to the original file even when UEFI rebases it."""
    require(sha256(file).hexdigest() == WINDOWS_SHA, 'not the reviewed Windows 10 Boot Manager')
    pe = le(file, 60, 4)
    require(file[:2] == b'MZ' and span(file, pe, 6) == b'PE\0\0\x64\x86', 'not an AMD64 PE')
    opt = span(file, pe + 24, le(file, pe + 20, 2))
    require(le(opt, 0, 2) == 0x20b and le(opt, 68, 2) == 10 and le(opt, 108, 4) >= 6,
            'expected PE32+ EFI application')
    entry, base, size, salign = (le(opt, 16, 4), le(opt, 24, 8), le(opt, 56, 4), le(opt, 32, 4))
    require(salign == 4096 and 0 < entry < size < 32 * 1024**2, 'unsupported image geometry')
    table = span(file, pe + 24 + len(opt), le(file, pe + 6, 2) * 40)
    sections = []
    for off in range(0, len(table), 40):
        vs, va, raw, ptr = struct.unpack_from('<4I', table, off + 8)
        flags = le(table, off + 36, 4)
        span(file, ptr, raw)
        require(not va % salign and va + max(raw, vs) <= size, 'section outside image')
        sections.append((va, min(vs, raw), ptr, flags))
    def containing(rva, n):
        return cold.one((s for s in sections if s[0] <= rva and rva + n <= s[0] + s[1]), 'initialized section')
    text, length, ptr, flags = containing(entry, 4)
    require((flags & 0xe0000020) == 0x60000020, 'entry section not read-only code')
    require(align(text + length, salign) <= min((s[0] for s in sections if s[0] > text), default=size),
            'entry section shares pages')
    # Only the admitted stack adjustment can witness retirement. Require the
    # complete instruction within one page and no fixup changing its bytes.
    require(entry % 4096 <= 4092 and file[ptr + entry - text:ptr + entry - text + 4] == bytes.fromhex('4883ec28'),
            'unexpected boot manager entry instruction')
    rr, rn = le(opt, 152, 4), le(opt, 156, 4)
    va, _, rp, _ = containing(rr, rn)
    reloc = span(file, rp + rr - va, rn)
    targets, i = [], 0
    while i < rn:
        page, block = struct.unpack('<II', span(reloc, i, 8))
        require(not page % 4096 and block >= 8 and not block % 4, 'invalid relocation block')
        for (item,) in struct.iter_unpack('<H', span(reloc, i + 8, block - 8)):
            typ, at = item >> 12, page + (item & 4095)
            if typ == 0:
                continue
            require(typ == 10 and at + 8 <= size, 'unsupported relocation')
            if at < text + length and at + 8 > text:
                require(text <= at and at + 8 <= text + length and not (at < entry + 4 and at + 8 > entry),
                        'partial or entry-opcode relocation')
                targets.append(at)
        i += block
    targets.sort()
    require(all(a + 8 <= b for a, b in zip(targets, targets[1:])), 'overlapping text relocations')
    return dict(file_sha256=WINDOWS_SHA, file_size=len(file), entry_rva=entry, preferred_base=base,
                image_size=size, section_align=salign, text_rva=text, text_len=length, text_file_offset=ptr,
                text_relocs=targets, text_reloc_count=len(targets))


def measure(code, nv, win):
    guest = cold.inspect(code, nv)
    dxe = guest['modules']['DxeCore']
    vol = cold.one((v for v in guest['volumes'] if v['phase'] == 'DXE'), 'DXE FV')
    start = vol['flash_gpa'] - CODE_BASE + dxe['image_origin'] - vol['base'] + dxe['text_rva']
    text = span(code, start, dxe['text_len'])
    require(sha256(text).hexdigest() == dxe['text_sha256'], 'wrong DXE text')
    marker = bytes.fromhex('41ba52445933ef')
    require(text.count(marker) == 1, 'need one RDY3 IO site in actual DXE Core')
    out = text.index(marker) + 6
    return dict(guest=guest, windows=windows_pe(win), ready_out_rva=dxe['text_rva'] + out,
                ready_out_window=span(text, out - 24, 32).hex())


def pins(m):
    header = cold.pin_header(m['guest']).decode('ascii')
    win = m['windows']
    lines = ['#ifndef W03_WINDOWS_PIN_H', '#define W03_WINDOWS_PIN_H']
    for key in ('file_size', 'entry_rva', 'image_size', 'section_align', 'text_rva', 'text_len', 'text_file_offset', 'text_reloc_count'):
        lines.append(f'#define W03_{key.upper()} 0x{win[key]:x}u')
    # Keep lines below compiler translation limits; this table is measured
    # input, not a second embedded copy of Windows executable text.
    rows = [','.join(f'0x{x:x}u' for x in win['text_relocs'][i:i+16]) for i in range(0, win['text_reloc_count'], 16)]
    lines += [f'#define W03_PREFERRED_BASE 0x{win["preferred_base"]:x}ull',
              '#define W03_FILE_SHA256 ' + cold.initializer(win['file_sha256']),
              '#define W03_TEXT_RELOCS {' + (', '+chr(92)+'\n').join(rows) + '}',
              f'#define W03_READY_OUT_RVA 0x{m["ready_out_rva"]:x}u',
              '#define W03_READY_OUT_WINDOW ' + cold.initializer(m['ready_out_window']), '#endif']
    return (header + '\n'.join(lines) + '\n').encode('ascii')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='cmd', required=True)
    prep = sub.add_parser('prepare')
    for name in ('code', 'vars'):
        prep.add_argument('--' + name + '-sha256', required=True)
    pack = sub.add_parser('pack')
    pack.add_argument('efi', type=Path)
    pack.add_argument('outiso', type=Path)
    for p in (prep, pack):
        p.add_argument('--inputs', type=Path, default=BUILD / 'w03')
    args = parser.parse_args()
    inputs = []
    for name, size in FILES:
        with (args.inputs / name).open('rb') as f:
            data = f.read(size + 1)
        require(len(data) == size, f'{name}: wrong byte count')
        inputs.append((name, data))
    m = measure(*(data for _, data in inputs))
    header = pins(m)
    if args.cmd == 'prepare':
        for key in ('code', 'vars'):
            require(m['guest'][key+'_sha256'] == getattr(args, key+'_sha256').lower(), 'guest build digest mismatch: ' + key)
        HEADER.write_bytes(header)
        MAP.write_text(json.dumps(m, sort_keys=True, indent=2) + '\n', encoding='ascii')
        print('SEC, DXE ready OUT RVA, Windows entry RVA:', *(hex(k) for k in (m['guest']['sec_entry'], m['ready_out_rva'], m['windows']['entry_rva'])))
    else:
        require(HEADER.read_bytes() == header, 'prepared pins changed before pack')
        from pack_iso import pack as pack_iso
        pack_iso(args.efi, args.outiso, esp_files=inputs)


if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError) as exc:
        raise SystemExit(f'W03 media rejected: {exc}') from None
