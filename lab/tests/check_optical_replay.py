"""Linux CI: exercise the file adapter using a sparse, entirely authored ISO-sized file."""
from pathlib import Path
import subprocess
import sys
import tempfile


def check(executable):
    # These are trial 134's independent sector coordinates, not decoded from the CDB.
    # Distinguish sector order and binary-mode handling (NUL, LF, CR, and 0x1a).
    first = bytes(range(256)) * 8
    second = bytes(reversed(range(256))) * 8
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / 'authored.img'
        with path.open('wb') as image:
            image.truncate(3_839_819_776)
            image.seek(247_061 * 2048)
            image.write(first + second)
        run = subprocess.run([executable, path], capture_output=True, check=True)
        if run.stdout != first + second:
            raise ValueError('raw adapter output did not match both authored sectors in order')
        with path.open('r+b') as image:
            image.truncate(247_062 * 2048)
        rejected = subprocess.run([executable, path], capture_output=True)
        if not rejected.returncode or rejected.stdout:
            raise ValueError('short image produced a successful or partial reply')
    print('authored file adapter: two exact sectors; truncated image refused without output')


if __name__ == '__main__':
    check(sys.argv[1])
