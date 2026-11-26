"""Read-only, scoped WinPE model -> service -> binary evidence (not a PnP simulator)."""
import argparse
from collections import Counter, defaultdict
import json
from pathlib import Path
import re
import stat
import sys


VENDOR = r'PCI\VEN_1AF4'
DEVICE = VENDOR + '&DEV_1048'
AHCI = r'PCI\CC_010601'
SCOPES = (('Windows/INF', '.inf'), ('Windows/System32/drivers', '.sys'))


def require(condition, message):
    if not condition:
        raise ValueError(message)


def regular(path, directory=False):
    info = path.lstat()
    check = stat.S_ISDIR if directory else stat.S_ISREG
    require(check(info.st_mode) and not getattr(info, 'st_file_attributes', 0) & 0x400,
            'expected plain directory/file (no links or reparse points): ' + path.name)
    return info.st_size


def bounded(path, limit):
    require(regular(path) <= limit, 'input too large: ' + path.name)
    with path.open('rb') as f:
        data = f.read(limit + 1)
    require(len(data) <= limit, 'input too large: ' + path.name)
    return data


def enumerated(root, inventory):
    """Compare sets, not just counts. The caller owns same-index archive provenance."""
    names = bounded(inventory, 4 << 20).decode('utf-8-sig').splitlines()
    expected = set()
    for line in names:
        path = line.replace('\\', '/').lower()
        require(any(path.rpartition('/')[0] == d.lower() and path.endswith(ext)
                    for d, ext in SCOPES), 'invalid inventory path; supply only the two flat scopes')
        require(path not in expected, 'duplicate inventory path: ' + path)
        expected.add(path)
    require(0 < len(expected) <= 8192, 'empty or oversize inventory')
    groups = []
    actual = set()
    for scope, ext in SCOPES:
        folder = root
        regular(folder, directory=True)
        for component in scope.split('/'):
            choices = [p for p in folder.iterdir() if p.name.lower() == component.lower()]
            require(len(choices) == 1, 'missing/ambiguous directory: ' + scope)
            folder = choices[0]
            regular(folder, directory=True)
        files = {}
        for p in folder.iterdir():
            if p.suffix.lower() == ext:
                regular(p)
                key = p.name.lower()
                require(key not in files, 'case collision: ' + key)
                files[key] = p
                actual.add(scope.lower() + '/' + key)
                require(len(actual) <= 8192, 'oversize extraction')
        require(files, 'empty scope: ' + scope)
        groups.append(files)
    require(actual == expected, f'inventory mismatch: missing={sorted(expected-actual)[:8]}, '
                                f'unexpected={sorted(actual-expected)[:8]}')
    return groups


def sections(text):
    """Discard comments outside quotes; join INF continuation lines before sectioning."""
    require('\x00' not in text, 'unsupported text encoding')
    result = defaultdict(list)
    current = None
    pending = ''
    for raw in text.split('\n'):
        require(len(raw) <= 65536, 'oversize INF line')
        body = re.match(r'(?:[^";\r\n]|"[^"\r\n]*")*', raw)
        line, tail = body.group().strip(), raw[body.end():].strip()
        require(not tail or tail.startswith(';'), 'unsupported quoting')
        if line.endswith('\\'):
            pending += line[:-1]
            require(len(pending) <= 65536, 'oversize continuation')
            continue
        line = pending + line
        pending = ''
        if not line:
            continue
        if line.startswith('['):
            require(re.fullmatch(r'\[[^\[\]]+\]', line), 'malformed section')
            current = line[1:-1].lower()
            result[current]  # Preserve intentionally empty sections.
        else:
            require(current is not None, 'line outside section')
            result[current].append(line)
    require(not pending, 'unterminated continuation')
    return result


def unquote(value):
    value = value.strip()
    if value.startswith('"') and value.endswith('"'):
        value = value[1:-1]
    require('"' not in value, 'unsupported quotation in field')
    return value


def pairs(lines):
    for line in lines:
        key, sep, value = line.partition('=')
        require(sep and key.strip(), 'expected key=value')
        yield key.strip().lower(), value.strip()


def token_reader(inf):
    localized = [s for s in inf if re.fullmatch(r'strings\.[0-9a-f]{4}', s)
                 and int(s[-4:], 16) % 1024 == 9]
    section = next((s for s in ('strings.0409', 'strings.0009') if s in localized), None)
    if section is None:
        require(len(localized) <= 1, 'ambiguous English Strings locale')
        section = localized[0] if localized else 'strings'
    strings = defaultdict(list)
    for key, value in pairs(inf.get(section, ())):
        strings[key].append(value)

    def expand(value):
        def sub(match):
            key = match[1].lower()
            if key.isdecimal():  # Preserve directory IDs, never substitute host directories.
                return match[0]
            require(len(strings[key]) == 1, 'missing/ambiguous Strings token: ' + key)
            result = unquote(strings[key][0])
            require('%' not in result, 'nested Strings token')
            return result
        return re.sub(r'%([^%]+)%', sub, unquote(value))
    return expand


def number(value):
    require(re.fullmatch(r'(?:0[xX][0-9a-fA-F]+|[0-9]+)', value), 'unsupported number: ' + value)
    return int(value, 16 if value.lower().startswith('0x') else 10)


def models(inf, build, product, expand):
    selected = set()
    for _, value in pairs(inf.get('manufacturer', ())):
        base, *targets = (expand(v).lower() for v in value.split(','))
        require(targets, 'undecorated Manufacturer outside AMD64 scope')
        require(len(targets) <= 64, 'oversize Manufacturer decoration list')
        choices = []
        for target in targets:
            arch, *parts = target.split('.')
            if arch in ('ntarm', 'ntarm64', 'ntx86', 'ntia64'):
                continue
            require(arch == 'ntamd64' and len(parts) <= 5, 'unsupported target: ' + target)
            major, minor, kind, suite, minimum = (parts + [''] * 5)[:5]
            version = number(major or '0'), number(minor or '0')
            require(not kind or number(kind) in (1, 2, 3), 'unsupported product: ' + target)
            if version > (10, 0) or (kind and number(kind) != product):
                continue
            if version == (10, 0) and number(minimum or '0') > build:
                continue
            require(not number(suite or '0'), 'unresolved suite mask: ' + target)
            require(not minimum or version == (10, 0), 'unsupported relative build: ' + target)
            choices.append(((version, bool(kind), number(minimum or '0')), base + '.' + target))
        if choices:
            # A newer version and a more-specific product constraint can compete.
            # Refuse incomparable candidates rather than guess Setup's tie-break.
            best = {s for k, s in choices if all(all(a >= b for a, b in zip(k, other))
                                                  for other, _ in choices)}
            require(len(best) == 1, 'ambiguous Manufacturer decoration')
            section = best.pop()
            require(section in inf, 'missing models section: ' + section)
            selected.add(section)
    return sorted(selected)


def role(device_id):
    if ('*' in device_id or '?' in device_id) and (VENDOR in device_id or 'CC_0100' in device_id):
        return 'wildcard-not-an-exact-ID'
    if device_id == DEVICE:
        return 'modern-device'
    if device_id == VENDOR:
        return 'vendor-only-compatible'
    if device_id == AHCI:
        return 'ahci-control-id'
    if device_id.startswith(DEVICE + '&') or device_id.startswith(VENDOR + '&CC_'):
        return 'requires-unconfirmed-pci-fields'
    if re.fullmatch(r'PCI\\CC_0100(?:[0-9A-F]{2})?(?:&DT_[0-9A-F]{4})?', device_id):
        return 'conditional-scsi-class'
    if device_id == VENDOR + '&DEV_1004' or device_id.startswith(VENDOR + '&DEV_1004&'):
        return 'transitional-scsi-not-modern'
    if device_id.startswith(VENDOR + '&'):
        return 'other-virtio-vendor-model'
    return None


def service_evidence(inf, base, expand, drivers, row):
    base = base.lower()
    signatures = [unquote(v).lower() for k, v in pairs(inf.get('version', ())) if k == 'signature']
    require(signatures in (['$windows nt$'], ['$chicago$']), 'missing/unsupported INF signature')
    require(not re.search(r'\.nt(?:amd64|x86|arm64|arm|ia64)?(?:\.|$)', base),
            'explicitly decorated DDInstall reference not resolved')
    install = next((base + suffix for suffix in ('.ntamd64', '.nt', '') if base + suffix in inf), None)
    require(install is not None, 'missing DDInstall')
    services = install + '.services'
    row.update(install_section=install, services_section=services)
    require(not any(line.partition('=')[0].strip().lower() == 'excludeid' for line in inf[install]),
            'unresolved DDInstall ExcludeID')
    require(services in inf, 'missing DDInstall.Services')
    assoc = []
    for key, value in pairs(inf[services]):
        require(key == 'addservice', 'unsupported Services directive: ' + key)
        fields = [expand(v) for v in value.split(',')]
        require(len(fields) >= 2, 'malformed AddService')
        if number(fields[1] or '0') & 2:
            require(len(fields) >= 3 and fields[0] and fields[2], 'incomplete associated AddService')
            assoc.append(fields[:3])
    require(len(assoc) == 1, 'expected exactly one associated AddService')
    name, flags, target = assoc[0]
    row.update(service=name, addservice_flags=flags, service_section=target)
    keys = defaultdict(list)
    for key, value in pairs(inf.get(target.lower(), ())):
        keys[key].append(value)
    for key in ('servicebinary', 'servicetype', 'starttype'):
        require(len(keys[key]) == 1, 'missing/ambiguous ' + key)
        row[key] = expand(keys[key][0])
    require(number(row['servicetype']) == 1, 'not a kernel driver service')
    require(number(row['starttype']) in (0, 1, 2, 3), 'disabled or unknown start type')
    binary = re.fullmatch(r'%12%\\([\w.-]+\.sys)', row['servicebinary'], re.I | re.ASCII)
    require(binary, 'ServiceBinary outside flat DIRID 12 scope')
    path = drivers.get(binary[1].lower())
    require(path, 'referenced binary not in image inventory')
    row['binary'] = SCOPES[1][0] + '/' + path.name
    size = regular(path)
    with path.open('rb') as f:
        dos = f.read(64)
        require(len(dos) == 64 and dos[:2] == b'MZ', 'not a DOS/PE header')
        offset = int.from_bytes(dos[60:64], 'little')
        require(64 <= offset <= size - 26, 'invalid PE offset')
        f.seek(offset)
        header = f.read(26)
    require(header[:6] == b'PE\0\0\x64\x86' and header[24:] == b'\x0b\x02',
            'not an AMD64 PE32+ header')
    row.update(binary_bytes=size, header='AMD64 PE32+', local_chain=True)


def inspect_inf(inf, build, product, drivers, filename):
    require(inf, 'no INF sections')
    expand = token_reader(inf)
    selected = models(inf, build, product, expand)
    rows = []
    count = 0
    for section in selected:
        for _, value in pairs(inf[section]):
            install, *ids = (expand(v) for v in value.split(','))
            require(install and ids and ids[0], 'invalid model entry: ' + section)
            count += 1
            for pos, device_id in enumerate(ids):
                kind = role(device_id.upper())
                if not kind:
                    continue
                row = dict(inf=filename, models=section, id=device_id, id_field=pos + 1,
                           role=kind, ddinstall=install, local_chain=False)
                if kind in ('modern-device', 'vendor-only-compatible', 'ahci-control-id'):
                    try:
                        service_evidence(inf, install, expand, drivers, row)
                    except ValueError as exc:
                        row['chain_gap'] = str(exc)
                rows.append(row)
                require(len(rows) <= 4096, 'too many evidence rows')
    return rows, len(selected), count


def scan(root, inventory, build, product):
    require(14310 <= build <= 99999 and product in (1, 2, 3), 'unsupported target OS')
    published, drivers = enumerated(root, inventory)
    require(sum(regular(p) for p in published.values()) <= 256 << 20, 'oversize INF collection')
    evidence, gaps = [], {}
    literals = {key: [] for key in ('VEN_1AF4', 'DEV_1048', 'CC_0100')}
    counts = Counter(published_inf=len(published), driver_sys=len(drivers), reviewed_inf=0,
                     selected_model_sections=0, reviewed_model_entries=0)
    for key, path in sorted(published.items()):
        try:
            raw = bounded(path, 8 << 20)
            codec = ('utf-16' if raw.startswith((b'\xff\xfe', b'\xfe\xff')) else
                     'utf-8-sig' if raw.startswith(b'\xef\xbb\xbf') else 'cp1252')
            text = raw.decode(codec)
            upper = text.upper()
            for needle, files in literals.items():
                if needle in upper:
                    files.append(path.name)
            rows, nsections, nentries = inspect_inf(sections(text), build, product, drivers, path.name)
            evidence.extend(rows)
            counts.update(reviewed_inf=1, selected_model_sections=nsections, reviewed_model_entries=nentries)
        except ValueError as exc:
            gaps[key] = str(exc)
        require(len(evidence) <= 8192, 'oversize evidence report')
    exact = [r for r in evidence if r['role'] == 'modern-device']
    control = any(r['role'] == 'ahci-control-id' and r['local_chain'] and
                  r['inf'].lower() == 'mshdc.inf' and r['service'].lower() == 'storahci' and
                  r['servicebinary'].lower() == r'%12%\storahci.sys' for r in evidence)
    return dict(target_os=f'en-US NTamd64 10.0.{build}, ProductType={product}', counts=counts,
                literal_mentions_not_models=literals, model_scope_gaps=gaps, evidence=evidence,
                inbox_storahci_control=control,
                modern_device=('local-model-service-binary-chain' if any(r['local_chain'] for r in exact)
                               else 'model-without-confirmed-local-chain' if exact
                               else 'inconclusive' if gaps or not control
                               else 'no-exact-device-ID-in-reviewed-models'),
                boundary='Published INF model declarations and referenced file headers only. No PnP '
                         'ranking, package validity, active service, boot or I/O conclusion.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('index_root', type=Path)
    parser.add_argument('--inventory', type=Path, required=True)
    parser.add_argument('--build', type=int, required=True)
    parser.add_argument('--product-type', type=int, choices=(1, 2, 3), required=True)
    args = parser.parse_args()
    try:
        report = scan(args.index_root, args.inventory, args.build, args.product_type)
    except (OSError, ValueError) as exc:
        print(json.dumps({'status': 'no-conclusion', 'reason': exc.strerror if isinstance(exc, OSError)
                          else str(exc)}), file=sys.stderr)
        return 2
    print(json.dumps(report, sort_keys=True, indent=2))
    return 0


if __name__ == '__main__':
    sys.exit(main())
