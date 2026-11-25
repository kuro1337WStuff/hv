"""Run from the lab checkout: python -m unittest lab.tests.guest_tx_evidence_test

The terminal tests need the retained private run 047. Do not generate a substitute
if unavailable; the journal frame alone cannot establish a fresh broker boot.
"""
from dataclasses import replace
from pathlib import Path
from shutil import copyfile
from tempfile import TemporaryDirectory
import unittest

from lab.guest_tx_evidence import Attempt, ReviewedStop, line_offsets, verify_guest_tx


# Independent positive from the reviewed 047 frame: HLT RIP 1172, 44 entries;
# denied COM2 OUT at 11a5, no advance; both L1 raw fields 4c323a383235300a,
# count 8 < sink capacity 16. This authorizes no other profile or invocation.
A047 = Attempt('1aa334e6324c43039c23009df0f84f56', 'l1-serial-8250',
               '1FD786103955F1108FC7370DEEF12DC941B6E310DBF8D14F5B0F9E392465C133')
STOP047 = ReviewedStop(A047, '29d42f92ffd014ea2657cc7008fd040c41080ed92c35b2821660a59d525172e2',
                       'c2e83949ce69269019649f99001736a012309c9b46ca501dbd64933579415648', 8,
                       'e43ba5ff5acd9ca6ab3a05735dcb0d3394c60695e1a7991131b3c3ee568d5c5a', False)
RUN = Path(__file__).resolve().parents[2] / 'build' / 'runs' / A047.run_id
TX047 = bytes.fromhex('4c323a383235300a')  # transcription, not a new live UART capture


class SerialEvidenceTest(unittest.TestCase):
    def test_split_lines_and_replay(self):
        # Two writers on the same boot: no UTF-8 decode, CR trimming, line
        # deduplication, synthetic phase identification, or boot count.
        raw = b'\x00\xff\r\nEARLY\r\nPA'
        self.assertEqual(list(line_offsets(raw, b'PASS\r\n')), [])
        raw += b'SS\r\nTTY\r\nPASS\r'
        self.assertEqual(list(line_offsets(raw, b'PASS\r\n')), [11])
        raw += b'\nNOT-PASS\r\nPASS\n'
        self.assertEqual(list(line_offsets(raw, b'PASS\r\n')), [11, 22])
        self.assertEqual(list(line_offsets(raw, b'PASS\n')), [38])
        with self.assertRaises(ValueError):
            list(line_offsets(raw, b'PASS'))

    @unittest.skipUnless(RUN.is_dir(), 'requires retained 047 broker evidence')
    def test_retained_stop_is_not_a_new_boot(self):
        self.assertEqual(verify_guest_tx(RUN, A047, STOP047, TX047), 'ATTENTION')
        for requested in (replace(A047, profile='l1-cpu-contract'),
                          replace(A047, run_id='7f7381905f8d4144899307b6fc8a8d2b'),
                          replace(A047, media_sha256='0' * 64)):
            with self.subTest(requested=requested), self.assertRaisesRegex(ValueError, 'wrong attempt'):
                verify_guest_tx(RUN, requested, STOP047, TX047)

    @unittest.skipUnless(RUN.is_dir(), 'requires retained 047 broker evidence')
    def test_guest_text_is_not_a_verdict(self):
        with self.assertRaisesRegex(ValueError, 'no independently reviewed'):
            verify_guest_tx(RUN, A047, None, b'PASS\r\n')
        for raw in (b'PASS\r\n', TX047[:-1], TX047 + b'PASS', TX047[:-1] + b'\r'):
            with self.subTest(raw=raw), self.assertRaisesRegex(ValueError, 'different guest TX'):
                verify_guest_tx(RUN, A047, STOP047, raw)
        with self.assertRaisesRegex(ValueError, 'incomplete'):
            verify_guest_tx(RUN, A047, replace(STOP047, overflow=True), TX047)

    @unittest.skipUnless(RUN.is_dir(), 'requires retained 047 broker evidence')
    def test_wrong_frame_or_missing_broker_evidence(self):
        with TemporaryDirectory() as tmp:
            candidate = Path(tmp) / A047.run_id
            candidate.mkdir()
            for name in ('queue.json', 'events.jsonl'):
                copyfile(RUN / name, candidate / name)
            # Real predecessor frame supplied under the correct profile filename.
            copyfile(RUN / 'prior-l1-cpu-contract.png', candidate / (A047.profile + '.png'))
            with self.assertRaisesRegex(ValueError, 'not the reviewed L1 stop frame'):
                verify_guest_tx(candidate, A047, STOP047, TX047)
            (candidate / 'events.jsonl').write_bytes(b'')
            with self.assertRaisesRegex(ValueError, 'not the reviewed broker record'):
                verify_guest_tx(candidate, A047, STOP047, TX047)


if __name__ == '__main__':
    unittest.main()
