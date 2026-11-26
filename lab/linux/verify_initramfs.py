"""Check the reviewed initramfs and command line without extracting or executing them.

Usage: python lab/linux/verify_initramfs.py PATH/initramfs.cpio PATH/cmdline.bin
Only the CLI's two pinned inputs are admitted. The structural check covers the
fixed exec chain, ELF dependency metadata and two device records. It does not
test dynamic linking, applet behavior or boot.
Single uncompressed newc subset of Linux's initramfs buffer format:
https://docs.kernel.org/driver-api/early-userspace/buffer-format.html
"""
import argparse
from dataclasses import dataclass
import hashlib
from pathlib import Path
import re
import stat
import struct
import sys

# Same reviewed size/digest table as the ISO packer and verify_media.py.
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'firmware'))
from linux_media import FILES

_PINS = {name: (size, digest) for name, _, size, digest in FILES}


class PinError(ValueError):
    """The supplied bytes are not the reviewed input."""


def _pinned(path, name):
    size, digest = _PINS[name]
    with path.open('rb') as stream:
        data = stream.read(size + 1)
    if len(data) != size:
        raise PinError(f'{name}: size pin mismatch (expected {size} bytes)')
    if hashlib.sha256(data).hexdigest() != digest:
        raise PinError(f'{name}: SHA-256 pin mismatch')
    return data


@dataclass(frozen=True)
class Member:
    mode: int
    rmajor: int
    rminor: int
    data: memoryview


def _span(buf, start, size, what):
    if start > len(buf) or size > len(buf) - start:
        raise ValueError(f'{what}: out of bounds')
    return buf[start:start + size]


def _aligned(buf, end):
    padding = (-end) % 4
    if any(_span(buf, end, padding, 'newc alignment')):
        raise ValueError('nonzero newc padding')
    return end + padding


def _path(raw):
    # Canonical relative ASCII components only; never normalize aliases or
    # resolve archive paths with host filesystem semantics.
    if not re.fullmatch(rb'[A-Za-z0-9_.-]+(?:/[A-Za-z0-9_.-]+)*', raw) or any(
            part in (b'.', b'..') for part in raw.split(b'/')):
        raise ValueError('unsupported archive path')
    return raw.decode('ascii')


def _members(archive):
    buf = memoryview(archive)
    members = {}
    pos = 0
    while pos < len(buf):
        header = bytes(_span(buf, pos, 110, 'newc header'))
        if not re.fullmatch(rb'070701[0-9A-Fa-f]{104}', header):
            raise ValueError('invalid newc magic or hex fields')
        (_, mode, _, _, nlink, _, size, _, _, major, minor, namesize, check) = (
            int(header[i:i + 8], 16) for i in range(6, 110, 8))
        if check or not 2 <= namesize <= 4096:
            raise ValueError('unsupported newc checksum or name length')
        name_start = pos + 110
        raw = bytes(_span(buf, name_start, namesize, 'newc name'))
        if raw[-1] != 0:
            raise ValueError('unterminated newc name')
        name = 'TRAILER!!!' if raw == b'TRAILER!!!\0' else _path(raw[:-1])
        start = _aligned(buf, name_start + namesize)
        data = _span(buf, start, size, f'newc {name} data')
        pos = _aligned(buf, start + size)
        if name == 'TRAILER!!!':
            if size or mode or pos != len(buf):
                raise ValueError('invalid or non-final newc trailer')
            return members
        if name in members:
            raise ValueError(f'duplicate newc member: {name}')
        kind = stat.S_IFMT(mode)
        if nlink != 1 or kind not in (stat.S_IFDIR, stat.S_IFREG, stat.S_IFLNK, stat.S_IFCHR):
            raise ValueError(f'{name}: unsupported link count or file type')
        if (size and kind in (stat.S_IFDIR, stat.S_IFCHR)) or (not size and kind == stat.S_IFLNK):
            raise ValueError(f'{name}: invalid payload for file type')
        parent, _, _ = name.rpartition('/')
        if parent and (parent not in members or not stat.S_ISDIR(members[parent].mode)
                       or not members[parent].mode & 0o111):
            raise ValueError(f'{name}: parent is not a searchable directory')
        members[name] = Member(mode, major, minor, data)
    raise ValueError('missing newc trailer')


ELFHDR = struct.Struct('<16sHHIQQQIHHHHHH')
PHDR = struct.Struct('<IIQQQQQQ')
DYN = struct.Struct('<qQ')
DT_NULL, DT_NEEDED, DT_STRTAB, DT_STRSZ, DT_SONAME = 0, 1, 5, 10, 14
_MUSL = b'libc.musl-x86_64.so.1'
_LOADER = 'lib/ld-musl-x86_64.so.1'


def _file_offset(loads, address, size, what):
    """Require one unambiguous file backing for every byte of the virtual span."""
    end = address + size
    offset = None
    for va, fileoff, filesz, memsz in loads:
        if not memsz or va >= end or va + memsz <= address:
            continue
        if offset is not None or address < va or end > va + filesz:
            raise ValueError(f'{what}: ambiguous or non-file-backed PT_LOAD mapping')
        offset = fileoff + (address - va)
    if offset is None:
        raise ValueError(f'{what}: no file-backed PT_LOAD mapping')
    return offset


def _dynamic_names(data, name, dynamic, loads):
    if dynamic is None:
        raise ValueError(f'{name}: missing PT_DYNAMIC')
    offset, va, size = dynamic
    if not size or size % DYN.size:
        raise ValueError(f'{name}: invalid PT_DYNAMIC entry framing')
    entries = _span(data, offset, size, f'{name} PT_DYNAMIC')
    if _file_offset(loads, va, size, f'{name} PT_DYNAMIC') != offset:
        raise ValueError(f'{name}: PT_DYNAMIC file and virtual offsets disagree')
    single = {}
    needed = []
    terminated = False
    for tag, value in DYN.iter_unpack(entries):
        if terminated:
            if (tag, value) != (DT_NULL, 0):
                raise ValueError(f'{name}: non-padding entry after DT_NULL')
        elif tag == DT_NULL:
            terminated = True
        elif tag == DT_NEEDED:
            needed.append(value)
        elif tag in (DT_STRTAB, DT_STRSZ, DT_SONAME):
            if tag in single:
                raise ValueError(f'{name}: duplicate dynamic tag {tag}')
            single[tag] = value
        elif tag in (15, 29):  # DT_RPATH, DT_RUNPATH: no search-path model here.
            raise ValueError(f'{name}: unsupported dynamic search path')
    if not terminated:
        raise ValueError(f'{name}: missing DT_NULL')
    if DT_STRTAB not in single or not single.get(DT_STRSZ):
        raise ValueError(f'{name}: missing DT_STRTAB or nonempty DT_STRSZ')
    size = single[DT_STRSZ]
    offset = _file_offset(loads, single[DT_STRTAB], size, f'{name} DT_STRTAB')
    strings = bytes(_span(data, offset, size, f'{name} DT_STRTAB'))
    if strings[0] or strings[-1]:
        raise ValueError(f'{name}: expected NUL-framed dynamic string table')

    def string_at(index):
        if index >= len(strings):
            raise ValueError(f'{name}: dynamic string offset outside DT_STRSZ')
        # The table's final NUL is in range; no search may reach adjacent bytes.
        return strings[index:strings.index(b'\0', index)]

    return ([string_at(index) for index in needed],
            string_at(single[DT_SONAME]) if DT_SONAME in single else None)


def _elf_metadata(data, name):
    ident, typ, machine, version, _, phoff, _, _, ehsize, entsize, num, *_ = ELFHDR.unpack(
        _span(data, 0, ELFHDR.size, f'{name} ELF header'))
    if ident[:7] != b'\x7fELF\x02\x01\x01' or machine != 62 or version != 1 or typ not in (2, 3):
        raise ValueError(f'{name}: expected executable ELF64 LE x86-64')
    if (ehsize != ELFHDR.size or phoff < ehsize or entsize != PHDR.size or not 0 < num < 0xffff):
        raise ValueError(f'{name}: unsupported ELF program header layout')
    table = _span(data, phoff, entsize * num, f'{name} program headers')
    interp = None
    loads = []
    dynamic = None
    for p_type, _, offset, va, _, size, memsz, _ in PHDR.iter_unpack(table):
        if p_type == 1:  # PT_LOAD
            if size > memsz or va + memsz > 1 << 64:
                raise ValueError(f'{name}: invalid PT_LOAD extent')
            _span(data, offset, size, f'{name} PT_LOAD')
            loads.append((va, offset, size, memsz))
        elif p_type == 2:  # PT_DYNAMIC
            if dynamic is not None or size != memsz:
                raise ValueError(f'{name}: duplicate or unsupported PT_DYNAMIC')
            dynamic = (offset, va, size)
        if p_type != 3:  # PT_INTERP
            continue
        if interp is not None:
            raise ValueError(f'{name}: duplicate PT_INTERP')
        if not 3 <= size <= 4096:
            raise ValueError(f'{name}: invalid PT_INTERP size')
        raw = bytes(_span(data, offset, size, f'{name} PT_INTERP'))
        if not raw.startswith(b'/') or not raw.endswith(b'\0'):
            raise ValueError(f'{name}: expected absolute NUL-terminated PT_INTERP')
        interp = (_path(raw[1:-1]), offset, size)
    needed, soname = _dynamic_names(data, name, dynamic, loads)
    return interp, needed, soname


def _required(members, name):
    if name not in members:
        raise ValueError(f'missing archive member: /{name}')
    return members[name]


def _exec(members, name):
    m = _required(members, name)
    if not stat.S_ISREG(m.mode) or not m.mode & 0o111:
        raise ValueError(f'/{name}: expected executable regular file')
    return m


def _inspect(archive, cmdline):
    """Structural observations only. Unit fixtures can exercise these without pins."""
    if not re.fullmatch(rb'[ -~]+\x00', cmdline) or b'"' in cmdline:
        raise ValueError('expected unquoted ASCII command line with one terminal NUL')
    tokens = cmdline[:-1].split()
    rdinit = [token[7:] for token in tokens if token.startswith(b'rdinit=')]
    if rdinit != [b'/init'] or b'--' in tokens:
        raise ValueError('expected unique kernel argument rdinit=/init')
    members = _members(archive)
    init_name = _path(rdinit[0][1:])
    init = _exec(members, init_name)
    line, lf, _ = bytes(init.data[:256]).partition(b'\n')
    if not lf or line != b'#!/bin/sh':
        raise ValueError('/init: expected #!/bin/sh followed by LF')
    shell = _path(line[3:])
    link = _required(members, shell)
    if not stat.S_ISLNK(link.mode) or link.data != b'busybox':
        raise ValueError(f'/{shell}: expected relative symlink to busybox')
    relative = bytes(link.data).decode('ascii')
    target = shell.rpartition('/')[0] + '/' + relative
    busybox = _exec(members, target)
    interp, needed, soname = _elf_metadata(busybox.data, target)
    if interp is None:
        raise ValueError(f'/{target}: missing PT_INTERP')
    loader, offset, size = interp
    ld = _exec(members, loader)
    ld_interp, ld_needed, ld_soname = _elf_metadata(ld.data, loader)
    if ld_interp is not None:
        raise ValueError(f'/{loader}: nested PT_INTERP not supported')
    if loader != _LOADER:
        raise ValueError(f'/{target}: expected PT_INTERP /{_LOADER}')
    if needed != [_MUSL] or soname is not None:
        raise ValueError(f'/{target}: expected only DT_NEEDED={_MUSL.decode()} and no DT_SONAME')
    if ld_soname != _MUSL or ld_needed:
        raise ValueError(f'/{loader}: expected DT_SONAME={_MUSL.decode()} and no DT_NEEDED')
    alias_name = 'lib/' + needed[0].decode()
    alias = _required(members, alias_name)
    basename = loader.rsplit('/', 1)[1]
    if not stat.S_ISLNK(alias.mode) or alias.data != basename.encode():
        raise ValueError(f'/{alias_name}: expected relative symlink to {basename}')
    lines = [f'newc 070701: {len(members)} members, final TRAILER!!!',
             f'rdinit=/{init_name} -> /{init_name} regular mode={init.mode:06o} shebang={line.decode()} LF',
             f'/{shell} symlink mode={link.mode:06o} -> {relative} (relative) -> /{target}',
             f'/{target} regular mode={busybox.mode:06o} ELF64 LE x86-64 '
             f'PT_INTERP offset={offset:#x} size={size} path=/{loader}',
             f'/{target} dynamic metadata: DT_NEEDED={needed[0].decode()} DT_SONAME=(none)',
             f'/{alias_name} symlink -> {basename} (relative) -> /{loader}',
             f'/{loader} regular mode={ld.mode:06o} ELF64 LE x86-64',
             f'/{loader} dynamic metadata: DT_SONAME={ld_soname.decode()} DT_NEEDED=(none)',
             'ELF metadata only; no dynamic linking or execution tested.']
    for name, mode, major, minor in [('dev/console', 0o020600, 5, 1), ('dev/null', 0o020666, 1, 3)]:
        node = _required(members, name)
        if (node.mode, node.rmajor, node.rminor) != (mode, major, minor):
            raise ValueError(f'/{name}: expected character mode={mode:06o} rdev={major}:{minor}')
        lines.append(f'/{name} character mode={node.mode:06o} rdev={node.rmajor}:{node.rminor}')
    return '\n'.join(lines)


def verify(archive_path, cmdline_path):
    archive = _pinned(archive_path, 'initramfs.cpio')
    cmdline = _pinned(cmdline_path, 'cmdline.bin')
    chain = _inspect(archive, cmdline)
    receipts = [f'{name}: pinned {_PINS[name][0]} bytes SHA-256={_PINS[name][1]}'
                for name in ('initramfs.cpio', 'cmdline.bin')]
    return '\n'.join([*receipts, chain])


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('initramfs', type=Path)
    parser.add_argument('cmdline', type=Path)
    args = parser.parse_args()
    try:
        print(verify(args.initramfs, args.cmdline))
    except (OSError, ValueError) as err:
        parser.exit(1, f'verify_initramfs: {err}\n')
