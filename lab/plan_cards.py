"""Read a Markdown task board and its optional declarative hv-plan comment."""
from dataclasses import dataclass
from graphlib import TopologicalSorter
import json
from pathlib import Path
import re


_BOARD_BYTES = 512 * 1024
_ID = re.compile(r'[A-Z][0-9]{2}')
_CHECKBOX = re.compile(r'- \[([ x])\][ \t]+(.*)')
_CARD = re.compile(r'([A-Z][0-9]{2})\b(.*)')
_FENCE = re.compile(r' {0,3}(`{3,}|~{3,})(.*)')
_PROFILE = re.compile(r'[a-z][a-z0-9]*(?:-[a-z0-9]+)*')
_RESERVED = re.compile(r'con|prn|aux|nul|(?:com|lpt)[0-9]')
_DIGEST = re.compile(r'[0-9a-f]{64}')
_PINS = {'lane', 'profile', 'iso', 'screen'}
_FIELDS = _PINS | {'after', 'note'}


@dataclass(frozen=True)
class Card:
    id: str
    title: str
    checked: bool
    after: tuple[str, ...]
    lane: str | None
    profile: str | None
    iso: str | None
    screen: str | None
    note: str

    @property
    def automatic(self) -> bool:
        return self.lane is not None  # read_board enforces all-or-none bindings


def _unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError('Duplicate JSON key in hv-plan.')
        result[key] = value
    return result


def _text(value):
    # These strings can appear in foreground status output. Do not accept
    # terminal escapes, lone surrogate escapes, or multi-line status records.
    return isinstance(value, str) and len(value) <= 2048 and all(c.isprintable() or c == '\t' for c in value)


def _make_card(key, title, checked, data, ids):
    if not isinstance(data, dict) or data.keys() - _FIELDS:
        raise ValueError(f'{key}: expected only supported card metadata fields.')
    pins = data.keys() & _PINS
    if pins and pins != _PINS:
        raise ValueError(f'{key}: automatic cards require lane, profile, iso and screen together.')
    after = data.get('after', [])
    if not isinstance(after, list) or any(not isinstance(d, str) or d not in ids for d in after):
        raise ValueError(f'{key}: after must list existing card IDs.')
    if len(set(after)) != len(after):
        raise ValueError(f'{key}: duplicate dependency.')
    lane = data.get('lane')
    profile = data.get('profile')
    if 'lane' in data and lane not in ('a', 'b'):
        raise ValueError(f'{key}: lane must be a or b.')
    if 'profile' in data and (not isinstance(profile, str) or len(profile) > 48 or
                              not _PROFILE.fullmatch(profile) or _RESERVED.fullmatch(profile)):
        raise ValueError(f'{key}: invalid queue profile slug.')
    for field in ('iso', 'screen'):
        if field in data and (not isinstance(data[field], str) or not _DIGEST.fullmatch(data[field])):
            raise ValueError(f'{key}: {field} must be a lowercase SHA-256 digest.')
    note = data.get('note', '')
    if not _text(note):
        raise ValueError(f'{key}: invalid note.')
    return Card(key, title, checked, tuple(after), lane, profile, data.get('iso'), data.get('screen'), note)


def read_board(path: Path) -> list[Card]:
    """Return validated cards in source order; raise ValueError for an invalid board."""
    with path.open('rb') as f:
        raw = f.read(_BOARD_BYTES + 1)
    if len(raw) > _BOARD_BYTES:
        raise ValueError('Board exceeds input limit.')
    lines = iter(raw.decode('utf-8-sig').replace('\r\n', '\n').replace('\r', '\n').split('\n'))
    rows = {}
    metadata = None
    fence = None
    comment = False
    for line in lines:
        if comment:
            comment = '-->' not in line
            continue
        marker = _FENCE.fullmatch(line)
        if fence is not None:
            if marker and marker[1].startswith(fence) and not marker[2].strip(' \t'):
                fence = None
            continue
        if marker and (marker[1][0] == '~' or '`' not in marker[2]):
            fence = marker[1]
            continue
        if line.rstrip(' \t') == '<!-- hv-plan':
            if metadata is not None:
                raise ValueError('Duplicate hv-plan metadata block.')
            body = []
            for part in lines:
                if part.strip(' \t') == '-->':
                    break
                if '-->' in part:
                    raise ValueError('Expected hv-plan closing delimiter on a separate line.')
                body.append(part)
            else:
                raise ValueError('Unclosed hv-plan metadata block.')
            try:
                metadata = json.loads('\n'.join(body), object_pairs_hook=_unique_object)
            except RecursionError as exc:
                raise ValueError('Excessive nesting in hv-plan metadata.') from exc
            if not isinstance(metadata, dict):
                raise ValueError('Expected an object keyed by card ID in hv-plan metadata.')
            continue
        if line.startswith('<!-- hv-plan'):
            raise ValueError('Expected hv-plan opening delimiter on a separate line.')
        if line.lstrip(' ').startswith('<!--'):
            comment = '-->' not in line
            continue
        checkbox = _CHECKBOX.fullmatch(line)
        if not checkbox:
            continue
        card = _CARD.fullmatch(checkbox[2])
        if not card:
            # Untagged legacy checklist items are not dispatch cards. A token
            # shaped like an ID must not silently disappear after a typo.
            if re.match(r'[A-Za-z]+[0-9]', checkbox[2]):
                raise ValueError('Malformed board card ID.')
            continue
        key, title = card[1], card[2].strip(' \t')
        if key in rows or not title or not _text(title):
            raise ValueError(f'{key}: duplicate card or invalid title.')
        rows[key] = (title, checkbox[1] == 'x')

    metadata = metadata or {}
    if any(not _ID.fullmatch(key) or key not in rows for key in metadata):
        raise ValueError('Metadata must reference existing board card IDs.')
    cards = [_make_card(key, title, checked, metadata.get(key, {}), rows)
             for key, (title, checked) in rows.items()]
    TopologicalSorter({c.id: c.after for c in cards}).prepare()  # rejects cycles without reordering the board
    return cards
