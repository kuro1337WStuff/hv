"""Renderer tests only. All timing records here are invented, never project evidence."""
from copy import deepcopy
import json
from pathlib import Path
import subprocess
import sys
from tempfile import TemporaryDirectory
import unittest
import xml.etree.ElementTree as ET

import cohort
import journal_dashboard as dashboard
from test_cohort import synthetic


def report():
    return {'title': 'Build & boot', 'records': [
        {'label': 'Console & boot', 'scope': 'nested', 'status': 'skipped', 'trial': '099',
         'commit': '0123456789abcdef', 'source': 'journal/trials/099.md'}], 'comparisons': []}


def text(image):
    root = ET.fromstring(image)
    return [node.text for node in root.findall('{http://www.w3.org/2000/svg}text')]


def pair_rows():
    baseline = synthetic()
    candidate = deepcopy(baseline)
    # Exercise the observed-only publishing branch in a temporary fixture, not a retained report.
    for rows in (baseline, candidate):
        for row in rows:
            row['condition']['origin'] = 'observed'
    for i, row in enumerate(candidate):
        row['condition']['runtime_sha256'] = 'f' * 64
        row['boot_id'] = f'candidate-{i}'
        row['receipt_sha256'] = f'{100+i:064x}'
        for trial in row['measured']:
            trial['elapsed_ns'] //= 2
    return baseline, candidate


class TestDashboard(unittest.TestCase):
    def setUp(self):
        temporary = TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)

    def measured_report(self, rows):
        data = report()
        pair = {'label': 'Workload elapsed'}
        for arm, boots, rev in zip(('baseline', 'candidate'), rows, ('0123456', 'abcdef0')):
            name = arm + '.json'
            (self.root / name).write_text(json.dumps(boots), encoding='utf-8')
            pair[arm] = {'id': arm + '-boots', 'commit': rev, 'file': name}
        data['comparisons'].append(pair)
        return data

    def test_cli_deterministic_accessible_unmeasured(self):
        source, dest = self.root / 'report.json', self.root / 'report.svg'
        data = report()
        data['records'] += [dict(data['records'][0], scope='host', status='held'),
                            dict(data['records'][0], scope='physical', status='not-run')]
        source.write_text(json.dumps(data), encoding='utf-8')
        command = [sys.executable, dashboard.__file__, str(source), '--output', str(dest)]
        subprocess.run(command, check=True)
        image = dest.read_bytes()
        self.assertEqual(image, dashboard.render(data, self.root))
        root = ET.fromstring(image)
        self.assertEqual(root.get('role'), 'img')
        self.assertEqual({child.get('id') for child in root if child.get('id')}, {'title', 'desc'})
        lines = text(image)
        for literal in ('Console & boot', 'Trial 099  /  commit 0123456789ab', 'journal/trials/099.md',
                        'HOST', 'NESTED', 'PHYSICAL', 'SKIPPED', 'HELD', 'NOT-RUN', 'UNMEASURED'):
            self.assertIn(literal, lines)
        self.assertNotIn('PASSED', lines)
        self.assertNotIn(b'%', image)
        source.write_text('{"title":"one","title":"two"}', encoding='utf-8')
        rejected = subprocess.run(command, capture_output=True)
        self.assertEqual(rejected.returncode, 2)
        self.assertIn(b'duplicate JSON field', rejected.stderr)
        self.assertEqual(dest.read_bytes(), image)

    def test_elapsed_uses_independent_boot_medians_and_direction(self):
        data = self.measured_report(pair_rows())
        lines = text(dashboard.render(data, self.root))
        self.assertIn('Baseline: baseline-boots  /  commit 0123456', lines)
        self.assertIn('Candidate: candidate-boots  /  commit abcdef0', lines)
        for value in (40, 20):
            self.assertIn(f'n = 5 independent cold boots   /   median of boot medians: {value} ns', lines)
        self.assertIn('Per-boot medians (ns): 20, 20, 40, 90, 90', lines)
        self.assertTrue(any('nested' in line and '+50.00%' in line for line in lines))
        pair = data['comparisons'][0]
        pair['baseline'], pair['candidate'] = pair['candidate'], pair['baseline']
        self.assertTrue(any('-100.00%' in line for line in text(dashboard.render(data, self.root))))

    def test_matching_constraints_and_receipts(self):
        a, b = map(cohort.summarize, pair_rows())
        for key in ('mode', 'platform_sha256', 'workload_sha256', 'clock_sha256', 'controls_sha256',
                    'boots', 'warmups', 'repeats', 'work_units', 'runtime_sha256'):
            with self.subTest(key=key), self.assertRaisesRegex(ValueError, 'unmatched|distinct runtime'):
                changed = deepcopy(b)
                changed['condition'][key] = a['condition'][key] if key == 'runtime_sha256' else 'different'
                dashboard.matched(a, changed)
        for key in ('boot_id', 'receipt_sha256'):
            with self.subTest(key=key), self.assertRaisesRegex(ValueError, 'reused'):
                changed = deepcopy(b)
                changed['boots'][-1][key] = a['boots'][0][key]
                dashboard.matched(a, changed)

    def test_timing_cannot_publish_synthetic_or_incomplete_inputs(self):
        for rows, message in [((synthetic(), synthetic()), 'observed'),
                              ((pair_rows()[0][:-1], pair_rows()[1]), 'missing or extra')]:
            with self.subTest(message=message), self.assertRaisesRegex(ValueError, message):
                dashboard.render(self.measured_report(rows), self.root)
        data = self.measured_report(pair_rows())
        (self.root / 'baseline.json').unlink()
        with self.assertRaises(FileNotFoundError):
            dashboard.render(data, self.root)

    def test_rejects_bad_metadata(self):
        for key, val in [('label', 'x' * 49), ('label', '<b>'), ('source', '../other'),
                         ('source', '/etc/passwd'), ('source', 'CON.txt'), ('trial', 99),
                         ('commit', 'unknown'), ('scope', 'unknown'), ('status', 'unspecified')]:
            with self.subTest(key=key, value=val), self.assertRaises(ValueError):
                data = report()
                data['records'][0][key] = val
                dashboard.render(data, self.root)
        data = report()
        data['records'].append(dict(data['records'][0]))
        with self.assertRaisesRegex(ValueError, 'duplicate evidence'):
            dashboard.render(data, self.root)
        del data['records'][0]['source']
        with self.assertRaisesRegex(ValueError, 'fields'):
            dashboard.render(data, self.root)
        for key, value, message in [('file', '../outside.json', 'component'),
                                    ('commit', '0123456789a', 'distinct commits'),
                                    ('id', 'baseline-boots', 'ambiguous cohort')]:
            with self.subTest(key=key), self.assertRaisesRegex(ValueError, message):
                data = self.measured_report(pair_rows())
                data['comparisons'][0]['candidate'][key] = value
                dashboard.render(data, self.root)


if __name__ == '__main__':
    unittest.main()
