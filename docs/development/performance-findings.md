# Performance findings

This page summarises the performance work on the recompiled game's normal path (CPU, SPU, RSX and
the GPU-offload experiments) from October 2026, and what each step measured. The opt-in Rust
render frontend has its own record:
[render frontend performance history](../render-frontend/design/performance-history.md).

All figures are from Apple Silicon macOS in the retail Training mission unless a row says
otherwise. They are historical: each was measured on the build of its day, under the rules in
[Verifying a run](verification.md), and absolute values are not comparable across rows measured
with different methods. "Update" means one accepted game update of the engine's main loop; "unique
guest frames per second" counts newly rendered frames, not display presentations. The switches that
came out of this work and are now on by default are listed, with reasons, in
[ps3recomp-flags.md](../ps3recomp-flags.md).

## Where the time goes

The engine's main thread spends most of each update waiting for render jobs to finish on the
emulated SPUs. On 20 October about 93% of the main thread's time in the mission was such a wait,
and five jobs accounted for about 90% of the update: the material-flush jobs for depth-normal,
main, shadow and other passes, and the multi-camera cull. That has held since, in different
proportions: at about 51 ms per update in late October, waits on the Main, Shadow and DepthNormal
jobs were 43%, 14% and 14% of the update.

So the frame is bounded by how fast SPU job code runs and by how quickly the runtime around it
(dispatch, DMA, locks, the FIFO walker that feeds the GPU) responds. Most of the gains below came
from that runtime, not from the job code.

## Steps that were kept

| Change | Before | After |
| --- | ---: | ---: |
| Inline-image packets no longer wait for the GPU to drain first (main menu) | 9 guest flips/s | 57 guest flips/s |
| Every streamed WWS job module lifted, selected by the module the job manager actually loaded | 411 ms/update | 221 ms/update |
| Runtime overhead round: no per-packet `getenv`, FIFO drain woken by the put pointer, lock-free lock-line reads, longer spin before parking, texture lookup memo, pooled Metal staging, residency kept across data DMAs | 139 ms/update | 66 ms/update |
| FIFO walker per draw: pipeline-key memo, vectorised depth rows, attribute-wise vertex fetch, GPU-side depth store and alias texture, one-pass index runs | 60 ms/update | 56 ms/update |
| SPU jobs: dispatch cache, native single-precision float under round-toward-zero, SPURS coordinator woken by SPU commits | 56 ms/update | 48–51 ms/update |
| Accepted runtime defaults applied on every host, not only macOS (Linux, installer build) | 4.6 guest frames/s | 21.5 guest frames/s |

Notes on these:

- **Inline images.** The game patches fragment-program constants through inline-image packets about
  a thousand times per menu frame. Draining the GPU before each one held the menu near 10 updates/s.
  Publishing pending reports without the drain keeps their order and removed the stall. The mission
  did not change at the time, because its bottleneck was elsewhere.
- **Lifted jobs.** Before this, about 2.6 SPU threads were continuously interpreting job code. A
  first attempt identified resident code by page protection; it was correct but doubled the mission
  time, because job loads rewrite the code buffer about 145,000 times a second. Selecting the
  lifted module by which module was loaded avoided that. Jobs also DMA data into their module image
  above the code, so the resident span covers the code alone.
- **Lock line.** The global lock-line lock saw about 1.2 million acquisitions a second, mostly job
  manager polling and PPU coherent stores, with waiters parked in the kernel. Removing the reads from
  the lock and spinning briefly before parking took lock waits off the top of the profile.
- **Interrupts in merged lifts.** Lifting job loops as merged chunks first hung the mission load,
  because the merged code skipped pending SPU interrupts that the jobs rely on for DMA-list
  notifications. The lifter now returns to the drain at a merged chunk's entry when an enabled
  event is pending.
- **SPU float.** Native single-precision float under round-toward-zero and flush-to-zero (arm64)
  sped up the heaviest geometry job's runs by a few percent, and the commit-driven coordinator wake
  cut worker idle gaps from about 1.6 ms to about 0.5 ms at the 90th percentile.
- **Bugs found on the way.** An unsynchronised file-descriptor table gave two audio streams the same
  descriptor and crashed reads intermittently; it was fixed in the SDK's file system code.

On 24 October the normal build ran at about 21.7 unique guest frames/s over two 60-second windows,
while the display showed about 33.7 presentations/s; the difference is repeated frames.

## Tried and not kept

| Attempt | Result |
| --- | --- |
| Generated PPU code and the SDK at O3 instead of O1 and O2 | No change (one matched pair) |
| One render job's translation unit at O3 instead of O2 | No end-to-end change |
| Register promotion in lifted SPU code | 19% fewer instructions in the hottest function, no change in job run time: the code is bound by local-store loads and spills around float helpers |
| Committing the deferred-report batch at every label | Stalled GPU completions in the mission |
| A smaller deferred-report batch (8 instead of 32) | Label latency fell, but command-buffer commits quadrupled, the walker saturated and updates slowed |
| Publishing completed labels after every draw | No change: the latency is the command-buffer round trip, not the batch |
| A DMA repeat back-off | No change: the job really is waiting on the RSX |
| Polling the SPURS coordinator every 50 µs | Same gain as the commit-driven wake, for a whole core of extra CPU |
| Real wake-ups for lightweight condition variables | No change: those waits were idle-worker timeouts, not on the frame path |
| A ready-demand SPURS scheduling target | No change |
| Pumping GCM from the timer | No change in menu update rate |
| Skipping empty Metal command buffers | Faster menu cadence in one pair, no robust mission change; stays off |
| Higher QoS class for the SPURS workers on macOS | No benefit |
| An overlay-discovery filter for SPU GET transfers | No measurable benefit |
| Batching vertex-program uploads | Exercised, no throughput gain |
| An inlined SPU drain launch checkpoint | About 2% more unique guest frames in one matched pair; too small to settle on its own |

A further round removed most of the cost of PPU plain stores to lines that only ever held a PPU
reservation (they no longer run the SPU holder scans) and gated the guest load and store watch
hooks behind one inlined load. That made the main thread cheaper but did not move the update time,
because the main thread was already waiting on jobs. The lock-line lock is still global; per-line
shards or a cheaper reservation index are the obvious next step there.

## Diagnostic removals

To find what limits an update, opt-in diagnostic stages emptied whole groups of render work while
keeping the game's own submission, synchronisation and completion paths. These are causal
experiments with frozen visibility, not optimisations or playable results.

- With draw preparation bypassed and four material-flush families emptied, the reduced workload ran
  at about 10 updates/s. Also emptying two world-graph stages raised it to about 24, and freezing
  particles as well to about 29.
- Omitting the guest's GPU geometry issue, and then CPU draw preparation as well, gave no large
  change, so neither was the limiter at that point.
- A dynamic-light job's own run took about 0.6 ms, but the main thread waited about 12 ms for it,
  because other jobs ran before it on the same SPU. Long waits are not necessarily long jobs.

## GPU offload

In late October the question was whether moving render preparation to the GPU would help.

- **Decision.** A backend-only change (for example converting vertices on the GPU) leaves all the
  CPU work that produces the commands in place. The useful boundary is earlier: publish changed
  scene inputs once and keep intermediates on the GPU through the last pass that reads them. A
  single command-buffer commit per frame was rejected as a goal in itself, because the guest
  consumes completed reports and labels before it produces later work.
- **Backend vertex conversion.** Implemented, bit-exact against the original pipeline (256 draws,
  colour and depth identical), and slower: 1.00 ms on the GPU path against 0.89 ms on the CPU per
  warm frame. It stays off.
- **Connected geometry preparation on the GPU.** Admitted only about 1.3% of geometry candidates in
  a live mission; most were refused because their material state was not yet published. Total
  update time stayed near 51 ms. No overall speedup was established.

That work led to the opt-in Rust render frontend, which replaces render preparation rather than
accelerating parts of it. Its measurements continue in the
[render frontend performance history](../render-frontend/design/performance-history.md).
