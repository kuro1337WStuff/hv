"""Synthetic short allocation descriptors; no ISO or pycdlib install needed."""
from io import BytesIO
import struct
from types import SimpleNamespace
import unittest

from lab.install_udf_extent import BLOCK, range_lba, short_extents


def selected_lba(second_lbn=41, second_type=0):
    # File entry in partition block 1. Data starts in partition block 9;
    # byte 65520 is in its 32nd sector, and the range enters the second AD.
    partition = SimpleNamespace(part_start_location=7, part_length=64)
    icb = SimpleNamespace(log_block_num=1, part_ref_num=0, extent_length=BLOCK)
    entry = SimpleNamespace(orig_extent_loc=8, info_len=33 * BLOCK,
                            icb_tag=SimpleNamespace(strategy_type=4, flags=0))
    fe = bytearray(BLOCK)
    struct.pack_into('<H', fe, 0, 261)
    struct.pack_into('<II', fe, 168, 0, 16)  # No extended attributes; two short ADs.
    struct.pack_into('<IIII', fe, 176, 32 * BLOCK, 9, second_type << 30 | BLOCK, second_lbn)
    source = BytesIO(bytes(entry.orig_extent_loc * BLOCK) + fe)
    return range_lba(short_extents(source, entry, partition, icb))


class InstallUdfExtentTest(unittest.TestCase):
    def test_crosses_to_contiguous_partition_relative_extent(self):
        self.assertEqual(selected_lba(), 47)  # absolute, not partition-relative 40

    def test_hole_or_continuation_in_crossed_extent_is_refused(self):
        for extent_type in (1, 2, 3):
            with self.subTest(extent_type=extent_type), self.assertRaisesRegex(ValueError, 'hole'):
                selected_lba(second_type=extent_type)

    def test_discontinuous_crossed_extent_is_refused(self):
        with self.assertRaisesRegex(ValueError, 'not physically contiguous'):
            selected_lba(second_lbn=42)


if __name__ == '__main__':
    unittest.main()
