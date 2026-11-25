"""Render journal evidence and matched, same-mode timing cohorts as static SVG."""
import argparse
import json
from pathlib import Path
import re
import textwrap
import xml.etree.ElementTree as ET

import cohort


STATUSES = {'passed': '#145640', 'partial': '#80500a', 'failed': '#982e38',
            'skipped': '#554382', 'held': '#734217', 'not-run': '#445368', 'unmeasured': '#43536a'}
SCOPES = ('host', 'nested', 'physical')
MODES = {'nested': 'nested', 'physical-guest': 'physical guest', 'native': 'native'}


def field(value, pattern, name):
    cohort.require(isinstance(value, str) and re.fullmatch(pattern, value), f'invalid {name}')
    return value


def label(value):
    field(value, r'[\x20-\x7e]{1,48}', 'label (1–48 printable ASCII characters)')
    cohort.require(value == value.strip() and not any(c in value for c in '<>'), 'invalid label')
    return value


def revision(value):
    return field(value, r'[a-f0-9]{7,40}', 'commit')


def relative(value):
    field(value, r'[A-Za-z0-9._/-]{1,80}', 'relative path')
    for part in value.split('/'):
        field(part, r'[a-zA-Z0-9][a-zA-Z0-9._-]*', 'path component')
        cohort.require(not part.endswith('.') and not re.fullmatch(
            r'(?i:con|prn|aux|nul|com[1-9]|lpt[1-9])(?:\..*)?', part), 'nonportable path')
    return value


def arm_summary(arm, directory):
    cohort.keys(arm, ('id', 'commit', 'file'))
    field(arm['id'], r'[A-Za-z0-9][A-Za-z0-9._-]{0,31}', 'cohort ID')
    revision(arm['commit'])
    name = relative(arm['file'])
    cohort.require(name.endswith('.json'), 'cohort file must be JSON')
    path = (directory / name).resolve()
    cohort.require(path.is_relative_to(directory.resolve()), 'cohort path leaves report directory')
    summary = cohort.load(path)
    cohort.require(summary['condition']['origin'] == 'observed', 'published timing requires observed cohorts')
    return summary


def matched(base, candidate):
    a, b = base['condition'], candidate['condition']
    for key in ('mode', 'origin', *cohort.HASH_FIELDS, *cohort.PLAN_FIELDS):
        if key == 'runtime_sha256':
            cohort.require(a[key] != b[key], 'expected distinct runtime hashes')
        else:
            cohort.require(a[key] == b[key], f'unmatched comparison: {key}')
    for key in ('receipt_sha256', 'boot_id'):
        cohort.require({boot[key] for boot in base['boots']}.isdisjoint(
            boot[key] for boot in candidate['boots']), f'reused {key} across cohorts')
    before, after = (s['median_of_boot_medians_ns'] for s in (base, candidate))
    return 100 * (before - after) / before


def render(report, directory):
    cohort.keys(report, ('title', 'records', 'comparisons'))
    title = label(report['title'])
    records, comparisons = report['records'], report['comparisons']
    cohort.require(isinstance(records, list) and 1 <= len(records) <= 64, 'expected 1–64 evidence rows')
    cohort.require(isinstance(comparisons, list) and len(comparisons) <= 16, 'expected 0–16 comparisons')
    svg = ET.Element('svg', xmlns='http://www.w3.org/2000/svg', role='img',
                     attrib={'aria-labelledby': 'title desc'})
    ET.SubElement(svg, 'title', id='title').text = title
    ET.SubElement(svg, 'desc', id='desc').text = ('Evidence by execution scope, with trial and source references. '
        + ('Timing compares independent cold boots from matched builds in the same mode.' if comparisons else
           'No matched performance measurement has been published in this report.'))

    def box(x, y, w, h, color, radius=0):
        ET.SubElement(svg, 'rect', x=str(x), y=str(y), width=str(w), height=str(h), rx=str(radius), fill=color)

    def text(x, y, value, size=16, fill='#142938', bold=False, mono=False):
        ET.SubElement(svg, 'text', x=str(x), y=str(y), fill=fill, attrib={
            'font-family': 'monospace' if mono else 'system-ui, sans-serif', 'font-size': str(size),
            'font-weight': '700' if bold else '400'}).text = value

    paper = ET.SubElement(svg, 'rect', width='960', fill='#eef3f5')
    box(0, 0, 960, 106, '#142f40')
    text(28, 32, 'FIELD JOURNAL   /   EVIDENCE REPORT', 14, '#8de0ce', True, True)
    text(28, 78, title, 27, '#ffffff', True, True)
    text(28, 145, '01   Recorded outcomes', 23, bold=True)
    text(28, 170, 'Each outcome applies only to its named check and execution scope.', 16)
    y = 186
    seen = set()
    for r in records:
        cohort.keys(r, ('label', 'scope', 'status', 'trial', 'commit', 'source'))
        name, scope, status = label(r['label']), r['scope'], r['status']
        cohort.require(scope in SCOPES and isinstance(status, str) and status in STATUSES, 'unknown scope or status')
        identity = (name.casefold(), scope)
        cohort.require(identity not in seen, 'duplicate evidence row')
        seen.add(identity)
        trial = field(r['trial'], r'[0-9]{3,6}', 'trial')
        commit, source = revision(r['commit']), relative(r['source'])
        box(24, y, 912, 96, '#ffffff', 8)
        box(24, y + 10, 4, 76, STATUSES[status], 2)
        text(42, y + 28, name, 17, bold=True, mono=True)
        text(665, y + 28, scope.upper(), 15, bold=True, mono=True)
        box(788, y + 8, 132, 29, STATUSES[status], 6)
        text(801, y + 28, status.upper(), 15, '#ffffff', True, True)
        text(42, y + 56, f'Trial {trial}  /  commit {commit[:12]}', 15, mono=True)
        text(42, y + 79, source, 14, '#445b68', mono=True)
        y += 102

    text(28, y + 37, '02   Performance', 23, bold=True)
    y += 54
    if not comparisons:
        box(24, y, 912, 77, '#dfe8ed', 8)
        text(42, y + 30, 'UNMEASURED', 17, bold=True, mono=True)
        text(42, y + 56, 'No matched timing cohorts in this report. Functional outcomes are not speed measurements.')
        y += 77
    names, sources, files = set(), {}, {}
    for pair in comparisons:
        cohort.keys(pair, ('label', 'baseline', 'candidate'))
        name = label(pair['label'])
        cohort.require(name.casefold() not in names, 'duplicate comparison')
        names.add(name.casefold())
        arms = [pair[k] for k in ('baseline', 'candidate')]
        summaries = [arm_summary(arm, directory) for arm in arms]
        for arm in arms:
            ident, version, filename = arm['id'], arm['commit'], arm['file']
            cohort.require(sources.setdefault(ident, (version, filename)) == (version, filename)
                           and files.setdefault(filename, ident) == ident, 'ambiguous cohort identity')
        revs = [arm['commit'] for arm in arms]
        shared = next((i for i, (a, b) in enumerate(zip(*revs)) if a != b), None)
        cohort.require(shared is not None, 'expected distinct commits')
        prefix = max(12, shared + 1)
        delta = matched(*summaries)
        mode = MODES[summaries[0]['condition']['mode']]
        box(24, y, 912, 63, '#164d51', 8)
        text(42, y + 26, name, 17, '#ffffff', True, True)
        text(42, y + 50, f'{mode}   /   elapsed time   /   improvement: {delta:+.2f}% (positive = faster)',
             15, '#ffffff', mono=True)
        y += 66
        for side, arm, summary in zip(('Baseline', 'Candidate'), arms, summaries):
            boots = summary['boots']
            values = ', '.join(str(boot['median_elapsed_ns']) for boot in boots)
            lines = textwrap.wrap('Per-boot medians (ns): ' + values, width=91)
            height = 70 + len(lines) * 20
            box(24, y, 912, height, '#ffffff', 6)
            text(42, y + 25, f"{side}: {arm['id']}  /  commit {arm['commit'][:prefix]}", 15, bold=True, mono=True)
            text(42, y + 50, f"n = {len(boots)} independent cold boots   /   median of boot medians: "
                 f"{summary['median_of_boot_medians_ns']} ns", 15, mono=True)
            for i, line in enumerate(lines):
                text(42, y + 73 + 20 * i, line, 14, '#445b68', mono=True)
            y += height + 4
        y += 6
    height = y + 24
    paper.set('height', str(height))
    svg.set('viewBox', f'0 0 960 {height}')
    svg.set('width', '960')
    svg.set('height', str(height))
    return ET.tostring(svg, encoding='utf-8', xml_declaration=True) + b'\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('report', type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    try:
        cohort.require(args.output.suffix.lower() == '.svg', 'output must use .svg')
        data = json.loads(args.report.read_text(encoding='utf-8'), object_pairs_hook=cohort.unique_fields)
        image = render(data, args.report.parent)
        args.output.write_bytes(image)
    except (OSError, ValueError) as error:
        parser.exit(2, f'report rejected: {error}\n')


if __name__ == '__main__':
    main()
