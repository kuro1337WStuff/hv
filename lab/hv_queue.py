"""Prepare a finite Hyper-V regression queue without elevating the build toolchain."""
import argparse
import base64
import hashlib
import io
import json
import os
from pathlib import Path
import re
import select
import shutil
import signal
import socket
import struct
import subprocess
import sys
import time
import uuid
import zlib

LAB = Path(__file__).resolve().parent
ROOT = LAB.parent
RUNS = ROOT / 'build' / 'runs'
DEFAULT_PROFILES = ('l2', 'cpuid', 'npt-write', 'npt-remap', 'npt-cross')
TERMINAL = {'COMPLETE', 'STOPPED', 'ATTENTION', 'ERROR'}


def read_limited(path, limit):
    with path.open('rb') as f:
        data = f.read(limit + 1)
    if not 0 < len(data) <= limit:
        raise ValueError(f'Invalid size for {path.name}.')
    return data


def profile_registry(raw):
    rows = json.loads(raw.decode('utf-8-sig'))
    if not isinstance(rows, list) or not 1 <= len(rows) <= 128:
        raise ValueError('Invalid profile manifest.')
    registry = {}
    stems = set()
    for row in rows:
        if not isinstance(row, dict) or set(row) != {'profile', 'stem', 'pass'}:
            raise ValueError('Invalid profile entry.')
        name, stem, digest = row['profile'], row['stem'], row['pass']
        if (not isinstance(name, str) or len(name) > 48 or
                not re.fullmatch('[a-z][a-z0-9]*(?:-[a-z0-9]+)*', name) or name in registry or
                (digest is not None and (not isinstance(digest, str) or not re.fullmatch('[0-9a-fA-F]{64}', digest)))):
            raise ValueError('Invalid or duplicate profile.')
        if name in {'con', 'prn', 'aux', 'nul'} or re.fullmatch('(?:com|lpt)[0-9]', name):
            raise ValueError('Reserved profile filename.')
        if name == 'probe':
            if stem is not None:
                raise ValueError('Probe is screen-only.')
        elif (not isinstance(stem, str) or len(stem) > 64 or
              not re.fullmatch('hv-[a-z0-9]+(?:-[a-z0-9]+)*', stem) or stem in stems):
            raise ValueError('Invalid or duplicate firmware image stem.')
        else:
            stems.add(stem)
        registry[name] = stem
    return registry


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


def prepare(path, profiles, rebuild, registry):
    queue = []
    media = LAB / 'media'
    media.mkdir(exist_ok=True)
    for profile in profiles:
        if (path / 'STOP').exists():
            break
        if rebuild:
            host_event(path, 'BUILD', profile)
            subprocess.run(['cmd.exe', '/d', '/c', r'firmware\build.cmd', profile], cwd=ROOT, check=True)
        image = ROOT / 'build' / (registry[profile] + '.iso')
        if not image.is_file():
            raise ValueError(f'Missing {image.name}; use run --rebuild.')
        sha = digest(image)
        staged = media / f'{registry[profile]}-{sha}.iso'
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


def check_peer(sock, pid):
    # Match the established server-side IPv4 tuple, not just a reusable listening port.
    import ctypes
    size = ctypes.c_ulong()
    table = ctypes.windll.iphlpapi.GetExtendedTcpTable
    status = table(None, ctypes.byref(size), False, socket.AF_INET, 5, 0)
    for _ in range(4):
        if status != 122 or not 4 <= size.value <= 32 * 1024 * 1024:
            raise ValueError('Cannot identify the session broker.')
        data = ctypes.create_string_buffer(size.value)
        status = table(data, ctypes.byref(size), False, socket.AF_INET, 5, 0)
        if status == 0:
            break
    else:
        raise ValueError('Cannot identify the session broker.')
    count, = struct.unpack_from('<I', data)
    if count > (len(data) - 4) // 24:
        raise ValueError('Invalid Windows TCP table.')
    addr = struct.unpack('<I', socket.inet_aton('127.0.0.1'))[0]
    expected = (5, addr, socket.htons(sock.getpeername()[1]), addr,
                socket.htons(sock.getsockname()[1]), pid)
    if not any(struct.unpack_from('<6I', data, 4 + i * 24) == expected for i in range(count)):
        raise ValueError('Stale session descriptor: server PID mismatch.')


def save_png(path, encoded):
    image = base64.b64decode(encoded, validate=True)
    if not image.startswith(b'\x89PNG\r\n\x1a\n'):
        raise ValueError('Invalid PNG signature.')
    pos = 8
    has_data = False
    while pos + 12 <= len(image):
        size, kind = struct.unpack_from('>I4s', image, pos)
        end = pos + 8 + size
        if end + 4 > len(image) or zlib.crc32(memoryview(image)[pos+4:end]) != struct.unpack_from('>I', image, end)[0]:
            raise ValueError('Invalid PNG chunk.')
        if pos == 8 and (kind != b'IHDR' or size != 13 or
                         not all(0 < n <= 8192 for n in struct.unpack_from('>II', image, pos + 8))):
            raise ValueError('Invalid PNG header.')
        has_data |= kind == b'IDAT' and size > 0
        pos = end + 4
        if kind == b'IEND' and size == 0 and pos == len(image) and has_data:
            with path.open('xb') as out:
                out.write(image)
            return
    raise ValueError('Incomplete PNG.')


class SessionInput(io.RawIOBase):
    """Give Windows Python regular opportunities to dispatch console Ctrl+C."""
    def __init__(self, sock):
        self.sock = sock
        self.deadline = time.monotonic() + 5

    def readable(self):
        return True

    def readinto(self, buffer):
        while not select.select([self.sock], [], [], 0.2)[0]:
            if self.deadline is not None and time.monotonic() >= self.deadline:
                raise TimeoutError('Session acknowledgement timed out.')
        return self.sock.recv_into(buffer)


def session_run(path, profiles, registry, manifest):
    queue = read_limited(path / 'queue.json', 8192)
    queue.decode('utf-8')
    checkout = hashlib.sha256(str(ROOT).lower().encode('utf-8')).hexdigest()[:20]
    program_files = Path(os.environ.get('ProgramW6432') or os.environ['ProgramFiles'])
    raw = read_limited(program_files / ('HvLabSession-' + checkout) / 'session.json', 4096)
    info = json.loads(raw)
    if not isinstance(info, dict) or type(info.get('pid')) is not int or not 0 < info['pid'] <= 0xffffffff or \
            type(info.get('port')) is not int or not 0 < info['port'] <= 65535 or \
            not isinstance(info.get('token'), str) or not re.fullmatch('[0-9a-f]{32}', info['token']):
        raise ValueError('Invalid session descriptor.')
    images = {f'{prefix}{profile}.png' for profile in registry for prefix in ('guard-', 'prior-')}
    images.update(p + '.png' for p in profiles)
    terminal = None
    with socket.create_connection(('127.0.0.1', info['port']), timeout=5) as sock, \
            io.BufferedReader(SessionInput(sock)) as stream:
        check_peer(sock, info['pid'])
        sock.sendall(f"{info['token']} {path.name}\n".encode('ascii'))
        for document in (queue, manifest):
            sock.sendall(struct.pack('>I', len(document)) + document)
        try:
            ack = stream.readline(1024)
        except OSError as exc:
            raise ValueError('No session acknowledgement; inspect the broker before retrying.') from exc
        if ack.startswith(b'ERROR '):
            raise ValueError('Session broker rejected request: ' + repr(ack[6:512].strip()))
        if ack not in (b'ACCEPTED\n', b'ACCEPTED\r\n'):
            raise ValueError('No session acknowledgement; inspect the broker before retrying.')
        sock.settimeout(None)
        stream.raw.deadline = None
        total = 0
        for _ in range(4096):
            line = stream.readline(12 * 1024 * 1024 + 1)
            total += len(line)
            if not line.endswith(b'\n') or total > 128 * 1024 * 1024:
                raise ValueError('Session disconnected or exceeded evidence limit before END; inspect VM.')
            tag, _, value = line.rstrip(b'\r\n').partition(b' ')
            if tag == b'IMAGE':
                name, _, data = value.partition(b' ')
                name = name.decode('ascii')
                if name not in images:
                    raise ValueError('Unexpected session evidence filename.')
                images.remove(name)
                save_png(path / name, data)
            elif tag == b'EVENT' and len(value) <= 16384 and terminal is None:
                item = json.loads(value)
                if not isinstance(item, dict) or any(not isinstance(item.get(k), str) for k in ('state', 'time', 'detail')) or \
                        not isinstance(item.get('profile') or '', str):
                    raise ValueError('Invalid session event.')
                if item['state'] in TERMINAL:
                    terminal = item
                    continue  # Publish verdict only after all evidence has arrived.
                record_event(path, item)
            elif tag == b'END' and value in (b'0', b'1', b'2', b'3') and terminal is not None:
                if int(value) != {'COMPLETE': 0, 'ERROR': 1, 'STOPPED': 2, 'ATTENTION': 3}[terminal['state']]:
                    raise ValueError('Session status disagrees with verdict.')
                record_event(path, terminal)
                return int(value)
            elif tag == b'ERROR':
                raise ValueError('Session broker error: ' + repr(value[:512]))
            else:
                raise ValueError('Unexpected session response; inspect VM.')
        raise ValueError('Session exceeded message limit.')


def record_event(path, item):
    with (path / 'events.jsonl').open('a', encoding='utf-8') as f:
        f.write(json.dumps(item) + '\n')
    print(item['state'], ascii(item['detail']), flush=True)


def run(args):
    if os.name != 'nt':
        raise ValueError('This lab requires Windows and Hyper-V.')
    manifest = read_limited(LAB / 'hv-profiles.json', 32768)
    registry = profile_registry(manifest)
    profiles = args.profiles or DEFAULT_PROFILES
    if len(set(profiles)) != len(profiles) or any(not registry.get(p) for p in profiles):
        raise ValueError('Use each supported profile at most once: ' + ', '.join(p for p in registry if registry[p]))
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
        prepare(path, profiles, args.rebuild, registry)
        if (path / 'STOP').exists():
            host_event(path, 'STOPPED', 'Stopped before VM management; no VM changes.')
            return 2
        if args.stage_only:
            host_event(path, 'READY', 'Images staged only. No worker started or VM changed.')
            print('In your existing administrator PowerShell window, run:', flush=True)
            worker = str(LAB / 'Invoke-HvQueue.ps1').replace("'", "''")
            run_dir = str(path).replace("'", "''")
            print(f"powershell.exe -NoProfile -File '{worker}' -RunDir '{run_dir}'", flush=True)
            return 0
        if args.session:
            host_event(path, 'READY', 'Images staged. Submitting to foreground broker; Ctrl+C requests STOP.')
            old = signal.signal(signal.SIGINT, lambda *_: request_stop(path))
            try:
                return session_run(path, profiles, registry, manifest)
            finally:
                signal.signal(signal.SIGINT, old)
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
    start.add_argument('profiles', nargs='*', help='Ordered firmware profiles from lab/hv-profiles.json (default: ' + ', '.join(DEFAULT_PROFILES) + ').')
    start.add_argument('--rebuild', action='store_true', help='Rebuild the selected firmware profiles before elevation.')
    mode = start.add_mutually_exclusive_group()
    mode.add_argument('--stage-only', action='store_true', help='Freeze the batch without starting a worker; print the command for an existing administrator PowerShell window.')
    mode.add_argument('--session', action='store_true', help='Use the existing foreground session broker without elevation.')
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
