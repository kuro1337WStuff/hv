"""Foreground dispatcher for reviewed Hyper-V regression cards on an editable task board."""
import argparse
from concurrent.futures import FIRST_COMPLETED, ThreadPoolExecutor, wait
from contextlib import contextmanager
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import re
import signal
import sys
import tempfile
import time
import uuid
if __package__:
    from .plan_cards import read_board
    from .plan_session import execute
else:
    from plan_cards import read_board
    from plan_session import execute


def utc():
    return datetime.now(timezone.utc).isoformat(timespec='seconds')


def state_dir(board):
    return board.parent / '.hv-plan' / board.stem


@contextmanager
def lease(directory):
    """An OS-owned lock survives contention but not the death of its owner."""
    directory.mkdir(parents=True, exist_ok=True)
    with (directory / 'lease').open('a+b') as lock:
        lock.seek(0)
        try:
            if os.name == 'nt':
                import msvcrt
                msvcrt.locking(lock.fileno(), msvcrt.LK_NBLCK, 1)
            else:
                import fcntl
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError as exc:
            raise ValueError('Another supervisor owns this board.') from exc
        yield


def load(directory):
    path = directory / 'receipts.json'
    try:
        records = json.loads(path.read_bytes())
    except FileNotFoundError:
        return {}
    if not isinstance(records, dict):
        raise ValueError('Invalid receipt ledger; inspect it before continuing.')
    for key, row in records.items():
        if (not isinstance(row, dict) or not re.fullmatch('[A-Z][0-9]{2}', key) or
                row.get('lane') not in ('a', 'b') or not re.fullmatch('[a-f0-9]{32}', row.get('run', '')) or
                row.get('state') not in ('ACTIVE', 'COMPLETE', 'ERROR', 'ATTENTION', 'STOPPED')):
            raise ValueError('Malformed receipt; refusing new admissions.')
    return records


def save(directory, records):
    # A crash after reserving an attempt is not permission for a second boot.
    _save_json(directory / 'receipts.json', records)


def _save_json(path, data):
    with tempfile.NamedTemporaryFile('w', encoding='utf-8', dir=path.parent, delete=False) as f:
        tmp = Path(f.name)
        try:
            json.dump(data, f, sort_keys=True, indent=2)
            f.flush()
            os.fsync(f.fileno())
        except BaseException:
            f.close()
            tmp.unlink(missing_ok=True)
            raise
    try:
        os.replace(tmp, path)
    finally:
        tmp.unlink(missing_ok=True)


def _operator_text(value):
    return isinstance(value, str) and bool(value.strip()) and value.isprintable()


def external_holds(directory):
    """Replay this board's operator decisions; an invalid journal cannot admit a card."""
    try:
        events = json.loads((directory / 'external-holds.json').read_bytes())
    except FileNotFoundError:
        return [], {}
    if not isinstance(events, list):
        raise ValueError('Invalid external hold journal; refusing admissions.')
    held = {}
    for event in events:
        if (not isinstance(event, dict) or event.get('lane') not in ('a', 'b') or
                not _operator_text(event.get('reason')) or not _operator_text(event.get('time'))):
            raise ValueError('Invalid external hold entry; refusing admissions.')
        lane = event['lane']
        if event.get('action') == 'hold' and lane not in held:
            held[lane] = event['reason']
        elif event.get('action') == 'unhold' and lane in held:
            del held[lane]
        else:
            raise ValueError('Invalid external hold transition; refusing admissions.')
    return events, held


def binding(card):
    return [card.lane, card.profile, card.iso, card.screen, sorted(card.after)]


def assess(cards, records, external, admitted=None):
    """History takes precedence over a checkbox. A regression never closes its parent."""
    passed = {c.id for c in cards if c.checked and not c.automatic and c.id not in records}
    for c in cards:
        row = records.get(c.id)
        if row and row['state'] == 'COMPLETE' and row.get('binding') == binding(c):
            passed.add(c.id)
    held = {lane: [f'external: {reason}'] for lane, reason in external.items()}
    for key, row in records.items():
        if row['state'] != 'COMPLETE' and not row.get('released'):
            held.setdefault(row['lane'], []).append(f"{key} {row['state']} {row['run']}")
    states = {}
    for c in cards:
        row = records.get(c.id)
        if c.id in passed:
            states[c.id] = ('DONE', '')
        elif row:
            states[c.id] = ('HOLD', f"{row['state']} {row['run']}" if row.get('binding') == binding(c) else 'contract changed; reserve a new card ID')
        elif c.automatic and c.checked:
            states[c.id] = ('HOLD', 'checked without a completion receipt')
        elif missing := [i for i in c.after if i not in passed]:
            states[c.id] = ('BLOCKED', 'requires ' + ', '.join(missing))
        elif not c.lane:
            states[c.id] = ('MANUAL', c.note or c.title)
        elif not c.id.startswith('R'):
            raise ValueError(f'{c.id}: only separate Rnn regression cards can dispatch a VM')
        elif c.lane in held:
            states[c.id] = ('LANE-HOLD', '; '.join(held[c.lane]))
        elif admitted is not None and admitted.get(c.id) != binding(c):
            states[c.id] = ('DEFERRED', 'new or edited contract; start a new bounded invocation')
        else:
            states[c.id] = ('READY', f'{c.lane}: {c.profile}')
    return states, held


def report(cards, records, external, admitted=None):
    states, held = assess(cards, records, external, admitted)
    outstanding = [c for c in cards if states[c.id][0] != 'DONE']
    print(f'{len(outstanding)} remaining / {len(cards)} board cards', flush=True)
    for lane in sorted(held):
        for reason in held[lane]:
            print(f'LANE-HOLD {lane}: {reason}', flush=True)
    for c in outstanding:
        state, reason = states[c.id]
        print(f'{c.id} {state:10} {reason}', flush=True)


def send_stops(directory, records, parent):
    for row in records.values():
        if row['state'] == 'ACTIVE' and not row.get('released'):
            run = parent / ('hv-lane-' + row['lane']) / 'build' / 'runs' / row['run']
            if run.is_dir():
                (run / 'STOP').touch(exist_ok=True)


def drive(board, parent, limit, minutes, *, submit=execute):
    directory = state_dir(board)
    stop = directory / 'STOP'
    with lease(directory), ThreadPoolExecutor(max_workers=2) as pool:
        records = load(directory)
        _, external = external_holds(directory)  # hold and unhold require this same lease
        active = {}
        admitted = {c.id: binding(c) for c in read_board(board) if c.automatic and not c.checked}
        count = 0
        end = time.monotonic() + minutes * 60
        rescan = True
        problem = False
        old = signal.signal(signal.SIGINT, lambda *_: stop.touch(exist_ok=True))
        try:
            while True:
                if stop.exists():
                    send_stops(directory, records, parent)
                if rescan:
                    try:
                        cards = read_board(board)
                        states, _ = assess(cards, records, external, admitted)
                    except (OSError, ValueError) as exc:
                        print(f'Board unreadable: {exc}; stopping admissions.', file=sys.stderr, flush=True)
                        stop.touch(exist_ok=True)
                        send_stops(directory, records, parent)
                        if not active:
                            return 1
                        problem = True
                    else:
                        print(f'RESCAN {utc()} remaining={sum(s != "DONE" for s, _ in states.values())}', flush=True)
                        busy = {records[k]['lane'] for k in active.values()}
                        for c in cards:
                            if stop.exists() or count >= limit or time.monotonic() >= end:
                                break
                            if states[c.id][0] != 'READY' or c.lane in busy:
                                continue
                            run = uuid.uuid4().hex
                            records[c.id] = dict(run=run, lane=c.lane, profile=c.profile, binding=binding(c), started=utc(), state='ACTIVE')
                            save(directory, records)
                            if stop.exists():
                                records[c.id].update(state='STOPPED', ended=utc())
                                save(directory, records)
                                break
                            active[pool.submit(submit, parent / ('hv-lane-' + c.lane), run, c)] = c.id
                            print(f'START {c.id} lane={c.lane} profile={c.profile} run={run}', flush=True)
                            busy.add(c.lane)
                            count += 1
                        if not active:
                            # STOP may have landed after an ACTIVE save but before submission.
                            report(cards, records, external, None if stop.exists() else admitted)
                            print('STOP requested' if stop.exists() else 'No more admissions in this invocation.', flush=True)
                            return 2 if stop.exists() else (1 if problem else 0)
                    rescan = False
                done, _ = wait(active, timeout=0.25, return_when=FIRST_COMPLETED)
                for f in done:
                    key = active.pop(f)
                    row = records[key]
                    try:
                        result = f.result()
                        if result not in ('COMPLETE', 'ATTENTION', 'ERROR', 'STOPPED'):
                            raise ValueError('Unknown worker result')
                    except Exception as exc:
                        result = 'ERROR'
                        row['detail'] = str(exc)[:1024]
                    row.update(state=result, ended=utc())
                    save(directory, records)
                    print(f'{key} {result} {row["run"]} {row.get("detail", "")}', flush=True)
                    problem |= result != 'COMPLETE'
                    if result == 'STOPPED':
                        stop.touch(exist_ok=True)
                    rescan = True
        finally:
            signal.signal(signal.SIGINT, old)
            if active:
                stop.touch(exist_ok=True)
                send_stops(directory, records, parent)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('status', 'run', 'stop', 'resume', 'release', 'hold', 'unhold'))
    parser.add_argument('board', type=Path)
    parser.add_argument('--lanes-root', type=Path, default=Path(__file__).resolve().parents[2], help='Parent of the dedicated hv-lane-a and hv-lane-b checkouts')
    parser.add_argument('--max-cards', type=int, default=8, help='Maximum new admissions in this invocation (1-64)')
    parser.add_argument('--minutes', type=float, default=60, help='Admission window (up to 900 minutes); already admitted finite batches may finish')
    parser.add_argument('--lane', choices=('a', 'b'), help='External lane to hold or unhold')
    parser.add_argument('--run-id', help='Exact held run to release after manual inspection')
    parser.add_argument('--reason', help='External hold reason, or recovery evidence for unhold/release')
    a = parser.parse_args(argv)
    if a.action in ('hold', 'unhold'):
        if not a.lane or not _operator_text(a.reason) or a.run_id is not None:
            parser.error(f'{a.action} requires --lane and a nonblank single-line --reason, not --run-id')
    elif a.lane is not None:
        parser.error('--lane is only for hold and unhold; release requires the exact --run-id')
    board, parent = a.board.resolve(), a.lanes_root.resolve()
    directory = state_dir(board)
    if not 1 <= a.max_cards <= 64 or not 0 < a.minutes <= 900:
        parser.error('Invalid admission budget')
    if a.action == 'run':
        return drive(board, parent, a.max_cards, a.minutes)
    if a.action == 'status':
        cards = read_board(board)
        print('Admissions stopped.' if (directory / 'STOP').exists() else 'Admission stop not set.')
        _, external = external_holds(directory)
        report(cards, load(directory), external)
    elif a.action == 'stop':
        directory.mkdir(parents=True, exist_ok=True)
        (directory / 'STOP').touch(exist_ok=True)
        send_stops(directory, load(directory), parent)
        print('Stop requested; no new admissions. In-flight Hyper-V actions stop cooperatively.')
    else:
        with lease(directory):
            if a.action == 'resume':
                (directory / 'STOP').unlink(missing_ok=True)
                print('Global stop cleared. No run submitted; lane holds remain.')
            elif a.action in ('hold', 'unhold'):
                events, held = external_holds(directory)
                if a.action == 'hold' and a.lane in held:
                    raise ValueError(f'Lane {a.lane} already has an external hold: {held[a.lane]}')
                if a.action == 'unhold' and a.lane not in held:
                    raise ValueError(f'Lane {a.lane} has no external hold.')
                events.append(dict(lane=a.lane, action=a.action, time=utc(), reason=a.reason.strip()))
                _save_json(directory / 'external-holds.json', events)
                print(f'External hold {"set" if a.action == "hold" else "cleared"} on lane {a.lane}; receipts unchanged.')
            else:
                if not a.run_id or not a.reason or not a.reason.strip():
                    parser.error('release requires --run-id and --reason')
                records = load(directory)
                matches = [r for r in records.values() if r['run'] == a.run_id and r['state'] != 'COMPLETE' and not r.get('released')]
                if len(matches) != 1:
                    raise ValueError('No single unreleased held attempt with that ID')
                matches[0]['released'] = {'time': utc(), 'reason': a.reason.strip()}
                save(directory, records)
                print('Run hold released; other lane holds remain. The attempt is non-passing and cannot run again.')
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, ValueError) as exc:
        sys.exit(str(exc))
