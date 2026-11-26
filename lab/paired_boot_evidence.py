"""Offline comparison for issue #107. Inputs are private, *already reviewed* artifacts.

Freeze BootPins and both broker Attempts from the dispatch, not from candidate
files. platform_sha256 identifies the reviewed platform configuration, including
the stable VM identity; source_commit identifies the clean source used for the build.
The operator must verify that this ISO came from that source and contains the
pinned Linux inputs (see lab/linux/verify_media.py); equal ISO names do not bind
builds. Nothing here discovers pins, issues reviews or operates a VM.
"""
from dataclasses import dataclass
from datetime import date
from hashlib import sha256
import json
from pathlib import Path
import re

from lab.guest_tx_evidence import Attempt, InitMilestones, ReviewedS5Stop, ReviewedStop, observe_init_shutdown


@dataclass(frozen=True)
class BootPins:
    platform_sha256: str  # lowercase digest of the frozen platform description
    profile: str
    source_commit: str
    media_sha256: str  # uppercase broker ISO digest


@dataclass(frozen=True)
class ReviewedPowerEvent:
    """Transcription of an original host-side event, not a guest or broker claim."""
    kind: str
    utc: str  # event time on the broker's UTC clock, not review/export time
    record_sha256: str  # hash of the original individual captured record bytes


@dataclass(frozen=True)
class ReviewedColdStart:
    """Operator-issued binding for one launch; independent of the TX/S5 review.

    From retained host lifecycle evidence, establish the *same VM* was Off
    immediately before a completed fresh start, not suspended, restored, reset
    or resumed. Check the intervening history, platform, source/media, run and
    broker event window. Only then transcribe each event's meaning, original
    timestamp and digest. Do not hash a filename, enclosing batch, generated
    receipt, guest output or a reserialization in lieu of the original record.
    A start request (including the broker's BOOT row) is not a completed start.
    If the record source can't establish these facts, no review may be issued.

    Provider-specific semantics, clock correlation, custody and the accuracy
    of the operator's review are trusted. Hashes detect substitution; they
    cannot authenticate a reviewer, fabricated host records or a compromised
    capture host. The comparator cannot manufacture this input for old runs.
    """
    pins: BootPins
    binding: ReviewedStop
    off: ReviewedPowerEvent
    start: ReviewedPowerEvent


@dataclass(frozen=True)
class BootEvidence:
    directory: Path
    requested: Attempt
    stop: ReviewedStop | None
    raw_tx: bytes
    s5: ReviewedS5Stop | None
    cold: ReviewedColdStart | None
    off_record: bytes  # original host-side record, independent of events.jsonl
    start_record: bytes


def _matches(pattern: str, value: str) -> bool:
    return isinstance(value, str) and re.fullmatch(pattern, value) is not None


_UTC = re.compile(r'([0-9]{4}-[0-9]{2}-[0-9]{2})T((?:[01][0-9]|2[0-3]):[0-5][0-9]:[0-5][0-9])'
                  r'(?:\.([0-9]{1,7}))?Z')


def _utc(value: str) -> str:
    # Compare UTC instants without rounding away the seventh .NET fractional
    # digit (Write-QueueEvent uses UtcNow.ToString('o')). No local timestamps.
    if not isinstance(value, str) or not (m := _UTC.fullmatch(value)):
        raise ValueError('expected a UTC event time')
    date.fromisoformat(m[1])
    return f'{m[1]}T{m[2]}.{(m[3] or "").ljust(7, "0")}'


def _event(event: ReviewedPowerEvent, record: bytes, kind: str) -> str:
    if (not isinstance(event, ReviewedPowerEvent) or event.kind != kind or
            not isinstance(record, bytes) or not record or sha256(record).hexdigest() != event.record_sha256):
        raise ValueError(f'no matching reviewed {kind} record')
    return _utc(event.utc)


def _launch_window(run: BootEvidence, pins: BootPins) -> tuple[str, str]:
    a, cold = run.requested, run.cold
    if (not _matches('[0-9a-fA-F]{32}', a.run_id) or
            (a.profile, a.media_sha256) != (pins.profile, pins.media_sha256)):
        raise ValueError('dispatch does not match the declared pair')
    if (not isinstance(cold, ReviewedColdStart) or run.stop is None or
            cold.binding != run.stop or cold.binding.attempt != a or cold.pins != pins):
        raise ValueError('no cold-start review bound to this dispatch and stop')
    off = _event(cold.off, run.off_record, 'powered-off')
    start = _event(cold.start, run.start_record, 'fresh-start-completed')
    if {cold.off.record_sha256, cold.start.record_sha256} & {
            run.stop.events_sha256, run.stop.frame_sha256, run.stop.tx_sha256}:
        raise ValueError('guest or broker artifact supplied as host power evidence')

    # Invoke-HvQueue.ps1: BOOT precedes Start-VM; WATCHING follows the Running
    # check. Bind these specific times to the reviewed bytes, not a caller's ID.
    # The single-run validator owns broker sequence, queue, terminal, frame,
    # TX and S5 validation; this window alone is never a boot observation.
    events = (run.directory / 'events.jsonl').read_bytes()
    if sha256(events).hexdigest() != run.stop.events_sha256:
        raise ValueError('not the reviewed broker record')
    lines = events.splitlines()
    if run.off_record.rstrip(b'\r\n') in lines or run.start_record.rstrip(b'\r\n') in lines:
        raise ValueError('broker row supplied as host power evidence')
    rows = (json.loads(line) for line in lines)
    times = [_utc(row.get('time')) for row in rows
             if row['state'] in ('BOOT', 'WATCHING', 'COMPLETE', 'ATTENTION')]
    if len(times) != 3 or not (times[0] <= start <= times[1] <= times[2] and off < start):
        raise ValueError('host launch outside the reviewed broker boot window or not preceded by Off')
    # Two completed runs of the same VM cannot overlap, even if their two
    # BOOT-to-WATCHING windows are disjoint. Include the final captured stop.
    return min(off, times[0]), times[2]


def compare_cold_boots(pins: BootPins, first: BootEvidence, second: BootEvidence) -> tuple[InitMilestones, InitMilestones]:
    """Return both original observations in argument order, never a new verdict.

    Pass exactly two frozen dispatches, their independently issued reviews and
    unmodified captures. An overlap on this one platform/VM is ambiguous; a
    change of run ID or a difference in frame/TX is never evidence of a start.
    """
    if (not _matches('[0-9a-f]{64}', pins.platform_sha256) or
            not _matches('[a-z0-9]+(?:-[a-z0-9]+)*', pins.profile) or
            not _matches('[0-9a-f]{40}|[0-9a-f]{64}', pins.source_commit) or
            not _matches('[0-9A-F]{64}', pins.media_sha256)):
        raise ValueError('invalid declared platform, profile, source or media pins')
    lo1, hi1 = _launch_window(first, pins)
    lo2, hi2 = _launch_window(second, pins)
    if first.requested.run_id.casefold() == second.requested.run_id.casefold():
        raise ValueError('same broker attempt')
    if not (hi1 < lo2 or hi2 < lo1):
        raise ValueError('replayed or overlapping boot evidence windows')
    if len({first.off_record, first.start_record, second.off_record, second.start_record}) != 4:
        raise ValueError('reused host power record')

    def observe(r: BootEvidence) -> InitMilestones:
        return observe_init_shutdown(r.directory, r.requested, r.stop, r.raw_tx, r.s5)

    return observe(first), observe(second)
