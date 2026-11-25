from contextlib import redirect_stderr, redirect_stdout
from io import StringIO
import json
from pathlib import Path
from tempfile import TemporaryDirectory
import unittest
from unittest.mock import Mock, patch

from lab.hv_plan import assess, drive, load, main, read_board, save, state_dir


class DispatchTest(unittest.TestCase):
    def setUp(self):
        tmp = TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)
        self.board = self.root / 'board.md'
        self.pins = dict(lane='b', profile='test', iso='0'*64, screen='1'*64)
        self.rows = {'R01': self.pins, 'R02': dict(self.pins, after=['R01'])}
        self.publish()

    def publish(self):
        self.board.write_text(''.join(f'- [ ] {k} check {k}\n' for k in self.rows) +
                              '<!-- hv-plan\n' + json.dumps(self.rows) + '\n-->\n')

    def run_with(self, fn):
        return drive(self.board, self.root, 4, 1, submit=fn)

    def cli(self, action, *args):
        with redirect_stdout(StringIO()) as out:
            self.assertEqual(main([action, str(self.board), '--lanes-root', str(self.root), *args]), 0)
        return out.getvalue()

    def test_finish_unlocks_next_exactly_once_across_restarts(self):
        calls = []
        def run(root, rid, card):
            self.assertEqual(load(state_dir(self.board))[card.id]['state'], 'ACTIVE')
            calls.append(card.id)
            return 'COMPLETE'
        self.assertEqual(self.run_with(run), 0)
        self.assertEqual(self.run_with(run), 0)
        self.assertEqual(calls, ['R01', 'R02'])

    def test_changed_media_not_admitted_after_reload(self):
        calls = []
        def run(root, rid, card):
            calls.append(card.id)
            self.rows['R02']['iso'] = '2'*64
            self.publish()
            return 'COMPLETE'
        self.assertEqual(self.run_with(run), 0)
        self.assertEqual(calls, ['R01'])

    def test_uncertain_removed_card_holds_lane_and_checked_card_is_not_proof(self):
        directory = state_dir(self.board)
        directory.mkdir(parents=True)
        save(directory, {'R00': dict(run='0'*32, state='ACTIVE', lane='b')})
        states, _ = assess(read_board(self.board), load(directory), {})
        self.assertEqual(states['R01'][0], 'LANE-HOLD')
        self.board.write_text(self.board.read_text().replace('[ ] R01', '[x] R01'))
        states, _ = assess(read_board(self.board), {}, {})
        self.assertEqual(states['R01'][0], 'HOLD')
        self.assertEqual(states['R02'][0], 'BLOCKED')

    def test_stop_does_not_start_successor(self):
        calls = []
        def run(root, rid, card):
            calls.append(card.id)
            (state_dir(self.board) / 'STOP').touch()
            return 'COMPLETE'
        self.assertEqual(self.run_with(run), 2)
        self.assertEqual(calls, ['R01'])

    def test_external_hold_fences_only_a_across_board_edits_and_restarts(self):
        self.rows = {'R01': dict(self.pins, lane='a'), 'R02': self.pins}
        self.publish()
        self.cli('hold', '--lane', 'a', '--reason', 'untracked session')
        submit = Mock(return_value='COMPLETE')
        directory = state_dir(self.board)

        def save_while_held(path, records):
            self.assertEqual(set(records), {'R02'}, 'held lane must never be reserved')
            save(path, records)

        with patch('lab.hv_plan.save', side_effect=save_while_held):
            self.assertEqual(self.run_with(submit), 0)
            submit.assert_called_once()
            self.assertEqual(submit.call_args.args[2].id, 'R02')
            self.assertEqual(load(directory)['R02']['state'], 'COMPLETE')

            del self.rows['R01']
            self.publish()
            self.cli('stop')
            self.cli('resume')
            self.assertIn('LANE-HOLD a: external: untracked session', self.cli('status'))
            with redirect_stdout(StringIO()) as out:
                self.assertEqual(self.run_with(submit), 0)
            self.assertIn('LANE-HOLD a: external: untracked session', out.getvalue())

            self.rows['R03'] = dict(self.pins, lane='a')
            self.publish()
            self.assertEqual(self.run_with(submit), 0)
            submit.assert_called_once()
            self.assertEqual(set(load(directory)), {'R02'})

        self.cli('unhold', '--lane', 'a', '--reason', 'operator verified session closed')
        self.assertEqual(self.run_with(submit), 0)
        self.assertEqual(self.run_with(submit), 0)
        self.assertEqual([c.args[2].id for c in submit.call_args_list], ['R02', 'R03'])
        events = json.loads((directory / 'external-holds.json').read_bytes())
        self.assertEqual([(e['lane'], e['action'], e['reason']) for e in events],
                         [('a', 'hold', 'untracked session'), ('a', 'unhold', 'operator verified session closed')])

    def test_external_and_receipt_holds_require_separate_recovery(self):
        directory = state_dir(self.board)
        directory.mkdir(parents=True)
        rid = '3' * 32
        save(directory, {'R00': dict(lane='b', run=rid, state='ACTIVE')})
        self.cli('hold', '--lane', 'b', '--reason', 'other session')
        with redirect_stderr(StringIO()), self.assertRaises(SystemExit):
            self.cli('unhold', '--lane', 'b', '--reason', '   ')
        self.assertIn('LANE-HOLD b: external: other session', self.cli('status'))
        self.cli('unhold', '--lane', 'b', '--reason', 'other session closed')
        self.assertIn(f'LANE-HOLD b: R00 ACTIVE {rid}', self.cli('status'))
        self.cli('hold', '--lane', 'b', '--reason', 'manual inspection')
        self.cli('release', '--run-id', rid, '--reason', 'original worker stopped')
        status = self.cli('status')
        self.assertIn('LANE-HOLD b: external: manual inspection', status)
        self.assertNotIn(rid, status)
        self.assertEqual(load(directory)['R00']['state'], 'ACTIVE')

    def test_operator_cannot_modify_external_holds_while_supervisor_owns_lease(self):
        self.cli('hold', '--lane', 'a', '--reason', 'inspection')
        self.rows = {'R01': self.pins}
        self.publish()
        def run(root, rid, card):
            for action, lane in [('hold', 'b'), ('unhold', 'a')]:
                with self.assertRaisesRegex(ValueError, 'Another supervisor'):
                    main([action, str(self.board), '--lane', lane, '--reason', 'operator decision'])
            return 'COMPLETE'
        self.assertEqual(self.run_with(run), 0)
        status = self.cli('status')
        self.assertIn('LANE-HOLD a: external: inspection', status)
        self.assertNotIn('LANE-HOLD b:', status)

    def test_invalid_external_hold_journal_refuses_admissions(self):
        directory = state_dir(self.board)
        directory.mkdir(parents=True)
        event = dict(lane='b', action='unhold', time='2026-01-01', reason='no matching hold')
        (directory / 'external-holds.json').write_text(json.dumps([event]))
        submit = Mock(return_value='COMPLETE')
        with self.assertRaisesRegex(ValueError, 'Invalid external hold'):
            self.run_with(submit)
        submit.assert_not_called()
        self.assertEqual(load(directory), {})

    def test_stop_after_reservation_reports_same_holds_as_status_without_submission(self):
        self.rows['R02'] = self.pins  # Also READY until the STOPPED receipt fences this lane.
        self.publish()
        directory = state_dir(self.board)
        def stop_after_active(path, records):
            save(path, records)
            if records['R01']['state'] == 'ACTIVE':
                (path / 'STOP').touch()

        submit = Mock(return_value='COMPLETE')
        with patch('lab.hv_plan.save', side_effect=stop_after_active), redirect_stdout(StringIO()) as out:
            self.assertEqual(self.run_with(submit), 2)
        submit.assert_not_called()
        records = load(directory)
        self.assertEqual(set(records), {'R01'})
        self.assertEqual(records['R01']['state'], 'STOPPED')
        lines = out.getvalue().splitlines()
        self.assertFalse(any(line.startswith('START ') for line in lines))
        self.assertEqual(lines[-1], 'STOP requested')
        status = self.cli('status').splitlines()
        self.assertEqual(status[0], 'Admissions stopped.')
        self.assertEqual(lines[lines.index('2 remaining / 2 board cards'):-1], status[1:])
        self.assertIn(['R01', 'HOLD', 'STOPPED', records['R01']['run']], [line.split() for line in status])
        self.assertTrue(any(line.startswith('R02 LANE-HOLD ') for line in status))
