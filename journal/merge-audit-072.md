# Integration audit after 072 · 2026-11-25

Scope: published branches, open PR bases and checks, trial records and non-mutating merge-tree simulations. This is a queue for review, not an approval. No firmware or lab tooling was merged, no shared branch rewritten, and no VM booted for this audit. [Current handoff](status.md) · [merge and rollback procedure](project-workflow.md).

## Main and CI

Owner and acceptance checklist: [maintenance issue #23](https://github.com/kuro1337WStuff/hv/issues/23).

- **First candidate: [#2](https://github.com/kuro1337WStuff/hv/pull/2), CI only.** Head `0da7a455` adds only `.github/workflows/host-controls.yml`. The checkout action is pinned, credentials are not persisted, and the fixed NPT C test exists on `main`. The recorded host check passed on an older base (`a65ac65`); the PR's description also implies process documentation that is not in its diff. Refresh against current `main`, correct the description and require both the fixed-NPT and published-trial checks on that revision before a squash. This does not qualify the firmware.
- **Do not merge `feature/l1-handoff` into `main` as a batch.** At `d388e05` vs `265f57d` the feature side was 40 commits and 88 changed files from the common base. A dry merge conflicted in `journal/README.md`. Keep the canonical main journal; historical feature evidence already has pinned links. Plan a separate, explicitly scoped runtime promotion with checks on its actual integrated source.
- `main` reported `protected=false` and the repository rulesets query returned no entries. A green workflow is information, not server-enforced review. After #2 is current, propose a PR requirement and the two stable checks; confirm the maintainer's recovery and bypass policy before changing settings. Do not enable a merge queue until workflows support its event. No repository settings changed here.

## Firmware review queue — integration branch only

| Order | PRs | Evidence and merge condition |
| --- | --- | --- |
| 1 | #4 → #10 (page transition, range audit) | 060, 063 and 067 cover the declared fixed fixtures. Land the parent with ancestry intact, retarget the child to `feature/l1-handoff` and check the reduced diff. The range audit does not qualify a general loader; 069 did not run. |
| 2 | #6 → #8 (UART THRE and IRQ4) | 061 and 062 cover the feature sources. 066/067 exercised the pair of UART/range profiles from combined tree `beb7e6bd`; compare the resulting integration tree and repeat affected controls if it differs. Those runs do not cover later merges. |
| 3 | #5 (PIT channel 2), then #9 (fixed EFER read) | Independent review candidates: 059 and 064, respectively. Check each against the updated integration base; reconcile shared profile dispatch and docs rather than accepting either side wholesale. #9 is not a general CPU-state ownership gate. |
| 4 | #11 (host PIC model) | Host control plus IRQ0 regressions 065/071, not guest ICW proof. Preserve ancestry for its child #12 and retarget that child only after checking its base and remaining diff. |
| Hold | #3, #12 and test-only #21 | #3 draft: corrected ACPI guest 058 unbooted. #12 draft: 068 failed. [Host trial 072](trials/072.md) covers the shared formatter correction and capacity, not either guest. #21 contains two host test files against #12 and can be reviewed separately; it is not a reason to promote its parent. |

The runtime stacks share build dispatch, handoff code, package selection, the profile manifest and lab documentation. A clean individual PR merge preview does not establish a clean or correct combined tree. After each parent: verify the actual target SHA, retarget children without deleting their base prematurely, review the new diff, and keep the rollback SHA. No forced history rewrite is needed.

## Lab tooling queue — hold both PRs

1. **#7 (`56db7a8`): check configuration drift before mutation.** `New-HvLane.ps1:69–84` sets and verifies automatic start off, automatic checkpoints off, standard checkpoints and Secure Boot off. `Invoke-HvQueue.ps1:172–197` checks lane identity, memory and devices again but omits those four settings. Require a host test for each drift: `ATTENTION` before checkpoint or boot. This is a code-level gap, not a claim that either live VM drifted.
2. **#18 (`68a5723`): external holds and truthful STOP reporting.** `lab/hv_plan.py:90–115` derives holds only from this board's receipts. Lane A's external 068 failure is not one of those receipts. Require the admission layer to honor that hold even for a newly pinned card, without contacting the broker; lane B must remain independent. At lines 173–185, a STOP between reservation and submission is persisted correctly but the final report can use the previous `READY` snapshot. Require a host regression showing no submission and the same held state in both the final report and a fresh `status`.

Trials 070/071 remain valid for the two successful lane B regressions. They did not cover external holds, configuration drift or that STOP window. No hosted checks were listed for either reviewed head. Exercise these paths off-VM, record exact updated SHAs and checks, then review #7 before retargeting #18 to the integration branch. Track under [#17](https://github.com/kuro1337WStuff/hv/issues/17). No live fault injection or release of a held VM is needed.

## Maintenance without history loss

- #19, #20 and #22 were squash-merged documentation/governance PRs. Their topic refs are cleanup candidates, **not** evidence that the corresponding firmware branches landed. Check for open dependents, local edits/worktrees and the matching squash content before retiring refs; no branch or worktree was deleted.
- Keep per-run screenshots and failing checkpoints. Raw run directories, frozen media and VM storage are not release artifacts. Establish retention with the operator before any deletion; never prune a held lane to make capacity. Main's ignore rules cover build output, media, machines and the private vault.
- There is no root license file on the audited `main`. If third-party reuse is intended, the owner must choose the license; do not select terms or republish reference manuals by default.
