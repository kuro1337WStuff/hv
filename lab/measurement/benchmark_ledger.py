"""Render an offline ledger of observed, matched cold-boot timing cohorts."""
import hashlib
import textwrap
import xml.etree.ElementTree as ET

import cohort
import journal_dashboard as dashboard


def feature_pair(pair, base, candidate):
    """Bind the OFF/ON bit to the controls digest recorded at capture, never just the plot label."""
    gate = pair['gate']
    cohort.keys(gate, ('feature', 'common_controls_sha256'))
    feature, common = gate['feature'], gate['common_controls_sha256']
    dashboard.field(feature, r'[A-Za-z][A-Za-z0-9_-]{0,31}', 'feature ID')
    cohort.digest(common)
    a, b = base['condition'], candidate['condition']
    cohort.require(a['mode'] in ('nested', 'physical-guest') and pair['baseline']['commit'] == pair['candidate']['commit'],
                   'feature toggle requires one guest build')
    for key in a:
        if key != 'controls_sha256':
            cohort.require(a[key] == b[key], 'unmatched feature toggle: ' + key)
    for state, condition in (('off', a), ('on', b)):
        contract = f'hv-feature-v1\n{common}\n{feature}\n{state}\n'.encode('ascii')
        cohort.require(hashlib.sha256(contract).hexdigest() == condition['controls_sha256'],
                       'feature state not bound to recorded controls: ' + state)
    return f'FEATURE OFF TO ON  /  {feature}  /  {dashboard.MODES[a["mode"]].upper()}'


def render(manifest, directory):
    cohort.keys(manifest, ('title', 'reports', 'benchmarks'))
    title = dashboard.label(manifest['title'])
    pairs = manifest['benchmarks']
    cohort.require(isinstance(pairs, list) and len(pairs) <= 16, 'expected 0–16 benchmarks')
    names = set()
    for pair in pairs:
        cohort.require(isinstance(pair, dict), 'expected benchmark object')
        fields = ('label', 'kind', 'baseline', 'candidate')
        cohort.keys(pair, fields + ('gate',) if pair.get('kind') == 'feature' else fields)
        name = dashboard.label(pair['label']).casefold()
        cohort.require(name not in names, 'duplicate benchmark label')
        names.add(name)
        cohort.require(pair['kind'] in ('build', 'native', 'feature'), 'unknown benchmark kind')

    svg = ET.Element('svg', xmlns='http://www.w3.org/2000/svg', width='960', role='img',
                     attrib={'aria-labelledby': 'title desc'})
    ET.SubElement(svg, 'title', id='title').text = title + ' — Benchmark ledger'
    ET.SubElement(svg, 'desc', id='desc').text = (
        'Observed matched comparisons show elapsed nanoseconds, boot counts, dispersion and sources. '
        'A negative change means the candidate took less time.' if pairs else
        'Unmeasured: no matched timing cohorts have been published.')
    paper = ET.SubElement(svg, 'rect', width='960', fill='#eef3f5')
    y = 20

    def section(lines, background, foreground, size=15, bold=False):
        nonlocal y
        # 84 monospace glyphs at 15px leave ample space inside the 912px card.
        lines = [part for line in lines for part in textwrap.wrap(line, width=84, break_on_hyphens=False)]
        height = len(lines) * 23 + 16
        ET.SubElement(svg, 'rect', x='24', y=str(y), width='912', height=str(height), fill=background)
        for i, line in enumerate(lines):
            ET.SubElement(svg, 'text', x='42', y=str(y + 24 + i * 23), fill=foreground, attrib={
                'font-family': 'monospace', 'font-size': str(size),
                'font-weight': '700' if bold else '400'}).text = line
        y += height

    section(['FIELD JOURNAL  /  BENCHMARK LEDGER', title], '#142f40', '#ffffff', 20, True)
    y += 12
    if not pairs:
        section(['UNMEASURED', 'No observed matched timing cohorts. No baseline or elapsed change is available.'],
                '#dfe8ed', '#142938')

    ids, paths, boot_ids, receipts = set(), set(), set(), set()
    for pair in sorted(pairs, key=lambda p: p['label'].casefold()):
        arms = (pair['baseline'], pair['candidate'])
        base, candidate = summaries = tuple(dashboard.arm_summary(arm, directory) for arm in arms)
        if pair['kind'] == 'build':
            a, b = (arm['commit'] for arm in arms)
            cohort.require(not (a.startswith(b) or b.startswith(a)), 'expected distinct commits')
            dashboard.matched(base, candidate)
            comparison = 'SAME-MODE BUILD  /  ' + dashboard.MODES[base['condition']['mode']].upper()
        elif pair['kind'] == 'feature':
            comparison = feature_pair(pair, base, candidate)
        else:
            cohort.compare(candidate, base)
            comparison = 'NATIVE BASELINE  /  PHYSICAL GUEST CANDIDATE'
        delta = candidate['median_of_boot_medians_ns'] - base['median_of_boot_medians_ns']
        section([pair['label'], 'OBSERVED  /  ' + comparison,
                 f'Elapsed change (candidate − baseline): {delta:+} ns; lower is better.'],
                '#164d51', '#ffffff', bold=True)
        for side, arm, summary in zip(('Baseline', 'Candidate'), arms, summaries):
            path = (directory / arm['file']).resolve()
            cohort.require(arm['id'] not in ids and path not in paths, 'reused cohort ID or source')
            ids.add(arm['id'])
            paths.add(path)
            boots = sorted(summary['boots'], key=lambda boot: boot['boot_id'])
            low, high = summary['boot_median_range_ns']
            condition = summary['condition']
            section([f"{side}: {arm['id']}  /  {dashboard.MODES[condition['mode']]}",
                     f"Median of boot medians: {summary['median_of_boot_medians_ns']} ns",
                     f'{len(boots)} independent cold boots  /  range of boot medians: {low} to {high} ns',
                     f"Source: {arm['file']}", f"Commit: {arm['commit']}",
                     'Runtime SHA-256: ' + condition['runtime_sha256'],
                     'Workload SHA-256: ' + condition['workload_sha256']], '#ffffff', '#142938')
            for boot in boots:
                boot_id, receipt = boot['boot_id'], boot['receipt_sha256']
                cohort.require(boot_id not in boot_ids and receipt not in receipts, 'reused boot ID or receipt')
                boot_ids.add(boot_id)
                receipts.add(receipt)
                section([f"Boot {boot_id}", f"Median: {boot['median_elapsed_ns']} ns  /  receipt SHA-256:",
                         receipt], '#e1ebed', '#263f4a')
            y += 4
        y += 8
    height = y + 20
    svg.set('viewBox', f'0 0 960 {height}')
    svg.set('height', str(height))
    paper.set('height', str(height))
    return ET.tostring(svg, encoding='utf-8', xml_declaration=True) + b'\n'
