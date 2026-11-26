"""Offline, temporary functional outcome fixtures; no measurements or journal access."""
import json
from pathlib import Path
from tempfile import TemporaryDirectory
import unittest
import xml.etree.ElementTree as ET

import outcome_matrix


NS = '{http://www.w3.org/2000/svg}'


def row(label='Check & result', status='partial', trial='001', commit='1234567', scope='host'):
    return dict(label=label, status=status, trial=trial, commit=commit, scope=scope,
                source=f'journal/trials/{trial}.md')


class TestOutcomeMatrix(unittest.TestCase):
    def setUp(self):
        temp = TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name) / 'reports'
        self.root.mkdir()

    def report(self, name, records):
        (self.root / name).write_text(json.dumps(dict(title='Report', records=records, comparisons=[])),
                                      encoding='utf-8')
        return name

    def render(self, *files):
        return outcome_matrix.render(dict(title='Builds & checks', reports=list(files), benchmarks=[]), self.root)

    def test_unequal_builds_scope_identity_and_missing(self):
        a = self.report('first.json', [row(), row('Cold boot', 'held'), row('Disk', 'failed'),
                                      row('Screen', 'not-run'), row('Sound', 'skipped'), row('Clock', 'unmeasured')])
        b = self.report('second.json', [row('CHECK & RESULT', 'passed', '002', 'abcdef0')])
        c = self.report('guest.json', [row(scope='nested')])
        svg = ET.fromstring(self.render(a, b, c))
        self.assertEqual(svg.get('width'), '960')
        self.assertEqual(svg.get('role'), 'img')
        for ident in svg.get('aria-labelledby').split():
            self.assertTrue(svg.find(f"*[@id='{ident}']").text)
        text = [el.text for el in svg.findall(NS + 'text')]
        self.assertEqual(text.count('PASSED'), 1)
        self.assertEqual(text.count('PARTIAL'), 2)
        self.assertEqual(text.count('NOT RECORDED'), 5)
        for status in ('FAILED', 'HELD', 'NOT-RUN', 'SKIPPED', 'UNMEASURED'):
            self.assertEqual(text.count(status), 1)
        self.assertEqual([t for t in text if t.casefold() == 'check & result'], ['CHECK & RESULT', 'Check & result'])
        self.assertIn('S001   B001   host   trial 001   commit 1234567', text)
        self.assertIn('S003   B003   nested   trial 001   commit 1234567', text)
        self.assertEqual(text.count('journal/trials/001.md'), 2)

    def test_deterministic_order_identical_citations_and_escaped_text(self):
        records = [row('Console & boot'), row('Disk "ready"', 'held')]
        a = self.report('a.json', records)
        duplicate = self.report('copy.json', [dict(records[0], label='console & boot')])
        b = self.report('b.json', [row('Console & boot', 'failed', '002', '1234567890abcdef')])
        image = self.render(a, b)
        self.report(a, records[::-1])
        self.assertEqual(image, self.render(b, duplicate, a))
        self.assertIn(b'Console &amp; boot', image)
        text = [el.text for el in ET.fromstring(image).findall(NS + 'text')]
        self.assertIn('Disk "ready"', text)
        self.assertIn('S002   B002   host   trial 002   commit 1234567890abcdef', text)
        self.assertLess(text.index('Console & boot'), text.index('Disk "ready"'))

    def test_conflicts_in_result_or_provenance_rejected(self):
        a = self.report('a.json', [row()])
        for changes in ({'status': 'passed'}, {'trial': '002'}, {'source': 'journal/other.md'}):
            with self.subTest(changes=changes):
                b = self.report('b.json', [dict(row(), label='CHECK & RESULT', **changes)])
                with self.assertRaisesRegex(ValueError, 'conflicting outcome or source'):
                    self.render(a, b)

    def test_archived_json_and_paths_checked(self):
        malicious = '../outside.json'
        for name in (malicious, '/absolute.json', 'https://example.test/r.json', 'NUL.json', 'a.txt'):
            with self.subTest(name=name), self.assertRaises(ValueError):
                self.render(name)
        a = self.report('a.json', [dict(row(), source='../outside.md')])
        with self.assertRaises(ValueError):
            self.render(a)
        path = self.root / a
        path.write_text('{"records": [{"status":"partial", "status":"passed"}]}', encoding='utf-8')
        with self.assertRaisesRegex(ValueError, 'duplicate JSON field'):
            self.render(a)
        path.write_bytes(b' ' * (128 * 1024 + 1))
        with self.assertRaisesRegex(ValueError, 'exceeds 128 KiB'):
            self.render(a)

    def test_symlink_outside_manifest_refused(self):
        target = self.root.parent / 'outside.json'
        target.write_text('{}', encoding='utf-8')
        try:
            (self.root / 'link.json').symlink_to(target)
        except (OSError, NotImplementedError) as exc:
            self.skipTest(f'cannot create symlink: {exc}')
        with self.assertRaisesRegex(ValueError, 'leaves manifest directory'):
            self.render('link.json')

    def test_panel_width_and_bounded_size(self):
        files = [self.report(f'{i}.json', [row(commit=f'{i:07x}', trial=f'{i:03}')]) for i in range(5)]
        text = [el.text for el in ET.fromstring(self.render(*files)).findall(NS + 'text')]
        self.assertEqual(text.count('HOST   /   CHECK'), 2)
        self.assertIn('B005', text)
        for i in range(3):
            self.report(files[i], [row(f'Check {j}', commit=f'{i:07x}') for j in range(64 if i < 2 else 1)])
        with self.assertRaisesRegex(ValueError, '128 distinct outcome cells'):
            self.render(*files[:3])
        with self.assertRaisesRegex(ValueError, '0–32 archived reports'):
            self.render(*(['0.json'] * 33))

    def test_empty_archive_is_not_a_result(self):
        text = [el.text for el in ET.fromstring(self.render()).findall(NS + 'text')]
        self.assertIn('No archived functional records.', text)
        self.assertNotIn('PASSED', text)


if __name__ == '__main__':
    unittest.main()
