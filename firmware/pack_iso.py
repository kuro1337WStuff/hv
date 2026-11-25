"""Place the removable-media x64 EFI executable inside an El Torito FAT image."""
import sys
from pathlib import Path
from tempfile import TemporaryDirectory

import pycdlib
from pyfatfs.PyFat import PyFat
from pyfatfs.PyFatFS import PyFatFS


def pack(binary: Path, output: Path, root_files=()) -> None:
    with TemporaryDirectory(dir=output.parent) as tmp:
        esp = Path(tmp) / 'esp.img'
        esp.touch()
        fat = PyFat()
        fat.mkfs(str(esp), PyFat.FAT_TYPE_FAT12, size=1440 * 1024, label='HVPROBE')
        fat.close()
        with PyFatFS(str(esp), utc=True) as fs:
            fs.makedirs('EFI/BOOT')
            with binary.open('rb') as src:
                fs.upload('EFI/BOOT/BOOTX64.EFI', src)
            for name, path in root_files:
                with path.open('rb') as src:
                    fs.upload(name, src)
        iso = pycdlib.PyCdlib()
        # Level 2 permits the four-character extension in limine.conf.
        iso.new(vol_ident='HVUEFIPROBE', interchange_level=2 if root_files else 1)
        try:
            iso.add_file(str(esp), iso_path='/ESP.IMG;1')
            for name, path in root_files:
                iso.add_file(str(path), iso_path=f'/{name.upper()};1')
            iso.add_eltorito('/ESP.IMG;1', platform_id=0xef, efi=True)
            image = Path(tmp) / 'probe.iso'
            iso.write(str(image))
        finally:
            iso.close()
        image.replace(output)


if __name__ == '__main__':
    pack(Path(sys.argv[1]), Path(sys.argv[2]))
