# State-first Main

`MAG_RENDER_MAIN_STATE_FIRST=1` (default off) splits Main's commit in two:
- **At the FIFO,** a state-only plan publishes Main's decoder state into a queue position
  reserved for Main's native work, so the FIFO drains on.
- **Later,** the full plan and the encode finish on another thread and fill the reserved
  position.

## The budget

The design only pays if the reserved position is filled before the GPU needs it. The FIFO thread
saves the full plan time; the late half adds a hand-off, a second plan (or a lease reused from the
prepared plan) and its own submit. The late commit therefore has to land within about one
update's slack after the boundary.

The adapter measures this directly: `[rust-main-state-first]` reports the time from the boundary
to the late commit (mean, maximum and a histogram), alongside its attempts, publications and
every fallback reason.

## What we measured

- **On its own,** state-first took Main off the FIFO thread (Main's FIFO time fell to about
  1.6 ms). The update still got slower, about 31–32 fps against 37–38 fps: the late commit and
  its fallbacks cost more than the FIFO saved.
- **With early completion and the present thread,** the result was the same (about 29 ms per
  update against about 24 ms).

**Conclusion:** state-first is parked, off by default. A narrower variant, publishing state as
soon as the materials land rather than at the boundary, is the only form still worth trying.
