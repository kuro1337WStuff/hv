"""x64 little-endian Win32 record fixtures; these tests never query the host."""
import struct
import unittest

from lab.win_core_inventory import core_members, inventory


def record(group, mask, *, group_count=1, tail=b'', flags=None, relation=0, size=None):
    if flags is None:
        flags = int(mask.bit_count() > 1)
    # Compose the three SDK structs separately, including the otherwise-unused
    # efficiency class and reserved words, so sizeof(pointer) here is irrelevant.
    affinity = struct.pack('<QH3H', mask, group, 0, 0, 0)
    processor = struct.pack('<BB20xH', flags, 5, group_count) + affinity + tail
    return struct.pack('<II', relation, 8 + len(processor) if size is None else size) + processor


class WinCoreInventoryTest(unittest.TestCase):
    def test_sparse_groups_and_variable_strides(self):
        first = record(0, (1 << 63) | (1 << 4), tail=b'\xcc' * 16)
        second = record(2, 1 << 4)
        report = inventory(second + first, 3, 'fixture')
        self.assertEqual(report, inventory(first + second, 3, 'fixture'))
        self.assertEqual(report['os_visible'], {
            'physical_cores': 2, 'logical_processors': 3, 'cores_with_smt_siblings': 1,
            'cores': [{'core_id': 'core-0:4', 'logical_processors': ['0:4', '0:63']},
                      {'core_id': 'core-2:4', 'logical_processors': ['2:4']}]})
        self.assertTrue(report['counts_agree'])
        self.assertFalse(report['target']['observed_shape_matches'])

    def test_target_shape_independent_of_brand_and_smt_off(self):
        for smt in (False, True):
            with self.subTest(smt=smt):
                width = 2 if smt else 1
                blob = b''.join(record(0, ((1 << width) - 1) << (i * width)) for i in range(16))
                report = inventory(blob, 16 * width, 'unrelated fixture CPU')
                self.assertEqual(report['cpu_model_registry'], 'unrelated fixture CPU')
                self.assertEqual(report['os_visible']['physical_cores'], 16)
                self.assertEqual(report['os_visible']['logical_processors'], 16 * width)
                self.assertEqual(report['os_visible']['cores_with_smt_siblings'], 16 if smt else 0)
                self.assertEqual(report['target']['observed_shape_matches'], smt)
                self.assertTrue(report['counts_agree'])

    def test_equal_totals_do_not_imply_two_threads_on_every_core(self):
        blob = record(0, 1) + record(0, 14) + b''.join(record(0, 3 << i) for i in range(4, 32, 2))
        self.assertFalse(inventory(blob, 32, None)['target']['observed_shape_matches'])

    def test_missing_core_detected_by_independent_count(self):
        report = inventory(record(0, 3), 4, None)
        self.assertEqual(report['os_visible']['logical_processors'], 2)
        self.assertEqual(report['independent_active_processors'], 4)
        self.assertFalse(report['counts_agree'])

    def test_bad_records(self):
        good = record(0, 3)
        cases = [(b'', 'missing'), (good + good[:7], 'truncated'), (good[:-1], 'record size'),
                 (record(0, 3, size=0), 'record size'), (record(0, 3, size=47), 'record size'),
                 (record(0, 3, relation=3), 'relationship'), (record(0, 0), 'affinity'),
                 (record(65535, 3), 'affinity'), (record(0, 1, group_count=0), 'GroupCount'),
                 (record(0, 3, group_count=2, tail=bytes(16)), 'GroupCount'),
                 (good + good, 'duplicate'), (good + record(0, 6), 'duplicate'),
                 (record(0, 3, flags=0), 'SMT'), (record(0, 1, flags=1), 'SMT')]
        for blob, message in cases:
            with self.subTest(blob=blob, message=message), self.assertRaisesRegex(ValueError, message):
                core_members(blob)


if __name__ == '__main__':
    unittest.main()
