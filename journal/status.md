# Project status · through 089

**On `main`:** firmware baseline [041](README.md), plus CI-only [#2](https://github.com/kuro1337WStuff/hv/pull/2) at `735c8b56` after both hosted checks passed on `54bace3b`. **On `feature/l1-handoff` only:** [#4](https://github.com/kuro1337WStuff/hv/pull/4) merged at `95abe46c`; [#10](https://github.com/kuro1337WStuff/hv/pull/10) retargeted, then squashed at `44fb6247`. The resulting firmware trees matched the source trees used in 060 and 063 respectively. Revert #10 first if both must be undone; #4 is a merge commit. No ordinary Linux boot or bare-metal qualification.

**Lab tooling on the feature branch:** [077](trials/077.md) integrated #7 and #18; [#32](https://github.com/kuro1337WStuff/hv/pull/32) subsequently landed at `f0ec885a`. Its source tree matched the one tested in [081](trials/081.md); firmware did not change. No live runner was deployed.

## Where to resume

| Work item | State | Next decision |
| --- | --- | --- |
| [PIC evidence #14](https://github.com/kuro1337WStuff/hv/issues/14) | [086](trials/086.md): #49 landed the shared formatter on the PIC topic only. All 15 host rows fit; the 17-character label and row 25 were refused. The corrected ISO (`0463d202…5a66`) is frozen off-runner. | #12 stays draft. A remains held; [068 FAIL](trials/068.md) still lacks four guest receipts. No replay, new screen approval or firmware promotion. |
| [CPU, loader and reference recovery #15](https://github.com/kuro1337WStuff/hv/issues/15) | [083](trials/083.md): #42 resolved the #9 guide conflict; the combined ISO's selected EFI matched the linker output. No combined guest run. [069 NOT RUN](trials/069.md); original VM held at 057. | #9 stays draft. CPU ownership and loader criteria remain open. Keep 058 unbooted until a separate recovery decision. No ordinary guest or physical promotion. |
| [Device integration #16](https://github.com/kuro1337WStuff/hv/issues/16) | [084](trials/084.md): the existing IRQ0 EFI differed only in timestamp and checksum fields; neither fresh ISO ran. [080](trials/080.md) covered PIT builds; #34 landed only on the #5 topic. [078](trials/078.md) froze UART media separately. | Hold #5 for combined guest evidence and #6 → #8 for qualified B and a distinct reservation using the retained 078 ISO (`262b1f47…0446b`). No new guest PASS, approved screen or clock calibration. |
| [Test supervisor #17](https://github.com/kuro1337WStuff/hv/issues/17) | [085](trials/085.md#bench-b-checkout-review-not-a-rollout): retained B has the older worker and a local manifest change; its manifest records match integration, its worker digest does not. No active session was queried. | Keep B fenced. A rollout needs observed old-session closure, released locks and exactly one fresh stage with the reviewed worker. No admission from [081](trials/081.md) or a checkout change. |

**Merge routing:** [Dated PR-by-PR audit](merge-audit-072.md) · [live open PRs](https://github.com/kuro1337WStuff/hv/pulls) · [workflow and rollback](project-workflow.md). #21's test merge did not promote #12. Check the actual base, scope and source revision before each integration.

**Feature integration, host evidence only:** [089](trials/089.md) traced periodic rising edges through IRQ0 masking, acceptance and EOI. [088](trials/088.md) verified pinned init serial milestones, not Linux shutdown. Neither proves guest IRQ delivery. Next: [#57 · independent virtual S5 witness](https://github.com/kuro1337WStuff/hv/issues/57).

## Feature evidence before the separate trial ledger

Older records stay at their original paths; these links pin the published revisions, not moving branch tips.

| Trial | Record and scope |
| --- | --- |
| 042–057 | [Feature journal](https://github.com/kuro1337WStuff/hv/blob/c50789a0dbe11690abed52f00a92d900775f7021/journal/README.md): staged inputs, failed 043 address expectation, corrected fetch precision, CPU/device guards and failed 057 ACPI display. |
| 058 | [Reserved, not booted](https://github.com/kuro1337WStuff/hv/blob/c50789a0dbe11690abed52f00a92d900775f7021/journal/test-report.md). Host fix is not a guest pass. |
| 059–061 | [PIT channel 2](https://github.com/kuro1337WStuff/hv/blob/65e0347d5e5f07ef2de78c23a45f98ddd681a7cc/journal/runs/059-l1-pit-ch2.md), [owned-page transition](https://github.com/kuro1337WStuff/hv/blob/429a6814bfbe4f0ad21acba345b5a2bdd7e62865/journal/runs/060-l1-page-transition.md), [UART THRE](https://github.com/kuro1337WStuff/hv/blob/1946d6eb6a49b74a591e3540a586d2bd96fcdc3e/journal/runs/061-l1-uart-thre.md). First frames reviewed after queue attention. |
| 062–065 | [IRQ4](https://github.com/kuro1337WStuff/hv/blob/0b7c28a4cb7f39d0e09073415dfcf2efa823a47b/journal/runs/062-l1-uart-irq4.md), [offline range audit](https://github.com/kuro1337WStuff/hv/blob/89de7b5fe740772b37f1bf88426d8bf38b414ced/journal/runs/063-l1-range-audit.md), [fixed EFER read](https://github.com/kuro1337WStuff/hv/blob/0fe588858882b35f0df82ad0a2700f0550b4e5a8/journal/runs/064-l1-efer-read.md), [IRQ0 regression](https://github.com/kuro1337WStuff/hv/blob/732c3d9ef7526cfded8e8f5b626f127bd8b50edc/journal/runs/065-l1-pit-irq0.md). |
| 066–067 | [IRQ4](https://github.com/kuro1337WStuff/hv/blob/0b7c28a4cb7f39d0e09073415dfcf2efa823a47b/journal/runs/066-l1-uart-irq4.md) and [range audit](https://github.com/kuro1337WStuff/hv/blob/89de7b5fe740772b37f1bf88426d8bf38b414ced/journal/runs/067-l1-range-audit.md): two cold boots from the same combined source tree, unchanged reviewed screens. Not a merge or a Linux boot. |

For a new work item, consult the [run reservation ledger #13](https://github.com/kuro1337WStuff/hv/issues/13) first. Do not allocate the next number from this snapshot.
