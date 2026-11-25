"""Prepare fixed P5 guest inputs, offline; does not start or modify any VM.

From the repository root: python lab/linux/prepare.py [new-output-directory]
Requires Python 3.11+ and the Windows system tar.exe (bsdtar elsewhere).
The output command line includes the Linux ABI's trailing NUL, not a newline.
"""
import argparse
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
from tempfile import TemporaryDirectory

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
ISO = ROOT / 'lab/media/alpine-virt-3.24.4-x86_64.iso'
ISO_PIN = '157d353f8e54252cad2d9812a0d56bb7fa2231fc154a110e1d180d7480ef8b4f'
MEMBERS = {
    'boot/vmlinuz-virt': '1e8ac7aebfab3f0fcb443eae94ad320efbbda51be91b928c32ca1740b44ebe9d',
    'boot/config-6.18.60-0-virt': '29636aa39722a1e48b440f3b48bd3f579f88f7f498552789a735791235e8050b',
    'boot/initramfs-virt': '255d02822102997ba6a01e014bf602a0d6d45b8bca934e4d448f7d3a7540abea',
    'usr/bin/busybox': '01a989eb4d1d04b0d146c790ac536abd88f374ec74a2e110c58910b840d42045',
    'usr/lib/ld-musl-x86_64.so.1': '095b047c631447e4fa50401fa7ffce01f678587040d65ba18c574981dc630de5',
}


def extract(tar, member, **source):
    # Only return bytes from named, hash-pinned members. No extraction to host paths.
    data = subprocess.run([tar, '-xOf', '-', member], stdout=subprocess.PIPE,
                          check=True, **source).stdout
    if hashlib.sha256(data).hexdigest() != MEMBERS[member]:
        raise ValueError(f'wrong SHA-256 for {member}')
    return data


def initramfs(busybox, musl, init):
    # Linux initramfs newc: fixed inode order, zero uid/gid/mtime, four-byte
    # alignment of both fields. Device numbers exist only in guest archive bytes.
    records = [
        ('bin', 0o040755, b'', 0, 0),
        ('dev', 0o040755, b'', 0, 0),
        ('lib', 0o040755, b'', 0, 0),
        ('proc', 0o040755, b'', 0, 0),
        ('dev/console', 0o020600, b'', 5, 1),
        ('dev/null', 0o020666, b'', 1, 3),
        ('bin/busybox', 0o100755, busybox, 0, 0),
        ('bin/sh', 0o120777, b'busybox', 0, 0),
        ('lib/ld-musl-x86_64.so.1', 0o100755, musl, 0, 0),
        ('lib/libc.musl-x86_64.so.1', 0o120777, b'ld-musl-x86_64.so.1', 0, 0),
        ('init', 0o100755, init, 0, 0),
        ('TRAILER!!!', 0, b'', 0, 0),
    ]
    archive = bytearray()
    for inode, (name, mode, data, major, minor) in enumerate(records, 1):
        name = name.encode('ascii') + b'\0'
        fields = (inode, mode, 0, 0, 1, 0, len(data), 0, 0, major, minor, len(name), 0)
        archive.extend(b'070701' + ''.join(f'{v:08x}' for v in fields).encode('ascii'))
        archive.extend(name)
        archive.extend(b'\0' * (-len(archive) % 4))
        archive.extend(data)
        archive.extend(b'\0' * (-len(archive) % 4))
    return archive


def prepare(output):
    tar = (Path(os.environ['SystemRoot']) / 'System32/tar.exe' if sys.platform == 'win32'
           else shutil.which('bsdtar'))
    if not tar:
        raise RuntimeError('bsdtar required to read ISO9660 and gzip/newc')
    files = {}
    with ISO.open('rb') as src:
        if hashlib.file_digest(src, 'sha256').hexdigest() != ISO_PIN:
            raise ValueError('wrong Alpine media SHA-256')
        for name in ('boot/vmlinuz-virt', 'boot/config-6.18.60-0-virt', 'boot/initramfs-virt'):
            src.seek(0)
            files[Path(name).name] = extract(tar, name, stdin=src)
    upstream = files.pop('initramfs-virt')
    busybox = extract(tar, 'usr/bin/busybox', input=upstream)
    musl = extract(tar, 'usr/lib/ld-musl-x86_64.so.1', input=upstream)
    # Normalize checkout line endings; the guest's shebang must end in LF.
    init = (HERE / 'init').read_text(encoding='ascii').encode('ascii')
    line, = (HERE / 'cmdline.txt').read_text(encoding='ascii').splitlines()
    files['cmdline.bin'] = line.encode('ascii') + b'\0'
    files['initramfs.cpio'] = initramfs(busybox, musl, init)
    digests = {name: hashlib.sha256(data).hexdigest() for name, data in files.items()}
    if digests['initramfs.cpio'] != '99db7e0bb0742619d85deaab031ff7a539cd8e46cb6b7dd8ed8dd01196e3baa4':
        raise ValueError('unexpected derived initramfs SHA-256')
    if digests['cmdline.bin'] != '5161eac9cd95353e2d47917e60e46bef0630b64f4bc2645f4cffd2b17f1bf958':
        raise ValueError('unexpected NUL-terminated command line SHA-256')
    sums = ''.join(f'{digest}  {name}\n' for name, digest in sorted(digests.items()))
    files['SHA256SUMS'] = sums.encode('ascii')
    output.parent.mkdir(parents=True, exist_ok=True)
    if output.exists():
        raise FileExistsError(f'output must be a new directory: {output}')
    with TemporaryDirectory(dir=output.parent) as tmp:
        staged = Path(tmp) / 'inputs'
        staged.mkdir()
        for name, data in files.items():
            (staged / name).write_bytes(data)
        staged.rename(output)
    print(sums, end='')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', nargs='?', type=Path, default=ROOT / 'build/linux-inputs')
    prepare(parser.parse_args().output)
