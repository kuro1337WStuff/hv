"""Generate the journal's functional history and timing ledger from retained records."""
import argparse
import json
from pathlib import Path

import benchmark_ledger
import cohort
import journal_dashboard
import outcome_matrix


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('manifest', type=Path)
    parser.add_argument('--output-dir', required=True, type=Path)
    args = parser.parse_args()
    try:
        manifest = json.loads(args.manifest.read_text(encoding='utf-8'), object_pairs_hook=cohort.unique_fields)
        cohort.keys(manifest, ('title', 'reports', 'benchmarks'))
        journal_dashboard.label(manifest['title'])
        # Validate both views before replacing either output on malformed input.
        directory = args.manifest.parent
        outcomes = outcome_matrix.render(manifest, directory)
        timings = benchmark_ledger.render(manifest, directory)
        (args.output_dir / 'functional-outcomes.svg').write_bytes(outcomes)
        (args.output_dir / 'benchmark-ledger.svg').write_bytes(timings)
    except (ValueError, OSError) as error:
        parser.exit(2, f'report rejected: {error}\n')


if __name__ == '__main__':
    main()
