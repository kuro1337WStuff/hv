"""Synthetic records only: test arithmetic and refusals, not any machine."""
import json
from pathlib import Path
from tempfile import TemporaryDirectory
import unittest

import cohort


def synthetic(mode='nested'):
    condition = dict(origin='synthetic', mode=mode, boots=5, repeats=3, warmups=1, work_units=100,
                     **{f: f'{i:064x}' for i, f in enumerate(cohort.HASH_FIELDS, start=1)})
    def trial(ns):
        reasons = {} if mode == 'native' else {'io_out': 2, 'io_in': 3, 'hlt': 1}
        return dict(elapsed_ns=ns, completed_units=100, reasons=reasons, entries=sum(reasons.values()),
                    tx_bytes=reasons.get('io_out', 0), busy_reads=reasons.get('io_out', 0))
    offset = {'nested': 0, 'physical-guest': 10, 'native': 20}[mode]
    return [dict(condition=condition.copy(), boot_id=f'synthetic-{mode}-{i}', receipt_sha256=f'{offset+i:064x}',
                 start='cold', outcome='complete', warmup=[trial(999)],
                 measured=[trial(t) for t in times]) for i, times in enumerate(
                     [(10, 20, 900), (10, 20, 901), (10, 40, 902), (80, 90, 91), (80, 90, 92)])]


class TestCohort(unittest.TestCase):
    def test_boot_weighting_not_pooled_trials(self):
        r = cohort.summarize(synthetic())
        self.assertEqual(r['condition']['origin'], 'synthetic')
        self.assertEqual(r['median_of_boot_medians_ns'], 40)  # pooled trial median would be 80
        self.assertEqual(r['boot_median_range_ns'], [20, 90])
        self.assertEqual(r['boots'][0]['elapsed_ns'], [10, 20, 900])
        self.assertEqual(r['boots'][0]['entries'], 18)
        self.assertEqual(r['boots'][0]['reason_counts'], dict(io_out=6, io_in=9, hlt=3))
        self.assertEqual((r['boots'][0]['tx_bytes'], r['boots'][0]['busy_reads']), (6, 6))
        self.assertNotIn('guest_over_native_elapsed_ratio', r)

    def test_reject_bad_trial_in_either_phase(self):
        for phase in ('warmup', 'measured'):
            for field, value in [('elapsed_ns', 0), ('elapsed_ns', 3_600_000_000_001),
                                 ('elapsed_ns', False), ('completed_units', 99), ('entries', 7),
                                 ('entries', True), ('entries', 2**64), ('entries', -1),
                                 ('reasons', {'hlt': 5, 'io_in': True}), ('reasons', {'hlt': 6.0}),
                                 ('reasons', {'hlt': 7, 'io_out': -1}), ('reasons', {'unknown': 6}),
                                 ('reasons', {'hlt': 6, 'io_in': 0}), ('tx_bytes', 3),
                                 ('busy_reads', 4), ('busy_reads', 3), ('busy_reads', 1)]:
                with self.subTest(phase=phase, field=field, value=value), self.assertRaises(ValueError):
                    rows = synthetic()
                    rows[0][phase][0][field] = value
                    cohort.summarize(rows)

    def test_reject_nonindependent_or_incomplete(self):
        for field, value in [('start', 'resume'), ('outcome', 'timeout'), ('boot_id', ''),
                             ('boot_id', 'synthetic-nested-1'), ('receipt_sha256', f'{1:064x}'),
                             ('receipt_sha256', 'not a digest'), ('warmup', []), ('measured', [])]:
            with self.subTest(field=field, value=value), self.assertRaises(ValueError):
                rows = synthetic()
                rows[0][field] = value
                cohort.summarize(rows)
        with self.assertRaises(ValueError):
            cohort.summarize(synthetic()[:-1])

    def test_reject_each_mixed_condition(self):
        for field, value in [('origin', 'observed'), ('mode', 'physical-guest'),
                             *((f, 'f'*64) for f in cohort.HASH_FIELDS), ('boots', 6),
                             ('warmups', True), ('repeats', 4), ('work_units', 101)]:
            with self.subTest(field=field), self.assertRaises(ValueError):
                rows = synthetic()
                rows[-1]['condition'][field] = value
                cohort.summarize(rows)

    def test_matched_native_only(self):
        native = cohort.summarize(synthetic('native'))
        rows = synthetic('physical-guest')
        for row in rows:
            row['condition']['runtime_sha256'] = 'a' * 64
            for t in row['measured']:
                t['elapsed_ns'] *= 2
        guest = cohort.summarize(rows)
        self.assertEqual(cohort.compare(guest, native)['guest_over_native_elapsed_ratio'], 2)
        with self.assertRaises(ValueError):
            cohort.compare(cohort.summarize(synthetic()), native)
        for row in rows:
            row['condition']['clock_sha256'] = 'c' * 64
        with self.assertRaises(ValueError):
            cohort.compare(cohort.summarize(rows), native)
        guest['boots'][0]['receipt_sha256'] = native['boots'][0]['receipt_sha256']
        with self.assertRaises(ValueError):
            cohort.compare(guest, native)

    def test_reject_mislabeled_execution_modes(self):
        for actual, claimed in [('nested', 'native'), ('native', 'physical-guest')]:
            with self.subTest(actual=actual), self.assertRaises(ValueError):
                rows = synthetic(actual)
                for row in rows:
                    row['condition']['mode'] = claimed
                cohort.summarize(rows)

    def test_json_file_and_duplicate_field_refusal(self):
        with TemporaryDirectory() as temp:
            path = Path(temp) / 'synthetic.json'
            data = json.dumps(synthetic())
            path.write_text(data, encoding='utf-8')
            self.assertEqual(cohort.load(path), cohort.summarize(synthetic()))
            path.write_text(data.replace('"cold"', '"resume", "start": "cold"', 1), encoding='utf-8')
            with self.assertRaisesRegex(ValueError, 'duplicate JSON field'):
                cohort.load(path)


if __name__ == '__main__':
    unittest.main()
