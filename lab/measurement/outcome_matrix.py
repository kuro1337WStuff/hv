"""Render archived, scoped functional checks; a missing record is not an outcome."""
import json
import textwrap
import xml.etree.ElementTree as ET

import cohort
import journal_dashboard as dashboard


COLUMNS = 4
MAX_CELLS = 128
MAX_REPORT_BYTES = 128 * 1024


def _report(directory, filename):
    name = dashboard.relative(filename)
    cohort.require(name.endswith('.json'), 'archived report must be a .json file')
    path = (directory / name).resolve()
    cohort.require(path.is_relative_to(directory), 'archived report leaves manifest directory')
    cohort.require(path.is_file(), 'archived report is not a regular file')
    with path.open('rb') as stream:
        data = stream.read(MAX_REPORT_BYTES + 1)
    cohort.require(len(data) <= MAX_REPORT_BYTES, 'archived report exceeds 128 KiB')
    report = json.loads(data.decode('utf-8'), object_pairs_hook=cohort.unique_fields)
    cohort.keys(report, ('title', 'records', 'comparisons'))
    dashboard.label(report['title'])
    records = report['records']
    cohort.require(isinstance(records, list) and 1 <= len(records) <= 64, 'expected 1–64 evidence rows')
    # This view consumes functional records only. Never follow a timing arm or a journal citation.
    pairs = report['comparisons']
    cohort.require(isinstance(pairs, list) and len(pairs) <= 16, 'expected 0–16 comparisons')
    return records


def _matrix(files, directory):
    cohort.require(isinstance(files, list) and len(files) <= 32, 'expected 0–32 archived reports')
    cells, labels = {}, {}
    for filename in files:
        seen = set()
        for record in _report(directory, filename):
            cohort.keys(record, ('label', 'scope', 'status', 'trial', 'commit', 'source'))
            label = dashboard.label(record['label'])
            scope, status = record['scope'], record['status']
            cohort.require(scope in dashboard.SCOPES and isinstance(status, str) and status in dashboard.STATUSES,
                           'unknown scope or status')
            check = (scope, label.casefold())
            cohort.require(check not in seen, 'duplicate evidence row in archived report')
            seen.add(check)
            labels[check] = min(labels.get(check, label), label)
            commit = dashboard.revision(record['commit'])
            trial = dashboard.field(record['trial'], r'[0-9]{3,6}', 'trial')
            source = dashboard.relative(record['source'])
            cell = (*check, commit)
            observation = (status, trial, source)
            cohort.require(cells.setdefault(cell, observation) == observation,
                           f'conflicting outcome or source for {scope}: {label} at {commit}')
            cohort.require(len(cells) <= MAX_CELLS, f'expected at most {MAX_CELLS} distinct outcome cells')
    return cells, labels


def render(manifest, directory):
    cohort.keys(manifest, ('title', 'reports', 'benchmarks'))
    title = dashboard.label(manifest['title'])
    cells, labels = _matrix(manifest['reports'], directory.resolve())
    # Group by scope first. Within each scope put the first cited trial first; do not
    # assume that abbreviated commit IDs, filenames, or manifest order are chronological.
    sources = sorted({(scope, commit, trial, path) for (scope, _, commit), (_, trial, path) in cells.items()},
                     key=lambda s: (dashboard.SCOPES.index(s[0]), int(s[2]), s[1], s[2], s[3]))
    source_ids = {s: f'S{i:03}' for i, s in enumerate(sources, 1)}
    builds_in_order = dict.fromkeys((scope, commit) for scope, commit, _, _ in sources)
    build_ids = {build: f'B{i:03}' for i, build in enumerate(builds_in_order, 1)}

    svg = ET.Element('svg', xmlns='http://www.w3.org/2000/svg', width='960', role='img',
                     attrib={'aria-labelledby': 'title desc'})
    ET.SubElement(svg, 'title', id='title').text = 'Functional outcome matrix: ' + title
    ET.SubElement(svg, 'desc', id='desc').text = (
        'Checks grouped by execution scope against cited build revisions. Every recorded status has a source key. '
        'Not recorded means no archived record for that check and build; it is not a pass, failure, or not-run result.')

    def box(x, y, width, height, color):
        return ET.SubElement(svg, 'rect', x=str(x), y=str(y), width=str(width), height=str(height), fill=color)

    def text(x, y, value, size=15, color='#142938', bold=False):
        ET.SubElement(svg, 'text', x=str(x), y=str(y), fill=color, attrib={
            'font-family': 'monospace', 'font-size': str(size), 'font-weight': '700' if bold else '400'}).text = value

    paper = box(0, 0, 960, 0, '#eef3f5')
    box(0, 0, 960, 90, '#142f40')
    text(24, 30, 'FIELD JOURNAL   /   FUNCTIONAL OUTCOME MATRIX', 15, '#8de0ce', True)
    text(24, 68, title, 25, '#ffffff', True)
    text(24, 118, 'Each column is a build in one scope. S-keys identify journal sources below.')
    text(24, 141, 'NOT RECORDED = no record; a check absent from a panel has no records in any of its columns.', 14)
    y = 160
    for scope in dashboard.SCOPES:
        builds = [commit for s, commit in build_ids if s == scope]
        checks = sorted(name for s, name in labels if s == scope)
        for start in range(0, len(builds), COLUMNS):
            panel = builds[start:start + COLUMNS]
            box(24, y, 912, 56, '#164d51')
            text(36, y + 24, scope.upper() + '   /   CHECK', 17, '#ffffff', True)
            for i, commit in enumerate(panel):
                x = 322 + i * 154
                text(x, y + 22, build_ids[scope, commit], 16, '#ffffff', True)
                text(x, y + 44, commit[:12], 14, '#ffffff')
            y += 56
            for name in checks:
                if not any((scope, name, commit) in cells for commit in panel):
                    continue
                lines = textwrap.wrap(labels[scope, name], width=30)
                height = max(50, 10 + 17 * len(lines))
                box(24, y, 912, height, '#ffffff')
                for i, line in enumerate(lines):
                    text(36, y + 20 + i * 17, line, 14, bold=True)
                for i, commit in enumerate(panel):
                    x = 322 + i * 154
                    record = cells.get((scope, name, commit))
                    if record is None:
                        text(x, y + 29, 'NOT RECORDED', 13, '#445b68')
                    else:
                        status, trial, path = record
                        box(x - 4, y + 4, 139, 22, dashboard.STATUSES[status])
                        text(x, y + 20, status.upper(), 14, '#ffffff', True)
                        text(x, y + 43, source_ids[scope, commit, trial, path], 14)
                y += height + 2
            y += 14
    if not cells:
        text(24, y + 23, 'No archived functional records.', 17, bold=True)
        y += 50
    else:
        text(24, y + 24, 'Source key   /   repository-relative journal paths', 20, bold=True)
        y += 38
        for s in sources:
            scope, commit, trial, path = s
            box(24, y, 912, 49, '#ffffff')
            text(36, y + 19, f'{source_ids[s]}   {build_ids[scope, commit]}   {scope}   trial {trial}   commit {commit}',
                 14, bold=True)
            text(36, y + 40, path, 14)
            y += 51
    height = str(y + 16)
    paper.set('height', height)
    svg.set('viewBox', '0 0 960 ' + height)
    svg.set('height', height)
    return ET.tostring(svg, encoding='utf-8', xml_declaration=True) + b'\n'
