"""Inventory OS-visible processor cores on the local x64 Windows host (read-only)."""
import argparse
import ctypes
import json
import platform
import struct
import sys
import sysconfig


RELATION_PROCESSOR_CORE = 0
RELATION_CACHE = 2
CACHE_UNIFIED = 0
ALL_PROCESSOR_GROUPS = 0xffff
ERROR_INSUFFICIENT_BUFFER = 122


def _records(data, relationship, minimum):
    """Yield bounded x64 EX records using their declared Size, including extensions."""
    offset = 0
    while offset < len(data):
        remaining = len(data) - offset
        if remaining < 8:
            raise ValueError(f'truncated record header at byte {offset}')
        kind, size = struct.unpack_from('<II', data, offset)
        if kind != relationship:
            raise ValueError(f'unexpected relationship {kind} at byte {offset}')
        if size < minimum or size > remaining:
            raise ValueError(f'invalid record size {size} at byte {offset}')
        yield offset, size
        offset += size


def _affinity_members(data, offset):
    mask, group = struct.unpack_from('<QH', data, offset)
    if not mask or group == ALL_PROCESSOR_GROUPS:
        raise ValueError(f'invalid group affinity at byte {offset}')
    return tuple((group, n) for n in range(64) if mask & (1 << n))


def core_members(data):
    """Return sorted (group, processor-number) sibling sets, not APIC IDs."""
    cores = []
    occupied = set()
    # 8-byte EX header + 24-byte PROCESSOR_RELATIONSHIP + 16-byte GROUP_AFFINITY.
    for offset, _ in _records(data, RELATION_PROCESSOR_CORE, 48):
        count, = struct.unpack_from('<H', data, offset + 30)
        if count != 1:  # A core, unlike a package, belongs to exactly one group.
            raise ValueError(f'core at byte {offset} has GroupCount {count}, expected 1')
        members = _affinity_members(data, offset + 32)
        overlap = occupied.intersection(members)
        if overlap:
            raise ValueError(f'duplicate logical processors {sorted(overlap)}')
        if bool(data[offset + 8] & 1) != (len(members) > 1):
            raise ValueError(f'SMT flag disagrees with affinity at byte {offset}')
        occupied.update(members)
        cores.append(members)
    if not cores:
        raise ValueError('missing processor core records')
    return sorted(cores)


def _unified_l3_records(data):
    caches = []
    # CACHE_RELATIONSHIP starts at EX byte 8; GroupCount at 38, affinity at 40.
    for offset, size in _records(data, RELATION_CACHE, 56):
        level, _, _, capacity, kind = struct.unpack_from('<BBHII', data, offset + 8)
        count, = struct.unpack_from('<H', data, offset + 38)
        count = count or 1  # Windows 10: reserved WORD, a single GroupMask.
        if 40 + 16 * count > size:
            raise ValueError(f'cache GroupCount {count} exceeds record size at byte {offset}')
        members = []
        groups = set()
        for i in range(count):
            affinity = _affinity_members(data, offset + 40 + i * 16)
            group = affinity[0][0]
            if group in groups:
                raise ValueError(f'duplicate group {group} in cache at byte {offset}')
            groups.add(group)
            members.extend(affinity)
        if level == 3 and kind == CACHE_UNIFIED:
            if not capacity:
                raise ValueError(f'zero unified L3 cache size at byte {offset}')
            caches.append((sorted(members), capacity))
    return sorted(caches)


def _lp_id(lp):
    return f'{lp[0]}:{lp[1]}'


def _core_id(first_lp):
    return 'core-' + _lp_id(first_lp)


def _l3_inventory(data, cores):
    core_for = {lp: core[0] for core in cores for lp in core}
    sharing = {}
    rows = []
    for index, (members, capacity) in enumerate(_unified_l3_records(data)):
        touched = sorted({core_for[lp] for lp in members if lp in core_for})
        rows.append({'size_bytes': capacity, 'logical_processors': list(map(_lp_id, members)),
                     'intersecting_core_ids': list(map(_core_id, touched))})
        for lp in members:
            sharing.setdefault(lp, set()).add(index)

    missing = sorted(core_for.keys() - sharing.keys())
    unknown = sorted(sharing.keys() - core_for.keys())
    overlap = sorted(lp for lp, indices in sharing.items() if len(indices) > 1)
    split = []
    for core in cores:
        assignments = [sharing.get(lp, set()) for lp in core]
        if len(set.union(*assignments)) > 1 and any(a != assignments[0] for a in assignments):
            split.append(_core_id(core[0]))
    if not rows:
        status = 'absent'
    elif missing or unknown or overlap or split:
        status = 'inconsistent'
    else:
        status = 'exact'
    return {'coverage': status, 'caches': rows, 'missing_lps': list(map(_lp_id, missing)),
            'unknown_lps': list(map(_lp_id, unknown)), 'overlapping_lps': list(map(_lp_id, overlap)),
            'split_sibling_core_ids': split}


def inventory(data, active, model, *, cache_data=None):
    cores = core_members(data)
    threads = sum(map(len, cores))
    if active <= 0:
        raise ValueError('independent active processor count must be positive')
    rows = []
    for core in cores:
        rows.append({'core_id': _core_id(core[0]), 'logical_processors': list(map(_lp_id, core))})
    report = {
        'scope': 'Local Windows-visible topology only; not board or all-core launch qualification.',
        'cpu_model_registry': model,
        'os_visible': {'physical_cores': len(cores), 'logical_processors': threads,
                       'cores_with_smt_siblings': sum(len(c) > 1 for c in cores), 'cores': rows},
        'independent_active_processors': active,
        'counts_agree': threads == active,
        'target': {'skus': ['Ryzen 9 9950X', 'Ryzen 9 9950X3D'],
                   'physical_cores': 16, 'logical_processors': 32, 'threads_per_core': 2,
                   'observed_shape_matches': len(cores) == 16 and all(len(c) == 2 for c in cores)},
    }
    if cache_data is not None:
        l3 = report['unified_l3'] = _l3_inventory(cache_data, cores)
        report['target']['core_shape_with_exact_l3_coverage'] = (
            report['target']['observed_shape_matches'] and report['counts_agree'] and l3['coverage'] == 'exact')
    return report


def _query_records(query, relationship):
    length = ctypes.c_uint32()
    buffer = None
    capacity = 0
    while not query(relationship, buffer, ctypes.byref(length)):
        error = ctypes.get_last_error()
        if error != ERROR_INSUFFICIENT_BUFFER:
            raise ctypes.WinError(error)
        if length.value <= capacity:
            raise ValueError('insufficient-buffer reply without a larger required size')
        capacity = length.value
        buffer = ctypes.create_string_buffer(capacity)
    if buffer is None or not 0 < length.value <= capacity:
        raise ValueError('invalid successful topology query length')
    return buffer[:length.value]


def collect(*, include_l3=False):
    if (sys.platform != 'win32' or sysconfig.get_platform() != 'win-amd64'
            or platform.machine().lower() not in ('amd64', 'x86_64') or ctypes.sizeof(ctypes.c_void_p) != 8):
        raise ValueError('requires x64 Python on a local x64 Windows host')
    import winreg

    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    query = kernel.GetLogicalProcessorInformationEx
    query.argtypes = [ctypes.c_int, ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint32)]
    query.restype = ctypes.c_int
    count = kernel.GetActiveProcessorCount
    count.argtypes = [ctypes.c_uint16]
    count.restype = ctypes.c_uint32

    cores = _query_records(query, RELATION_PROCESSOR_CORE)
    caches = _query_records(query, RELATION_CACHE) if include_l3 else None
    active = count(ALL_PROCESSOR_GROUPS)
    if not active:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE,
                            r'HARDWARE\DESCRIPTION\System\CentralProcessor\0') as key:
            name, kind = winreg.QueryValueEx(key, 'ProcessorNameString')
            model = (name.strip() or None) if kind == winreg.REG_SZ else None
    except OSError:
        model = None  # Missing descriptive metadata does not invalidate topology.
    report = inventory(cores, active, model, cache_data=caches)
    if include_l3:
        # platform_version can describe kernel32.dll (26100 even on OS build 26200).
        release, version, _, _ = platform.win32_ver()
        report['windows'] = {'release': release, 'version': version}
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--l3', action='store_true', help='also map unified L3 cache sharing to cores')
    args = parser.parse_args()
    try:
        report = collect(include_l3=args.l3)
    except (OSError, ValueError) as error:
        print(f'Inventory failed: {error}', file=sys.stderr)
        return 1
    print(json.dumps(report, indent=2))
    return 0 if report['counts_agree'] and (not args.l3 or report['unified_l3']['coverage'] == 'exact') else 1


if __name__ == '__main__':
    sys.exit(main())
