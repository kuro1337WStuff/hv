"""Contract tests for callers of read_board; no queue or VM is involved."""
import json
from pathlib import Path
from tempfile import TemporaryDirectory
import unittest

from lab.plan_cards import Card, read_board


def block(data):
    return '<!-- hv-plan\n' + json.dumps(data, indent=2) + '\n-->\n'


class BoardTest(unittest.TestCase):
    def read(self, source):
        with TemporaryDirectory() as tmp:
            board = Path(tmp) / 'board.md'
            board.write_bytes(source.encode('utf-8'))
            return read_board(board)

    def test_mixed_legacy_board_preserves_order_and_full_titles(self):
        source = '''\ufeff# Dispatch board
- [ ] B09: review **both** candidates — keep both receipts
  - [x] X00 nested checklist
- [ ] Gather receipts
````md
```
- [x] B09 duplicate example, not a card
<!-- hv-plan
this is not metadata
-->
````
  ~~~markdown
- [ ] X02 another example
  ~~~~
<!-- retired cards
- [x] X03 not visible
-->
- [x] A01 frozen regression; see `receipt` and its notes
- [ ] C15 Manual handoff
'''
        pins = {'lane': 'b', 'profile': 'l1-regression-3', 'iso': 'a' * 64, 'screen': 'b' * 64}
        # JSON order is not board order, and a card may refer forward.
        source += block({'A01': pins, 'B09': {'after': ['A01'], 'note': 'Review only.'}})
        cards = self.read(source.replace('\n', '\r\n'))
        self.assertEqual(cards, [
            Card('B09', ': review **both** candidates — keep both receipts', False, ('A01',),
                 None, None, None, None, 'Review only.'),
            Card('A01', 'frozen regression; see `receipt` and its notes', True, (),
                 'b', 'l1-regression-3', 'a' * 64, 'b' * 64, ''),
            Card('C15', 'Manual handoff', False, (), None, None, None, None, ''),
        ])
        self.assertEqual([c.automatic for c in cards], [False, True, False])

    def test_unannotated_card_is_manual_and_partial_bindings_are_invalid(self):
        row = '- [ ] A01 One task\n'
        self.assertFalse(self.read(row)[0].automatic)
        all_pins = {'lane': 'a', 'profile': 'reviewed', 'iso': '1' * 64, 'screen': '2' * 64}
        for field in all_pins:
            pins = {k: v for k, v in all_pins.items() if k != field}
            with self.subTest(missing=field), self.assertRaises(ValueError):
                self.read(block({'A01': pins}) + row)

    def test_refuses_ambiguous_or_invalid_board_structure(self):
        rows = '- [ ] A01 One\n- [x] A02 Two\n'
        for tail in [
            '- [x] A01 Duplicate\n', '- [ ] a01 Wrong case\n', '- [ ] A001 Wrong width\n',
            '- [ ] A01more Missing boundary\n', block({'Z00': {}}), block({'a01': {}}),
            block({'A01': {'after': ['A02', 'A02']}}), block({'A01': {'after': ['A03']}}),
            block({'A01': {'after': ['A01']}}), block({'A01': {'after': ['A02']}, 'A02': {'after': ['A01']}}),
            block({}) + block({}), '<!-- hv-plan\n{}\n', '<!-- hv-plan {} -->\n', '<!-- hv-plan\n{} -->\n',
            '<!-- hv-plan\n{"A01":{},"A01":{}}\n-->\n', '<!-- hv-plan\n{"A01":{"after":[],"after":[]}}\n-->\n',
            '<!-- hv-plan\ninvalid\n-->\n', block([]), block({'A01': None}),
        ]:
            with self.subTest(tail=tail), self.assertRaises(ValueError):
                self.read(rows + tail)

    def test_refuses_invalid_dispatch_fields_even_on_checked_cards(self):
        pins = {'lane': 'a', 'profile': 'reviewed', 'iso': 'a' * 64, 'screen': 'b' * 64}
        for field, value in [
            ('lane', 'default'), ('lane', 'A'), ('profile', '../escape'), ('profile', 'a--b'),
            ('profile', 'lpt7'), ('profile', 'nul'), ('profile', 'a' * 49), ('profile', None),
            ('iso', 'A' * 64), ('iso', 'e' * 63), ('screen', 'build/result.png'), ('screen', 1),
            ('note', ['text']), ('note', 'terminal\x1b'), ('after', 'B01'), ('after', [1]),
            ('command', 'build'), ('path', 'another-checkout'),
        ]:
            with self.subTest(field=field, value=value), self.assertRaises(ValueError):
                self.read('- [x] A01 One\n' + block({'A01': {**pins, field: value}}))

    def test_refuses_oversize_and_excessively_nested_input(self):
        with self.assertRaises(ValueError):
            self.read('#' * (512 * 1024 + 1))
        with self.assertRaises(ValueError):
            self.read('<!-- hv-plan\n' + '[' * 4096 + '0' + ']' * 4096 + '\n-->\n')


if __name__ == '__main__':
    unittest.main()
