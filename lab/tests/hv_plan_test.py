import json
from pathlib import Path
from tempfile import TemporaryDirectory
import unittest

from lab.hv_plan import assess, drive, load, read_board, save, state_dir


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
        states = assess(read_board(self.board), load(directory))
        self.assertEqual(states['R01'][0], 'LANE-HOLD')
        self.board.write_text(self.board.read_text().replace('[ ] R01', '[x] R01'))
        states = assess(read_board(self.board), {})
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
