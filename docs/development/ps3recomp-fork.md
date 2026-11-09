# The ps3recomp fork

mag-recomp builds on [ps3recomp](https://github.com/sp00nznet/ps3recomp). Changes MAG needs in the
SDK are made in the fork, [tekulvw/ps3recomp](https://github.com/tekulvw/ps3recomp), and offered
upstream where they are useful to other titles.

## Roles and pins

Four checkouts are pinned in [`profiles/framework.json`](../../profiles/framework.json), each to a
commit with an immutable `mag-recomp/*` tag. [BUILDING.md](../../BUILDING.md#2-ps3recomp-checkouts)
has the current table.

| Role | Used for | Moves |
| --- | --- | --- |
| `runtime` | The SDK compiled into the launcher and engine | Often: each runtime release is a new `mag-recomp/runtime-YYYY.MM.DD` tag |
| `lifter-spu` | Inventory and the launcher and engine SPU lifts | Rarely |
| `lifter-ppu` | PPU and firmware lifting. An optional `splice_after` list names tagged fixed lifters for single-function splices; the current pin has none | Rarely |
| `spu-programs` | Lifting every embedded engine SPU program | Rarely |

Lifters move rarely on purpose: a new lifter changes the generated sources and every identity
derived from them, so it needs a full re-derivation and fresh acceptance. A runtime pin change
needs no re-derivation when the lifters and code generation are unchanged.

`allowed_runtime_revisions` and the `strict_*` lists in the same file say which runtime revisions
the build and the strict checks accept. A build that must use an unmerged SDK supplies an explicit
candidate manifest instead (see BUILDING.md); its receipt marks the runtime experimental, and the
allowlist is left alone.

## Making an SDK change

1. Branch from the fork's `main`, one focused change per branch.
2. Write a synthetic regression that fails before the change. SDK tests never need game files.
3. Make new behaviour opt-in unless it fixes a plain bug. MAG turns on the switches it needs from its
   own side, and lists each one in [ps3recomp-flags.md](../ps3recomp-flags.md);
   `tools/check_ps3recomp_flags.py` keeps that list honest.
4. Merge into the fork, tag the merged commit `mag-recomp/runtime-YYYY.MM.DD`, then open a
   mag-recomp PR that moves the pin. The pin PR updates `profiles/framework.json`, the BUILDING.md
   table and any CI reference to the old revision, and says what the new runtime brings for each
   platform.
5. Keep commit identities. The fork takes upstream changes by merging rather than rebasing, so
   every commit a pin or receipt names stays reachable. Re-applying a patch as a new commit produces
   a different SHA, which the revision checks rightly reject.

Each runtime tag records what it adds. For example `mag-recomp/runtime-2026.11.08` brought the
Vulkan RSX backend on Windows, an SDL on-screen keyboard and keyboard pad on Linux, the D3D12
native targets, and opt-in preservation sessions for the guest write watch. It does not yet contain
a merge of the newest upstream release.

## What the fork carries

The fork's changes fall into a few groups. The groups matter more than individual commits, which
are listed in the fork's history.

- **Host portability.** POSIX implementations of PPU lightweight mutexes and condition variables,
  host thread stacks sized for static TLS on glibc, and Windows and Linux builds of the backends.
- **Missing or incorrect HLE behaviour that MAG reaches.** Examples from bring-up: the timezone
  syscall, the process-entry environment and argument storage, directory enumeration, extended
  system-module queries and loading, message-dialog abort, strict-order GPU IO mappings, SPURS
  taskset workload registration and workload flags, and network-dialog results that match the
  configured connection state. Each was a strict stop first and a focused branch with a regression
  after.
- **SPU and SPURS execution.** Private DMA and reservation state for every live SPU context, verified
  residency for lifted job modules, the dispatch and MFC lookup caches, interrupt-aware merged-chunk
  lifting, and scheduler wake-ups driven by SPU commits instead of a fixed timed pass.
- **RSX and presentation.** Checked report and label publication with deferred, ticketed reports;
  inline-image handling without a GPU drain; GPU-side depth views; the Metal, Vulkan and D3D12
  draw engines; and present threads.
- **Media.** Sail playback with an optional FFmpeg 9 decoder for the intro movies.
- **Diagnostics.** Opt-in tracing, sampling and counters (see [Verifying a run](verification.md)).
  All are off by default and never required for a normal build.
- **Extension seams.** Build extensions, lifecycle callbacks and lift hooks that let a port add
  its own code without patching the SDK.

## Upstream

Changes go upstream to `sp00nznet/ps3recomp` (default branch `master`) as small, opt-in pull
requests with synthetic tests and no MAG-specific references. Some fork work has no upstream
counterpart yet, such as the checked-completion report path: those are design discussions rather
than pull requests. When upstream gains its own version of something the fork carries, the fork
adopts or reconciles with upstream's version instead of sending its own.

The open contributions are
[the fork owner's pull requests on upstream](https://github.com/sp00nznet/ps3recomp/pulls?q=is%3Apr+author%3Atekulvw).
