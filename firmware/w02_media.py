"""Admit the fixed cold W02 profile from authenticated split flash; pack precisely those bytes."""
import argparse
from hashlib import sha256
import json
from pathlib import Path
import re
import struct
from uuid import UUID

from w01_media import (BUILD, CODE_BASE, EDK2, FFS_GUIDS, NV_DATA, SECFV, SEC_CORE, SEC_OFFSET,
                       align, fv_header, inspect_code, le, require, span)

HEADER = BUILD / 'w02_pin.h'
FILES = (('W02CODE.FD', 0x37c000), ('W02VARS.FD', 0x84000))
RESET_PAGE_SHA = '2ed89d453f0312b6fd3576afc024109ca64740718511237d4b928da4fe70cd61'
COMPACT = UUID('48db5e17-707c-472d-91cd-1613e7ef51b0')
NESTED = UUID('9e21fd93-9c72-4c15-8c4b-e77f1db2d792')
PEIFV = UUID('6938079b-b503-4e3d-9d24-b28337a25806')
DXEFV = UUID('7cb8bdc9-f8eb-4f34-aaea-3ee4af6516a1')
# (FFS GUID, expected PI file type, role)
MODULES = {
    UUID('52c05b14-0b98-496c-bc3b-04b50211d680'): (4, 'PeiCore'),
    UUID('9b3ada4f-ae56-4c24-8dea-f03b7558ae50'): (6, 'PcdPeim'),
    UUID('05af1e9c-81fa-49bd-87ed-f39ff34962b8'): (6, 'W02BoardPei'),
    UUID('86d70125-baa3-4296-a62f-602bebbb9081'): (6, 'DxeIpl'),
    UUID('d6a2cb7f-6a18-4e2f-b43b-9920a733700a'): (5, 'DxeCore'),
}


def one(items, what):
    found = list(items)
    require(len(found) == 1, f'expected exactly one {what}')
    return found[0]


def ffs(volume, name):
    hdrlen = fv_header(volume, FFS_GUIDS)
    ext = le(volume, 52, 2)
    require(ext >= hdrlen and span(volume, ext, 16) == name.bytes_le and le(volume, ext + 16, 4) >= 20,
            f'wrong FV extension: {name}')
    span(volume, ext, le(volume, ext + 16, 4))
    p = align(hdrlen, 8)
    while p < len(volume):
        h = span(volume, p, 24)
        if h == b'\xff' * 24:
            require(all(b == 255 for b in volume[p:]), 'non-erased tail of FV')
            break
        n = le(h, 20, 3)
        require(h[23] == 0xf8 and not h[19] & 1 and 24 <= n < 0xffffff
                and (sum(h) - h[17] - h[23]) % 256 == 0, 'invalid FFS file header')
        data = span(volume, p + 24, n - 24)
        require((sum(data) + h[17]) % 256 == 0 if h[19] & 0x40 else h[17] == 0xaa,
                'invalid FFS file checksum')
        yield UUID(bytes_le=bytes(h[:16])), h[18], p + 24, data
        p = align(p + n, 8)


def sections(data):
    pos = 0
    while pos < len(data):
        n, kind = le(data, pos, 3), le(data, pos + 3, 1)
        require(4 <= n < 0xffffff, 'unsupported section size')
        yield kind, pos + 4, span(data, pos + 4, n - 4)
        pos = align(pos + n, 4)


def image_info(image, origin, role):
    # The GCC profile emits unstripped PE32+ headers and equal section/file
    # alignment. Validate those properties rather than scanning the FD for EF.
    p = le(image, 60, 4)
    require(image[:2] == b'MZ' and 64 <= p <= 0xffff and span(image, p, 4) == b'PE\0\0'
            and le(image, p + 4, 2) == 0x8664, f'{role}: not AMD64 PE')
    opt = span(image, p + 24, le(image, p + 20, 2))
    require(len(opt) >= 160 and le(opt, 0, 2) == 0x20b and le(opt, 108, 4) >= 6
            and le(opt, 32, 4) == le(opt, 36, 4) != 0, f'{role}: unsupported PE32+ layout')
    preferred = le(opt, 24, 8)
    require(preferred == (0 if role == 'DxeCore' else origin), f'{role}: unexpected preferred base {preferred:#x}')
    table = span(image, p + 24 + len(opt), le(image, p + 6, 2) * 40)
    text = []
    for off in range(0, len(table), 40):
        name = bytes(table[off:off + 8]).rstrip(b'\0')
        vs, va, rs, rp = struct.unpack_from('<IIII', table, off + 8)
        flags = le(table, off + 36, 4)
        if rs:
            require(rp >= p + 24 + len(opt) + len(table) and rp == va,
                    f'{role}: PE section not file-backed 1:1')
            span(image, rp, rs)
        if flags & 0x20000000:
            require(name == b'.text' and not flags & 0x80000000 and 0 < vs <= rs,
                    f'{role}: unexpected executable section')
            text.append((va, bytes(span(image, rp, vs))))
    rva, data = one(text, f'{role} executable section')
    entry = le(opt, 16, 4)
    require(rva <= entry < rva + len(data), f'{role}: entry not in .text')
    if role != 'SecMain':
        require(not ((rva | len(data)) & 4095) and (role == 'DxeCore' or not (origin & 4095)),
                f'{role}: text is not page-isolated')
        # Raw XIP and relocated instances must have the *same* .text hash.
        # Validate the real relocation directory; never zero or mask text.
        rel = span(image, le(opt, 152, 4), le(opt, 156, 4))
        off = 0
        while off < len(rel):
            page, length = le(rel, off, 4), le(rel, off + 4, 4)
            require(length >= 8 and not length % 4 and not page % 4096, 'invalid reloc block')
            for word, in struct.iter_unpack('<H', span(rel, off + 8, length - 8)):
                typ, target = word >> 12, page + (word & 4095)
                if typ:
                    require(typ == 10 and target + 8 <= le(opt, 56, 4) and
                            (target + 8 <= rva or target >= rva + len(data)),
                            f'{role}: unsupported relocation or relocated text')
            off += length
    rec = dict(role=role, image_origin=origin, preferred_base=preferred, entry_rva=entry,
               text_rva=rva, text_len=len(data), text_sha256=sha256(data).hexdigest(),
               source_pe_sha256=sha256(image).hexdigest())
    if role in ('PeiCore', 'DxeCore'):
        prefix = b'\x41\xba' + (b'PEI2' if role == 'PeiCore' else b'DXE2')
        sig = prefix + b'\xef'
        require(data.count(sig) == 1, f'{role}: missing or duplicate witness')
        out = data.index(sig) + len(prefix)
        rec.update(out_rva=rva + out, out_prefix=prefix.hex(), out_window=span(data, out - 24, 32).hex())
    return rec


def inspect(code, nv):
    require(sha256(code[-4096:]).hexdigest() == RESET_PAGE_SHA, 'differs from W01 reviewed reset page')
    fv_header(memoryview(nv), {NV_DATA})
    sec_entry = inspect_code(code)
    codeview = memoryview(code)
    _, _, so, sb = one((f for f in ffs(codeview[SEC_OFFSET:], UUID(bytes_le=SECFV))
                        if f[0].bytes_le == SEC_CORE and f[1] == 3), 'SEC core FFS')
    _, io, image = one((s for s in sections(sb) if s[0] == 16), 'SEC PE32')
    sec = image_info(image, CODE_BASE + SEC_OFFSET + so + io, 'SecMain')
    require(sec['image_origin'] + sec['entry_rva'] == sec_entry, 'SEC entry mismatch')
    fs = [f for f in ffs(codeview[:SEC_OFFSET], COMPACT) if f[1] != 0xf0]
    guid, kind, outeroff, body = one(fs, 'cold FV image FFS')
    require((guid, kind) == (NESTED, 11), 'wrong compact FFS identity')
    nested = [s for s in sections(body) if s[0] != 0x19]
    require(len(nested) == 2, 'expected exactly two uncompressed FVs')
    volumes = []
    modules = {}
    for (phase, name, base), (section, off, volume) in zip((('PEI', PEIFV, 0x830000),
                                                           ('DXE', DXEFV, 0x900000)), nested):
        require(section == 0x17 and len(volume) == 0x40000, 'unexpected nested FV section')
        volumes.append(dict(phase=phase, guid=str(name), base=base, size=len(volume),
                            flash_gpa=CODE_BASE + outeroff + off, sha256=sha256(volume).hexdigest()))
        for guid, kind, fo, fb in ffs(volume, name):
            if kind == 0xf0 or (phase == 'PEI' and kind == 2 and str(guid) == '1b45cc0a-156a-428a-af62-49864da0e6e6'):
                continue
            require(guid in MODULES and MODULES[guid][0] == kind and (kind == 5) == (phase == 'DXE'),
                    f'unexpected {phase} FFS {guid}/{kind}')
            _, role = MODULES[guid]
            require(role not in modules, f'duplicate {role}')
            _, io, image = one((s for s in sections(fb) if s[0] == 16), f'{role} PE32')
            modules[role] = dict(guid=str(guid), **image_info(image, base + fo + io, role))
    require(len(modules) == len(MODULES), 'missing board module')
    return dict(edk2=EDK2, reset_page_sha256=RESET_PAGE_SHA, sec=sec, sec_entry=sec_entry,
                volumes=volumes, modules=modules,
                code_sha256=sha256(code).hexdigest(), vars_sha256=sha256(nv).hexdigest())




def initializer(hexbytes):
    return '{' + ','.join(f'0x{b:02x}' for b in bytes.fromhex(hexbytes)) + '}'


def pin_header(m):
    out = ['/* Derived offline from the authenticated W02 flash pair. */', '#ifndef W02_PIN_H', '#define W02_PIN_H']

    def emit(name, value):
        out.append(f'#define {name} {hex(value) + "u" if isinstance(value, int) else value}')

    emit('W01_SEC_GPA', m['sec_entry'])
    emit('W01_CODE_SHA256', initializer(m['code_sha256']))
    emit('W01_VARS_SHA256', initializer(m['vars_sha256']))
    sec = m['sec']
    emit('W02_SEC_TEXT_BASE', sec['image_origin'] + sec['text_rva'])
    emit('W02_SEC_TEXT_LEN', sec['text_len'])
    for vol in m['volumes']:
        phase = vol['phase']
        pref = f'W02_{phase}_'
        for field in ('base', 'size', 'sha256'):
            val = vol[field]
            emit(pref + 'FV_' + field.upper(), initializer(val) if field == 'sha256' else val)
        emit(pref + 'FV_CODE_OFFSET', vol['flash_gpa'] - CODE_BASE)
        mod = m['modules']['PeiCore' if phase == 'PEI' else 'DxeCore']
        emit(pref + 'FV_IMAGE_BASE', mod['image_origin'])
        for field in ('text_rva', 'text_len', 'text_sha256', 'out_rva', 'out_window', 'out_prefix'):
            val = mod[field]
            emit(pref + field.upper(), initializer(val) if isinstance(val, str) else val)
    infos = [f'{{0x{x["image_origin"]:x}u,0x{x["text_rva"]:x}u,0x{x["text_len"]:x}u,{initializer(x["text_sha256"])}}}'
             for x in m['modules'].values() if x['role'] != 'DxeCore']
    emit('W02_PEI_IMAGES', '{' + ','.join(infos) + '}')
    return ('\n'.join(out) + '\n#endif\n').encode('ascii')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    cmd = parser.add_subparsers(dest='command', required=True)
    prepare = cmd.add_parser('prepare')
    for name in ('code', 'vars'):
        prepare.add_argument(f'--{name}-sha256', required=True, help='from authenticated build log')
    pack = cmd.add_parser('pack')
    pack.add_argument('efi', type=Path)
    pack.add_argument('outiso', type=Path)
    for sub in (prepare, pack):
        sub.add_argument('--inputs', type=Path, default=BUILD / 'w02')
    args = parser.parse_args()
    files = []
    for name, n in FILES:
        with (args.inputs / name).open('rb') as stream:
            data = stream.read(n + 1)
        require(len(data) == n, f'{name}: incorrect size')
        files.append((name, data))
    if args.command == 'prepare':
        for (name, data), expected in zip(files, (args.code_sha256, args.vars_sha256)):
            require(re.fullmatch('[a-fA-F0-9]{64}', expected) and sha256(data).hexdigest() == expected.lower(),
                    f'{name}: authenticated digest mismatch')
    manifest = inspect(*(b for _, b in files))
    header = pin_header(manifest)
    if args.command == 'prepare':
        BUILD.mkdir(exist_ok=True)
        HEADER.write_bytes(header)
        evidence = json.dumps(manifest, indent=2, sort_keys=True) + '\n'
        (BUILD / 'w02-preboot-map.json').write_text(evidence, encoding='utf-8')
        print(evidence, end='')
    else:
        require(HEADER.read_bytes() == header, 'flash or witness identity differs from prepared pin')
        from pack_iso import pack as pack_iso
        pack_iso(args.efi, args.outiso, esp_files=files)


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError) as exc:
        raise SystemExit(f'W02 media rejected: {exc}') from None
