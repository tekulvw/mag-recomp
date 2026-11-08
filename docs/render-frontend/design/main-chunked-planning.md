# Chunked Main planning

`MAG_RENDER_MAIN_CHUNK_PLAN=K` (K in 2..8, default off) plans Main's items in K contiguous ranges
on K threads, then merges the results into one packet. That packet renders exactly what the serial
planner's packet renders.

## Why

Planning Main (turning the captured cohort into a native scene packet) ran serially on the FIFO
thread. It was the largest single block of FIFO busy time. Items are independent except for the
walk state they inherit (render state, group transitions, pass openings), so the work can be split
once that state is known at each range boundary.

## How it works

1. **Skeleton.** One serial walk over all items, on the real packet, in a state-only mode.
   - There is no item accounting, no implied rasters, and no geometry work after the
     qualification step.
   - It applies every item's state writes, every group transition (with its producer passes) and
     every Main pass opening, so the real packet ends up holding every pass and producer entry.
   - It records each range's starting walk state and effects, and what each transition and opening
     left behind.
2. **Chunks.** Each range is planned in full into a copy of the skeleton packet's tables, starting
   from its recorded state.
   - At a transition or opening, a chunk takes the skeleton's recorded result instead of performing
     it again. A chunk therefore adds only its own items' draws, resources and accounting.
   - The first range runs on the planning thread with its cache. The others run on persistent
     worker threads, each with its own warm compiler cache.
3. **Merge.** In range order, each chunk's resources are interned into the real packet.
   - Its upload buffers are moved over without copying.
   - Its Group-0 jobs are packed again in order.
   - Its draws are appended to their passes.
   - Items are interleaved with the skeleton's producer items in serial order.

**What matches the serial packet:** passes, draw order within each pass, item order, every draw's
resources and bytes, and the final state, statistics and effects.

**What may differ:** resource table indices and upload offsets, because producer entries come
first. The packet is equivalent, not byte-identical. Synthetic tests compare chunked and serial
plans by resolved content at K=2,3,4,6,8.

**Fallback:** any failure (a chunk error, a structure mismatch or a merge refusal) restores the
packet and effects and runs the serial planner. Errors and refusals are therefore always the
serial planner's.

## Making it fast

The first version scaled poorly. The chunks contended on the allocator and started with cold
caches. Three changes fixed most of it:
- persistent workers with per-thread compiler caches;
- reusing per-thread scratch vectors for index and uniform data;
- a lighter skeleton, which skips every per-item cost that only the chunks need.

Merge cost was reduced by moving upload buffers instead of copying them.

## Choosing the ranges

- **Default:** equal item counts.
- **`MAG_RENDER_MAIN_CHUNK_SPLIT=cost` (default off):** equal estimated planning cost instead.
  - Each item's estimate comes from its captured geometry before any walk: a constant, plus terms
    for its draws, the vertices of draws prepared on the CPU, and its indices. Draws prepared on
    the GPU count only as draws.
  - The coefficients start from fixed defaults and are refitted after every plan. A decayed,
    regularised least-squares fit relates each chunk's summed features to its measured time.
  - Any contiguous cut plans the same packet, so the split only moves work between threads.

**Stealing:** once the planning thread finishes its own range, it runs any range no worker has
started yet. A worker that has not woken would otherwise hold up the whole plan. Each job sits in
a slot only one thread can take, and the call waits for all jobs even when it unwinds.

## Counters

Every 120 chunked plans, `[mag main chunk plan]` prints:
- the split mode;
- skeleton, chunk and merge times;
- the slowest and fastest chunk;
- a balance figure: summed chunk time over K times the slowest chunk, where 1.0 is even;
- the mean and worst time from hand-off to a worker chunk's start;
- the chunks the planning thread stole;
- the largest range's share of the estimated cost.

The wall time of the chunk phase always equals the slowest chunk, so "slowest ≈ wall" says nothing
about balance. The balance figure is the one to read.

## What we measured

- **On the Mac,** K=4 cut Main's FIFO time from about 12 ms to 9.7 ms per update, and FIFO busy
  from about 17.5 ms to 15.3 ms.
  - Frame rate did not move by itself, because the game thread set the update time.
  - K=6 planned about 0.6 ms faster than K=4.
  - The lighter skeleton brought the K=6 plan to about 3.4 ms in total.
- **On Linux (8 cores, 16 SMT threads),** the chunk phase did not shrink from K=4 to K=6.
  - Equal-count ranges reached a balance of only 0.66–0.70.
  - The cost split raised it to 0.91 and cut the slowest chunk from about 5 ms to about 4 ms.
  - K=6 also raised the 95th-percentile update by about 11 ms. That came from the native worker's
    poll growing with K (see [fifo-and-native-worker.md](fifo-and-native-worker.md)), not from
    planning.
  - Caution for Linux: the cost split showed occasional p95 spikes, which are still under
    investigation. K=4 is the safe setting there.
