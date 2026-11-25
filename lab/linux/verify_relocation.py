"""Compare one reconstructed relocation page with a fixed artifact witness. No VM access."""
import argparse
import hashlib
from pathlib import Path


IMAGE_BYTES = 0xc10400
IMAGE_HASH = '1e8ac7aebfab3f0fcb443eae94ad320efbbda51be91b928c32ca1740b44ebe9d'
PAGE_HASH = '541da574ff7a2e05dd9e4c84a50b33989e6d652bf1d493d2fe3a13e4f12bd4ba'
PAGE = 4096
COPY_BASE = 0x29c3000
ENTRY = 0x35c1000
RAM_END = 0x8000000


def reconstruct(image):
    """Return the 4 KiB relocated entry page reconstructed from pinned bzImage bytes.

    Raise ValueError on an unknown image or copy window. Does not emulate earlier
    guest writes or grant execute permission to the resulting bytes.
    """
    if len(image) != IMAGE_BYTES or hashlib.sha256(image).hexdigest() != IMAGE_HASH:
        raise ValueError('unreviewed image')
    setup = (image[0x1f1] + 1) * 512
    copied = int.from_bytes(image[0x1f4:0x1f8], 'little') * 16
    offset = ENTRY - COPY_BASE
    first = setup + offset
    if (setup + copied != len(image) or offset + PAGE > copied or
            (first, first + PAGE) != (0xc02000, 0xc03000)):
        raise ValueError('unexpected copy window')
    # Slice of the pinned STD; REP MOVSQ, not an emulation of earlier guest writes.
    dest = bytearray(PAGE)
    for i in range(PAGE - 8, -1, -8):
        dest[i:i + 8] = image[first + i:first + i + 8]
    return dest


def compare(page, gpa=ENTRY, backing=('guest', ENTRY), writable_backings=()):
    """Check one candidate page against the fixed guest-physical destination and digest.

    Backing IDs and writable aliases are supplied by the offline model, not read
    from a live NPT. Raise ValueError on a mismatch; never change page permissions.
    """
    if gpa % PAGE:
        raise ValueError('unaligned GPA')
    if not 0 <= gpa <= RAM_END - PAGE:
        raise ValueError('outside guest RAM')
    if gpa != ENTRY:
        raise ValueError('wrong destination')
    if backing != ('guest', ENTRY):
        raise ValueError('not the designated guest backing')
    if backing in writable_backings:
        raise ValueError('candidate backing has writable mapping')
    if len(page) != PAGE or hashlib.sha256(page).hexdigest() != PAGE_HASH:
        raise ValueError('destination page differs')


def require_refusal(label, reason, fn, *args, **kwargs):
    """Require the named ValueError; a different refusal does not verify this case."""
    try:
        fn(*args, **kwargs)
    except ValueError as exc:
        if str(exc) != reason:
            raise
        print(f'{label}: refused ({reason})')
        return
    raise ValueError(f'{label}: missing refusal')


def witness(image):
    """Run the offline controls; consumes a mutable image buffer for the final corruption test."""
    page = reconstruct(image)
    compare(page)
    print(f'file [0xc02000,0xc03000) -> GPA {ENTRY:#x}: {PAGE} bytes SHA256 {PAGE_HASH}')
    for index in (0, PAGE - 1):
        page[index] ^= 1
        require_refusal(f'changed byte +{index:#x}', 'destination page differs', compare, page)
        page[index] ^= 1
    require_refusal('adjacent page', 'wrong destination', compare, page, ENTRY + PAGE)
    require_refusal('interior address', 'unaligned GPA', compare, page, ENTRY + 1)
    require_refusal('RAM end', 'outside guest RAM', compare, page, RAM_END)
    require_refusal('private frame', 'not the designated guest backing', compare, page,
                    backing=('private', ENTRY))
    require_refusal('writable alias', 'candidate backing has writable mapping', compare, page,
                    writable_backings=(('guest', ENTRY),))
    # Same length; damage outside the window so a page-only source check would pass.
    image[0] ^= 1
    require_refusal('changed image', 'unreviewed image', reconstruct, image)
    print('Offline page witness only; Linux remains NX.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('vmlinuz', type=Path)
    args = parser.parse_args()
    try:
        with args.vmlinuz.open('rb') as source:
            image = bytearray(source.read(IMAGE_BYTES + 1))
        witness(image)
    except (OSError, ValueError) as err:
        parser.exit(1, f'verify_relocation: {err}\n')
