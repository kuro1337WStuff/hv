"""Synthetic extracted-index fixtures only. No WIM, host INF, or SetupAPI access."""
from pathlib import Path
from tempfile import TemporaryDirectory
import unittest

from lab.winpe_storage import scan


CONTROL = r'''[Version]
Signature="$Windows NT$"
[Manufacturer]
Maker=Controllers,NTamd64...0x01,NTamd64...0x03,NTx86
[Controllers.NTamd64...0x01]
PublishOnly=Deploy
AHCI=AHCI_Install,PCI\CC_010601
[Controllers.NTamd64...0x03]
Server=AHCI_Install,PCI\VEN_1AF4&DEV_1048
[Controllers.NTx86]
x86=AHCI_Install,PCI\VEN_1AF4&DEV_1048
[NotModels]
Other=AHCI_Install,PCI\VEN_1AF4&DEV_1048
[AHCI_Install.NT]
ExcludeID=PCI\VEN_8086&DEV_2653&SUBSYS_0F001179
[AHCI_Install.NT.Services]
AddService=filter,,Unrelated
AddService=storahci,%Assoc%,StorageService
[StorageService]
ServiceType=1
StartType=0
ServiceBinary=%12%\storahci.sys
[Strings]
Assoc=0x2
'''
VIO = r'''[Version]
Signature="$Windows NT$"
[Manufacturer]
Maker=Models,NTamd64,NTamd64.10.0...19041,NTamd64.10.0...22000
[Models.NTamd64]
Old=Unrelated,PCI\VEN_1AF4&DEV_1048
[Models.NTamd64.10.0...22000]
New=Unrelated,PCI\VEN_1AF4&DEV_1048
[Models.NTamd64.10.0...19041]
Modern=Install,PCI\VEN_1AF4&DEV_1048&SUBSYS_00000000, \
  %ModernID%
[Install]
[Install.Services]
AddService="vio",2,Miniport
[Miniport]
ServiceBinary=%12%\vio.sys
StartType=3
ServiceType=1
[Strings]
ModernID="PCI\VEN_1AF4&DEV_1048" ; not text search
'''


def header(machine=0x8664):
    # Only the DOS offset, PE signature, machine and optional-header magic are probed.
    return b'MZ' + bytes(58) + (64).to_bytes(4, 'little') + b'PE\0\0' + machine.to_bytes(2, 'little') + bytes(18) + b'\x0b\x02'


class StorageEvidenceTest(unittest.TestCase):
    def setUp(self):
        tmp = TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)
        self.infs = self.root / 'Windows/INF'
        self.sys = self.root / 'Windows/System32/drivers'
        self.infs.mkdir(parents=True)
        self.sys.mkdir(parents=True)
        self.manifest = self.root / 'index-paths.txt'
        (self.infs / 'mshdc.inf').write_text(CONTROL, encoding='utf-16')
        (self.sys / 'storahci.sys').write_bytes(header())
        self.manifest.write_text('Windows/Inf/mshdc.inf\nWindows/System32/drivers/storahci.sys\n', encoding='utf-8')

    def scan(self):
        return scan(self.root, self.manifest, 19041, 1)

    def publish(self, text, with_binary=True):
        (self.infs / 'oem1.inf').write_text(text, encoding='utf-8-sig')
        with self.manifest.open('a', encoding='utf-8') as f:
            f.write(r'Windows\INF\oem1.inf' + '\n')
            if with_binary:
                (self.sys / 'vio.sys').write_bytes(header())
                f.write('Windows/System32/drivers/vio.sys\n')

    def test_control_not_nonmodel_or_server_or_x86_or_driverstore(self):
        package = self.root / 'Windows/System32/DriverStore/FileRepository/stale'
        package.mkdir(parents=True)
        (package / 'vioscsi.inf').write_text(VIO)
        (package / 'vio.sys').write_bytes(header())
        report = self.scan()
        self.assertEqual(report['modern_device'], 'no-exact-device-ID-in-reviewed-models')
        self.assertTrue(report['inbox_storahci_control'])
        self.assertEqual(report['counts']['published_inf'], 1)
        self.assertEqual(report['counts']['reviewed_model_entries'], 1)
        self.assertEqual(report['literal_mentions_not_models']['VEN_1AF4'], ['mshdc.inf'])
        row, = report['evidence']
        self.assertEqual(row['excluded_ids'], [r'PCI\VEN_8086&DEV_2653&SUBSYS_0F001179'])
        self.assertEqual((row['models'], row['services_section'], row['service'], row['binary']),
                         ('controllers.ntamd64...0x01', 'ahci_install.nt.services', 'storahci',
                          'Windows/System32/drivers/storahci.sys'))

    def test_excluded_compatible_id_is_not_a_positive(self):
        contents = CONTROL.replace(r'PCI\VEN_8086&DEV_2653&SUBSYS_0F001179', r'PCI\CC_010601')
        (self.infs / 'mshdc.inf').write_text(contents, encoding='utf-16')
        report = self.scan()
        self.assertFalse(report['inbox_storahci_control'])
        self.assertIn('explicitly excluded', report['evidence'][0]['chain_gap'])

    def test_modern_id_in_second_model_field_of_selected_build(self):
        self.publish(VIO)
        report = self.scan()
        self.assertEqual(report['model_scope_gaps'], {})
        self.assertEqual(report['modern_device'], 'local-model-service-binary-chain')
        row, = [r for r in report['evidence'] if r['role'] == 'modern-device']
        self.assertEqual((row['models'], row['id_field'], row['service_section'], row['servicebinary']),
                         ('models.ntamd64.10.0...19041', 2, 'Miniport', r'%12%\vio.sys'))

    def test_transport_and_generic_ids_are_separate(self):
        ids = (r'PCI\VEN_1AF4&DEV_1004,PCI\VEN_1AF4&DEV_1042,PCI\VEN_1AF4,'
               r'PCI\CC_0100,PCI\VEN_1AF4&DEV_104*,PCI\VEN_DEAD&DEV_1048')
        self.publish(VIO.replace('%ModernID%', ids))
        report = self.scan()
        self.assertEqual(report['modern_device'], 'no-exact-device-ID-in-reviewed-models')
        self.assertEqual({r['role'] for r in report['evidence'] if r['inf'] == 'oem1.inf'},
                         {'requires-unconfirmed-pci-fields', 'transitional-scsi-not-modern',
                          'other-virtio-vendor-model', 'vendor-only-compatible',
                          'conditional-scsi-class', 'wildcard-not-an-exact-ID'})

    def test_partial_chain_is_not_a_positive(self):
        self.publish(VIO, with_binary=False)
        for text, reason in ((VIO, 'not in image inventory'),
                             (VIO.replace('"vio",2', '"vio",0'), 'associated AddService'),
                             (VIO.replace('[Install.Services]', '[Wrong.Services]'), 'missing DDInstall.Services'),
                             (VIO.replace('%12%', '%13%'), 'DIRID 12'),
                             (VIO.replace('AddService=', 'Needs=External\nAddService='), 'unsupported Services')):
            with self.subTest(reason=reason):
                (self.infs / 'oem1.inf').write_text(text, encoding='utf-16')
                report = self.scan()
                self.assertEqual(report['modern_device'], 'model-without-confirmed-local-chain')
                row, = [r for r in report['evidence'] if r['role'] == 'modern-device']
                self.assertIn(reason, row['chain_gap'])
                self.assertFalse(row['local_chain'])

    def test_wrong_machine_not_an_inbox_control(self):
        (self.sys / 'storahci.sys').write_bytes(header(0x014c))
        report = self.scan()
        self.assertFalse(report['inbox_storahci_control'])
        self.assertEqual(report['modern_device'], 'inconclusive')

    def test_uncertain_decoration_not_a_model_absence(self):
        self.publish(VIO.replace('NTamd64.10.0...19041,', 'NTamd64.10.0..0x80.19041,'))
        report = self.scan()
        self.assertTrue(report['inbox_storahci_control'])
        self.assertIn('suite mask', report['model_scope_gaps']['oem1.inf'])
        self.assertEqual(report['modern_device'], 'inconclusive')

    def test_incomplete_inventory_same_count_substitution(self):
        (self.infs / 'mshdc.inf').rename(self.infs / 'stale.inf')
        with self.assertRaisesRegex(ValueError, 'inventory mismatch'):
            self.scan()

    def test_missing_authority(self):
        self.manifest.unlink()
        with self.assertRaises(FileNotFoundError):
            self.scan()


if __name__ == '__main__':
    unittest.main()
