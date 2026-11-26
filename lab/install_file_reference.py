"""Hash the outer installation member by name, independently of any proposed sector map."""
import argparse
import hashlib
import json
import subprocess


MEMBER = 'sources/install.esd'
OFFSET = 65520
LENGTH = 64


def reference(image, seven_zip):
    # Stream the entire member for file identity. Retain only the fixed slice, never ESD bytes on disk.
    file_hash = hashlib.sha256()
    fragment = bytearray()
    size = 0
    with subprocess.Popen([seven_zip, 'x', '-so', '-bd', '--', str(image), MEMBER],
                          stdout=subprocess.PIPE, stdin=subprocess.DEVNULL) as child:
        while chunk := child.stdout.read(1024 * 1024):
            end = size + len(chunk)
            start_byte, end_byte = max(size, OFFSET), min(end, OFFSET + LENGTH)
            if start_byte < end_byte:
                fragment.extend(chunk[start_byte - size:end_byte - size])
            file_hash.update(chunk)
            size = end
        if child.wait() != 0:
            raise ValueError('archive reader failed; no witness')
    if len(fragment) != LENGTH or not any(fragment):
        raise ValueError('missing, short or all-zero installation range')
    return dict(member=MEMBER, file_size=size, file_sha256=file_hash.hexdigest(),
                file_offset=OFFSET, length=LENGTH, range_sha256=hashlib.sha256(fragment).hexdigest())


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image')
    parser.add_argument('--seven-zip', default='7z', help='path to the 7-Zip archive reader')
    args = parser.parse_args()
    try:
        print(json.dumps(reference(args.image, args.seven_zip), sort_keys=True))
    except (OSError, ValueError) as exc:
        parser.exit(2, f'file reference: {exc}\n')
