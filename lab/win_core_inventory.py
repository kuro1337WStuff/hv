"""Inventory OS-visible processor cores on the local x64 Windows host (read-only)."""
import ctypes
import json
import platform
import struct
import sys
import sysconfig


RELATION_PROCESSOR_CORE = 0
ALL_PROCESSOR_GROUPS = 0xffff
ERROR_INSUFFICIENT_BUFFER = 122


def core_members(data):
    """Return sorted (group, processor-number) sibling sets, not APIC IDs."""
    offset = 0
    cores = []
    occupied = {}
    while offset < len(data):
        remaining = len(data) - offset
        if remaining < 8:
            raise ValueError(f'truncated record header at byte {offset}')
        kind, size = struct.unpack_from('<II', data, offset)
        if kind != RELATION_PROCESSOR_CORE:
            raise ValueError(f'unexpected relationship {kind} at byte {offset}')
        # 8-byte EX header + 24-byte PROCESSOR_RELATIONSHIP prefix +
        # 16-byte x64 GROUP_AFFINITY (64-bit KAFFINITY, WORD group, 3 WORDs).
        if size < 48 or size > remaining:
            raise ValueError(f'invalid record size {size} at byte {offset}')
        count, = struct.unpack_from('<H', data, offset + 30)
        if count != 1:  # A core, unlike a package, belongs to exactly one group.
            raise ValueError(f'core at byte {offset} has GroupCount {count}, expected 1')
        mask, group = struct.unpack_from('<QH', data, offset + 32)
        if not mask or group == ALL_PROCESSOR_GROUPS:
            raise ValueError(f'invalid group affinity at byte {offset}')
        overlap = occupied.get(group, 0) & mask
        if overlap:
            raise ValueError(f'duplicate logical processor in group {group}, bits {overlap:#x}')
        members = tuple((group, n) for n in range(64) if mask & (1 << n))
        if bool(data[offset + 8] & 1) != (len(members) > 1):
            raise ValueError(f'SMT flag disagrees with affinity at byte {offset}')
        occupied[group] = occupied.get(group, 0) | mask
        cores.append(members)
        offset += size  # The API returns variable-size records, not a C array.
    if not cores:
        raise ValueError('missing processor core records')
    return sorted(cores)


def inventory(data, active, model):
    cores = core_members(data)
    threads = sum(map(len, cores))
    if active <= 0:
        raise ValueError('independent active processor count must be positive')
    rows = []
    for core in cores:
        ids = [f'{group}:{number}' for group, number in core]
        rows.append({'core_id': 'core-' + ids[0], 'logical_processors': ids})
    return {
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


def collect():
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

    length = ctypes.c_uint32()
    buffer = None
    capacity = 0
    while not query(RELATION_PROCESSOR_CORE, buffer, ctypes.byref(length)):
        error = ctypes.get_last_error()
        if error != ERROR_INSUFFICIENT_BUFFER:
            raise ctypes.WinError(error)
        if length.value <= capacity:
            raise ValueError('insufficient-buffer reply without a larger required size')
        capacity = length.value
        buffer = ctypes.create_string_buffer(capacity)
    if buffer is None or length.value > capacity:
        raise ValueError('invalid successful topology query length')
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
    return inventory(buffer[:length.value], active, model)


def main():
    try:
        report = collect()
    except (OSError, ValueError) as error:
        print(f'Inventory failed: {error}', file=sys.stderr)
        return 1
    print(json.dumps(report, indent=2))
    return 0 if report['counts_agree'] else 1


if __name__ == '__main__':
    sys.exit(main())
