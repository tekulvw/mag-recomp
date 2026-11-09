# Capture preservation (copy on first write)

Early completion releases Main's job at the submit while the capture is still reading the job's
inputs (see [main-early-completion.md](main-early-completion.md)). Today the capture is kept
honest after the fact: a guard at the end of the capture declares it torn when a span it read live
may have changed. Preservation is the alternative we built and measured: the capture reads its
inputs as they were at the submit, so it cannot tear in the first place.

It is opt-in (`MAG_RENDER_EC_PRESERVE=1` on the adapter side, off by default) and did not pay in
the game. It stays in the code, inert, because the runtime side is cheap and a future capture may
need it. The rest of this note records the design and why it did not help.

## Behaviour

1. **At the submit,** the spans learned to change every update are copied eagerly, as without
   preservation (a few hundred spans, under 200 KB per update in the measured scenario).
2. **Also at the submit,** a preservation session opens. The rest of the previous capture's read
   set is write-protected through the runtime's guest write watch and marked *preserve*. Pages
   whose spans are all served by the eager copy are not armed: they need no protection, and they
   are the pages written most often.
3. **The session is sealed** once everything is armed. The seal is the moment the capture's reads
   refer to.
4. **The first write to a preserved page after the seal** copies the page's current bytes into a
   preallocated slot before the page becomes writable; then the write goes ahead as usual. Writes
   announced by the runtime (kernel reads into guest memory, protection changes) preserve the
   same way first.
5. **A capture read** of a span is answered page by page, from the first of:
   - the eager copy;
   - a slot holding that page as of the seal;
   - live memory, when the page has stayed protected since before the seal. The reader checks
     again after copying, and if a slot appeared meanwhile it takes the slot instead.
6. **Unpreserved pages.** A page that was never armed (a span that is new this update), whose
   arming was refused, or that was written when no slot was free cannot be had as of the seal.
   The read then returns live bytes and the ordinary guard checks them, so such a span can still
   tear.
7. **At capture end** the session closes. A slot is reusable once no open session still needs it.

Every capture read path has to go through this lookup, not only the plain guest copy: in-place
compares against retained data, reads of textures located through the RSX address translation,
and the capture's diagnostic window. Retained proxies (spans the adapter re-uses because the write
watch says they are clean) need one more rule: a proxy verified during an early capture against
bytes as of the submit must not be armed as if the live bytes matched, because a write between
the submit and the arming would then leave it "clean" with stale content. Such a proxy is left
unarmed and counted. This rule applies whenever early completion is on, with or without
preservation.

## Runtime side

The copy has to happen between "the fault handler has claimed the page" and "the page is made
writable", which only the runtime can reach, so this part lives in our ps3recomp fork. It is in the
pinned runtime (`mag-recomp/runtime-2026.11.08`): `runtime/memory/vm_watch.h` declares
`vm_preserve_begin`, `vm_watch_preserve`, `vm_preserve_seal`, `vm_preserve_read`,
`vm_preserve_end` and `vm_preserve_stats`, behind the presence macro `PS3RECOMP_VM_PRESERVE_API`.
It does nothing unless `PS3RECOMP_VM_WRITE_WATCH=1` and a session is open.

- **Memory is allocated up front,** by the first session: a slot pool sized by
  `PS3RECOMP_VM_PRESERVE_POOL_MB` (default 32 MB, 2,048 slots of 16 KB on Apple Silicon), and
  per-host-page tables for the arming epoch and the newest slot. At most four sessions are open
  at once.
- **The fault path stays signal-safe.** It uses only atomics, a `memcpy` from a page that is still
  readable into the preallocated pool, and the protection change it already made. There is no
  allocation and no lock, and the copy cannot fault again. When no slot is free, the page is
  released as usual and counted as lost; readers then see it as unpreserved.
- **Slots form newest-first chains per page** and are published with a sequence counter, so a
  reader copying a slot can detect a concurrent reuse and retry. Readers never block writers:
  they only read, and the page stays protected until its slot is published. Ordering makes the
  live read correct: a write needs the page unprotected, and the unprotect follows the slot's
  publication.
- **Windows** runs the same claim and release code from its vectored exception handler, using
  the same primitives it already used there. Host pages are 4 KB on Windows and Linux.
- **Tests:** `tests/test_vm_preserve.c` in the runtime covers single and multi-page spans written
  after the seal, spans mixing preserved and live pages, writer threads stamping per-page
  generations while a reader reads repeatedly (the reader must never see a write after the seal),
  pool exhaustion, the announced-write and protection-change paths, and an unchanged fault path
  when no session is open.

## Cost

Measured on one Apple Silicon Mac, one thread, 16 KB pages: a first write to a watched page costs
about 3.4 µs, and about 4.0 µs with the copy, so preservation adds about 0.65 µs per fault. Arming
costs about 0.74 µs per page. In the game, with many threads, the protection changes cost more.

## What the game showed

Preservation did not reduce tears, and it made the capture longer (about 9.7–10.0 ms against
8.6 ms).

- **Every remaining tear was on a span at a new address each update,** mostly the submitted job's
  own command block, whose slot in the job pool changes every update. The previous capture's read
  set cannot cover such a span, so it was always read live.
- **The capture slowed down** because about a thousand reads per capture found no preserved page
  and were read twice, in-place matches against retained data had to copy instead, and thousands
  of RSX-memory spans were armed at every submit.
- **Only about 40 pages per capture were actually preserved,** close to the 35–50 write-watch
  faults per capture we counted.

The fix that addressed the real cause was simpler: always copy the submitted job's command block
at the submit (see [main-early-completion.md](main-early-completion.md)).

## Counters

`[rust-early-completion]` adds, under preservation: pages read from slots, unpreserved page reads,
preserved faults and bytes copied, the pool's high-water mark and pool-full events, write-watch
faults per capture and per update, and pages re-protected per submit. A torn capture under
preservation prints its spans, because any tear there is unexpected.
