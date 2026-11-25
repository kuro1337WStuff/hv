"""Check a qualification ISO and its three pinned FAT inputs without mounting it.

Run with build/tools/packenv/Scripts/python.exe. Supply the whole-ISO SHA-256
from the reviewed mount event, not a hash derived from the file under test.
"""
import argparse
import hashlib
import io
from pathlib import Path
import re
import sys

import pycdlib
from fs.errors import FSError
from pycdlib.pycdlibexception import PyCdlibException
from pyfatfs import PyFATException
from pyfatfs.PyFatFS import PyFatBytesIOFS

# Share only the reviewed input pins. Do not read build/linux-inputs, consult
# a generated manifest, or invoke either the preparation or the packing code.
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'firmware'))
from linux_media import FILES


ESP_PATH = '/ESP.IMG;1'


def verify(path, expected_sha256):
    if not re.fullmatch('[0-9a-fA-F]{64}', expected_sha256):
        raise ValueError('expected ISO SHA-256 must be exactly 64 hex digits')
    # Parse the exact bytes authenticated here, even if the path is replaced.
    data = path.read_bytes()
    iso_hash = hashlib.sha256(data).hexdigest()
    if iso_hash != expected_sha256.lower():
        raise ValueError(f'ISO SHA-256 mismatch: expected {expected_sha256}, got {iso_hash}')

    report = [f'ISO {iso_hash}  {len(data)} bytes  {path}']
    iso = pycdlib.PyCdlib()
    with io.BytesIO(data) as snapshot:
        iso.open_fp(snapshot)
        try:
            # Check the actual default boot entry, not just a similarly named FAT file.
            catalog = iso.eltorito_boot_catalog
            if catalog is None or catalog.validation_entry.platform_id != 0xef:
                raise ValueError('missing default EFI El Torito boot entry')
            entry = catalog.initial_entry
            if (entry.boot_indicator != 0x88 or entry.boot_media_type != 0 or
                    entry.load_rba != iso.get_record(iso_path=ESP_PATH).extent_location()):
                raise ValueError(f'EFI boot entry does not reference {ESP_PATH} as a bootable no-emulation image')

            with iso.open_file_from_iso(iso_path=ESP_PATH) as esp, PyFatBytesIOFS(esp) as fat:
                for _, name, size, pin in FILES:
                    actual_size = fat.getsize(name)
                    if actual_size != size:
                        raise ValueError(f'{name} size mismatch: expected {size}, got {actual_size}')
                    with fat.openbin(name, 'r') as member:
                        digest = hashlib.file_digest(member, 'sha256').hexdigest()
                    if digest != pin:
                        raise ValueError(f'{name} SHA-256 mismatch: expected {pin}, got {digest}')
                    report.append(f'FAT {digest}  {actual_size} bytes  {name}')
        finally:
            iso.close()
    return '\n'.join(report)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('iso', type=Path)
    parser.add_argument('sha256', help='reviewed SHA-256 of the complete ISO')
    args = parser.parse_args()
    try:
        print(verify(args.iso, args.sha256))
    except (OSError, ValueError, RuntimeError, PyCdlibException, PyFATException, FSError) as err:
        parser.exit(1, f'verify_media: {err}\n')
