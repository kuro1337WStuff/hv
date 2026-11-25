# Project status · 2026-11-25, after 078

**On `main`:** firmware baseline [041](README.md), plus CI-only [#2](https://github.com/kuro1337WStuff/hv/pull/2) at `735c8b56` after both hosted checks passed on `54bace3b`. **On `feature/l1-handoff` only:** [#4](https://github.com/kuro1337WStuff/hv/pull/4) merged at `95abe46c`; [#10](https://github.com/kuro1337WStuff/hv/pull/10) retargeted, then squashed at `44fb6247`. The resulting firmware trees matched the source trees used in 060 and 063 respectively. Revert #10 first if both must be undone; #4 is a merge commit. No ordinary Linux boot or bare-metal qualification.

**Lab tooling on the feature branch:** [077](trials/077.md) integrated #7 at `0d9641f6` and #18 at `c8aa8d53`, after the guide-only conflict repair #29. The integrated firmware subtree did not change. No live runner was deployed.

## Where to resume

| Work item | State | Next decision |
| --- | --- | --- |
| [PIC evidence #14](https://github.com/kuro1337WStuff/hv/issues/14) | [068 FAIL](trials/068.md); [072 host control](trials/072.md) isolated the 13-character width refusal at 11/24 rows. Lane A held; #12 draft. | Review existing shared formatter #3 with host regression #21. Do not reboot or register the failed frame. Any guest confirmation requires a separate reservation, reviewed recovery and all refusal witnesses. |
| [CPU, loader and reference recovery #15](https://github.com/kuro1337WStuff/hv/issues/15) | CPU/loader contract incomplete; [069 NOT RUN](trials/069.md); original VM held at 057 | Review remaining CPU-state ownership and loader criteria independently. Keep 058 unbooted until an explicit recovery decision. No ordinary guest or physical promotion. |
| [Device integration #16](https://github.com/kuro1337WStuff/hv/issues/16) | [078: UART media frozen and read back](trials/078.md) on the new combined source. The selected EFI matched the linker output; historical 061 media was distinguished. [076](trials/076.md) explains the prior comparison's field differences for its own rebuild, not a guest result. | Hold #6 → #8. After a reviewed B stage exists, reserve a distinct live attempt using the retained 078 ISO (`262b1f47…0446b`) and existing screen oracle. No new digest approval or guest PASS. |
| [Test supervisor #17](https://github.com/kuro1337WStuff/hv/issues/17) | [077 host integration](trials/077.md): preflight controls and 14 supervisor tests passed on the combined tree; #7 and #18 landed on `feature/l1-handoff`. B still had an old checkout and 0/1 matching protected workers. | No qualified B stage. Preserve evidence and establish a trusted, unambiguous worker before any live admission. Never substitute A (068) or the reference VM (057). |

**Merge routing:** [Dated PR-by-PR merge audit and maintenance queue](merge-audit-072.md) · [live open PRs](https://github.com/kuro1337WStuff/hv/pulls). The host formatter test #21 targets the held PIC branch #12; it is not a reason to merge #12. Check the actual base, scope and source revision before each integration. [Project workflow](project-workflow.md) covers rollback and dependent PR retargeting.

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
