# The FIFO thread and the native worker

## Where the FIFO thread's time goes

The runtime's opt-in busy accounting (`PS3RECOMP_RSX_FIFO_BUSY=1`) splits each update's FIFO
thread time into nested, exclusive categories:
- idle, waiting for the put pointer;
- drain, draw, sync, flush and hand-off;
- present, drawable and pump;
- lease waits;
- extension categories: Main, Depth, Shadow and the frame poll.

It prints one `[rsx fifo busy]` line per window.

**Findings:**
- **Main dominates.** Main was about 70% of FIFO busy time (about 12 ms of about 17.5 ms per
  update). Draws, drains and presents together were about 3.3 ms.
- **The game thread usually sets the update time, not the FIFO.** With chunked planning, FIFO busy
  fell below 16.7 ms while idle time grew by the same amount and the frame rate stayed put.
- **Early completion moves the game thread's wait onto the FIFO.** Idle time fell from about 11 ms
  to 2 ms, and Main's share rose by about 10 ms, mostly waiting for the capture.
- **A present thread on its own was neutral.** It moved present off the walker (about 0.5 ms to
  0.04 ms) and changed nothing else.

## The native worker's synchronous poll

The frame poll runs on the FIFO thread between drains. With `MAG_RENDER_WORKER_POLL_NOWAIT=1` it
returns immediately while the native worker is busy. When the worker is idle, the poll runs
synchronously on the worker, and the FIFO waits for its reply. That synchronous poll retires
completed submissions: it destroys each submission's per-pass objects and frees the buffers no
longer referenced.

### Why it grew with chunked planning on Linux

- Each upload arena of a packet becomes its own device buffer on Vulkan. The buffer is created at
  submit and freed in that poll.
- The chunked merge moves each chunk's arenas into the packet rather than copying them, so a
  K-chunk packet carries up to 3K arenas instead of 3.
- On Linux, each device allocation and free is a kernel buffer-object operation. The poll wait
  therefore grew by about 1.5–1.9 ms per update at K=4 and K=6, and its worst single wait reached
  11–14 ms. That matched the K=6 p95 jump.

### What fixed it

- **`MAG_RENDER_RESIDENT_ARENAS=1`** (default off, in `crates/mag-render-native`) rewrites
  upload arena buffers whose submission has completed, in place, instead of creating and freeing
  one per submission. A buffer goes back to the free list only after its submission's real
  completion, and the in-place write refuses while any use is still pending.
  - On Linux this cut the poll wait from about 3.3 ms to 1.1 ms per update and its worst wait
    from about 7.7 ms to 2.5 ms.
  - The control arm's update time improved from about 36.3 ms to 33.2 ms.
  - The reuse pool held at most 16 free buffers, so at K=4 (about 17 arenas per submission) it
    overflowed and still freed buffers in the poll. The pool should be sized from use: at least
    twice the most arenas a recent submission used.
- **`MAG_RENDER_PACKET_RETIRE=1`** (default off) drops the adapter's last reference to a
  released packet on a helper thread instead of inside the poll. On Linux it recovered only about
  0.3–0.6 ms, because the dominant cost was the device buffer frees, not the packet's CPU memory.

On Metal, buffer creation and release are cheap, and the poll stays small (0.03–0.5 ms).
