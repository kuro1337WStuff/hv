"""Run from the lab checkout: python -m unittest lab.tests.guest_tx_evidence_test

Only the retained-run tests need private run 047. The milestone tests use
temporary host fixtures to exercise the offline API; they are not boot evidence.
"""
from dataclasses import replace
from hashlib import sha256
import json
from pathlib import Path
from shutil import copyfile
from tempfile import TemporaryDirectory
import unittest

from lab.guest_tx_evidence import (
    Attempt, InitMilestones, ReviewedS5Stop, ReviewedStop, line_offsets, observe_init_milestones,
    observe_init_shutdown, verify_guest_tx,
)


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


class _BoundInitCase(unittest.TestCase):
    user = b'HV:P5:USER pid=1 kernel=6.18.60-0-virt'
    timer = b'HV:P5:TIMER before=9.99 after=10.00'
    shutdown = b'HV:P5:SHUTDOWN'
    prefix = b'\xff\x00\r\nboot quoted: HV:P5:SHUTDOWN\n'
    between = b'\nkernel: unrelated output\r\n'
    raw = prefix + user + b'\r\n' + between + timer + b'\n' + shutdown + b'\r\n'

    def setUp(self):
        tmp = TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.attempt = Attempt('test-run', 'test-profile', 'AB' * 32)
        self.directory = Path(tmp.name) / self.attempt.run_id
        self.directory.mkdir()
        (self.directory / 'queue.json').write_text(json.dumps([
            {'profile': self.attempt.profile, 'sha256': self.attempt.media_sha256},
        ]))
        self.frame = b'opaque test-only frame fixture'
        (self.directory / (self.attempt.profile + '.png')).write_bytes(self.frame)
        self.review = self.stage_stop('ATTENTION')

    def stage_stop(self, disposition):
        a = self.attempt
        events = [{'state': 'CHECKPOINT', 'detail': f"Saving prior state as 'HvQueue before {a.run_id}'."},
                  {'state': 'MOUNT', 'profile': a.profile,
                   'detail': f'Mounting reviewed {a.profile} media; SHA256={a.media_sha256}'},
                  {'state': 'BOOT', 'profile': a.profile}, {'state': 'WATCHING', 'profile': a.profile},
                  {'state': disposition, 'profile': a.profile}]
        record = b''.join(json.dumps(row).encode() + b'\n' for row in events)
        (self.directory / 'events.jsonl').write_bytes(record)
        return ReviewedStop(a, sha256(record).hexdigest(), sha256(self.frame).hexdigest(),
                            len(self.raw), sha256(self.raw).hexdigest(), False)


class InitMilestonesTest(_BoundInitCase):
    def test_offsets_values_and_unchanged_disposition(self):
        user_at = len(self.prefix)
        timer_at = user_at + len(self.user) + 2 + len(self.between)
        shutdown_at = timer_at + len(self.timer) + 1
        for state in ('ATTENTION', 'COMPLETE'):
            with self.subTest(state=state):
                review = self.stage_stop(state)
                self.assertEqual(observe_init_milestones(self.directory, self.attempt, review, self.raw),
                                 InitMilestones(state, user_at, timer_at, shutdown_at, b'9.99', b'10.00'))

    def assert_bound_but_invalid(self, raw):
        # Bind this deliberately malformed fixture so rejection tests the
        # milestone grammar, not an unrelated digest or count mismatch.
        review = replace(self.review, count=len(raw), tx_sha256=sha256(raw).hexdigest())
        with self.assertRaises(ValueError):
            observe_init_milestones(self.directory, self.attempt, review, raw)

    def test_invalid_milestones_despite_matching_review(self):
        u, t, s = self.user, self.timer, self.shutdown
        for lines in ((u, t), (u, s), (t, s), (t, u, s), (u, s, t), (u, t, t, s),
                      (u, t, s, u), (u, t, s, u, t, s), (u, t, b'HV:P5:OTHER', s),
                      (u.replace(b'pid=1', b'pid=10'), t, s), (u + b'x', t, s), (u, t, s + b' '),
                      (b'prefix ' + u, t, s), (u, b'\v' + t, s), (u, t, b'noise\r' + s),
                      (u, t, s + b'\r\r')):
            with self.subTest(lines=lines):
                self.assert_bound_but_invalid(b'\n'.join(lines) + b'\n')
        for raw in (self.raw[:-1], self.raw + b'HV:P5:', self.raw + s + b'\r'):
            with self.subTest(tail=raw[-16:]):
                self.assert_bound_but_invalid(raw)

    def test_timer_matches_proc_uptime_and_shell_integer_comparison(self):
        last = b'9223372036854775807.00'
        raw = self.raw.replace(self.timer, b'HV:P5:TIMER before=9223372036854775806.99 after=' + last)
        review = replace(self.review, count=len(raw), tx_sha256=sha256(raw).hexdigest())
        self.assertEqual(observe_init_milestones(self.directory, self.attempt, review, raw).after, last)
        for before, after in ((b'9.00', b'9.99'), (b'10.00', b'9.99'), (b'9', b'10'),
                              (b'9.9', b'10.00'), (b'9.99', b'10.000'), (b'09.99', b'10.00'),
                              (b'-1.00', b'0.00'), (b'9.99', b'+10.00'), (b'9e0', b'10.00'),
                              (b'9.99', b'10.0\xff'), (last, b'9223372036854775808.00'),
                              (b'9.99', b'10000000000000000000.00')):
            with self.subTest(before=before, after=after):
                bad = b'HV:P5:TIMER before=' + before + b' after=' + after
                self.assert_bound_but_invalid(self.raw.replace(self.timer, bad))

    def test_no_observations_without_original_binding(self):
        # Never derive a replacement review from the substituted input.
        for review, raw, reason in (
                (None, self.raw, 'no independently reviewed'),
                (None, b'invalid TX', 'no independently reviewed'),
                (replace(self.review, attempt=replace(self.attempt, run_id='other')), self.raw, 'wrong attempt'),
                (replace(self.review, overflow=True), self.raw, 'incomplete'),
                (replace(self.review, count=len(self.raw) + 1), self.raw, 'incomplete'),
                (self.review, self.raw.replace(b'9.99', b'8.99'), 'different guest TX')):
            with self.subTest(reason=reason), self.assertRaisesRegex(ValueError, reason):
                observe_init_milestones(self.directory, self.attempt, review, raw)

        for name, reason in (('events.jsonl', 'not the reviewed broker record'),
                             (self.attempt.profile + '.png', 'not the reviewed L1 stop frame'),
                             ('queue.json', 'wrong frozen worklist')):
            with self.subTest(name=name):
                path = self.directory / name
                original = path.read_bytes()
                path.write_bytes(b'[]')
                with self.assertRaisesRegex(ValueError, reason):
                    observe_init_milestones(self.directory, self.attempt, self.review, self.raw)
                path.write_bytes(original)


class InitShutdownTest(_BoundInitCase):
    def witness(self, review):
        # Hypothetical, separately transcribed L1 fields. No historical run
        # supplies these values; neither the raw TX nor the API computes them.
        return ReviewedS5Stop(binding=replace(review), terminal_cause='PM1a S5',
                              exit_code=0x7b, io_info1=0x06040020, operand=0x3401, lifecycle=2,
                              faulting_rip=0x1234, terminal_rip=0x1234,
                              faulting_entries=37, terminal_entries=37)

    def test_reviewed_final_out_preserves_bound_milestones(self):
        for state in ('ATTENTION', 'COMPLETE'):
            with self.subTest(state=state):
                review = self.stage_stop(state)
                result = observe_init_shutdown(self.directory, self.attempt, review, self.raw,
                                               self.witness(review))
                self.assertEqual(result, InitMilestones(
                    state, self.raw.index(self.user), self.raw.index(self.timer),
                    self.raw.rindex(self.shutdown), b'9.99', b'10.00'))

    def test_one_wrong_terminal_field_is_not_shutdown(self):
        s5 = self.witness(self.review)
        for change in (
                {'terminal_cause': 'timeout'}, {'exit_code': 0x78}, {'lifecycle': 1},
                {'io_info1': 0x06040021},  # IN
                {'io_info1': 0x06040024}, {'io_info1': 0x06040028},  # STR or REP
                {'io_info1': 0x06040010}, {'io_info1': 0x06040040}, {'io_info1': 0x06040030},
                {'io_info1': 0x06000020}, {'operand': 0x1401}, {'operand': 0x13401},
                {'terminal_rip': 0x1236}, {'terminal_entries': 38}, {'terminal_entries': 36},
                {'faulting_entries': 0, 'terminal_entries': 0},
                {'faulting_entries': True, 'terminal_entries': True},
                {'faulting_rip': None, 'terminal_rip': None}):
            with self.subTest(change=change), self.assertRaises(ValueError):
                observe_init_shutdown(self.directory, self.attempt, self.review, self.raw,
                                      replace(s5, **change))

    def test_terminal_review_cannot_cross_artifacts(self):
        s5 = self.witness(self.review)
        for other in (replace(self.review, attempt=replace(self.attempt, run_id='other')),
                      replace(self.review, attempt=replace(self.attempt, profile='other')),
                      replace(self.review, attempt=replace(self.attempt, media_sha256='CD' * 32)),
                      replace(self.review, frame_sha256='01' * 32),
                      replace(self.review, events_sha256='02' * 32),
                      replace(self.review, tx_sha256='03' * 32)):
            with self.subTest(other=other), self.assertRaisesRegex(ValueError, 'same reviewed stop'):
                observe_init_shutdown(self.directory, self.attempt, self.review, self.raw,
                                      replace(s5, binding=other))
        # Two agreeing reviews are not sufficient if the actual file differs.
        (self.directory / (self.attempt.profile + '.png')).write_bytes(b'substituted frame')
        with self.assertRaisesRegex(ValueError, 'not the reviewed L1 stop frame'):
            observe_init_shutdown(self.directory, self.attempt, self.review, self.raw, s5)

    def test_neither_guest_text_nor_review_alone_is_evidence(self):
        raw = self.raw + b'reboot: Power down\nPM1a S5 port=0x604 data=0x3401\n'
        review = replace(self.review, count=len(raw), tx_sha256=sha256(raw).hexdigest())
        with self.assertRaisesRegex(ValueError, 'no S5 review'):
            observe_init_shutdown(self.directory, self.attempt, review, raw, None)
        with self.assertRaisesRegex(ValueError, 'no S5 review'):
            observe_init_shutdown(self.directory, self.attempt, None, raw, self.witness(review))
        # Bind the ambiguous TX so rejection comes from the real milestone parser.
        raw += self.shutdown + b'\n'
        review = replace(review, count=len(raw), tx_sha256=sha256(raw).hexdigest())
        with self.assertRaisesRegex(ValueError, 'extra HV:P5: record'):
            observe_init_shutdown(self.directory, self.attempt, review, raw, self.witness(review))


if __name__ == '__main__':
    unittest.main()
