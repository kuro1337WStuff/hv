"""Synthetic comparator controls only. These records do not represent Linux boots."""
from dataclasses import replace
from hashlib import sha256
import json
from pathlib import Path
from tempfile import TemporaryDirectory
import unittest

from lab.guest_tx_evidence import Attempt, InitMilestones, ReviewedS5Stop, ReviewedStop
from lab.paired_boot_evidence import (
    BootEvidence, BootPins, ReviewedColdStart, ReviewedPowerEvent, compare_cold_boots,
)


def digest(data):
    return sha256(data).hexdigest()


PINS = BootPins('ab' * 32, 'linux-test', 'cd' * 20, 'EF' * 32)
USER = b'HV:P5:USER pid=1 kernel=6.18.60-0-virt\r\n'
TIMER = b'HV:P5:TIMER before=9.99 after=10.00\n'
RAW = USER + TIMER + b'HV:P5:SHUTDOWN\n'
FRAME = b'opaque synthetic frame'


def fixture(parent, number, state, *, minute=None, terminal=None, pins=PINS, raw=RAW, frame=FRAME):
    a = Attempt(f'{number:032x}', pins.profile, pins.media_sha256)
    directory = parent / a.run_id
    directory.mkdir()
    (directory / 'queue.json').write_text(json.dumps([{'profile': a.profile, 'sha256': a.media_sha256}]))
    (directory / f'{a.profile}.png').write_bytes(frame)
    time = f'2026-01-02T00:{number if minute is None else minute:02d}:'
    rows = [dict(state='CHECKPOINT', detail=f"Saving prior state as 'HvQueue before {a.run_id}'."),
            dict(state='MOUNT', profile=a.profile, detail=f'Mounting reviewed {a.profile} media; SHA256={a.media_sha256}'),
            dict(state='BOOT', profile=a.profile, time=time + '02.0000001Z'),
            dict(state='WATCHING', profile=a.profile, time=time + '02.0000003Z'),
            dict(state=state, profile=a.profile, time=terminal or time + '03Z')]
    events = b''.join(json.dumps(r).encode() + b'\n' for r in rows)
    (directory / 'events.jsonl').write_bytes(events)
    stop = ReviewedStop(a, digest(events), digest(frame), len(raw), digest(raw), False)
    s5 = ReviewedS5Stop(stop, 'PM1a S5', 0x7b, 0x06040020, 0x3401, 2, 0x1234, 0x1234, 37, 37)

    # Stand-ins for original host records, not a proposed event-provider format.
    off_time, start_time = time + '01Z', time + '02.0000002Z'
    off_record = f'{off_time} fixture VM observed powered off'.encode()
    start_record = f'{start_time} fixture VM completed fresh power on'.encode()
    cold = ReviewedColdStart(pins, stop, ReviewedPowerEvent('powered-off', off_time, digest(off_record)),
                             ReviewedPowerEvent('fresh-start-completed', start_time, digest(start_record)))
    return BootEvidence(directory, a, stop, raw, s5, cold, off_record, start_record)


class PairedBootEvidenceTest(unittest.TestCase):
    def setUp(self):
        tmp = TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)
        self.a = fixture(self.root, 1, 'ATTENTION')
        self.b = fixture(self.root, 2, 'COMPLETE')

    def test_identical_tx_and_frame_independent_starts_in_caller_order(self):
        self.assertEqual(self.a.stop.frame_sha256, self.b.stop.frame_sha256)
        self.assertEqual(self.a.raw_tx, self.b.raw_tx)
        a = InitMilestones('ATTENTION', 0, len(USER), len(USER) + len(TIMER), b'9.99', b'10.00')
        b = replace(a, disposition='COMPLETE')
        self.assertEqual(compare_cold_boots(PINS, self.a, self.b), (a, b))
        self.assertEqual(compare_cold_boots(PINS, self.b, self.a), (b, a))
        # Neither run is a template for the other; valid timer readings/offsets may differ.
        c = fixture(self.root, 3, 'ATTENTION', raw=b'boot\n' + RAW.replace(b'10.00', b'11.21'))
        self.assertEqual(compare_cold_boots(PINS, self.a, c), (a, InitMilestones(
            'ATTENTION', 5, 5 + len(USER), 5 + len(USER) + len(TIMER), b'9.99', b'11.21')))

    def test_two_distinct_dispatches_required(self):
        with self.assertRaisesRegex(ValueError, 'same broker attempt'):
            compare_cold_boots(PINS, self.a, self.a)

    def test_relabelled_run_with_valid_worklist_and_distinct_frame(self):
        # Even rewrite the checkpoint ID and re-hash the copy. Different bytes
        # in the two broker files/PNGs are not a second launch window.
        copy = fixture(self.root, 3, 'COMPLETE', minute=1, frame=b'different frame')
        with self.assertRaisesRegex(ValueError, 'overlapping boot evidence windows'):
            compare_cold_boots(PINS, self.a, copy)

    def test_second_launch_before_first_terminal_is_not_independent(self):
        late_stop = fixture(self.root, 3, 'ATTENTION', minute=0, terminal='2026-01-02T00:02:10Z')
        with self.assertRaisesRegex(ValueError, 'overlapping boot evidence windows'):
            compare_cold_boots(PINS, late_stop, self.b)

    def test_same_native_record_rebound_to_a_second_stop(self):
        for key in ('off', 'start'):
            with self.subTest(key=key):
                record = getattr(self.a, key + '_record')
                evt = replace(getattr(self.b.cold, key), record_sha256=digest(record))
                b = replace(self.b, cold=replace(self.b.cold, **{key: evt}), **{key + '_record': record})
                with self.assertRaisesRegex(ValueError, 'reused host power record'):
                    compare_cold_boots(PINS, self.a, b)

    def test_no_shared_stale_or_missing_provenance(self):
        b = self.b
        for altered in (replace(b, directory=self.a.directory), replace(b, cold=None), replace(b, cold=self.a.cold),
                        replace(b, cold=replace(b.cold, binding=self.a.stop)),
                        replace(b, off_record=b''), replace(b, start_record=b'not the reviewed record'),
                        replace(b, start_record=b.raw_tx), replace(b, off_record=b.start_record),
                        replace(b, cold=replace(b.cold, off=None)), replace(b, cold=replace(b.cold, start=None))):
            with self.subTest(altered=altered), self.assertRaises(ValueError):
                compare_cold_boots(PINS, self.a, altered)

    def test_cold_labels_cannot_replace_original_records(self):
        # Even an independently reviewed *empty file* is not a power event.
        b = self.b
        for record in (b'', True, None):
            evt = replace(b.cold.start, record_sha256=digest(b''))
            with self.subTest(record=record), self.assertRaisesRegex(ValueError, 'fresh-start-completed record'):
                compare_cold_boots(PINS, self.a, replace(b, start_record=record, cold=replace(b.cold, start=evt)))

    def test_saved_paused_resume_and_start_request_are_not_fresh(self):
        for key, kind in (('off', 'saved'), ('off', 'paused'), ('start', 'resume'),
                          ('start', 'restore'), ('start', 'start-requested')):
            # Bind the variant so this tests the reviewed meaning, not a stale hash.
            record = ('fixture: ' + kind).encode()
            evt = replace(getattr(self.b.cold, key), kind=kind, record_sha256=digest(record))
            b = replace(self.b, cold=replace(self.b.cold, **{key: evt}), **{key + '_record': record})
            with self.subTest(kind=kind), self.assertRaises(ValueError):
                compare_cold_boots(PINS, self.a, b)

    def test_broker_and_guest_bytes_are_not_independent_host_records(self):
        b = self.b
        events = (b.directory / 'events.jsonl').read_bytes()
        boot_request = next(row for row in events.splitlines(keepends=True) if json.loads(row)['state'] == 'BOOT')
        for record in (b.raw_tx, FRAME, events, boot_request):
            evt = replace(b.cold.start, record_sha256=digest(record))
            with self.subTest(record=record), self.assertRaisesRegex(ValueError, 'as host power evidence'):
                compare_cold_boots(PINS, self.a, replace(b, start_record=record, cold=replace(b.cold, start=evt)))

    def test_bound_start_must_fall_in_actual_broker_window(self):
        for utc in ('2026-01-02T00:02:02.0000000Z', '2026-01-02T00:02:02.0000004Z',
                    self.a.cold.start.utc, '2026-02-30T00:02:02Z', 'not a timestamp', None):
            evt = replace(self.b.cold.start, utc=utc)
            with self.subTest(utc=utc), self.assertRaises(ValueError):
                compare_cold_boots(PINS, self.a, replace(self.b, cold=replace(self.b.cold, start=evt)))
        late_off = replace(self.b.cold.off, utc=self.b.cold.start.utc)
        with self.assertRaisesRegex(ValueError, 'not preceded by Off'):
            compare_cold_boots(PINS, self.a, replace(self.b, cold=replace(self.b.cold, off=late_off)))

    def test_mixed_or_unpinned_comparison(self):
        for n, change in enumerate(({'source_commit': '11' * 20}, {'platform_sha256': '22' * 32},
                                    {'profile': 'other'}, {'media_sha256': '33' * 32}), start=3):
            other = replace(PINS, **change)
            alt = fixture(self.root, n, 'COMPLETE', pins=other)
            with self.subTest(change=change), self.assertRaises(ValueError):
                compare_cold_boots(PINS, self.a, alt)
            # Agreement between runs is not agreement with the declaration.
            with self.subTest(declaration=change), self.assertRaises(ValueError):
                compare_cold_boots(other, self.a, self.b)
        for change in ({'source_commit': 'branch'}, {'platform_sha256': ''}, {'media_sha256': 'latest'}, {'profile': '../bad'}):
            with self.subTest(change=change), self.assertRaisesRegex(ValueError, 'invalid declared'):
                compare_cold_boots(replace(PINS, **change), self.a, self.b)

    def test_missing_timer_or_s5_either_run_and_broker_tampering(self):
        c = fixture(self.root, 3, 'ATTENTION', raw=RAW.replace(TIMER, b''))
        for bad, reason in ((c, 'three HV:P5'), (replace(self.b, s5=None), 'no S5 review'),
                            (replace(self.b, s5=replace(self.b.s5, terminal_entries=38)), 'unresumed OUT')):
            for runs in ((self.a, bad), (bad, self.a)):
                with self.subTest(reason=reason, first=runs[0].requested.run_id), self.assertRaisesRegex(ValueError, reason):
                    compare_cold_boots(PINS, *runs)
        (self.b.directory / 'events.jsonl').write_bytes((self.a.directory / 'events.jsonl').read_bytes())
        with self.assertRaisesRegex(ValueError, 'not the reviewed broker record'):
            compare_cold_boots(PINS, self.a, self.b)


if __name__ == '__main__':
    unittest.main()
