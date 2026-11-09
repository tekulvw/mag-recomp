# State-first Main

`MAG_RENDER_MAIN_STATE_FIRST=1` (default off) splits Main's commit in two:
- **At the FIFO boundary,** the real boundary answers are fetched, a state-only plan runs, Main's
  queue position is reserved with its imports (the native crate's position reservations), and
  Main's decoder state is published at that position. The FIFO then drains on.
- **Later, on another thread,** the full plan runs (usually the prepared plan, made before the
  boundary from predicted boundary answers and checked at the boundary). Its final state must
  equal the published state; a mismatch is fatal. Its packet is then committed into the reserved
  position.

It works end to end: in the measured rounds every published state got its late commit, with no
declines and no state mismatches. It does not pay.

## Why a late Main costs more than it saves

GPU command buffers run in the order they were queued. Everything queued after Main's reserved
position, including later FIFO flushes and the present, waits until the late commit fills it. On
Metal the presentation layer has three drawables, which is already its maximum, and those late
frames hold all three. So the FIFO thread stops blocking on Main and starts blocking on acquiring
the next drawable instead.

Measured on the Mac, state-first against the control arm, per update:
- Main's hand-off on the FIFO thread fell from about 12 ms to about 1.7 ms.
- Waiting for the next drawable rose from about 0.03 ms to about 2.3 ms, and present-time flushes
  from about 0.07 ms to about 2.4 ms.
- A present command buffer took a median of about 19.6 ms from commit to completion, against
  about 13.6 ms.
- The runtime's frames-in-flight and frames-ahead bounds never waited, so they are not the limit.

There is no deeper presentation pipeline to buy, and turning display sync off (tearing) was
rejected.

## The budget

The budget is the time from Main's FIFO boundary until Main's first commands must be committed so
that the GPU never waits on Main's position. Indirectly, from the present latency above: the
state-first arm's present is about 6 ms later than the control's, and its late commit lands at the
boundary plus the rest of the prepared plan (5–7 ms) plus the encode (1–2 ms). That puts the
budget at about **2–3 ms after the boundary**.

The adapter now measures it directly: `[rust-main-state-first]` reports the time from the boundary
to the late commit (mean, maximum and a histogram), alongside its attempts, publications and
every fallback reason.

Neither shortening the plan nor deferring the capture fits on its own: a retained plan (re-using
each item's draw structure from the previous update) would still take about 3.5–4.5 ms plus the
encode, and with the deferred capture the capture itself lands well after the boundary. What can
fit is getting Main's *first* commands to the GPU early and streaming the rest while the GPU runs:
see [main-late-encode.md](main-late-encode.md).

## What we measured

- **On its own,** state-first took Main off the FIFO thread (Main's FIFO time fell to about
  1.6 ms). The update still got slower, about 31–32 fps against 37–38 fps: the late commit and
  its fallbacks cost more than the FIFO saved.
- **With chunked planning, early completion and the present thread,** the result was the same
  (about 29 ms per update against about 24 ms for the same arm without state-first).

**Conclusion:** state-first is parked, off by default. A narrower variant, publishing state as
soon as the materials land rather than at the boundary, is the only form still worth trying.
