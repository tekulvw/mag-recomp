# Streaming Main's encode (designs, not built)

These are designs for getting Main's first GPU commands committed within the state-first budget
(about 2–3 ms after Main's FIFO boundary; see [main-state-first.md](main-state-first.md)). Only
their first step, chunked planning with a merged submit, has been built
([main-chunked-planning.md](main-chunked-planning.md)). The rest is recorded so that a later
attempt starts from what we learned.

## Starting point

With present on its own thread (`PS3RECOMP_RSX_PRESENT_THREAD`), the FIFO thread no longer
blocks acquiring a drawable. A late Main still holds back every command buffer queued after its
position, and the FIFO thread can still stall behind the GPU in three places: synchronous flushes
that wait for completion, a full presentation staging ring, and the runtime's frames-ahead bound.

Before chunking, one measured round's means were:

| Stage | Time |
| --- | --- |
| Capture, in total | 9.1 ms |
| Capture: resources (serial) | 1.7 ms |
| Capture: geometry (three concurrent ranges) | 7.3 ms |
| Capture: materials (on a helper) | 5.2 ms |
| From the prepared plan's start to the FIFO boundary | 1.0 ms |
| The prepared plan | 8.6 ms (7.7 ms still left at the boundary) |
| Submit | 3.9 ms |
| Main on the GPU | 6.8 ms |

So Main reached the GPU in one piece about 11.6 ms after the boundary. The target is Main's first
work on the GPU 1–2 ms after the boundary, with the CPU then producing the rest at least as fast
as the GPU runs it, so Main's position never starves the queue.

The observation that makes this possible: the GPU needs Main's first commands when it reaches
Main's position, not all of Main.

## Design: chunked, streamed parts

**Ranges.** K contiguous item ranges (three by default), the same as the capture's geometry
ranges, so chunk k needs only capture range k plus the resources and materials.

**Skeleton.** The state-only walk that already runs at the boundary also records the pass
skeleton (attachments, load, store and clear actions, viewport and scissor), which items go to
which pass, and each chunk's entry state.

**Pass splits.** A pass that a chunk boundary cuts becomes two render passes. The first keeps the
clear or load and is forced to store; the continuation loads, with the same viewport and scissor.
Discarding stores and the resolve stay on the final half only. Tail-group and no-draw items keep
their places. If the native pass limit (16) would be exceeded, the frame falls back to one part.

**Depth prepass.** Draws that test for equal depth need the complete prepass, so every prepass
draw has to come before every equal-test colour draw. Each chunk therefore makes two parts, its
prepass replays P_k and its colour passes C_k, committed in the order P_0 … P_(K−1), then
C_0 … C_(K−1). Joined, the P parts reproduce the single-part prepass list in the same order; only
the render-pass boundaries differ.

**Exactness.** Joining the chunks must equal the single-part plan: the same draws, pipelines and
uniform bytes; each chunk's exit state equal to the next chunk's entry state from the walk; and a
final state equal to the published one. The state chain is checked per chunk and at the end, and
a mismatch stays fatal, so committing early adds no new risk.

**Predicted entry states.** Before the boundary, entry states come from the prepared plan's
predicted answers. The boundary checks them; on a mismatch every chunk is planned again, because
all entry states change.

**Native side.** The native crate would reserve 2K ordered command buffers at Main's position
(generalising today's reserved commit from "passes of one packet" to "one packet per slot") and
accept each part into its slot, checked against the reservation's imports. The last part carries
the completion ticket, and polling waits for every slot. Because execution follows enqueue order,
parts may be encoded and committed in any order. A refusal before any part commits declines the
frame as today; a failure after a commit marks the submission incomplete and counts it. On Vulkan
it uses the reserved-order submission path with 2K ordered submits.

**Late half.** At the boundary, state-first publishes the state and reserves the slots. Planning
runs on pool threads. A single encoder encodes in slot order and commits each part as soon as it
is encoded: P_k when chunk k is planned, then the colour parts. A "hold" policy that commits
everything at the end is kept for comparison and for early completion.

### Torn captures

With early completion off, the job is held until the capture ends, so a capture cannot tear and
parts can stream. With early completion on, the guard's verdict exists only at the end of the
capture, and an encoded, enqueued command buffer cannot be dropped (a decline commits it empty).
So no part is encoded into a reserved buffer before the guard passes; planning and buffer uploads
may go ahead. On a tear, the position is declined, the chunk plans are discarded and the frame
keeps its previous Main. Per-chunk guards were rejected: a later torn chunk would leave a partial
frame.

### Estimates

With K=3, the chunk plans take about a third of the plan plus overhead and finish about 2 ms after
the boundary. P_0, which is depth-only and cheap to encode, would reach the GPU about 2.3 ms after
the boundary, and the colour parts would follow at about 1.2 ms each while the GPU runs Main's
6.8 ms. With a retained plan (below) hitting, P_0 could go at about 1 ms after the boundary.

Caveat: parallel planning of the prepass's vertex packing alone gave no gain in an earlier
attempt, so full-chunk scaling has to be measured before committing to the design. Chunked
planning has since measured it (about 1.9× at K=4 and 2.3× at K=6 in a 3,000-item synthetic
test).

### Risks

- The pass limit: splits add up to K−1 passes, plus the prepass splits.
- The GPU cost of splits: extra tile load and store bandwidth at each split.
- Concurrency in the compiler cache under parallel plans.
- 2K command buffers per Main.
- Speculative plans discarded on a prepared-plan miss (rare: 17 in 2,224 in one round).

## Retained per-item planning

A per-item cache under the chunk planner. Its key is the item's identity plus a digest of the
entry state and inputs that affect the draw structure. A hit splices the retained draws (program
variant, raster and pipeline state, texture bindings, vertex-input layout, packed vertex bytes,
qualification) and re-emits the uniforms from the current values; a miss plans the item normally.
Because entry states are per chunk, retention runs per chunk, in parallel. It would be proven with
byte-identical fixtures against the full planner over changing multi-frame cohorts, plus a sampled
full-plan check at run time that turns retention off on any mismatch.

## Fused per-range capture and plan

Under early completion the FIFO thread waits for the capture (about 8.6 ms) and then runs the
chunked plan (about 4 ms), one after the other. This design overlaps them: chunk k is planned as
soon as its own inputs exist, while other ranges are still being captured.

- **One set of ranges,** chosen at capture start, serves geometry, materials, resources and the
  plan chunks.
- **What chunk k needs:** the packet prelude and the initial state from predicted boundary
  answers (both need only the cohort and the materials' prologue, which finish before any item
  range); the skeleton walk through range k, which needs materials for every item before the end
  of range k but no geometry; and geometry and materials for range k.
- **Out-of-order ranges.** Range jobs post "done" events to one coordinator. The skeleton consumes
  materials ranges strictly in order and never walks a range whose materials have not landed, so
  its state chain is always exact. Chunk k starts once the skeleton has passed range k and
  geometry k has landed; chunks then run in any order, and the merge runs in range order as
  today. The skeleton is cheap (about 0.3 ms in total), so it keeps up.
- **Hand-off.** The finished plan is offered to the FIFO as a prepared plan that finished early;
  the boundary checks its predicted answers as usual.
- **Fallbacks** to today's path: a capture exactness rerun, a skeleton or chunk error, a structure
  mismatch, a prediction miss, or a torn capture. Correctness never depends on this design.

**Status:** the first increment, per-range publication from the capture, is built and inert
without a consumer. The rest is parked: the capture's ranges finish at about the same time, so
overlapping them with the plan would gain only about 0.5–1.5 ms.
