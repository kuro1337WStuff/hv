"""Compare two authored SCSI child types against the published cdrom.inf only."""
import argparse
import hashlib
import json
from pathlib import Path
import re

from .winpe_storage import (bounded, enumerated, models, pairs, require,
                            sections, service_evidence, token_reader)


# Microsoft, Identifiers for SCSI Devices: generic identifiers indexed by PDT.
GENERIC_IDS = {0: 'GenDisk', 5: 'GenCdRom'}
CDROM_GUID = '{4d36e965-e325-11ce-bfc1-08002be10318}'


def inquiry(device_type):
    # PQ=0, SPC-3, response data format 2; 31 additional bytes (36 total).
    response = bytes((device_type, 0, 5, 2, 31, 0, 0, 0)) + b'LAB     OPTICAL CONTROL 0001'
    return dict(inquiry_hex=response.hex(), type=response[0] & 31,
                compatible_id=GENERIC_IDS[response[0] & 31], status='inconclusive')


def model_matches(inf, expand):
    version = list(pairs(inf.get('version', ())))
    for field, expected in [('class', 'cdrom'), ('classguid', CDROM_GUID),
                            ('signature', '$windows nt$')]:
        require([expand(v).lower() for k, v in version if k == field] == [expected],
                'missing, ambiguous or unexpected ' + field)
    selected = models(inf, 19041, 1, expand)
    require(selected, 'no applicable AMD64 models')
    matches = {v.lower(): [] for v in GENERIC_IDS.values()}
    for section in selected:
        count = 0
        for _, value in pairs(inf[section]):
            install, *ids = (expand(v) for v in value.split(','))
            require(install and '%' not in install, 'invalid install section reference')
            for position, identity in enumerate(ids, 1):
                require(identity and not any(c in identity for c in '%?,"'), 'unresolved model ID')
                count += 1
                if identity.lower() in matches:
                    matches[identity.lower()].append(dict(models=section, id=identity,
                                                          id_field=position, ddinstall=install))
        require(count, 'empty or ID-less selected models: ' + section)
    return matches


def resolve(inf, expand, drivers, matches, inf_name, inf_hash, result):
    rows = matches[result['compatible_id'].lower()]
    if not rows:
        result['status'] = 'no-match'
        return
    require(len(rows) == 1, 'ambiguous exact generic ID model declarations')
    row = rows[0]
    versioned = re.compile(re.escape(row['ddinstall'].lower()) + r'\.nt(?:amd64)?\.\d')
    require(not any(versioned.match(section) for section in inf),
            'unsupported version-decorated install section')
    service_evidence(inf, row['ddinstall'], expand, drivers, row)
    for section in (row['install_section'], row['service_section'].lower()):
        for key, _ in pairs(inf[section]):
            require(key not in ('include', 'needs') and '%' not in key,
                    'unresolved directive in ' + section + ': ' + key)
    require(row['service'].lower() == 'cdrom' and row['servicebinary'].lower() == r'%12%\cdrom.sys',
            'not the named optical class service and binary')
    row.update(inf='Windows/INF/' + inf_name, inf_sha256=inf_hash,
               binary_sha256=hashlib.sha256(bounded(drivers['cdrom.sys'], 64 << 20)).hexdigest())
    result.update(status='candidate', evidence=row)


def report(root: Path, inventory: Path):
    cases = {'optical': inquiry(5), 'disk': inquiry(0)}
    try:
        published, drivers = enumerated(root, inventory)
        path = published.get('cdrom.inf')
        require(path is not None, 'missing published cdrom.inf')
        raw = bounded(path, 8 << 20)
        codec = ('utf-16' if raw.startswith((b'\xff\xfe', b'\xfe\xff')) else
                 'utf-8-sig' if raw.startswith(b'\xef\xbb\xbf') else 'cp1252')
        inf = sections(raw.decode(codec))
        expand = token_reader(inf)
        matches = model_matches(inf, expand)
        inf_hash = hashlib.sha256(raw).hexdigest()
        for case in cases.values():
            try:
                resolve(inf, expand, drivers, matches, path.name, inf_hash, case)
            except (ValueError, OSError) as exc:
                case['reason'] = str(exc)
    except (ValueError, OSError) as exc:
        for case in cases.values():
            case['reason'] = str(exc)
    return dict(controller='PCI 8086:2922', target='en-US NTamd64 10.0.19041 ProductType=1',
                scope='Exact generic ID in published Windows/INF/cdrom.inf only; not hardware-ID '
                      'enumeration, driver ranking, disk coverage, binding, boot or I/O.', **cases)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('index_root', type=Path)
    parser.add_argument('--inventory', type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(report(args.index_root, args.inventory), indent=2))


if __name__ == '__main__':
    main()
