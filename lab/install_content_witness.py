"""Check a pinned byte range mapped to consecutive optical-media sectors.

Supply independently measured layout and digest values; this does not identify
an ISO, parse its filesystem, or establish which file owns the extent.
Run `python lab/install_content_witness.py --help` for the raw-image CLI.
"""
import argparse
import hashlib
import os
import re


SECTOR = 2048


def verify(image, *, media_size, file_size, file_offset, length, first_lba, expected_sha256):
    """Return the lowercase digest, or raise ValueError for invalid evidence.

    first_lba is the absolute sector containing the first requested byte.
    file_offset is relative to the file; the rest of that file may be fragmented.
    """
    for name, value in (("media_size", media_size), ("file_size", file_size),
                        ("file_offset", file_offset), ("length", length),
                        ("first_lba", first_lba)):
        if type(value) is not int or value < 0:
            raise ValueError(f"{name} must be a nonnegative integer")
    if not media_size or media_size % SECTOR:
        raise ValueError("media size must be a positive whole number of sectors")
    if file_size > media_size or not 0 < length <= 2 * SECTOR or file_offset + length > file_size:
        raise ValueError("invalid file size or witness range (maximum 4096 bytes)")
    start = first_lba * SECTOR + file_offset % SECTOR
    if start + length > media_size:
        raise ValueError("mapped range lies outside the media")
    if not isinstance(expected_sha256, str) or not re.fullmatch(r'[0-9a-fA-F]{64}', expected_sha256):
        raise ValueError("expected SHA-256 must be exactly 64 hexadecimal digits")
    if image.seek(0, os.SEEK_END) != media_size:
        raise ValueError("image size does not match measured media size")
    image.seek(start)
    digest = hashlib.sha256()
    remaining = length
    while remaining:
        chunk = image.read(min(remaining, 65536))
        if not chunk:
            raise ValueError("truncated image while reading the range")
        digest.update(chunk)
        remaining -= len(chunk)
    actual = digest.hexdigest()
    if actual != expected_sha256.lower():
        raise ValueError(f"range SHA-256 mismatch: measured {actual}")
    return actual


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image', help='raw optical image to open read-only')
    parser.add_argument('--media-size', type=int, required=True, help='measured image size in bytes')
    parser.add_argument('--file-size', type=int, required=True, help='measured member size in bytes')
    parser.add_argument('--file-offset', type=int, required=True, help='first requested byte within the file')
    parser.add_argument('--length', type=int, required=True, help='number of bytes to hash')
    parser.add_argument('--first-lba', type=int, required=True, help='absolute 2048-byte LBA containing the first requested byte')
    parser.add_argument('--expected-sha256', required=True, help='independently pinned digest of the range')
    args = vars(parser.parse_args())
    try:
        with open(args.pop('image'), 'rb') as image:
            print(verify(image, **args))
    except (OSError, ValueError) as error:
        parser.exit(1, f'{parser.prog}: {error}\n')


if __name__ == '__main__':
    main()
