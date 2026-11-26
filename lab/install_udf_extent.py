"""Map install.esd bytes [65520, 65584) on the #188 optical image, read-only."""
import argparse
import io
import json
import os
import struct
from importlib.metadata import PackageNotFoundError, version


BLOCK = 2048
MEDIA_SIZE = 3839819776
UDF_PATH = '/sources/install.esd'
START, END = 65520, 65584


class MetadataReader(io.FileIO):
    def read(self, size=-1):
        # pycdlib walks directory metadata while opening the image. Never let
        # an unsupported directory/descriptor length turn that into a GiB read.
        if not 0 <= size <= 4 * 1024 * 1024:
            raise ValueError('unsupported metadata read length')
        return super().read(size)


def short_extents(source, entry, partition, icb):
    """Read absolute byte positions and lengths; ECMA-167 IV/14.9, 14.14.1."""
    base = partition.part_start_location
    if (icb.part_ref_num != 0 or icb.extent_length != BLOCK or
            icb.log_block_num >= partition.part_length or
            base + icb.log_block_num != entry.orig_extent_loc):
        raise ValueError('unsupported file entry reference')
    source.seek(entry.orig_extent_loc * BLOCK)
    fe = source.read(BLOCK)
    if (len(fe) != BLOCK or struct.unpack_from('<H', fe)[0] != 261 or
            entry.icb_tag.strategy_type != 4 or entry.icb_tag.flags & 7):
        raise ValueError('expected a direct file entry with short allocation descriptors')
    attrs, ad_bytes = struct.unpack_from('<II', fe, 168)
    start = 176 + attrs
    if not ad_bytes or ad_bytes % 8 or start + ad_bytes > BLOCK:
        raise ValueError('invalid inline short descriptor table')
    extents = []
    total = 0
    for offset in range(start, start + ad_bytes, 8):
        # 1.22.0's UDFShortAD.parse masks off the type before exposing it.
        # Check the on-disk high bits: only recorded and allocated (00) maps.
        length, lbn = struct.unpack_from('<II', fe, offset)
        if not 0 < length < 1 << 30 or total % BLOCK:
            raise ValueError('hole, continuation, or unsupported extent layout')
        if lbn * BLOCK + length > partition.part_length * BLOCK:
            raise ValueError('data extent outside physical partition')
        total += length
        # As in PyCdlib.get_file_byte_extents(udf_path=...): short-AD LBNs
        # are partition-relative; the partition start is an absolute media LBA.
        extents.append(((base + lbn) * BLOCK, length))
    if total != entry.info_len:
        raise ValueError('extent lengths do not cover exactly the information length')
    return extents


def mapping(source, iso, udf):
    if iso.logical_block_size != BLOCK or iso.udf_root is None:
        raise ValueError('expected UDF with 2048-byte logical blocks')
    descs = iso.udf_main_descs
    if len(descs.partitions) != 1 or len(descs.logical_volumes) != 1:
        raise ValueError('expected one physical partition and logical volume')
    partition = descs.partitions[0]
    volume = descs.logical_volumes[0]
    maps = volume.partition_maps
    fsd = volume.logical_volume_contents_use
    if (len(maps) != 1 or not isinstance(maps[0], udf.UDFType1PartitionMap) or
            maps[0].vol_seqnum != 1 or maps[0].part_num != partition.part_num or
            fsd.part_ref_num != 0 or fsd.log_block_num != 0 or
            (partition.part_start_location + partition.part_length) * BLOCK > MEDIA_SIZE):
        raise ValueError('unsupported partition map or file set location')

    target = iso.get_record(udf_path=UDF_PATH)
    if target.icb_tag.file_type != 5 or target.info_len < END:
        raise ValueError('expected a regular file containing the selected range')
    # Check the directory path as well: the library resolves UDF locations
    # against partition zero and does not expose the original short-AD type.
    short_extents(source, iso.udf_root, partition, iso.udf_file_set.root_dir_icb)
    short_extents(source, target.parent, partition, target.parent.file_ident.icb)
    extents = short_extents(source, target, partition, target.file_ident.icb)
    return {'file_size': target.info_len, 'first_lba': range_lba(extents)}


def range_lba(extents):
    logical = 0
    first = None
    for absolute, length in extents:
        lo, hi = max(START, logical), min(END, logical + length)
        if lo < hi:
            position = absolute + lo - logical
            if first is None:
                first = position
            elif position != first + lo - START:
                raise ValueError('selected bytes are not physically contiguous')
        logical += length
    if logical < END or first is None or first % BLOCK != START % BLOCK:
        raise ValueError('selected range is not sector-mappable')
    return first // BLOCK


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image')
    args = parser.parse_args()
    try:
        if version('pycdlib') != '1.22.0':
            parser.error('requires pycdlib==1.22.0')
    except PackageNotFoundError:
        parser.error('requires pycdlib==1.22.0')
    import pycdlib
    from pycdlib import udf
    from pycdlib.pycdlibexception import PyCdlibException

    try:
        with MetadataReader(args.image, 'rb') as source:
            if os.fstat(source.fileno()).st_size != MEDIA_SIZE:
                raise ValueError('unexpected optical image size')
            iso = pycdlib.PyCdlib()
            iso.open_fp(source)
            try:
                result = mapping(source, iso, udf)
            finally:
                iso.close()
    except (OSError, ValueError, struct.error, PyCdlibException) as err:
        parser.exit(1, f'{parser.prog}: {err}\n')
    print(json.dumps(result))


if __name__ == '__main__':
    main()
