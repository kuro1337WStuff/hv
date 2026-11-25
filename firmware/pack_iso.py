"""Place the removable-media x64 EFI executable inside an El Torito FAT image."""
import sys
from pathlib import Path
from tempfile import TemporaryDirectory

import pycdlib
from pyfatfs.PyFat import PyFat
from pyfatfs.PyFatFS import PyFatFS


def pack(binary: Path, output: Path) -> None:
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
        iso = pycdlib.PyCdlib()
        iso.new(vol_ident='HVUEFIPROBE')
        try:
            iso.add_file(str(esp), iso_path='/ESP.IMG;1')
            iso.add_eltorito('/ESP.IMG;1', platform_id=0xef, efi=True)
            image = Path(tmp) / 'probe.iso'
            iso.write(str(image))
        finally:
            iso.close()
        image.replace(output)


if __name__ == '__main__':
    pack(Path(sys.argv[1]), Path(sys.argv[2]))
