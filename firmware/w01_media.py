"""Admit a source-built W01 flash pair offline, then pack those exact bytes."""
import argparse
from hashlib import sha256
from pathlib import Path
import re
import struct
from uuid import UUID


BUILD = Path(__file__).resolve().parents[1] / 'build'
HEADER = BUILD / 'w01_pin.h'
EDK2 = '556dcfb466e9f05affeaff881d738807f8156d3c'
CODE_BASE = 0xffc84000
SEC_OFFSET = 0x348000  # OvmfPkgDefines.fdf.inc, FD_SIZE_IN_KB=4096
FILES = (('W01CODE.FD', 0x37c000), ('W01VARS.FD', 0x84000))
FFS_GUIDS = {UUID(g).bytes_le for g in ('8c8ce578-8a3d-4f1c-9935-896185c32dd3',
                                         '5473c07a-3dcb-4dca-bd6f-1e9689e7349a')}
SECFV = UUID('763bed0d-de9f-48f5-81f1-3e90e1b1a015').bytes_le
SEC_CORE = UUID('df1ccef6-f301-4a63-9661-fc6030dcc880').bytes_le
VTF = UUID('1ba0062e-c779-4582-8566-336ae8f78f09').bytes_le
NV_DATA = UUID('fff12b8d-7696-4c8b-a985-2747075b4f50').bytes_le


def require(ok, message):
    if not ok:
        raise ValueError(message)


def span(buf, off, count):
    require(0 <= off <= len(buf) and 0 <= count <= len(buf) - off, 'field outside enclosing object')
    return buf[off:off + count]


def le(buf, off, count):
    return int.from_bytes(span(buf, off, count), 'little')


def align(value, n):
    return (value + n - 1) & -n


def fv_header(fv, guids):
    require(bytes(span(fv, 16, 16)) in guids and le(fv, 32, 8) == len(fv)
            and span(fv, 40, 4) == b'_FVH' and fv[55] == 2, 'invalid FV identity or length')
    length = le(fv, 48, 2)
    require(length >= 72 and length % 2 == 0 and le(fv, 44, 4) & 0x800, 'invalid FV header')
    header = span(fv, 0, length)
    require(sum(v for v, in struct.iter_unpack('<H', header)) % 65536 == 0, 'FV checksum mismatch')
    total = 0
    for off in range(56, length - 7, 8):
        n, block = struct.unpack_from('<II', header, off)
        if n == block == 0:
            break
        require(n and block, 'invalid FV block map')
        total += n * block
    else:
        raise ValueError('unterminated FV block map')
    require(total == len(fv), 'FV blocks do not cover volume')
    return length


def xip_entry(image, gpa, kind):
    # Match UefiCpuPkg/ResetVector/Vtf0/Ia32/SearchForSecEntry.nasm.inc.
    # Independently translate through the section table: the raw byte reached
    # by the reset code must be inside the file-backed executable section.
    if kind == 0x10:
        require(span(image, 0, 2) == b'MZ', 'SEC PE DOS signature')
        nt = le(image, 0x3c, 4)
        require(0x40 <= nt <= 0xffff and span(image, nt, 4) == b'PE\0\0', 'invalid SEC PE offset/signature')
        require(le(image, nt + 4, 2) == 0x8664, 'SEC is not AMD64')
        count = le(image, nt + 6, 2)
        opt = span(image, nt + 24, le(image, nt + 20, 2))
        require(len(opt) >= 112 and le(opt, 0, 2) == 0x20b, 'SEC is not PE32+')
        rva, base = le(opt, 16, 4), le(opt, 24, 8)
        require(le(opt, 32, 4) == le(opt, 36, 4) != 0, 'SEC PE not XIP aligned')
        table, adjust = nt + 24 + len(opt), 0
    else:
        require(span(image, 0, 2) == b'VZ' and le(image, 2, 2) == 0x8664, 'invalid SEC TE')
        count, rva, base = image[4], le(image, 8, 4), le(image, 16, 8)
        table, adjust = 40, 40 - le(image, 6, 2)
        require(adjust <= 0, 'invalid TE stripped size')

    require(count and base == gpa + adjust, 'SEC image is not rebased to flash')
    sections = span(image, table, count * 40)
    entry = rva + adjust
    span(image, entry, 1)
    hits = 0
    for sh in range(0, len(sections), 40):
        vs, va, size, ptr = struct.unpack_from('<IIII', sections, sh + 8)
        if size:
            require(ptr + adjust >= table + len(sections), 'section data overlaps SEC headers')
            span(image, ptr + adjust, size)
        if va <= rva < va + max(vs, size):
            require(rva < va + min(vs or size, size) and le(sections, sh + 36, 4) & 0x20000000
                    and ptr + adjust + rva - va == entry, 'SEC entry is not file-backed executable XIP')
            hits += 1
    require(hits == 1, 'missing or ambiguous SEC entry section')
    return gpa + entry


def sec_file(body, base):
    off = 0
    entries = []
    while off < len(body):
        size, kind = le(body, off, 3), le(body, off + 3, 1)
        require(4 <= size < 0xffffff, 'invalid SEC section size')
        data = span(body, off + 4, size - 4)
        if kind in (0x10, 0x12):
            entries.append(xip_entry(data, base + off + 4, kind))
        elif kind not in (0x14, 0x15, 0x19):  # version, UI, alignment RAW
            raise ValueError('unexpected section in uncompressed SEC file')
        off = align(off + size, 4)
    require(len(entries) == 1, 'expected one uncompressed SEC executable')
    return entries[0]


def reset_entry(fv, body_start):
    # OvmfPkg/ResetVector/Ia16/ResetVectorVtf0.nasm.inc (X64): both
    # paths share the first instructions, but the legacy branch is 16-bit.
    v = bytes(fv[-16:])
    require(fv[-20:-16] == b'VTF\0' and v[:8] == bytes.fromhex('0f20c0 a801 7405 e9')
            and v[12] == 0xe9 and v[15] == 0x90, 'invalid X64 reset vector')
    flat = len(fv) - 4 + int.from_bytes(v[8:12], 'little', signed=True)
    real = len(fv) - 1 + int.from_bytes(v[13:15], 'little', signed=True)
    require(body_start <= flat < len(fv) - 16 and max(body_start, len(fv) - 0x10000) <= real < len(fv) - 16,
            'reset branch leaves VTF')
    require(span(fv, real, 4) == b'\xbfBP\xeb', 'real-mode target is not EarlyBspInitReal16')


def inspect_code(code):
    # Flat32SearchForBfvBase selects the FIRST top-down 4 KiB candidate
    # with an FFS GUID and a length ending exactly at 4 GiB.
    for off in range(len(code) - 4096, SEC_OFFSET - 1, -4096):
        if code[off + 16:off + 32] in FFS_GUIDS and le(code, off + 32, 8) == len(code) - off:
            break
    else:
        raise ValueError('reset search cannot find SECFV')
    require(off == SEC_OFFSET, 'reset search selects a different FV')
    fv = memoryview(code)[off:]
    header_size = fv_header(fv, FFS_GUIDS)
    ext = le(fv, 52, 2)
    require(ext >= header_size and span(fv, ext, 16) == SECFV and le(fv, ext + 16, 4) >= 20,
            'SECFV extension identity mismatch')
    span(fv, ext, le(fv, ext + 16, 4))
    cursor = align(header_size, 8)
    entry = None
    saw_reset = False
    while cursor < len(fv):
        hdr = span(fv, cursor, 24)
        guid, kind, attr = hdr[:16], hdr[18], hdr[19]
        size = le(hdr, 20, 3)
        require(hdr[23] == 0xf8 and not attr & 1 and 24 <= size < 0xffffff,
                'invalid FFS state, header or size')
        require((sum(hdr) - hdr[17] - hdr[23]) % 256 == 0, 'FFS header checksum mismatch')
        body = span(fv, cursor + 24, size - 24)
        require((sum(body) + hdr[17]) % 256 == 0 if attr & 0x40 else hdr[17] == 0xaa,
                'FFS data checksum mismatch')
        if kind == 3 and guid == SEC_CORE:
            require(entry is None, 'duplicate SEC core')
            entry = sec_file(body, CODE_BASE + off + cursor + 24)
        elif kind == 1 and guid == VTF:
            require(cursor + size == len(fv), 'VTF is not the volume top file')
            reset_entry(fv, cursor + 24)
            saw_reset = True
        else:
            require(kind == 0xf0, 'unexpected FFS file in SECFV')
        cursor = align(cursor + size, 8)
    require(saw_reset and entry is not None, 'missing SEC core or VTF')
    return entry


def read_flash(inputs):
    result = []
    for name, size in FILES:
        with (inputs / name).open('rb') as stream:
            data = stream.read(size + 1)
        require(len(data) == size, f'{name}: expected {size} bytes')
        result.append((name, data))
    return result


def pin_header(entry, digests):
    lines = [f'/* W01: tianocore/edk2 {EDK2}, X64 RELEASE GCC, 4 MiB. */',
             '#ifndef W01_PIN_H', '#define W01_PIN_H', f'#define W01_SEC_GPA 0x{entry:08x}ULL']
    for name, digest in zip(('CODE', 'VARS'), digests):
        init = ', '.join(f'0x{b:02x}' for b in digest)
        lines.append(f'#define W01_{name}_SHA256 {{ {init} }}')
    return ('\n'.join(lines) + '\n#endif\n').encode('ascii')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    cmds = parser.add_subparsers(dest='command', required=True)
    prepare = cmds.add_parser('prepare', help='use independent digests from the authenticated pinned build log')
    for name in ('code', 'vars'):
        prepare.add_argument(f'--{name}-sha256', required=True)
    pack = cmds.add_parser('pack')
    pack.add_argument('efi', type=Path)
    pack.add_argument('outiso', type=Path)
    for cmd in (prepare, pack):
        cmd.add_argument('--inputs', type=Path, default=BUILD / 'w01')
    args = parser.parse_args()
    files = read_flash(args.inputs)
    digests = [sha256(data).digest() for _, data in files]
    if args.command == 'prepare':
        for (name, _), digest, expected in zip(files, digests, (args.code_sha256, args.vars_sha256)):
            require(re.fullmatch('[0-9a-fA-F]{64}', expected) and digest.hex() == expected.lower(),
                    f'{name}: authenticated build digest mismatch')
    code, nv = (data for _, data in files)
    fv_header(memoryview(nv), {NV_DATA})
    entry = inspect_code(code)
    header = pin_header(entry, digests)
    if args.command == 'prepare':
        BUILD.mkdir(exist_ok=True)
        HEADER.write_bytes(header)
        print(f'EDK2={EDK2} reset={code[-16:].hex()} SEC_GPA=0x{entry:08x}')
        for (name, _), digest in zip(files, digests):
            print(f'{digest.hex()}  {name}')
    else:
        with HEADER.open('rb') as stream:
            require(stream.read(len(header) + 1) == header, 'flash pair/SEC entry differs from prepared pin')
        from pack_iso import pack as pack_iso
        pack_iso(args.efi, args.outiso, esp_files=files)


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError) as exc:
        raise SystemExit(f'W01 media rejected: {exc}') from None
