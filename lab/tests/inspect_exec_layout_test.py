"""Linked page accounting tests; no vendor image, guest code, or VM required."""
from contextlib import redirect_stdout
import hashlib
from io import StringIO
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


if __name__ == '__main__':
    unittest.main()
