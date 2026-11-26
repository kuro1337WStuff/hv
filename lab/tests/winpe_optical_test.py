"""Static child-ID evidence from synthetic, independently inventoried files only."""
from hashlib import sha256
from pathlib import Path
from tempfile import TemporaryDirectory
import unittest

from lab.winpe_optical import report


GUID = '{4D36E965-E325-11CE-BFC1-08002BE10318}'
INF = rf'''[Version]
Signature="$Windows NT$"
Class=CDROM
ClassGuid={GUID}
[Manufacturer]
Maker=cdrom_device,NTamd64,NTx86
[cdrom_device.NTamd64]
Optical=cdrom_install,GenCdRom
[cdrom_device.NTx86]
Other=cdrom_install,GenDisk
[cdrom_install]
[cdrom_install.Services]
AddService=cdrom,2,cdrom_ServiceInstallSection
[cdrom_ServiceInstallSection]
ServiceBinary=%12%\cdrom.sys
StartType=1
ServiceType=1
'''
INF_PATH = 'Windows/INF/cdrom.inf'
SYS_PATH = 'Windows/System32/drivers/cdrom.sys'


def pe_header(machine=0x8664):
    dos = b'MZ' + bytes(58) + (64).to_bytes(4, 'little')
    return dos + b'PE\0\0' + machine.to_bytes(2, 'little') + bytes(18) + b'\x0b\x02'


class OpticalEvidenceTest(unittest.TestCase):
    def setUp(self):
        temp = TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name) / 'index'
        self.inf = self.root / INF_PATH
        self.driver = self.root / SYS_PATH
        self.inf.parent.mkdir(parents=True)
        self.driver.parent.mkdir(parents=True)
        self.inf.write_text(INF, encoding='utf-16')
        self.driver.write_bytes(pe_header())
        self.inventory = Path(temp.name) / 'index-paths.txt'
        self.inventory.write_text(f'{INF_PATH}\n{SYS_PATH}\n', encoding='utf-8')

    def snapshot(self):
        return report(self.root, self.inventory)

    def assert_not_optical_candidate(self):
        self.assertIn(self.snapshot()['optical']['status'], ('no-match', 'inconclusive'))

    def assert_ownership_refused(self):
        # An invalid inventory may reject the whole input before a report exists.
        try:
            result = self.snapshot()
        except (FileNotFoundError, ValueError):
            return
        for child in ('optical', 'disk'):
            self.assertIn(result[child]['status'], ('no-match', 'inconclusive'))

    def test_optical_chain_and_disk_control(self):
        result = self.snapshot()
        for key, expected in [('optical', (5, 'GenCdRom', 'candidate')),
                              ('disk', (0, 'GenDisk', 'no-match'))]:
            child = result[key]
            self.assertEqual((child['type'], child['compatible_id'], child['status']), expected)
        evidence = result['optical']['evidence']
        self.assertEqual(evidence['inf_sha256'], sha256(self.inf.read_bytes()).hexdigest())
        self.assertEqual(evidence['binary_sha256'], sha256(pe_header()).hexdigest())
        for field, value in {'service': 'cdrom', 'binary': SYS_PATH, 'models': 'cdrom_device.NTamd64',
                             'id': 'GenCdRom', 'install_section': 'cdrom_install',
                             'services_section': 'cdrom_install.Services',
                             'service_section': 'cdrom_ServiceInstallSection'}.items():
            self.assertEqual(evidence[field].casefold(), value.casefold(), field)

    def test_controller_class_or_guid_cannot_claim_optical(self):
        for old, new in [('Class=CDROM', 'Class=SCSIAdapter'),
                         (GUID, '{4D36E97B-E325-11CE-BFC1-08002BE10318}')]:
            with self.subTest(replacement=new):
                self.inf.write_text(INF.replace(old, new), encoding='utf-16')
                self.assert_not_optical_candidate()

    def test_only_selected_exact_model_id_counts(self):
        contents = INF.replace('GenCdRom', 'GenCdRomExtra').replace('GenDisk', 'GenCdRom')
        self.inf.write_text(contents, encoding='utf-16')
        self.assert_not_optical_candidate()

    def test_invalid_or_wrong_machine_binary(self):
        for binary in (pe_header()[:64], pe_header(machine=0x14c)):
            with self.subTest(header=binary[64:70]):
                self.driver.write_bytes(binary)
                self.assert_not_optical_candidate()

    def test_unassociated_or_disabled_service(self):
        for old, new in [('AddService=cdrom,2', 'AddService=cdrom,0'), ('StartType=1', 'StartType=4')]:
            with self.subTest(replacement=new):
                self.inf.write_text(INF.replace(old, new), encoding='utf-16')
                self.assert_not_optical_candidate()

    def test_selected_disk_row_prevents_clean_negative(self):
        self.inf.write_text(INF.replace('[cdrom_device.NTx86]',
                                        'Disk=cdrom_install,GenDisk\n[cdrom_device.NTx86]'),
                            encoding='utf-16')
        self.assertIn(self.snapshot()['disk']['status'], ('candidate', 'inconclusive'))

    def test_missing_inventory(self):
        self.inventory.unlink()
        self.assert_ownership_refused()

    def test_incomplete_inventory(self):
        self.inventory.write_text(INF_PATH + '\n', encoding='utf-8')
        self.assert_ownership_refused()

    def test_same_count_inventory_substitution(self):
        self.inventory.write_text(f'{INF_PATH}\nWindows/System32/drivers/other.sys\n', encoding='utf-8')
        self.assert_ownership_refused()


if __name__ == '__main__':
    unittest.main()
