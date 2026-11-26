"""Linked page accounting tests; no vendor image, guest code, or VM required."""
from contextlib import redirect_stdout
import hashlib
from io import StringIO
import struct
import unittest
from unittest.mock import patch

from lab.linux import inspect_exec_layout as layout


class LinkedPageTest(unittest.TestCase):
    # Page starts two bytes into the load: 'ab' is on the previous page.
    elf = b'LABELabcde'
    load = (1, 6, 5, 0x8ffe, 0x1ffe, 5, 4098, 4096)

    def test_file_then_zero_fill_with_independent_section_offset(self):
        sections = [(1, 1, 6, 0x8fff, 0, 3, 0, 0, 1, 0)]
        out = StringIO()
        with redirect_stdout(out):
            layout._report_page(self.elf, 0x2001, [self.load], sections, b'\0.init\0')
        text = out.getvalue()
        self.assertIn('file=[0x7,0xa) length=0x3', text)
        self.assertIn('ELF zero-fill GPA=[0x2003,0x3000)', text)
        self.assertIn('section=.init flags=0x6 VA=[0x9000,0x9002) section file=[0x1,0x3)', text)
        self.assertIn('section gap VA=[0x9002,0xa000)', text)
        digest = hashlib.sha256(b'cde' + bytes(4093)).hexdigest()
        self.assertIn(f'SHA256={digest} length=4096', text)

    def test_overlap_gap_and_short_file_never_get_digest(self):
        # Second load overlaps only zero-fill, not any file-backed bytes.
        extra = (1, 4, 0, 0x9008, 0x2008, 0, 1, 1)
        cases = [(self.elf, [self.load, extra], 'overlapping', None),
                 (self.elf, [extra], 'not completely covered', 'unmapped GPA=[0x2000,0x2008)'),
                 (self.elf, [], 'not completely covered', 'unmapped GPA=[0x2000,0x3000)'),
                 (self.elf[:-1], [self.load], 'truncated', None)]
        for elf, loads, error, gap in cases:
            with self.subTest(error=error, gap=gap):
                out = StringIO()
                with redirect_stdout(out), self.assertRaisesRegex(ValueError, error):
                    layout._report_page(elf, 0x2001, loads, [], b'')
                if gap:
                    self.assertIn(gap, out.getvalue())
                self.assertNotIn('SHA256=', out.getvalue())

    def test_wrong_hash_refused_before_inflate(self):
        with patch.object(layout.gzip, 'decompress') as inflate:
            with self.assertRaisesRegex(ValueError, 'unreviewed kernel'):
                layout.inspect(bytes(12649472))
            inflate.assert_not_called()


class AlternativesTest(unittest.TestCase):
    names = b'\0.altinstructions\0'

    def report(self, base, page, sites, tail=b'', **overrides):
        data = bytearray(b'prefix')
        for i, (start, length) in enumerate(sites):
            data.extend(struct.pack('<iiIBB', start - (base + 14*i), -19, 0xaabbccdd, length, 51))
        data.extend(tail)
        section = (1, overrides.get('kind', 1), 2, base, 6,
                   overrides.get('size', len(data)-6), 0, 0, 1, overrides.get('entsize', 0))
        out = StringIO()
        with redirect_stdout(out):
            layout._report_alternatives(data, [section], self.names, page)
        return out.getvalue()

    def test_clipped_union_and_empty_spans(self):
        sites = [(0xffe, 5), (0xffe, 5), (0x1002, 3), (0x1003, 0),
                 (0x1ffe, 4), (0xfff, 1), (0x2000, 1)]
        text = self.report(0x3000, 0x1000, sites)
        self.assertIn('records=7 intersecting=4 unique_original_starts=3 unique_page_bytes=7', text)
        self.assertIn('record=1 original VA=[0xffe,0x1003) intersection VA=[0x1000,0x1003)', text)
        self.assertIn('intersection VA=[0x1ffe,0x2000)', text)

    def test_unsigned_limits(self):
        limit = 1 << 64
        page = limit - 4096
        self.assertIn('unique_page_bytes=1', self.report(page, page, [(limit-1, 1)]))
        for base, target, length in [(page, limit-1, 2), (page, limit, 0), (0, -1, 1)]:
            with self.subTest(target=target), self.assertRaisesRegex(ValueError, 'original VA'):
                self.report(base, page, [(target, length)])

    def test_malformed_tables(self):
        for kwargs in [dict(tail=b'x'), dict(size=28), dict(entsize=16), dict(kind=8)]:
            with self.subTest(kwargs=kwargs), self.assertRaisesRegex(ValueError, 'invalid .altinstructions'):
                self.report(0x3000, 0x1000, [(0, 0)], **kwargs)
        with self.assertRaisesRegex(ValueError, 'invalid .altinstructions'):
            self.report((1 << 64)-1, 0, [((1 << 64)-1, 0)])


if __name__ == '__main__':
    unittest.main()
