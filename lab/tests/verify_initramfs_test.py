"""Pin admission and intentionally unpinned init-chain fixtures; no boot assets needed."""
from pathlib import Path
import struct
from tempfile import TemporaryDirectory
import unittest

from lab.linux import verify_initramfs as check


CMD = b'console=ttyS0 rdinit=/init\0'
# Use a *different* interpreter name and offset than the retained BusyBox.
# A positive result must come from the program header and the named member.
LOADER = 'lib/unit-ld.so'
INTERP = b'/' + LOADER.encode() + b'\0'


def elf(*, interp=None, machine=62, interp_offset=0x1a4, interp_count=1):
    ident = b'\x7fELF\x02\x01\x01' + bytes(9)
    num = interp_count if interp is not None else 1
    hdr = struct.pack('<16sHHIQQQIHHHHHH', ident, 3, machine, 1, 0, 64, 0, 0, 64, 56, num, 0, 0, 0)
    if interp is None:
        return hdr + bytes(56)  # PT_NULL; the check does not simulate a loader.
    ph = struct.pack('<IIQQQQQQ', 3, 4, interp_offset, 0, 0, len(interp), len(interp), 1)
    return (hdr + ph * num).ljust(0x1a4, b'\0') + interp


def record(name, mode=0, data=b'', major=0, minor=0, nlink=1):
    raw = name.encode() + b'\0'
    fields = [1, mode, 0, 0, nlink, 0, len(data), 0, 0, major, minor, len(raw), 0]
    head = b'070701' + ''.join(f'{f:08x}' for f in fields).encode() + raw
    return head + bytes(-len(head) % 4) + data + bytes(-len(data) % 4)


def fixture(**changes):
    entries = {'bin': (0o040755,), 'lib': (0o040755,), 'dev': (0o040755,),
               'init': (0o100755, b'#!/bin/sh\n'), 'bin/sh': (0o120777, b'busybox'),
               'bin/busybox': (0o100755, elf(interp=INTERP)), LOADER: (0o100755, elf()),
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
        for field in ('9 members, final TRAILER!!!', 'rdinit=/init', 'shebang=#!/bin/sh LF',
                      '/bin/sh symlink mode=120777 -> busybox (relative) -> /bin/busybox',
                      f'PT_INTERP offset=0x1a4 size={len(INTERP)} path=/{LOADER}',
                      f'/{LOADER} regular mode=100755 ELF64 LE x86-64',
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
