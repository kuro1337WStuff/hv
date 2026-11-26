"""Replay the fixed trial 134 READ(10) on a private ISO; emit digests, never media bytes."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import stat
import subprocess


# Pins from journal/reports/install-content-134.witness.json. An arbitrary
# image and a matching caller-supplied JSON do not constitute trial 134.
BLOCK = 2048
LBA = 247061
OFFSET = 2032
LENGTH = 64
PINS = {
    'scope': 'host-only', 'filesystem': 'UDF', 'member': 'sources/install.esd',
    'media_size': 3839819776,
    'media_sha256': 'a11116c0645d892d6a5a7c585ecc1fa13aa66f8c7cc6b03bf1f27bd16860cc35',
    'file_size': 3135611742,
    'file_sha256': '42d234f69a8cacb657ac67b24b72b40a3c3ce8844ec0d947c9e2ca862e819cfb',
    'file_offset': 65520, 'length': LENGTH, 'logical_block_size': BLOCK,
    'first_lba': LBA, 'sector_offset': OFFSET, 'last_lba': LBA + 1,
    'raw_byte_offset': LBA * BLOCK + OFFSET,
    'range_sha256': '8d0dd15b53bcd7a6448f5fc564381655a87356b3f19909b3123143b55f0d9461',
    'one_block_shift_sha256': '72776bd9740844b75785fdf3792a1368b29a09dbbdfc7c209d6eef0fe419457f',
    'guest_read': 'not-run',
}


def regular_path(path):
    if os.name == 'nt' and path.drive.startswith('\\\\'):
        raise ValueError('expected a local file, not a UNC or device path')
    path = path.resolve(strict=True)
    if (os.name == 'nt' and path.drive.startswith('\\\\')) or not path.is_file():
        raise ValueError('expected a local regular file')
    return path


def load_witness(path):
    with regular_path(path).open('rb') as source:
        if not stat.S_ISREG(os.fstat(source.fileno()).st_mode):
            raise ValueError('expected a regular witness file')
        data = source.read(16385)
    if len(data) > 16384:
        raise ValueError('oversized witness JSON')
    witness = json.loads(data)
    if not isinstance(witness, dict) or any(type(witness.get(k)) is not type(v) or witness[k] != v
                                             for k, v in PINS.items()):
        raise ValueError('identity or range is not the published trial 134 witness')
    return witness


def command_response(executable, image):
    # Bound even an erroneous executable's output. Never echo its binary pipe.
    with subprocess.Popen([executable, image], stdin=subprocess.DEVNULL,
                          stdout=subprocess.PIPE, stderr=subprocess.DEVNULL) as child:
        response = child.stdout.read(2 * BLOCK + 1)
        if len(response) > 2 * BLOCK:
            child.kill()
            raise ValueError('oversized READ(10) response')
        if child.wait() != 0 or len(response) != 2 * BLOCK:
            raise ValueError('replay failed or did not return exactly two sectors')
    return response


def fingerprint(info):
    return info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns


def replay(image, witness_json, replay_exe):
    witness = load_witness(witness_json)
    image, replay_exe = regular_path(image), regular_path(replay_exe)
    with image.open('rb') as source:
        info = os.fstat(source.fileno())
        if not stat.S_ISREG(info.st_mode) or info.st_size != witness['media_size']:
            raise ValueError('image type or size differs from trial 134')
        digest = hashlib.sha256()
        while chunk := source.read(1024 * 1024):
            digest.update(chunk)
        media_hash = digest.hexdigest()
        if media_hash != witness['media_sha256']:
            raise ValueError('full image SHA-256 differs from trial 134')

        # Read the complete sector pair independently, not just the witness:
        # matching the middle 64 bytes alone would miss damage on either side.
        source.seek(LBA * BLOCK)
        direct = source.read(2 * BLOCK)
        source.seek(witness['raw_byte_offset'] + BLOCK)
        shifted = hashlib.sha256(source.read(LENGTH)).hexdigest()
        if shifted != witness['one_block_shift_sha256']:
            raise ValueError('one-block-shift control differs from trial 134')
        response = command_response(replay_exe, image)
        if fingerprint(info) != fingerprint(image.stat()) or fingerprint(info) != fingerprint(os.fstat(source.fileno())):
            raise ValueError('image changed during verification')
        if response != direct:
            raise ValueError('returned sectors differ from the direct ISO read')

    # Last 16 bytes of LBA 247061 followed by the first 48 of LBA 247062.
    range_hash = hashlib.sha256(response[OFFSET:OFFSET + LENGTH]).hexdigest()
    if range_hash != witness['range_sha256']:
        raise ValueError('cross-sector SHA-256 differs from trial 134')
    return {'media_sha256': media_hash, 'response_sha256': hashlib.sha256(response).hexdigest(),
            'range_sha256': range_hash, 'one_block_shift_sha256': shifted}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image', type=Path, metavar='IMAGE')
    parser.add_argument('witness_json', type=Path, metavar='WITNESS_JSON')
    parser.add_argument('replay_exe', type=Path, metavar='REPLAY_EXE')
    args = parser.parse_args()
    try:
        print(json.dumps(replay(**vars(args)), sort_keys=True))
    except OSError:
        parser.exit(1, 'replay: cannot open an input file or run the supplied executable\n')
    except ValueError as error:
        parser.exit(1, f'replay: {error}\n')


if __name__ == '__main__':
    main()
