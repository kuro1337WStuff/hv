"""Build the fixed SysV ELF and its UEFI-only optical image on Windows x64."""
import argparse
import hashlib
from pathlib import Path
import shutil
import subprocess
import sys
from tempfile import TemporaryDirectory
import time
from urllib.request import urlopen
from zipfile import ZipFile

from pyfatfs.DosDateTime import DosDateTime

CROSS = Path(__file__).resolve().parent
sys.path.insert(0, str(CROSS.parent))
from pack_iso import pack

BUILD = CROSS.parents[1] / 'build'
TOOLS = BUILD / 'tools'
LIMINE = 'aad3edd370955449717a334f0289dee10e2c5f01'
LIMINE_URL = f'https://raw.githubusercontent.com/limine-bootloader/limine/{LIMINE}/'
LIMINE_DIR = TOOLS / f'limine-{LIMINE}'
LIMINE_HASHES = {
    'BOOTX64.EFI': '51ca2b20ed14bbc678a396cdfb0c1d941acbc155cabebc9bfafabe4cdae1be73',
    'limine.h': 'f8b35287579dbe50e9d84cca7b558e39b2b64e499f9b37ef5ebe969c05104c33',
    'LICENSE': '6ea0b80a0ed3d69204ae56a397cd0b557823b4f2826890ec5776d2e34752f4cf',
}
WHEEL = 'ziglang-0.13.0-py3-none-win_amd64.whl'
WHEEL_URL = ('https://files.pythonhosted.org/packages/e0/0c/'
             '66f4710aedf5544e15779ff9c6e770322986e15daf9299b1a4df0be19e09/' + WHEEL)
WHEEL_HASH = 'b64b2aa936ebbfeecfbb2611df331a5474edebe1246b98c2f56d6ae4d58b71fc'


def verified(path: Path, url: str, expected: str) -> Path:
    """Use the offline cache when present; never use an unverified input."""
    path.parent.mkdir(parents=True, exist_ok=True)
    source = path
    if not path.exists():
        source = path.with_name(path.name + '.part')
        print(f'Downloading {path.name}', flush=True)
        with urlopen(url, timeout=120) as remote, source.open('wb') as local:
            shutil.copyfileobj(remote, local)
    digest = hashlib.sha256()
    with source.open('rb') as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b''):
            digest.update(chunk)
    if digest.hexdigest() != expected:
        raise SystemExit(f'{source}: SHA-256 {digest.hexdigest()}, expected {expected}')
    if source != path:
        source.replace(path)
    return path


def build_elf(wheel: Path, elf: Path, broadcast: bool) -> None:
    # Only the native clang/lld driver and builtin C headers are needed. Use a
    # fresh extraction of the verified wheel, not executables from PATH or WSL.
    with TemporaryDirectory(dir=TOOLS) as tmp:
        work = Path(tmp)
        with ZipFile(wheel) as z:
            for name in z.namelist():
                if name == 'ziglang/zig.exe' or name.startswith('ziglang/lib/include/'):
                    z.extract(name, work)
        zig = work / 'ziglang' / 'zig.exe'
        cflags = ['--target=x86_64-unknown-none-elf', '-std=c11', '-O2',
                  '-ffreestanding', '-fno-pic', '-mcmodel=kernel', '-mno-red-zone',
                  '-mgeneral-regs-only', '-fno-stack-protector', '-fno-unwind-tables',
                  '-fno-asynchronous-unwind-tables', '-Wall', '-Wextra', '-Werror',
                  '-nostdinc', '-isystem', zig.parent / 'lib/include', '-I', LIMINE_DIR]
        if broadcast:
            cflags.append('-DHV_NPT_BROADCAST')
        objects = []
        for name in ('main', 'svm'):
            obj = work / (name + '.o')
            subprocess.run([zig, 'clang', *cflags, '-c', CROSS / (name + '.c'), '-o', obj], check=True)
            objects.append(obj)
        subprocess.run([zig, 'ld.lld', '-m', 'elf_x86_64', '-static', '--build-id=none',
                        '-z', 'max-page-size=0x1000', '-T', CROSS / 'link.ld',
                        '-o', elf, *objects], check=True)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('profile', nargs='?', choices=('npt-cross', 'npt-broadcast'), default='npt-cross')
    profile = parser.parse_args().profile
    broadcast = profile == 'npt-broadcast'
    for name, digest in LIMINE_HASHES.items():
        verified(LIMINE_DIR / name, LIMINE_URL + name, digest)
    wheel = verified(TOOLS / WHEEL, WHEEL_URL, WHEEL_HASH)
    elf = BUILD / ('broadcast.elf' if broadcast else 'cross.elf')
    build_elf(wheel, elf, broadcast)
    loader = BUILD / ('NPTBROADCAST.EFI' if broadcast else 'NPTCROSS.EFI')
    shutil.copyfile(LIMINE_DIR / 'BOOTX64.EFI', loader)

    # Both libraries consult the wall clock (including in the final ISO write).
    # This standalone process only: give FAT and ISO records one UTC timestamp.
    epoch = 946684800  # 2000-01-01 00:00:00 UTC
    time.time = lambda: epoch
    time.localtime = lambda seconds=epoch: time.gmtime(seconds)
    DosDateTime.now = staticmethod(lambda tz=None: DosDateTime(2000, 1, 1))
    pack(loader, BUILD / f'hv-{profile}.iso', (
        ('limine.conf', CROSS / 'limine.conf'), ('cross.elf', elf),
        ('limine.txt', LIMINE_DIR / 'LICENSE'),
    ))


if __name__ == '__main__':
    main()
