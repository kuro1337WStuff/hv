"""Pack the fixed Linux admission inputs on the same ESP as the L1 executable."""
import argparse
import hashlib
from pathlib import Path

from pack_iso import pack


INPUTS = Path(__file__).resolve().parents[1] / 'build/linux-inputs'
# Independent of the generated SHA256SUMS: pins, not a user-provided manifest.
FILES = (
    ('vmlinuz-virt', 'VMLINUZ.BIN', 12649472,
     '1e8ac7aebfab3f0fcb443eae94ad320efbbda51be91b928c32ca1740b44ebe9d'),
    ('initramfs.cpio', 'INITRD.BIN', 1476964,
     '99db7e0bb0742619d85deaab031ff7a539cd8e46cb6b7dd8ed8dd01196e3baa4'),
    ('cmdline.bin', 'CMDLINE.BIN', 123,
     '5161eac9cd95353e2d47917e60e46bef0630b64f4bc2645f4cffd2b17f1bf958'),
)


def pack_linux(binary: Path, output: Path) -> None:
    files = []
    for source, name, size, digest in FILES:
        with (INPUTS / source).open('rb') as stream:
            data = stream.read(size + 1)
        if len(data) != size or hashlib.sha256(data).hexdigest() != digest:
            raise SystemExit(f'Linux admission input does not match pin: {source}')
        # Pass the verified bytes, not a path that could change before upload.
        files.append((name, data))
    pack(binary, output, esp_files=files)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('efi', type=Path)
    parser.add_argument('outiso', type=Path)
    args = parser.parse_args()
    pack_linux(args.efi, args.outiso)
