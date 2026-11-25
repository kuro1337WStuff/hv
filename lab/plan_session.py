"""Submit a single frozen board regression to its existing foreground lane session."""
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import struct
import zlib

if __package__:
    from . import hv_queue
else:  # python lab/hv_plan.py
    import hv_queue


def _frame_digest(path):
    """Recompute HvConsole's top-down RGB565 digest from the WPF encoder's RGB8 PNG."""
    png = hv_queue.read_limited(path, 9 * 1024 * 1024)
    header = struct.pack('>IIBBBBB', 1024, 768, 8, 2, 0, 0, 0)
    if not png.startswith(b'\x89PNG\r\n\x1a\n\0\0\0\rIHDR' + header):
        raise ValueError('Expected a 1024x768 RGB console PNG.')
    pos = 8
    compressed = bytearray()
    while pos + 12 <= len(png):
        size, kind = struct.unpack_from('>I4s', png, pos)
        end = pos + 8 + size
        if end + 4 > len(png) or zlib.crc32(memoryview(png)[pos+4:end]) != struct.unpack_from('>I', png, end)[0]:
            raise ValueError('Invalid console PNG chunk.')
        if kind == b'IDAT':
            compressed.extend(memoryview(png)[pos+8:end])
        pos = end + 4
        if kind == b'IEND' and size == 0 and pos == len(png):
            break
    else:
        raise ValueError('Incomplete console PNG.')

    stride = 1024 * 3
    length = (stride + 1) * 768
    decoder = zlib.decompressobj()
    try:
        pixels = decoder.decompress(compressed, length + 1)
    except zlib.error as exc:
        raise ValueError('Invalid console PNG pixels.') from exc
    if len(pixels) != length or not decoder.eof or decoder.unused_data:
        raise ValueError('Unexpected console PNG pixel count.')
    digest = hashlib.sha256()
    prior = bytearray(stride)
    for start in range(0, length, stride + 1):
        kind = pixels[start]
        row = bytearray(pixels[start+1:start+1+stride])
        if kind > 4:
            raise ValueError('Unknown PNG filter.')
        if kind:
            for i, value in enumerate(row):
                a = row[i-3] if i >= 3 else 0
                b = prior[i]
                if kind == 1:
                    predictor = a
                elif kind == 2:
                    predictor = b
                elif kind == 3:
                    predictor = (a + b) // 2
                else:  # Paeth: nearest of left, above and upper-left, in that tie order.
                    c = prior[i-3] if i >= 3 else 0
                    pa, pb, pc = abs(b-c), abs(a-c), abs(a+b-2*c)
                    predictor = a if pa <= pb and pa <= pc else b if pb <= pc else c
                row[i] = (value + predictor) & 255
        packed = bytearray(1024 * 2)
        for i in range(1024):
            r, g, b = row[3*i], row[3*i+1], row[3*i+2]
            packed[2*i] = ((g & 28) << 3) | (b >> 3)
            packed[2*i+1] = (r & 248) | (g >> 5)
        digest.update(packed)
        prior = row
    return digest.hexdigest()


def _verify(path, card, queue, status):
    if hv_queue.read_limited(path / 'queue.json', 8192) != queue:
        raise ValueError('Retained worklist differs from the submitted queue.')
    raw = hv_queue.read_limited(path / 'events.jsonl', 256 * 1024)
    if not raw.endswith(b'\n'):
        raise ValueError('Incomplete session event record.')
    rows = [json.loads(line) for line in raw.splitlines()]
    if any(not isinstance(e, dict) or any(not isinstance(e.get(k), str) for k in
               ('state', 'detail', 'time', 'profile')) for e in rows):
        raise ValueError('Malformed session event record.')
    terminal = [e for e in rows if e['state'] in hv_queue.TERMINAL]
    codes = {'COMPLETE': 0, 'ERROR': 1, 'STOPPED': 2, 'ATTENTION': 3}
    if terminal != [rows[-1]] or status != codes[rows[-1]['state']]:
        raise ValueError('Incomplete or conflicting session terminal status.')
    state = rows[-1]['state']
    profile, sha = card.profile, card.iso.upper()
    actual = [e['state'] for e in rows[:-1]]
    expected = ['PREFLIGHT']
    if 'PRIOR' in actual:
        expected.append('PRIOR')
    expected.extend(('CHECKPOINT', 'NEXT'))
    if 'GUARD' in actual:
        expected.append('GUARD')
    expected.extend(('MOUNT', 'BOOT', 'WATCHING', 'SKIP' if 'SKIP' in actual else 'PASS'))
    # A stopped or held worker may report only a prefix, even an empty one.
    if actual != expected[:len(actual)] or (state == 'COMPLETE' and (actual != expected or actual[-1] != 'PASS')):
        raise ValueError('No matching single cold boot and terminal disposition.')
    preflight = rows[0]
    vm = f'HV-SVM-L1-{card.lane.upper()}'
    guid = r'[0-9a-fA-F]{8}(?:-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}'
    if actual and not re.match(rf'VM={vm} ID={guid}\. ', preflight['detail']):
        raise ValueError('Preflight is not from the requested dedicated VM.')
    proofs = {
        'CHECKPOINT': f"Saving prior state as 'HvQueue before {path.name}'.",
        'MOUNT': f'Mounting reviewed {profile} media; SHA256={sha}',
        'BOOT': 'Starting the isolated VM.',
        'PASS': f'Fresh VM console: {profile}.png; SHA256={sha}',
    }
    if rows[-1]['profile'] not in ('', profile) or (state == 'COMPLETE' and rows[-1]['profile'] != profile):
        raise ValueError('Terminal disposition is for a different profile.')
    for e in rows[:-1]:
        if e['profile'] != ('' if e['state'] in ('PREFLIGHT', 'PRIOR', 'CHECKPOINT') else profile):
            raise ValueError('Session profile does not match the single submitted item.')
        if e['state'] in proofs and e['detail'] != proofs[e['state']]:
            raise ValueError('Session event does not match the run, ISO or live evidence.')
    if 'PASS' in actual and _frame_digest(path / (profile + '.png')) != card.screen:
        raise ValueError('Broker PASS disagrees with the returned console pixels.')
    return state


def execute(root: Path, run_id: str, card) -> str:
    """Run a validated automatic card. Never rebuild, stage, elevate or retry."""
    if os.name != 'nt':
        raise ValueError('Foreground Hyper-V sessions require Windows.')
    if (card.lane not in ('a', 'b') or root.name != 'hv-lane-' + card.lane or
            not re.fullmatch('[0-9a-f]{32}', run_id)):
        raise ValueError('Expected the requested dedicated lane checkout and a 32-hex run ID.')
    root = Path(os.path.abspath(root))
    if root.resolve(strict=True) != root:
        raise ValueError('Lane checkout must not be redirected.')
    if any(not isinstance(v, str) or not re.fullmatch('[0-9a-f]{64}', v) for v in (card.iso, card.screen)):
        raise ValueError('Automatic runs require reviewed ISO and screen digest pins.')
    path = root / 'build' / 'runs' / run_id
    if path.parent.resolve() != path.parent:
        raise ValueError('Run directory must belong to the dedicated checkout.')
    path.mkdir(parents=True, exist_ok=False)

    def stopped():
        if os.path.lexists(path / 'STOP'):
            hv_queue.host_event(path, 'STOPPED', 'Stopped before session admission; no VM changes.')
            return True
        return False

    if stopped():
        return 'STOPPED'
    try:
        lab = root / 'lab'
        manifest = hv_queue.read_limited(lab / 'hv-profiles.json', 32768)
        registry = hv_queue.profile_registry(manifest)
        profile = card.profile
        stem = registry.get(profile)
        expected = {'cpuid': 'hv-cpuid-resume', 'l2': 'hv-l2-smoke'}.get(profile, 'hv-' + str(profile))
        if not stem or stem != expected or profile == 'l1-handoff':
            raise ValueError('Profile is not a reviewed lane image with its expected stem.')
        row = next(r for r in json.loads(manifest.decode('utf-8-sig')) if r['profile'] == profile)
        if not row['pass'] or row['pass'].lower() != card.screen:
            raise ValueError('Lane manifest does not authorize the pinned PASS framebuffer.')
        sha = card.iso.upper()
        image = lab / 'media' / f'{stem}-{sha}.iso'
        info = image.lstat()
        if (image.resolve(strict=True) != image or not stat.S_ISREG(info.st_mode) or info.st_nlink != 1 or
                info.st_file_attributes & stat.FILE_ATTRIBUTE_REPARSE_POINT or hv_queue.digest(image) != sha):
            raise ValueError('Incorrect or redirected frozen lane media.')
        queue = json.dumps([{'profile': profile, 'sha256': sha}]).encode('ascii')
        with (path / 'queue.json').open('xb') as f:
            f.write(queue)
        if stopped():
            return 'STOPPED'
        status = hv_queue.session_run(path, (profile,), registry, manifest, root=root, queue=queue)
    except OSError as exc:
        hv_queue.host_event(path, 'ERROR', str(exc))
        return 'ERROR'
    try:
        return _verify(path, card, queue, status)
    except OSError as exc:
        raise ValueError('Missing submitted queue, broker record or console evidence.') from exc
