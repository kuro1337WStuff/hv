# Project status · 2026-11-25, after 072

**Integrated firmware:** [041 on `main`](README.md). **Latest trial:** [072](trials/072.md) identified the PIC evidence failure on the host; 068 remains failed, 069 did not run, and 070/071 remain nested regression passes. No feature firmware was promoted. Ryzen 9 9950X3D bare-metal qualification and an ordinary Linux boot remain outstanding.

## Where to resume

| Work item | State | Next decision |
| --- | --- | --- |
| [PIC evidence #14](https://github.com/kuro1337WStuff/hv/issues/14) | [068 FAIL](trials/068.md); [072 host control](trials/072.md) isolated the 13-character width refusal at 11/24 rows. Lane A held; #12 draft. | Review existing shared formatter #3 with host regression #21. Do not reboot or register the failed frame. Any guest confirmation requires a separate reservation, reviewed recovery and all refusal witnesses. |
| [CPU, loader and reference recovery #15](https://github.com/kuro1337WStuff/hv/issues/15) | CPU/loader contract incomplete; [069 NOT RUN](trials/069.md); original VM held at 057 | Review remaining CPU-state ownership and loader criteria independently. Keep 058 unbooted until an explicit recovery decision. No ordinary guest or physical promotion. |
| [Device integration #16](https://github.com/kuro1337WStuff/hv/issues/16) | Fixed device controls only | Review UART #6 before #8, PIC #11 before #12; PIT #5 is independent. Requalify changed integration trees before merging. |
| [Test supervisor #17](https://github.com/kuro1337WStuff/hv/issues/17) | [070 COMPLETE](trials/070.md) → [071 COMPLETE](trials/071.md); lane B retained at IRQ0 | Review dedicated lanes #7 before supervisor #18. After 071: 12 open board cards; 2 manual, 10 dependency-blocked, none eligible. A stopped scheduler is expected here. |

**Merge routing:** [open PRs](https://github.com/kuro1337WStuff/hv/pulls). #2 targets `main` for host CI. #3–#7, #9 and #11 target `feature/l1-handoff`, not `main`. #8 is stacked on #6; #10 on #4; #12 on #11; #18 on #7. Check each live PR's base and reviewed SHA before merging. A completed regression or publication of this page does not authorize a stack merge. [Project workflow](project-workflow.md) covers rollbacks and dependent PR retargeting.

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
