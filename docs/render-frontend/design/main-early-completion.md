# Early completion, the torn-capture guard and the re-commit

## Deferred capture

With `MAG_RENDER_DEFERRED_CAPTURE=1`, the heavy part of Main's capture (geometry, materials,
resources) runs on a worker after the job is submitted, instead of inline in the submit. The game
is still held until the capture ends: the capture reads the job's inputs from guest memory, and
the game may reuse them as soon as it sees the job complete.

## Early completion

`MAG_RENDER_EARLY_COMPLETION=1` releases the job at the submit, so the game thread no longer
waits for the capture. It needs the deferred capture and the guest write watch
(`MAG_RENDER_WRITE_WATCH=1` with the runtime's `PS3RECOMP_VM_WRITE_WATCH=1`). To keep the
capture's inputs those of the submit, it uses four mechanisms:

- **Snapshot at submit.** The spans that change from one update to the next are copied
  synchronously, and the worker reads them from that copy. The spans are learned from earlier
  captures by capture part and span: every span the skinning part read, plus every span the guard
  saw really change. The submitted job's own command block is always copied. Its slot in the job
  pool differs every update, so learning by address never finds it, and the game reuses that slot
  as soon as the job is released.
- **Watch at submit.** Every span the previous capture read is write-protected through the watch.
- **Guard at capture end.** A span that was read live (outside the snapshot) tears the capture in
  either of two cases:
  - its bytes no longer match what was read, so it changed after the read;
  - it was written since the submit, and what was read differs from the previous capture's
    bytes, so it changed before the read.
- **Valve.** Twelve torn captures within the last 240 early ones switch early completion off for
  that run.

**What it bought:** the game thread's wait moved onto the FIFO thread, which now waits for the
capture (about 8.6 ms) and then plans. FIFO busy rose from about 15 ms to 22–24 ms per update.
The update time improved by 1–2 fps, close to run-to-run noise.

## The torn re-commit

A torn capture used to decline that frame's Main commit. The frame then had no Main draws and no
depth prepass, and the stale target showed as a black flicker every few seconds. Under early
completion, five to fifteen frames per run tore this way.

The re-commit is on whenever early completion is on; `MAG_RENDER_TORN_RECOMMIT=0` turns it off
for comparison. It works like this:
- After every accepted Main packet, the adapter keeps that packet (shared, not copied), the images
  it imported (each with the request it was resolved from), its image writes and its final state.
- A torn frame commits the kept packet again, stamped with its own identity, after asking for
  every import again at this boundary. Consecutive torn frames commit the same packet again.
- If any import is gone, resized, reformatted or less initialised, the frame instead commits one
  pass on Main's output. That pass loads the output (the last committed Main image) when it still
  resolves as kept, and clears it otherwise. Only an output that no longer resolves leaves the
  frame declined.

**Measured:** declined commits dropped to zero on every early-completion arm, against five to
seven on the arm with the re-commit off. Re-commits equalled torn captures, none were refused, and
there was no measurable cost.

## Copy-on-write capture snapshot: what we tried, and why it did not pay

`MAG_RENDER_EC_PRESERVE=1` (default off; needs the runtime's write-watch preservation sessions)
reads the capture's inputs as of the submit instead of guarding them afterwards.
- At the submit, the rest of the previous capture's read set is armed for preservation.
- The first write to a preserved page copies the page into a preallocated slot before the page
  becomes writable. The copy happens in the fault handler, without allocating.
- Capture reads come from the eager copy, else from the preserved copy, else live.

**In the game, it did not reduce tears.** Every remaining tear was on a span that appears at a
new address every update, mostly the submitted job's command block. The previous capture's read
set cannot cover a span like that, so it was read live. Preservation also made the capture longer
(about 9.7–10.0 ms, against 8.6 ms):
- about a thousand reads per capture found no preserved page and were read twice;
- in-place matches had to be copied instead;
- thousands of RSX-memory spans were armed at every submit.

Only about 40 pages per capture were actually preserved, close to the 35–50 write-watch faults per
capture we counted.

**The fix that addressed the real cause** was the always-copied job command block described
above. Preservation stays in the code, inert. Its runtime cost per fault is small (about 0.65 µs
for a 16 KB page copy on top of a 3.4 µs fault, single-threaded), should a future capture need
it.

## Counters

- `[rust-early-completion]` prints, among others:
  - capture and queue times;
  - torn captures, split by after-read and before-read;
  - torn spans by class: the job's own block, another job-pool record, or anything else;
  - write-watch faults per capture and per frame;
  - the re-commit outcomes;
  - the preservation counts.
- `[rust-torn-recommit]` lists the first re-commit outcomes.
- `[rust-early-completion] torn capture` lists the first torn captures with their spans' classes.
