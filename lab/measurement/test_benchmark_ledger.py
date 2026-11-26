"""In-memory invented timings for ledger tests only; never retained journal evidence."""
from copy import deepcopy
import hashlib
import json
from pathlib import Path
import unittest
from unittest.mock import patch
import xml.etree.ElementTree as ET

import benchmark_ledger as ledger
from test_cohort import synthetic


NS = '{http://www.w3.org/2000/svg}'
ROOT = Path(__file__).resolve().parent


def fixture(kind='build', modes=None):
    modes = modes or (('nested', 'nested') if kind == 'build' else ('native', 'physical-guest'))
    pair = {'label': 'Work & time', 'kind': kind}
    files = {}
    for side, mode in zip(('baseline', 'candidate'), modes):
        name = side + '.json'
        candidate = side == 'candidate'
        pair[side] = {'id': side, 'file': name, 'commit': 'def1234' if candidate and kind == 'build' else 'abc1234'}
        files[name] = rows = synthetic(mode)
        for i, row in enumerate(rows):
            # Exercise the observed-only branch without writing purported measurements to disk.
            row['condition']['origin'] = 'observed'
            if candidate:
                row['condition']['runtime_sha256'] = 'a' * 64
                row['boot_id'] = f'candidate-{i}'
                row['receipt_sha256'] = f'{100+i:064x}'
                for trial in row['measured']:
                    trial['elapsed_ns'] //= 2
    return {'title': 'Research & reporting', 'reports': [], 'benchmarks': [pair]}, files


def render(manifest, files):
    def read(path, *, encoding):
        return json.dumps(files[path.relative_to(ROOT).as_posix()])
    with patch.object(Path, 'read_text', read):
        return ledger.render(manifest, ROOT)


def texts(image):
    return [node.text for node in ET.fromstring(image).findall(NS + 'text')]


class BenchmarkLedgerTests(unittest.TestCase):
    def test_empty_is_accessible_unmeasured_not_zero(self):
        manifest = {'title': 'A & B', 'reports': [], 'benchmarks': []}
        image = render(manifest, {})
        root = ET.fromstring(image)
        self.assertEqual(root.get('role'), 'img')
        self.assertEqual(root.get('aria-labelledby'), 'title desc')
        self.assertIn('A & B', root.find(NS + 'title').text)
        self.assertIn('Unmeasured', root.find(NS + 'desc').text)
        self.assertIn('UNMEASURED', texts(image))
        self.assertNotRegex(' '.join(texts(image)), r'\d')
        self.assertEqual(image, render(manifest, {}))

    def test_build_elapsed_direction_provenance_and_dispersion(self):
        manifest, files = fixture()
        image = render(manifest, files)
        lines = texts(image)
        for line in ('Work & time', 'OBSERVED  /  SAME-MODE BUILD  /  NESTED',
                     'Elapsed change (candidate − baseline): -20 ns; lower is better.',
                     'Median of boot medians: 40 ns', 'Median of boot medians: 20 ns',
                     '5 independent cold boots  /  range of boot medians: 20 to 90 ns',
                     'Source: baseline.json', 'Commit: abc1234', 'Source: candidate.json', 'Commit: def1234',
                     'Workload SHA-256: ' + files['baseline.json'][0]['condition']['workload_sha256']):
            self.assertIn(line, lines)
        for name, rows in files.items():
            start = lines.index('Source: ' + name)
            stop = lines.index('Candidate: candidate  /  nested') if name == 'baseline.json' else len(lines)
            for row in rows:
                self.assertIn('Boot ' + row['boot_id'], lines[start:stop])
                self.assertIn(row['receipt_sha256'], lines[start:stop])
            rows.reverse()
        self.assertEqual(image, render(manifest, files))
        pair = manifest['benchmarks'][0]
        pair['baseline'], pair['candidate'] = pair['candidate'], pair['baseline']
        self.assertIn('Elapsed change (candidate − baseline): +20 ns; lower is better.', texts(render(manifest, files)))

    def test_toggle_requires_recorded_off_on_on_the_same_binary(self):
        manifest, files = fixture('feature', ('nested', 'nested'))
        pair = manifest['benchmarks'][0]
        pair['gate'] = {'feature': 'capture', 'common_controls_sha256': 'b' * 64}
        for side, flag in [('baseline', b'off'), ('candidate', b'on')]:
            binding = hashlib.sha256(b'hv-feature-v1\n' + b'b' * 64 + b'\ncapture\n' + flag + b'\n').hexdigest()
            for row in files[side + '.json']:
                row['condition'].update(runtime_sha256='c' * 64, controls_sha256=binding)
        lines = texts(render(manifest, files))
        self.assertTrue(any('capture' in line and 'off to on' in line.lower() for line in lines))
        self.assertTrue(any('-20 ns' in line for line in lines))
        pair['baseline'], pair['candidate'] = pair['candidate'], pair['baseline']
        with self.assertRaisesRegex(ValueError, 'not bound'):
            render(manifest, files)

    def test_physical_guest_vs_native_may_share_commit(self):
        lines = texts(render(*fixture('native')))
        self.assertIn('OBSERVED  /  NATIVE BASELINE  /  PHYSICAL GUEST CANDIDATE', lines)
        self.assertIn('Elapsed change (candidate − baseline): -20 ns; lower is better.', lines)
        self.assertEqual(lines.count('Commit: abc1234'), 2)

    def test_refuses_wrong_modes(self):
        for kind, modes in [('native', ('native', 'nested')), ('native', ('physical-guest', 'native')),
                            ('build', ('nested', 'physical-guest'))]:
            with self.subTest(kind=kind, modes=modes), self.assertRaisesRegex(ValueError, 'mode|physical guest'):
                render(*fixture(kind, modes))
        manifest, files = fixture()
        for row in files['candidate.json']:
            row['condition']['mode'] = 'other'
        with self.assertRaisesRegex(ValueError, 'unknown execution mode'):
            render(manifest, files)

    def test_refuses_unmatched_or_unobserved_conditions(self):
        for kind in ('native', 'build'):
            for key, value in [('platform_sha256', 'e'*64), ('workload_sha256', 'e'*64),
                               ('clock_sha256', 'e'*64), ('controls_sha256', 'e'*64),
                               ('warmups', 2), ('origin', 'synthetic')]:
                manifest, files = fixture(kind)
                for row in files['candidate.json']:
                    row['condition'][key] = value
                    if key == 'warmups':
                        row['warmup'] *= 2
                with self.subTest(kind=kind, key=key), self.assertRaisesRegex(ValueError, 'observed|unmatched'):
                    render(manifest, files)
        manifest, files = fixture()
        for row in files['candidate.json']:
            row['condition']['runtime_sha256'] = files['baseline.json'][0]['condition']['runtime_sha256']
        with self.assertRaisesRegex(ValueError, 'distinct runtime'):
            render(manifest, files)

    def test_refuses_reused_evidence(self):
        for kind in ('build', 'native'):
            for key in ('receipt_sha256', 'boot_id'):
                manifest, files = fixture(kind)
                files['candidate.json'][0][key] = files['baseline.json'][0][key]
                with self.subTest(kind=kind, key=key), self.assertRaisesRegex(ValueError, 'reused'):
                    render(manifest, files)
        manifest, files = fixture()
        pair = deepcopy(manifest['benchmarks'][0])
        pair['label'] = 'Second comparison'
        manifest['benchmarks'].append(pair)
        with self.assertRaisesRegex(ValueError, 'reused cohort'):
            render(manifest, files)
        for side in ('baseline', 'candidate'):
            arm = pair[side]
            arm['id'] += '-copy'
            arm['file'] = arm['id'] + '.json'
            files[arm['file']] = files[side + '.json']
        with self.assertRaisesRegex(ValueError, 'reused boot'):
            render(manifest, files)

    def test_rejects_ambiguous_build_revision_and_unsafe_path(self):
        for key, value, message in [('commit', 'abc1234', 'distinct commits'),
                                    ('commit', 'abc1234ff', 'distinct commits'),
                                    ('file', '../outside.json', 'component')]:
            manifest, files = fixture()
            manifest['benchmarks'][0]['candidate'][key] = value
            with self.subTest(key=key, value=value), self.assertRaisesRegex(ValueError, message):
                render(manifest, files)

    def test_layout_wraps_maximum_length_provenance(self):
        manifest, files = fixture()
        pair = manifest['benchmarks'][0]
        pair['label'] = manifest['title'] = 'W' * 48
        arm = pair['candidate']
        arm.update(id='W'*32, file='W'*75 + '.json', commit='f'*40)
        files[arm['file']] = files.pop('candidate.json')
        files[arm['file']][0]['boot_id'] = 'W'*128
        for line in texts(render(manifest, files)):
            self.assertLessEqual(len(line), 84)


if __name__ == '__main__':
    unittest.main()
