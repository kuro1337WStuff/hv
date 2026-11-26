"""Pin admission and intentionally unpinned init-chain fixtures; no boot assets needed."""
from pathlib import Path
import struct
from tempfile import TemporaryDirectory
import unittest

from lab.linux import verify_initramfs as check


CMD = b'console=ttyS0 rdinit=/init\0'
# Independent byte layout: the virtual addresses are not usable file offsets,
# and neither the string offsets nor the program headers match the retained ELFs.
MUSL = b'libc.musl-x86_64.so.1'
LOADER = 'lib/ld-musl-x86_64.so.1'
ALIAS = 'lib/' + MUSL.decode()
INTERP = b'/' + LOADER.encode() + b'\0'
STRINGS = b'\0wrong.so\0' + MUSL + b'\0'
MUSL_INDEX = STRINGS.index(MUSL)
DYN_OFF, DYN_VA = 0x240, 0x80240
STR_OFF, STR_VA = 0x480, 0x10480
TABLE = [(5, STR_VA), (10, len(STRINGS))]


def phdr(kind, offset, va, size, memsz=None):
    return struct.pack('<IIQQQQQQ', kind, 4, offset, va, va, size, size if memsz is None else memsz, 1)


def elf(*, interp=None, machine=62, interp_offset=0x1a4, interp_count=1, entries=None, strings=STRINGS,
        str_load=None, dyn_header=None, extra_headers=()):
    ident = b'\x7fELF\x02\x01\x01' + bytes(9)
    if entries is None:
        name_tag = 14 if interp is None else 1  # loader SONAME or BusyBox NEEDED
        entries = [*TABLE, (name_tag, MUSL_INDEX), (0, 0), (0, 0)]
    dynamic = b''.join(struct.pack('<qQ', tag, value) for tag, value in entries)
    headers = [phdr(1, DYN_OFF, DYN_VA, len(dynamic)),
               str_load or phdr(1, STR_OFF, STR_VA, len(strings)),
               dyn_header or phdr(2, DYN_OFF, DYN_VA, len(dynamic)), *extra_headers]
    if interp is not None:
        headers.extend([phdr(3, interp_offset, 0, len(interp))] * interp_count)
    hdr = struct.pack('<16sHHIQQQIHHHHHH', ident, 3, machine, 1, 0, 64, 0, 0, 64, 56, len(headers), 0, 0, 0)
    image = (hdr + b''.join(headers)).ljust(0x1a4, b'\0') + (interp or b'')
    # A decoy name and a NUL outside the declared string table are not metadata.
    return (image.ljust(DYN_OFF, b'\0') + dynamic).ljust(STR_OFF, b'\0') + strings + b'\0' + MUSL


def record(name, mode=0, data=b'', major=0, minor=0, nlink=1):
    raw = name.encode() + b'\0'
    fields = [1, mode, 0, 0, nlink, 0, len(data), 0, 0, major, minor, len(raw), 0]
    head = b'070701' + ''.join(f'{f:08x}' for f in fields).encode() + raw
    return head + bytes(-len(head) % 4) + data + bytes(-len(data) % 4)


def fixture(**changes):
    entries = {'bin': (0o040755,), 'lib': (0o040755,), 'dev': (0o040755,),
               'init': (0o100755, b'#!/bin/sh\n'), 'bin/sh': (0o120777, b'busybox'),
               'bin/busybox': (0o100755, elf(interp=INTERP)), LOADER: (0o100755, elf()),
               ALIAS: (0o120777, b'ld-musl-x86_64.so.1'),
               'dev/console': (0o020600, b'', 5, 1), 'dev/null': (0o020666, b'', 1, 3)}
    entries.update(changes)
    return b''.join(record(name, *args) for name, args in entries.items() if args is not None) + record('TRAILER!!!')


class AdmissionTest(unittest.TestCase):
    def test_structurally_good_fixture_is_not_admitted(self):
        with TemporaryDirectory() as tmp:
            archive = Path(tmp) / 'initramfs.cpio'
            archive.write_bytes(fixture())
            with self.assertRaisesRegex(check.PinError, r'initramfs.cpio: size pin mismatch'):
                check.verify(archive, Path(tmp) / 'cmdline.bin')

    def test_same_size_corruption_fails_each_content_pin(self):
        with TemporaryDirectory() as tmp:
            for name in ('initramfs.cpio', 'cmdline.bin'):
                with self.subTest(name=name):
                    path = Path(tmp) / name
                    with path.open('wb') as stream:
                        stream.truncate(check._PINS[name][0])
                    with self.assertRaisesRegex(check.PinError, 'SHA-256 pin mismatch'):
                        check._pinned(path, name)


class ChainTest(unittest.TestCase):
    def test_observations_come_from_fixture_bytes(self):
        result = check._inspect(fixture(), CMD)
        for field in ('10 members, final TRAILER!!!', 'rdinit=/init', 'shebang=#!/bin/sh LF',
                      '/bin/sh symlink mode=120777 -> busybox (relative) -> /bin/busybox',
                      f'PT_INTERP offset=0x1a4 size={len(INTERP)} path=/{LOADER}',
                      f'/{LOADER} regular mode=100755 ELF64 LE x86-64',
                      '/bin/busybox dynamic metadata: DT_NEEDED=libc.musl-x86_64.so.1 DT_SONAME=(none)',
                      f'/{ALIAS} symlink -> ld-musl-x86_64.so.1 (relative) -> /{LOADER}',
                      f'/{LOADER} dynamic metadata: DT_SONAME=libc.musl-x86_64.so.1 DT_NEEDED=(none)',
                      '/dev/console character mode=020600 rdev=5:1', '/dev/null character mode=020666 rdev=1:3'):
            self.assertIn(field, result)

    def test_cmdline_selection(self):
        for cmd in (b'init=/init\0', b'rdinit=/bad\0', b'-- rdinit=/init\0', b'rdinit=/init rdinit=/init\0',
                    b'rdinit=/init', b'rdinit=/init\0hidden\0'):
            with self.subTest(cmd=cmd), self.assertRaisesRegex(ValueError, 'rdinit|terminal NUL'):
                check._inspect(fixture(), cmd)

    def test_shebang_link_and_exec_modes(self):
        for name, args, msg in [
            ('init', (0o100755, b'#!/bin/sh\r\n'), 'followed by LF'),
            ('init', (0o100755, b'#!/bin/sh'), 'followed by LF'),
            ('init', (0o100644, b'#!/bin/sh\n'), 'executable regular'),
            ('bin/sh', (0o120777, b'/bin/busybox'), 'relative symlink'),
            ('bin/sh', (0o100755, b'busybox'), 'relative symlink'),
            ('bin/busybox', (0o100644, elf(interp=INTERP)), 'executable regular'),
            (LOADER, None, 'missing archive member'),
            (LOADER, (0o120777, b'elsewhere'), 'executable regular'),
            (LOADER, (0o100644, elf()), 'executable regular'),
            (LOADER, (0o100755, elf(machine=3)), 'ELF64 LE x86-64'),
            (LOADER, (0o100755, elf(interp=INTERP)), 'nested PT_INTERP'),
        ]:
            with self.subTest(name=name, reason=msg), self.assertRaisesRegex(ValueError, msg):
                check._inspect(fixture(**{name: args}), CMD)

    def test_elf_header_ranges_and_interp(self):
        for data, msg in [(b'\x7fELF', 'ELF header: out of bounds'),
                          (elf(interp=INTERP, machine=3), 'ELF64 LE x86-64'),
                          (elf()[:65], 'program headers: out of bounds'), (elf(), 'missing PT_INTERP'),
                          (elf(interp=INTERP, interp_count=2), 'duplicate PT_INTERP'),
                          (elf(interp=INTERP, interp_offset=2**64-1), 'PT_INTERP: out of bounds'),
                          (elf(interp=b'/lib/missing.so\0'), 'missing archive member: /lib/missing'),
                          (elf(interp=b'lib/relative.so\0'), 'absolute NUL-terminated'),
                          (elf(interp=INTERP[:-1]), 'absolute NUL-terminated'),
                          (elf(interp=INTERP + b'x\0'), 'unsupported archive path')]:
            with self.subTest(reason=msg), self.assertRaisesRegex(ValueError, msg):
                check._inspect(fixture(**{'bin/busybox': (0o100755, data)}), CMD)

    def test_dependency_alias_binds_the_interp_member(self):
        for replacement in (None, (0o100755, elf()), (0o120777, INTERP[:-1]),
                            (0o120777, b'wrong.so'), (0o120777, b'./ld-musl-x86_64.so.1')):
            with self.subTest(alias=replacement), self.assertRaisesRegex(ValueError, 'missing archive member|relative symlink'):
                check._inspect(fixture(**{ALIAS: replacement}), CMD)
        alternate = 'lib/other.so'
        with self.assertRaisesRegex(ValueError, 'expected PT_INTERP'):
            check._inspect(fixture(**{'bin/busybox': (0o100755, elf(interp=b'/lib/other.so\0')),
                                      alternate: (0o100755, elf()), ALIAS: (0o120777, b'other.so')}), CMD)

    def test_exact_dynamic_names_in_each_member(self):
        needed = (1, MUSL_INDEX)
        soname = (14, MUSL_INDEX)
        for name, tags, message in [
                ('bin/busybox', [], 'expected only DT_NEEDED'),
                ('bin/busybox', [(1, 1)], 'expected only DT_NEEDED'),  # points at wrong.so
                ('bin/busybox', [needed, needed], 'expected only DT_NEEDED'),
                ('bin/busybox', [needed, (1, 1)], 'expected only DT_NEEDED'),
                ('bin/busybox', [needed, soname], 'expected only DT_NEEDED'),
                (LOADER, [], 'expected DT_SONAME'),
                (LOADER, [(14, 1)], 'expected DT_SONAME'),
                (LOADER, [soname, needed], 'no DT_NEEDED'),
                (LOADER, [soname, soname], 'duplicate dynamic tag'),
                (LOADER, [(14, len(STRINGS))], 'outside DT_STRSZ')]:
            with self.subTest(name=name, tags=tags), self.assertRaisesRegex(ValueError, message):
                data = elf(interp=None if name == LOADER else INTERP, entries=[*tags, *TABLE, (0, 0)])
                check._inspect(fixture(**{name: (0o100755, data)}), CMD)

    def test_dynamic_entries_and_string_framing(self):
        name = (1, MUSL_INDEX)
        end = (0, 0)
        for entries, strings, message in [
                ([*TABLE, name], STRINGS, 'missing DT_NULL'),
                ([*TABLE, end, name], STRINGS, 'after DT_NULL'),
                ([TABLE[0], TABLE[0], TABLE[1], name, end], STRINGS, 'duplicate dynamic tag'),
                ([*TABLE, TABLE[1], name, end], STRINGS, 'duplicate dynamic tag'),
                ([TABLE[1], name, end], STRINGS, 'missing DT_STRTAB'),
                ([TABLE[0], name, end], STRINGS, 'DT_STRSZ'),
                ([TABLE[0], (10, 0), name, end], STRINGS, 'DT_STRSZ'),
                ([*TABLE, (1, len(STRINGS)), end], STRINGS, 'outside DT_STRSZ'),
                ([*TABLE, name, end], STRINGS[:-1] + b'!', 'NUL-framed'),
                ([*TABLE, name, end], b'!' + STRINGS[1:], 'NUL-framed'),
                ([*TABLE, name, (29, 1), end], STRINGS, 'unsupported dynamic search path'),
                ([(5, STR_OFF), TABLE[1], name, end], STRINGS, 'no file-backed PT_LOAD'),
                ([TABLE[0], (10, 2**64-1), name, end], STRINGS, 'non-file-backed PT_LOAD')]:
            with self.subTest(entries=entries, strings=strings), self.assertRaisesRegex(ValueError, message):
                data = elf(interp=INTERP, entries=entries, strings=strings)
                check._inspect(fixture(**{'bin/busybox': (0o100755, data)}), CMD)

    def test_dynamic_segments_and_load_bounds(self):
        size = len(STRINGS)
        for overrides, message in [
                ({'dyn_header': phdr(0, 0, 0, 0)}, 'missing PT_DYNAMIC'),
                ({'extra_headers': [phdr(2, DYN_OFF, DYN_VA, 16)]}, 'duplicate or unsupported PT_DYNAMIC'),
                ({'dyn_header': phdr(2, DYN_OFF, DYN_VA, 17)}, 'entry framing'),
                ({'dyn_header': phdr(2, DYN_OFF, DYN_VA, 16, 32)}, 'unsupported PT_DYNAMIC'),
                ({'dyn_header': phdr(2, STR_OFF, DYN_VA, 16)}, 'offsets disagree'),
                ({'dyn_header': phdr(2, 2**64-1, DYN_VA, 16)}, 'PT_DYNAMIC: out of bounds'),
                ({'str_load': phdr(1, STR_OFF, STR_VA, size, size-1)}, 'invalid PT_LOAD extent'),
                ({'str_load': phdr(1, STR_OFF, 2**64-1, size)}, 'invalid PT_LOAD extent'),
                ({'str_load': phdr(1, 2**64-1, STR_VA, size)}, 'PT_LOAD: out of bounds'),
                ({'str_load': phdr(1, STR_OFF, STR_VA, size-1, size)}, 'non-file-backed PT_LOAD'),
                ({'extra_headers': [phdr(1, STR_OFF, STR_VA, size)]}, 'ambiguous'),
                ({'extra_headers': [phdr(1, 0, STR_VA+1, 0, 1)]}, 'ambiguous')]:
            with self.subTest(overrides=overrides), self.assertRaisesRegex(ValueError, message):
                data = elf(interp=INTERP, **overrides)
                check._inspect(fixture(**{'bin/busybox': (0o100755, data)}), CMD)

    def test_character_type_permissions_and_reference_device_numbers(self):
        for name, args in [('dev/console', (0o100600, b'', 5, 1)), ('dev/console', (0o020666, b'', 5, 1)),
                           ('dev/console', (0o020600, b'', 1, 5)), ('dev/null', (0o020666, b'', 1, 2))]:
            with self.subTest(name=name, args=args), self.assertRaisesRegex(ValueError, f'{name}: expected character'):
                check._inspect(fixture(**{name: args}), CMD)

    def test_strict_newc_framing_and_namespace(self):
        end = record('TRAILER!!!')
        init = record('init', 0o100755, b'x')
        oversized = init[:54] + b'ffffffff' + init[62:]
        for archive, msg in [(init, 'missing newc trailer'), (end + end, 'non-final newc trailer'),
                             (record('TRAILER!!!', data=b'x'), 'invalid or non-final'),
                             (init + init + end, 'duplicate newc member: init'),
                             (record('x/../init', 0o100755) + end, 'unsupported archive path'),
                             (record('a\0b', 0o100755) + end, 'unsupported archive path'),
                             (record('x/y', 0o100755) + end, 'parent is not'),
                             (record('x', 0o100755, nlink=2) + end, 'unsupported link count'),
                             (init[:-1] + b'!' + end, 'nonzero newc padding'),
                             (oversized + end, 'init data: out of bounds'),
                             (init[:111], 'newc name: out of bounds'),
                             (init[:20], 'newc header: out of bounds'),
                             (init[:6] + b'+' + init[7:] + end, 'invalid newc magic or hex')]:
            with self.subTest(reason=msg), self.assertRaisesRegex(ValueError, msg):
                check._inspect(archive, CMD)


if __name__ == '__main__':
    unittest.main()
