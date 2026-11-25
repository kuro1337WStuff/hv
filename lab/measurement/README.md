# Journal dashboard

Render a checked-in report as a static SVG with Python 3.9+ (standard library only). From the repository root:

```sh
python lab/measurement/journal_dashboard.py journal/reports/feature-phase.json --output journal/reports/feature-phase.svg
python -m unittest discover -s lab/measurement -p "test_*.py"
```

The output directory must exist. The renderer reads the report and any explicitly named local cohort files. It does not collect timings, boot a VM, inspect the host, fetch sources or read trial notes. The same inputs give the same SVG bytes. The image contains a title, description and visible outcome labels; when embedding it, provide alt text and link the source report so readers can inspect the records without the image.

## Evidence records

The UTF-8 JSON object has exactly `title` (display text), `records` (1–64 rows, in reading order) and `comparisons` (0–16 pairs). Use **`"comparisons": []`** until matched, observed timings exist. Missing or rejected timing inputs do not produce a chart of zeros. Every evidence row has exactly these fields:

| Field | Meaning |
| --- | --- |
| `label` | Name the specific check, not a whole milestone. Unique within its scope, ignoring case. |
| `scope` | `host`, `nested` or `physical`: the environment to which the claim applies. A host test is not a guest boot. |
| `status` | `passed`, `failed`, `partial`, `held`, `not-run`, `skipped` or `unmeasured`. A hold is a decision not to advance; not-run means the check was not executed; skipped records an explicit skip. None count as a pass. |
| `trial` | Journal trial identifier as a string of 3–6 digits; retain its leading zeros. |
| `commit` | 7–40 lowercase hexadecimal digits. Identifies the reviewed source revision **or** immutable evidence record the publisher cites, not necessarily the current `main` tip. |
| `source` | Repository-relative source path, e.g. `journal/trials/099.md`. The publisher checks that the cited file/revision supports the row. |

Title and labels accept 1–48 printable ASCII characters, without `<`, `>` or leading/trailing spaces. Paths use `/`, at most 80 characters, and portable components starting with a letter or digit followed by letters, digits, `.`, `_` or `-`. No URLs, traversal, absolute paths or Windows device names. Duplicate JSON keys, unknown keys and incomplete records are rejected rather than silently ignored.

## Adding a matched build comparison

Each `comparisons` entry has a unique `label` and two arms, `baseline` and `candidate`. The shape below illustrates **metadata only**, not an existing measurement:

```json
{
  "label": "Fixed workload elapsed time",
  "baseline": {"id": "before", "commit": "aaaaaaa", "file": "cohorts/before.json"},
  "candidate": {"id": "after", "commit": "bbbbbbb", "file": "cohorts/after.json"}
}
```

Use a stable cohort `id` (1–32 letters, digits, `.`, `_` or `-`, beginning with a letter or digit). `commit` here identifies the **actual build**. Give enough hex digits to distinguish the two revisions. `file` names a `.json` file beneath the **report directory**, not the repository root. Reusing an arm across comparisons is fine if its ID, build and file are consistent.

Each file uses the existing [`cohort.py`](cohort.py) input contract: a JSON array with one record per independent cold boot. To inspect one on its own, run `python lab/measurement/cohort.py path/to/cohort.json`.

- Each boot supplies `condition`, `boot_id`, `receipt_sha256`, `start: "cold"`, `outcome: "complete"`, and arrays `warmup` and `measured`. Boot IDs and receipt hashes must be unique both within and between the two arms.
- `condition` declares `origin` (only `observed` is publishable), `mode` (`nested`, `physical-guest`, or `native`), and the SHA-256 hashes `platform_sha256`, `workload_sha256`, `clock_sha256`, `controls_sha256`, `runtime_sha256`. Each hash is 64 lowercase hex digits identifying retained bytes. Record the predeclared integer plan: `boots` (at least 5), `warmups` (at least 1 per boot), `repeats` (at least 2 measured per boot), and `work_units` (at least 1 per trial).
- Each trial has `elapsed_ns`, `completed_units`, `entries`, `reasons`, `tx_bytes` and `busy_reads`. The loader verifies completed work, phase sizes, mode-appropriate balanced exit counters and drained UART accounting in both phases. Do not omit slow boots, substitute resumed boots, relabel synthetic rows as observed, or provide aggregate-only results. The publisher retains and verifies the cited receipts; the renderer validates records, not the provenance behind them.
- Baseline and candidate must have the same mode, platform, workload, clock, controls and entire plan. Their runtime hashes must differ. This dashboard does **not** compare guest to native or nested to physical. The standalone `cohort.compare` ratio has a different purpose and is not a build-to-build change.

The SVG shows cohort IDs, build commits, number of independent boots per arm, the median measured elapsed time **within each boot** (in input order, excluding warmups), and each arm’s median of those boot medians (nanoseconds). Improvement is `100 × (baseline − candidate) / baseline` using the arm medians: positive is faster; negative is slower. It is a descriptive comparison, not a confidence interval or proof of a causal speedup. Outcome labels never supply timing data.
