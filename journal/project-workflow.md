# Project workflow

`main` records what was tried, even when the implementation stays on a feature branch. A published trial is a record of an observation or decision, not approval to merge code.

## Where to resume

Start with [current status](status.md) on `main` and the [trial reservation index](https://github.com/kuro1337WStuff/hv/issues/13). Use the status page to find the current gate, blocker and next action; use the work-item issue to find its owner, dependencies and open PRs. Follow the [journal index](README.md) and the linked trial before starting or repeating work. If sources disagree, check the dated evidence and append a correction rather than inferring a pass from a closed issue or merged PR.

| Location | What belongs there |
| --- | --- |
| `journal/trials/NNN.md` | The public record for one reserved trial, including holds and non-runs. |
| `journal/README.md`, `journal/status.md` | The journal index and the current handoff, respectively. |
| `journal/test-report.md`, `journal/images/` | Historical results and reviewed evidence. Check existing published paths before adding or linking evidence. |
| `journal/reports/` | Source JSON and generated SVG figures. Link both from the trial; do not hand-edit the rendered values. |
| `firmware/`, `lab/` | Firmware sources, and lab tooling and instructions, respectively. Unmerged implementation belongs on its actual feature base. |
| GitHub Issues and PRs | Work ownership, dependencies, run reservations, review and integration decisions. |

## Start a work item

1. Find or open a work-item issue. Name the capability, owner, concrete acceptance criteria, dependencies, actual base and trial run ID (or mark it unreserved until allocated). Link related issues rather than opening competing trackers for the same work.
2. Reserve each new `NNN` in **one project-wide run-tracking issue**, not in the feature issue alone. Check that tracker's reservations and the published journal paths and index, including historical entries. Have the tracker owner record allocations serially, linking each ID to its work item; use the ID only after it is recorded. A held or cancelled allocation is never recycled. The journal number is not a CI job or VM invocation ID.
3. Branch `feature/<one-capability>` from the actual dependency: current `main` for independent work, or the prerequisite branch tip for stacked work. Open a draft implementation PR targeting that same base. Link the work item and list acceptance checks, dependencies, rollback and outstanding evidence. Keep each PR independently understandable and revertible.

## Publish each trial immediately

For **every** trial, open a separate docs-only PR directly to `main` for `journal/trials/NNN.md`; do not wait for implementation review, VM access or success. Include the work item, branch and exact revision evaluated, intended check and acceptance criterion, what actually happened, limits, and the next action. For a hold or non-run, state the reason and what was **not** exercised. Link the trial from the journal index and reflect the resulting gate in `journal/status.md`. Publish only reviewed public technical evidence; omit raw logs, VM media, credentials and identifying details. Check that evidence links resolve on their stated revision, not just on the topic branch.

In the PR body, put the following line in its own paragraph, replacing `NNN` with the reserved number (no bullet, link markup or backticks):

Journal: journal/trials/NNN.md

Report hosted CI, reviewed VM evidence and the integration decision separately. A successful build is not a VM pass; a skipped run, a journal merge or a green status check never automatically approves a change or marks a trial passed. Keep the implementation PR in draft while its required evidence is missing. When evidence or interpretation changes, append a dated correction to the same trial in a docs-only PR: identify the superseded statement, evidence and revised conclusion. Preserve the original observation and update the index/status pointers as needed. Reserve a new ID for a new trial.

PRs to `feature/l1-handoff` run the [portable device controls](trials/114.md) on GitHub-hosted Ubuntu; [115](trials/115.md) added the shared master PIC check. Compare the actual checked-out PR merge and device traces with the trial's expected results. These jobs do not use either VM bench. PRs to `main` keep their separate fixed-NPT and journal checks.

For a journal figure, retain the source revision, trial ID, scope and disposition for every row. Regenerate the SVG using the [reporting guide](../lab/measurement/README.md), review it, then publish it and its input together. To report a performance change, retain the raw matched baseline and candidate cohorts as well. Without those runs, leave performance **unmeasured**; neither a passing capability check nor an illustrative screenshot is a timing sample.

## Merge and close

Squash an independent change into its declared target so a single revert removes it. For stacked changes, merge the parent with a **merge commit** to preserve its ancestry. After the parent lands, verify each child's PR is still open and targets the intended surviving base; retarget if needed and review its diff and checks again. Do not assume automatic retargeting or merge an entire integration branch to publish a trial. Remove a branch only after its intended changes have landed or been explicitly abandoned and no dependent branch or PR still needs it.

A journal PR **references** its work item (for example, `Refs #123`); it does not close a capability issue just because a record landed. Use closing keywords only on the change that satisfies all acceptance criteria and only when that PR should close the issue on `main`. Otherwise link without auto-closing and close explicitly after acceptance. If duplicate or conflicting issues exist, identify the canonical issue, cross-link both and record the disposition; close a duplicate as duplicate or superseded, not completed. Leave unmet or disputed criteria open in the canonical issue and visible in status.

## Roll back without erasing evidence

Review downstream dependencies first. Branch `fix/revert-<topic>` from the actual merge target. For a squash use `git revert <squash-sha>`; for a merge commit verify the first parent is the intended base before `git revert -m 1 <merge-sha>`. Abort a conflicted revert with `git revert --abort` if its effects are unclear. Open a rollback PR to that target, link the affected work item and trial, and repeat the affected acceptance checks. Update status and append the rollback disposition to the record. Never reset or force-push shared history or delete the original trial to imply it did not happen.
