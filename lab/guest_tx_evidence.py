"""Offline, single-cold-run evidence comparison. No discovery, VM or verdict creation."""
from dataclasses import dataclass
from hashlib import sha256
import json
from pathlib import Path
import re


@dataclass(frozen=True)
class Attempt:
    run_id: str
    profile: str
    media_sha256: str  # uppercase, as frozen in queue.json


@dataclass(frozen=True)
class ReviewedStop:
    """Trusted reviewer input, never parsed from guest TX or inferred from a timeout.

    Review the captured L1 exit/cause, PC, guest witness, independent negative,
    raw byte count/digest and overflow before issuing this binding. Hash the
    original PNG file (not the broker's RGB565 digest) and broker events file.
    """
    attempt: Attempt
    events_sha256: str
    frame_sha256: str
    count: int
    tx_sha256: str  # independently read/computed from L1 capture fields
    overflow: bool


def line_offsets(raw: bytes, wire_line: bytes):
    """Offsets of complete, exactly matching LF-delimited wire lines in one capture.

    Include the actual terminator (CRLF or LF) in wire_line. Offsets are
    observations, not console IDs, terminal decisions or a count of boots.
    """
    if not wire_line.endswith(b'\n') or b'\n' in wire_line[:-1]:
        raise ValueError('expected one complete wire line')
    offset = 0
    while (hit := raw.find(wire_line, offset)) >= 0:
        if hit == 0 or raw[hit - 1] == 10:
            yield hit
        offset = hit + len(wire_line)


def verify_guest_tx(directory: Path, requested: Attempt, review: ReviewedStop | None, raw: bytes) -> str:
    """Return the broker's unchanged disposition, or raise on missing/mixed evidence.

    requested comes from the frozen dispatch, review from separate terminal
    inspection. Do not populate either from candidate TX or from an unreviewed
    capture. Hashes detect substitution; they do not authenticate a compromised
    capture host. Neither a repeated marker nor a repeated call is another boot.
    """
    if review is None:
        raise ValueError('no independently reviewed L1 stop')
    if review.attempt != requested or directory.name != requested.run_id:
        raise ValueError('wrong attempt')
    queue = json.loads((directory / 'queue.json').read_bytes())
    if queue != [{'profile': requested.profile, 'sha256': requested.media_sha256}]:
        raise ValueError('wrong frozen worklist')
    events = (directory / 'events.jsonl').read_bytes()
    if sha256(events).hexdigest() != review.events_sha256:
        raise ValueError('not the reviewed broker record')
    rows = [json.loads(row) for row in events.splitlines()]
    states = {'CHECKPOINT', 'MOUNT', 'BOOT', 'WATCHING', 'COMPLETE', 'ATTENTION', 'ERROR', 'STOPPED'}
    relevant = [e for e in rows if e['state'] in states]
    if ([e['state'] for e in relevant[:4]] != ['CHECKPOINT', 'MOUNT', 'BOOT', 'WATCHING'] or
            len(relevant) != 5 or relevant[-1] != rows[-1] or
            relevant[0]['detail'] != f"Saving prior state as 'HvQueue before {requested.run_id}'." or
            relevant[-1]['state'] not in ('COMPLETE', 'ATTENTION') or
            any(e['profile'] != requested.profile for e in relevant[1:]) or
            relevant[1]['detail'] != f'Mounting reviewed {requested.profile} media; SHA256={requested.media_sha256}'):
        raise ValueError('no matching single cold boot and terminal disposition')
    frame = (directory / (requested.profile + '.png')).read_bytes()
    if sha256(frame).hexdigest() != review.frame_sha256:
        raise ValueError('not the reviewed L1 stop frame')
    if review.overflow or len(raw) != review.count or sha256(raw).hexdigest() != review.tx_sha256:
        raise ValueError('incomplete or different guest TX')
    return relevant[-1]['state']


@dataclass(frozen=True)
class InitMilestones:
    """Bound wire observations: line-start byte offsets and both raw uptime values."""
    disposition: str
    user_offset: int
    timer_offset: int
    shutdown_offset: int
    before: bytes
    after: bytes


# /proc/uptime prints two decimals; init's x86-64 BusyBox test compares signed seconds.
_UPTIME = rb'((?:0|[1-9][0-9]{0,18})\.[0-9]{2})'
_TIMER = re.compile(rb'HV:P5:TIMER before=' + _UPTIME + rb' after=' + _UPTIME)


def observe_init_milestones(directory: Path, requested: Attempt, review: ReviewedStop | None,
                            raw: bytes) -> InitMilestones:
    """Check one reviewed TX for the three ordered lines emitted by lab/linux/init.

    Example: obs = observe_init_milestones(run_dir, frozen_attempt, reviewed_stop, raw_tx)
    obs.disposition remains the broker's disposition, including ATTENTION. The
    offsets and timer readings describe bytes, not proof of boot or poweroff.
    Missing bindings or malformed/missing milestones do not yield observations.
    """
    disposition = verify_guest_tx(directory, requested, review, raw)
    records = []
    start = 0
    while start < len(raw):
        lf = raw.find(b'\n', start)
        if lf < 0:
            raise ValueError(f'incomplete wire line at byte {start}')
        if raw.startswith(b'HV:P5:', start):
            if len(records) == 3:
                raise ValueError(f'extra HV:P5: record at byte {start}')
            end = lf - 1 if raw[lf - 1] == 13 else lf
            records.append((start, raw[start:end]))
        start = lf + 1

    if len(records) != 3:
        raise ValueError('expected three HV:P5: records')
    (user_offset, user), (timer_offset, timer), (shutdown_offset, shutdown) = records
    if user != b'HV:P5:USER pid=1 kernel=6.18.60-0-virt':
        raise ValueError(f'expected pinned USER record at byte {user_offset}')
    match = _TIMER.fullmatch(timer)
    if not match:
        raise ValueError(f'expected TIMER record at byte {timer_offset}')
    before, after = match.groups()
    if not (int(before[:-3]) < int(after[:-3]) <= (1 << 63) - 1):
        raise ValueError('TIMER seconds did not advance within the signed 64-bit range')
    if shutdown != b'HV:P5:SHUTDOWN':
        raise ValueError(f'expected SHUTDOWN record at byte {shutdown_offset}')
    return InitMilestones(disposition, user_offset, timer_offset, shutdown_offset, before, after)
