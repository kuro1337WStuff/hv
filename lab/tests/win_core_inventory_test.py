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


def cache(*group_masks, mib=32, level=3, kind=0, count=None, tail=b''):
    prefix = struct.pack('<BBHII18xH', level, 0xff, 64, mib << 20, kind,
                         len(group_masks) if count is None else count)
    masks = b''.join(struct.pack('<QH6x', mask, group) for group, mask in group_masks)
    payload = prefix + masks + tail
    return struct.pack('<II', 2, 8 + len(payload)) + payload


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


class WinL3InventoryTest(unittest.TestCase):
    cores = b''.join(record(0, 3 << bit) for bit in range(0, 32, 2))

    def test_symmetric_and_asymmetric_same_core_shape(self):
        low = cache((0, 0xffff), count=0, tail=b'\xcc' * 16)  # Legacy GroupMask.
        for high_mib in (32, 96):
            with self.subTest(high_mib=high_mib):
                high = cache((0, 0xffff0000), mib=high_mib)
                report = inventory(self.cores, 32, 'fixture', cache_data=high + low)
                l3 = report['unified_l3']
                self.assertTrue(report['target']['core_shape_with_exact_l3_coverage'])
                self.assertEqual(l3['coverage'], 'exact')
                self.assertEqual(l3['caches'], [
                    {'size_bytes': size << 20, 'logical_processors': [f'0:{i}' for i in range(n, n+16)],
                     'intersecting_core_ids': [f'core-0:{i}' for i in range(n, n+16, 2)]}
                    for n, size in [(0, 32), (16, high_mib)]])
                self.assertEqual(report, inventory(self.cores, 32, 'fixture', cache_data=low + high))

    def test_one_cache_spans_groups(self):
        cores = record(2, 1 << 63) + record(0, 1 << 63)
        data = cache((2, 1 << 63), (0, 1 << 63))
        l3 = inventory(cores, 2, None, cache_data=data)['unified_l3']
        self.assertEqual(l3['coverage'], 'exact')
        self.assertEqual(l3['caches'], [{'size_bytes': 32 << 20, 'logical_processors': ['0:63', '2:63'],
                                         'intersecting_core_ids': ['core-0:63', 'core-2:63']}])

    def test_reject_bad_membership_despite_matching_active_and_core_counts(self):
        for first, second, field, expected in [
                (0xffff, 0xffff0001, 'overlapping_lps', ['0:0']),
                (0xfffc, 0xffff0000, 'missing_lps', ['0:0', '0:1']),
                (0xffff, 0x1ffff0000, 'unknown_lps', ['0:32']),
                (0x7fff, 0xffff8000, 'split_sibling_core_ids', ['core-0:14'])]:
            with self.subTest(field=field):
                data = cache((0, first)) + cache((0, second))
                report = inventory(self.cores, 32, None, cache_data=data)
                self.assertTrue(report['counts_agree'])
                self.assertTrue(report['target']['observed_shape_matches'])
                self.assertFalse(report['target']['core_shape_with_exact_l3_coverage'])
                self.assertEqual(report['unified_l3']['coverage'], 'inconsistent')
                self.assertEqual(report['unified_l3'][field], expected)

    def test_no_unified_l3(self):
        for data in (b'', cache((0, 1), level=2) + cache((0, 1), kind=2)):
            report = inventory(record(0, 1), 1, None, cache_data=data)
            self.assertEqual(report['unified_l3']['coverage'], 'absent')
            self.assertEqual(report['unified_l3']['caches'], [])

    def test_malformed_not_absent(self):
        good = cache((0, 1))
        for data, error in [(good[:-1], 'record size'), (good + good[:7], 'truncated'),
                            (cache(count=0), 'record size'), (record(0, 1), 'relationship'),
                            (cache((0, 1), count=2, level=2) + good, 'GroupCount'),
                            (cache((0, 0)), 'affinity'), (cache((65535, 1)), 'affinity'),
                            (cache((2, 1), (2, 2)), 'duplicate group'), (cache((0, 1), mib=0), 'size')]:
            with self.subTest(error=error, data=data), self.assertRaisesRegex(ValueError, error):
                inventory(record(0, 1), 1, None, cache_data=data)


if __name__ == '__main__':
    unittest.main()
