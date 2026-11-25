"""Summarize complete, independent cold boots from one predeclared timing cohort."""
import argparse
from collections import Counter
import json
from pathlib import Path
import re
from statistics import median


# All hashes identify the exact bytes of retained files, not a product name.
HASH_FIELDS = ('platform_sha256', 'workload_sha256', 'clock_sha256',
               'controls_sha256', 'runtime_sha256')
PLAN_FIELDS = ('boots', 'warmups', 'repeats', 'work_units')
MODES = ('nested', 'physical-guest', 'native')
REASONS = {'cpuid', 'msr_read', 'msr_write', 'io_in', 'io_out', 'hlt', 'vmmcall', 'npf'}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def keys(obj, names):
    require(isinstance(obj, dict) and set(obj) == set(names),
            f'expected exactly these fields: {", ".join(names)}')


def count(value, minimum=0):
    require(type(value) is int and minimum <= value < 2**64,
            f'expected integer in [{minimum}, 2^64)')


def digest(value):
    require(isinstance(value, str) and re.fullmatch('[0-9a-f]{64}', value),
            'expected lowercase SHA-256')


def check_condition(c):
    keys(c, ('origin', 'mode', *HASH_FIELDS, *PLAN_FIELDS))
    require(c['origin'] in ('synthetic', 'observed'), 'unknown data origin')
    require(c['mode'] in MODES, 'unknown execution mode')
    for field in HASH_FIELDS:
        digest(c[field])
    for field, minimum in zip(PLAN_FIELDS, (5, 1, 2, 1)):
        count(c[field], minimum)


def check_trial(t, c):
    keys(t, ('elapsed_ns', 'completed_units', 'entries', 'reasons', 'tx_bytes', 'busy_reads'))
    count(t['elapsed_ns'], 1)
    require(t['elapsed_ns'] <= 3_600_000_000_000, 'trial exceeds one-hour capture limit')
    count(t['completed_units'], 1)
    require(t['completed_units'] == c['work_units'], 'incomplete or different workload')
    count(t['entries'])
    reasons = t['reasons']
    require(isinstance(reasons, dict) and reasons.keys() <= REASONS, 'unknown exit reason')
    for n in reasons.values():
        count(n, 1)  # omit zero buckets
    require(sum(reasons.values()) == t['entries'], 'unbalanced entry/exit count')
    require((c['mode'] == 'native') == (t['entries'] == 0), 'unexpected entry count for mode')
    for field, reason in (('tx_bytes', 'io_out'), ('busy_reads', 'io_in')):
        count(t[field])
        require(t[field] <= reasons.get(reason, 0), f'{field} exceeds byte-IO exits')
    # The polled byte device starts and ends the trial drained. A THR write
    # creates one busy interval, consumed once by an LSR read (not every IN).
    require(t['busy_reads'] == t['tx_bytes'], 'undrained or inconsistent UART busy intervals')


def summarize(rows):
    require(isinstance(rows, list) and rows, 'expected nonempty array of boots')
    condition = None
    ids, receipts, boots = set(), set(), []
    for row in rows:
        keys(row, ('condition', 'boot_id', 'receipt_sha256', 'start', 'outcome', 'warmup', 'measured'))
        c = row['condition']
        check_condition(c)  # validate independently: e.g., Python equates True and 1
        require(condition is None or c == condition, 'mixed cohort conditions')
        condition = c
        require(row['start'] == 'cold' and row['outcome'] == 'complete', 'not a completed cold boot')
        bid = row['boot_id']
        require(isinstance(bid, str) and re.fullmatch(r'[A-Za-z0-9._-]{1,128}', bid), 'invalid boot ID')
        digest(row['receipt_sha256'])
        require(bid not in ids and row['receipt_sha256'] not in receipts, 'reused boot ID or receipt')
        ids.add(bid)
        receipts.add(row['receipt_sha256'])
        for phase, size in (('warmup', 'warmups'), ('measured', 'repeats')):
            trials = row[phase]
            require(isinstance(trials, list) and len(trials) == c[size], f'wrong {phase} trial count')
            for trial in trials:
                check_trial(trial, c)
        reasons = Counter()
        durations = []
        for trial in row['measured']:
            reasons.update(trial['reasons'])
            durations.append(trial['elapsed_ns'])
        boots.append({'boot_id': bid, 'receipt_sha256': row['receipt_sha256'],
                      'elapsed_ns': durations, 'median_elapsed_ns': median(durations),
                      'reason_counts': dict(reasons),
                      'entries': sum(t['entries'] for t in row['measured']),
                      'tx_bytes': sum(t['tx_bytes'] for t in row['measured']),
                      'busy_reads': sum(t['busy_reads'] for t in row['measured'])})
    require(len(boots) == condition['boots'], 'missing or extra independent boots')
    medians = [b['median_elapsed_ns'] for b in boots]
    return {'condition': condition, 'boots': boots, 'median_of_boot_medians_ns': median(medians),
            'boot_median_range_ns': [min(medians), max(medians)]}


def compare(guest, native):
    g, n = guest['condition'], native['condition']
    require(g['mode'] == 'physical-guest' and n['mode'] == 'native', 'native comparison requires physical guest')
    for field in (*HASH_FIELDS, *PLAN_FIELDS, 'origin'):
        if field != 'runtime_sha256':
            require(g[field] == n[field], f'unmatched native control: {field}')
    for field in ('boot_id', 'receipt_sha256'):
        require({b[field] for b in guest['boots']}.isdisjoint(b[field] for b in native['boots']),
                'reused boot between arms')
    return {'guest': guest, 'native': native,
            'guest_over_native_elapsed_ratio': guest['median_of_boot_medians_ns'] / native['median_of_boot_medians_ns']}


def unique_fields(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, f'duplicate JSON field: {key}')
        result[key] = value
    return result


def load(path):
    return summarize(json.loads(path.read_text(encoding='utf-8'), object_pairs_hook=unique_fields))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('cohort', type=Path)
    parser.add_argument('--native', type=Path, help='separate matched native cold boots')
    args = parser.parse_args()
    try:
        result = load(args.cohort)
        if args.native:
            result = compare(result, load(args.native))
    except (OSError, ValueError) as error:
        parser.exit(2, f'cohort rejected: {error}\n')
    print(json.dumps(result, indent=2, allow_nan=False))


if __name__ == '__main__':
    main()
