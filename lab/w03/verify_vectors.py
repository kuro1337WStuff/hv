"""Verify immutable W03 DxeCore exception vectors in one already authenticated W03CODE.FD."""
import argparse
from hashlib import sha256
import json
from pathlib import Path
import re
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'firmware'))
import w02_media as cold
from w01_media import SEC_OFFSET, align, le, require, span


STRIDE = 18  # Also the copy length of the frozen EDK II hook-after header.
VECTORS = 256
# Frozen AsmGetTemplateAddressMap: LEA RAX,[RIP+d]; MOV [RCX],RAX;
# MOV qword [RCX+8],imm32; LEA RAX,[RIP+d]; MOV [RCX+16],RAX. Check RET
# separately: 141 implements the same three output fields, followed by stores.
MAP = re.compile(rb'\x48\x8d\x05(.{4})\x48\x89\x01\x48\xc7\x41\x08(.{4})'
                 rb'\x48\x8d\x05(.{4})\x48\x89\x41\x10', re.DOTALL)


def frozen_dxe(code):
    require(len(code) == 0x37c000, 'wrong W03CODE.FD size')
    outer = cold.ffs(memoryview(code)[:SEC_OFFSET], cold.COMPACT)
    guid, kind, _, body = cold.one((f for f in outer if f[1] != 0xf0), 'compact FV file')
    require((guid, kind) == (cold.NESTED, 11), 'wrong nested FV identity')
    # The fixed profile has one nested FV-image FFS with PEI then DXE.
    volumes = [s for s in cold.sections(body) if s[0] != 0x19]
    require(len(volumes) == 2 and all(s[0] == 0x17 and len(s[2]) == 0x40000 for s in volumes),
            'wrong nested firmware volumes')
    modules = cold.ffs(volumes[1][2], cold.DXEFV)
    _, _, _, payload = cold.one((f for f in modules if cold.MODULES.get(f[0]) == (f[1], 'DxeCore')),
                                'DXE core FFS')
    return bytes(cold.one((s for s in cold.sections(payload) if s[0] == 16), 'DXE core PE32')[2])


def verify(image):
    # This also checks AMD64 PE32+, zero preferred base, RVA == raw offset,
    # page-isolated executable text, and no DIR64 touching that text.
    info = cold.image_info(image, 0, 'DxeCore')
    pe = le(image, 60, 4)
    opt = span(image, pe + 24, le(image, pe + 20, 2))
    require(le(opt, 32, 4) == 4096, 'expected 4 KiB PE section alignment')
    table = span(image, pe + 24 + len(opt), le(image, pe + 6, 2) * 40)
    areas = {}
    end = align(le(opt, 60, 4), 4096)  # Keep headers and each section on separate pages.
    for i in range(0, len(table), 40):
        name = table[i:i + 8].rstrip(b'\0')
        vs, va, rs = struct.unpack_from('<III', table, i + 8)
        flags = le(table, i + 36, 4)
        require(name not in areas and va >= end and va % 4096 == 0, 'duplicate or overlapping PE sections')
        end = va + align(max(vs, rs), 4096)
        areas[name] = va, min(vs, rs), flags
    require(end <= le(opt, 56, 4), 'section outside PE image')

    def section(name, permissions):
        require(name in areas, f'missing {name!r} section')
        va, size, flags = areas[name]
        require(flags >> 29 == permissions, f'wrong {name!r} page permissions')
        return va, span(image, va, size)

    text_va, text = section(b'.text', 3)  # READ | EXECUTE
    data_va, data = section(b'.data', 6)  # READ | WRITE
    getter = cold.one(MAP.finditer(text), 'DXE template address map getter')
    require(span(text, getter.end(), 1) == b'\xc3', 'getter continues after output stores (old live patcher?)')

    def lea(group):
        return text_va + getter.end(group) + int.from_bytes(getter[group], 'little', signed=True)

    vectors, stride, hook = lea(1), int.from_bytes(getter[2], 'little'), lea(3)
    require(stride == STRIDE, 'getter stride differs from 18-byte hook copy length')
    stubs = span(text, vectors - text_va, VECTORS * stride)
    common = None
    for n in range(VECTORS):
        off = n * stride
        require(stubs[off:off + 7] == b'\x68' + n.to_bytes(4, 'little') + b'\x50\xe9',
                f'vector {n}: expected push imm32(vector), push rax, jmp rel32')
        target = vectors + off + 11 + struct.unpack_from('<i', stubs, off + 7)[0]
        require(stubs[off + 11:off + stride] == b'\x90' * 7, f'vector {n}: unexpected padding')
        if common is None:
            common = target
        require(common == target, f'vector {n}: wrong common entry target')
    # CommonInterruptEntry: cli; pop rax; xchg rcx,[rsp].
    require(span(text, common - text_va, 6) == bytes.fromhex('fa 58 48 87 0c 24'),
            'vector target is not CommonInterruptEntry')

    source = span(data, hook - data_va, STRIDE)
    require(source[:8] == bytes.fromhex('68 00 00 00 00 50 48 b8') and source[16:] == b'\xff\xe0',
            'getter hook pointer is not the copyable .data header')
    hook_end = le(source, 8, 8)  # zero preferred base, validated above
    # HookAfterStubHeaderEnd: mov rax,rsp; and sp,-16; sub rsp,0x18; push rcx.
    require(span(text, hook_end - text_va, 12) == bytes.fromhex('48 89 e0 66 83 e4 f0 48 83 ec 18 51'),
            'hook immediate does not point to the hook-after entry in .text')

    rr, rn = struct.unpack_from('<II', opt, 152)
    require(b'.reloc' in areas, 'missing .reloc')
    rv, length, _ = areas[b'.reloc']
    require(rv <= rr and rr + rn <= rv + length, 'relocation directory outside .reloc')
    reloc = span(image, rr, rn)
    overlaps = []
    pos = 0
    while pos < rn:  # Block bounds and permitted types already checked by image_info.
        page, block = struct.unpack_from('<II', reloc, pos)
        for word, in struct.iter_unpack('<H', span(reloc, pos + 8, block - 8)):
            at = page + (word & 0xfff)
            if word >> 12 and at < hook + STRIDE and at + 8 > hook:
                overlaps.append((word >> 12, at))
        pos += block
    require(overlaps == [(10, hook + 8)], 'expected exactly one DIR64 covering only the hook immediate')
    return dict(dxe_pe_sha256=info['source_pe_sha256'], text_sha256=info['text_sha256'],
                getter_rva=text_va + getter.start(), table_rva=vectors, vectors=VECTORS, stride=stride,
                common_rva=common, hook_source_rva=hook, hook_dir64_rva=hook + 8, hook_target_rva=hook_end)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('code', type=Path, metavar='W03CODE.FD')
    parser.add_argument('--negative-control', action='store_true',
                        help='also require rejection of a corrupted vector 128 in an in-memory PE copy')
    args = parser.parse_args()
    code = args.code.read_bytes()
    image = frozen_dxe(code)
    result = verify(image)
    if args.negative_control:
        bad = bytearray(image)
        bad[result['table_rva'] + 128 * STRIDE + 1] ^= 1
        try:
            verify(bytes(bad))
        except ValueError as exc:
            require(str(exc).startswith('vector 128:'), f'control rejected for unexpected reason: {exc}')
            result['negative_control'] = str(exc)
        else:
            raise ValueError('corrupted vector 128 accepted')
    print(json.dumps(dict(code_sha256=sha256(code).hexdigest(), **result), sort_keys=True))


if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError) as exc:
        raise SystemExit(f'W03 vectors rejected: {exc}') from None
