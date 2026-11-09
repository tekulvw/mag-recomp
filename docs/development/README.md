# Development notes

How the project is developed, measured and kept honest. These pages are for contributors; players
want the [README](../../README.md) and the [installer guide](../../installer/README.md), and a
from-source build is in [BUILDING.md](../../BUILDING.md).

| Page | Contents |
| --- | --- |
| [Working rules](workflow.md) | Starting point for new work, normal defaults versus experiments, and the engineering rules that have held since bring-up |
| [The ps3recomp fork](ps3recomp-fork.md) | How SDK changes are made, tagged, pinned and offered upstream |
| [Verifying a run](verification.md) | What counts as evidence from a game run, how frame rate is counted, and the offline assessors in `tools/` |
| [Performance findings](performance-findings.md) | What was tried on the CPU, SPU, RSX and GPU-offload paths, what each measured, and what was kept |

The render frontend has its own design notes and performance history under
[`docs/render-frontend/`](../render-frontend.md).
