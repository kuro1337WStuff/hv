"""Independent raw optical range checks; no ISO or compiler is required."""
import hashlib
import io
import unittest

from lab.install_content_witness import verify


class InstallContentWitnessTest(unittest.TestCase):
    def setUp(self):
        # Two sectors precede the file. The marker spans its sectors 31/32.
        self.image = bytearray(40 * 2048)
        marker = bytes(range(64))
        self.start = 2 * 2048 + 65520
        self.image[self.start:self.start + 64] = marker
        self.pin = dict(media_size=len(self.image), file_size=35 * 2048,
                        file_offset=65520, length=64, first_lba=33,
                        expected_sha256=hashlib.sha256(marker).hexdigest())

    def check(self, **changes):
        return verify(io.BytesIO(self.image), **(self.pin | changes))

    def test_straddles_sector_boundary(self):
        self.assertEqual(self.check(), self.pin['expected_sha256'])

    def test_range_does_not_imply_an_unbroken_whole_file_extent(self):
        marker = self.image[self.start:self.start + 64]
        self.image[2032:2096] = marker  # a later logical file extent placed near the front
        self.assertEqual(self.check(first_lba=0), self.pin['expected_sha256'])

    def test_one_block_shift(self):
        with self.assertRaisesRegex(ValueError, 'SHA-256 mismatch'):
            self.check(first_lba=34)

    def test_same_size_inner_corruption(self):
        self.image[self.start + 17] ^= 1
        with self.assertRaisesRegex(ValueError, 'SHA-256 mismatch'):
            self.check()

    def test_truncated_media_even_after_range(self):
        del self.image[-1]
        with self.assertRaisesRegex(ValueError, 'image size'):
            self.check()

    def test_short_read_after_size_measurement(self):
        class Truncated(io.BytesIO):
            def read(self, size):
                if self.tell() >= start + 16:
                    return b''
                return super().read(min(size, 16))

        start = self.start
        with self.assertRaisesRegex(ValueError, 'truncated'):
            verify(Truncated(self.image), **self.pin)

    def test_invalid_evidence(self):
        for changes in (dict(media_size=0), dict(media_size=len(self.image) - 1),
                        dict(file_size=0), dict(file_size=65583), dict(length=0), dict(length=4097),
                        dict(length=-1), dict(file_offset=-1), dict(file_offset=None),
                        dict(first_lba=-1), dict(first_lba=39),
                        dict(expected_sha256=''), dict(expected_sha256='z' * 64)):
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                self.check(**changes)


if __name__ == '__main__':
    unittest.main()
