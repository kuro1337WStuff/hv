"""Prepare a finite Hyper-V regression queue without elevating the build toolchain."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time
import uuid

LAB = Path(__file__).resolve().parent
ROOT = LAB.parent
RUNS = ROOT / 'build' / 'runs'
PROFILES = {'l2': 'hv-l2-smoke', 'cpuid': 'hv-cpuid-resume', 'npt-write': 'hv-npt-write',
            'npt-remap': 'hv-npt-remap', 'npt-cross': 'hv-npt-cross',
            'npt-broadcast': 'hv-npt-broadcast', 'forensic-write': 'hv-forensic-write',
            'vcpu-gpr': 'hv-vcpu-gpr', 'irq-wake': 'hv-irq-wake'}
DEFAULT_PROFILES = ('l2', 'cpuid', 'npt-write', 'npt-remap', 'npt-cross')
TERMINAL = {'COMPLETE', 'STOPPED', 'ATTENTION', 'ERROR'}


def digest(path):
    with path.open('rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest().upper()


def select_run(run_id):
    if not re.fullmatch('[0-9a-f]{32}', run_id):
        raise ValueError('Expected the 32-character run ID printed at startup.')
    path = RUNS / run_id
    if not path.is_dir():
        raise ValueError('Run not found: ' + run_id)
    return path


def request_stop(path):
    (path / 'STOP').touch(exist_ok=True)
    print('Stop requested for', path.name, flush=True)


def host_event(path, state, detail):
    # Launcher writes before the worker starts, or after it exits without a verdict.
    with (path / 'events.jsonl').open('a', encoding='utf-8') as f:
        json.dump({'time': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
                   'state': state, 'profile': '', 'detail': detail}, f)
        f.write('\n')
    print(state, detail, flush=True)


def new_events(path, offset):
    try:
        with (path / 'events.jsonl').open('rb') as f:
            f.seek(offset)
            for line in f:
                if not line.endswith(b'\n'):
                    break  # The worker is still writing this record.
                yield json.loads(line.decode('utf-8-sig')), f.tell()
    except FileNotFoundError:
        return


def watch(path, proc=None, offset=0):
    while True:
        # Check exit before reading, so the final drain includes all worker output.
        exited = proc is not None and proc.poll() is not None
        for item, offset in new_events(path, offset):
            state = item['state']
            print(f"{item['time']}  {state:10} {item.get('profile') or ''}  {item['detail']}", flush=True)
            if state in TERMINAL:
                return {'COMPLETE': 0, 'STOPPED': 2, 'ATTENTION': 3, 'ERROR': 1}[state]
        if exited:
            host_event(path, 'ERROR', f'Worker exited without a terminal event (status {proc.returncode}). Inspect the VM before restarting.')
            return 1
        time.sleep(0.2)


def prepare(path, profiles, rebuild):
    queue = []
    media = LAB / 'media'
    media.mkdir(exist_ok=True)
    for profile in profiles:
        if (path / 'STOP').exists():
            break
        if rebuild:
            host_event(path, 'BUILD', profile)
            subprocess.run(['cmd.exe', '/d', '/c', r'firmware\build.cmd', profile], cwd=ROOT, check=True)
        image = ROOT / 'build' / (PROFILES[profile] + '.iso')
        if not image.is_file():
            raise ValueError(f'Missing {image.name}; use run --rebuild.')
        sha = digest(image)
        staged = media / f'{PROFILES[profile]}-{sha}.iso'
        if not staged.exists():
            temp = media / (staged.name + '.' + path.name + '.tmp')
            try:
                shutil.copyfile(image, temp)
                if digest(temp) != sha:
                    raise ValueError('Build image changed while staging: ' + profile)
                try:
                    temp.rename(staged)  # Windows refuses to replace an existing destination.
                except FileExistsError:
                    pass
            finally:
                temp.unlink(missing_ok=True)
        if digest(staged) != sha:
            raise ValueError('Incorrect digest in lab media: ' + staged.name)
        queue.append({'profile': profile, 'sha256': sha})
    (path / 'queue.json').write_text(json.dumps(queue, indent=2), encoding='ascii')


def run(args):
    if os.name != 'nt':
        raise ValueError('This lab requires Windows and Hyper-V.')
    profiles = args.profiles or DEFAULT_PROFILES
    if len(set(profiles)) != len(profiles) or any(p not in PROFILES for p in profiles):
        raise ValueError('Use each supported profile at most once: ' + ', '.join(PROFILES))
    path = RUNS / uuid.uuid4().hex
    path.mkdir(parents=True)
    print(f'Run: {path.name}\nWatch: python lab/hv_queue.py watch {path.name}\nStop:  python lab/hv_queue.py stop {path.name}', flush=True)
    proc = None
    try:
        host_event(path, 'PREPARING', 'Queue: ' + ', '.join(profiles) + '. Five-second stop window before staging.')
        for _ in range(25):
            if (path / 'STOP').exists():
                break
            time.sleep(0.2)
        prepare(path, profiles, args.rebuild)
        if (path / 'STOP').exists():
            host_event(path, 'STOPPED', 'Stopped before VM management; no VM changes.')
            return 2
        host_event(path, 'READY', 'Images staged. Starting visible Hyper-V worker. Press Q there, or Ctrl+C here, to stop.')
        offset = (path / 'events.jsonl').stat().st_size
        # One visible worker for this batch. No service, scheduled task, stored credential,
        # execution-policy override or change to the operator's elevation settings.
        script = """$ErrorActionPreference='Stop'; $argLine='-NoProfile -File "'+$env:HV_QUEUE_WORKER+'" -RunDir "'+$env:HV_QUEUE_RUN+'"'; $p=Start-Process powershell.exe -Verb RunAs -ArgumentList $argLine -PassThru -Wait; exit $p.ExitCode"""
        proc = subprocess.Popen(['powershell.exe', '-NoProfile', '-Command', script],
                                env={**os.environ, 'HV_QUEUE_WORKER': str(LAB / 'Invoke-HvQueue.ps1'), 'HV_QUEUE_RUN': str(path)},
                                creationflags=subprocess.CREATE_NEW_PROCESS_GROUP)
        return watch(path, proc, offset)
    except KeyboardInterrupt:
        request_stop(path)
        if proc is not None:
            return watch(path, proc)
        host_event(path, 'STOPPED', 'Cancelled during preparation. No VM changes.')
        return 2
    except (OSError, ValueError, subprocess.SubprocessError) as exc:
        if proc is not None:
            request_stop(path)
        else:
            host_event(path, 'ERROR', str(exc))
        raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='action', required=True)
    start = sub.add_parser('run', help='Stage and run profiles in order; stop on the first non-PASS.')
    start.add_argument('profiles', nargs='*', help='Ordered subset of ' + ', '.join(PROFILES) + ' (default: all).')
    start.add_argument('--rebuild', action='store_true', help='Rebuild the selected firmware profiles before elevation.')
    for action in ('watch', 'stop'):
        sub.add_parser(action).add_argument('run_id')
    args = parser.parse_args()
    if args.action == 'run':
        return run(args)
    path = select_run(args.run_id)
    if args.action == 'watch':
        return watch(path)
    request_stop(path)
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, ValueError, subprocess.SubprocessError) as err:
        print(err, file=sys.stderr)
        sys.exit(1)
