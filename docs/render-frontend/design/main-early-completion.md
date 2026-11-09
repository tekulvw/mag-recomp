# Early completion, the torn-capture guard and the re-commit

## Deferred capture

With `MAG_RENDER_DEFERRED_CAPTURE=1`, the heavy part of Main's capture (geometry, materials,
resources) runs on a worker after the job is submitted, instead of inline in the submit. The game
is still held until the capture ends: the capture reads the job's inputs from guest memory, and
the game may reuse them as soon as it sees the job complete.

## Early completion

`MAG_RENDER_EARLY_COMPLETION=1` (default off) releases the job at the submit, so the game thread
no longer waits for the capture. It needs the deferred capture and the guest write watch
(`MAG_RENDER_WRITE_WATCH=1` with the runtime's `PS3RECOMP_VM_WRITE_WATCH=1`). To keep the
capture's inputs those of the submit, it uses four mechanisms:

- **Snapshot at submit.** The spans that change from one update to the next are copied
  synchronously, and the worker reads them from that copy. The spans are learned from earlier
  captures by capture part and record, not by fixed address: every span the skinning part read,
  plus every span the guard saw really change. The submitted job's own command block is always
  copied. Its slot in the job pool differs every update, so learning by address never finds it,
  and the game reuses that slot as soon as the job is released.
- **Watch at submit.** Every span the previous capture read is write-protected through the watch.
- **Guard at capture end.** A span that was read live (outside the snapshot) tears the capture in
  either of two cases:
  - its bytes no longer match what was read, so it changed after the read;
  - it was written since the submit, and what was read differs from the previous capture's
    bytes, so it changed before the read.

  A torn capture fails, so that frame's own packet is never planned. What the frame commits
  instead is the re-commit below.
- **Valve.** Twelve torn captures within the last 240 early ones latch early completion off for
  the rest of that VM's run. The deferred capture then holds the game again, as before.

Two fixes came out of routing every capture read through the snapshot:
- **Retained proxies.** A span the adapter re-uses because the watch says it is clean must not be
  armed after being verified only against the snapshot's bytes: a write between the submit and
  the arming would leave it "clean" with stale content. Such a proxy is left unarmed.
- **Per-read locks.** Looking up the snapshot took a global lock on every capture read. Once
  material ranges were captured in parallel with geometry, that lock made the parallel capture
  slower than the serial one (see R92 and R93 in [performance-history.md](performance-history.md)).
  The lookup now uses a per-thread cache refreshed by generation and a sharded served set.

**What it bought:** the game thread's wait moved onto the FIFO thread, which now waits for the
capture (about 8.6 ms) and then plans. FIFO busy rose from about 15 ms to 22–24 ms per update.
On its own the update improved by 1–2 fps, close to run-to-run noise; it became worthwhile once
the capture itself got shorter (R93 and R94).

## The torn re-commit

A torn capture used to decline that frame's Main commit. The frame then had no Main draws and no
depth prepass, and the stale target showed as a black flicker every few seconds. Under early
completion, five to fifteen frames per run tore this way, and torn captures equalled declined
commits in every arm.

The re-commit is on whenever early completion is on; `MAG_RENDER_TORN_RECOMMIT=0` turns it off
for comparison. It works like this:
- After every Main packet the native worker accepts, the adapter keeps that packet (shared, not
  copied), the images it imported (each with the request it was resolved from), its image writes
  and its final decoder state. A later accepted packet replaces it; a VM change drops it.
- A torn frame commits the kept packet again, stamped with its own identity. Its imports are
  asked for again at this boundary first: each must still resolve, with the same format and
  extent, and with at least the planes it had when kept. The kept packet is not consumed, so
  consecutive torn frames commit it again.
- If any import is stale (gone, resized, reformatted or less initialised), the frame commits one
  explicit pass on Main's output instead. That pass loads the output, which is the last
  committed Main image, when it still resolves as kept and initialised, and clears it otherwise.
  Only an output that no longer resolves leaves the frame declined.
- Loading the output approximates "show the previous frame". A true copy of the presented image
  would need the runtime's presenter.

**Measured:** declined commits dropped to zero on every early-completion arm, against five to
seven on the arm with the re-commit off. Re-commits equalled torn captures, none were refused, and
there was no measurable cost.

## Copy-on-write capture

We also built a capture that reads its inputs as of the submit instead of guarding them
afterwards (`MAG_RENDER_EC_PRESERVE=1`, default off). It did not reduce tears, because the
remaining tears were on spans at a new address every update, and it lengthened the capture. The
design, its runtime support and the measurements are in
[capture-preservation.md](capture-preservation.md).

## Counters

- Every 120 early captures, `[rust-early-completion]` prints, among others:
  - capture and queue times;
  - torn captures, split by after-read and before-read;
  - torn spans by class: the job's own block, another job-pool record, or anything else;
  - write-watch faults per capture and per update;
  - the re-commit outcomes (re-committed, kept output, cleared output, declined, refused);
  - the preservation counts.
- `[rust-torn-recommit]` lists the first re-commit outcomes.
- `[rust-early-completion] torn capture` lists the first torn captures with their spans' classes.
