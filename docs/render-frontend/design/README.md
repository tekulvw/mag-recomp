# Render frontend design notes

These notes describe mechanisms of the game adapter's Main path and what measurements taught us
about them. They are our own designs; none of them describes the original program. The
behaviour the frontend must reproduce is specified separately, in
[../behavior/README.md](../behavior/README.md).

| Note | Covers |
| --- | --- |
| [main-chunked-planning.md](main-chunked-planning.md) | Planning Main's items in parallel ranges, and choosing the ranges |
| [main-early-completion.md](main-early-completion.md) | Releasing the game before the capture ends, the torn-capture guard and the re-commit |
| [capture-preservation.md](capture-preservation.md) | Reading the capture's inputs as of the submit with copy on first write, and why it did not pay |
| [main-state-first.md](main-state-first.md) | Publishing Main's state early and committing its encode late, and the latency budget that rules it |
| [main-late-encode.md](main-late-encode.md) | Streaming Main's encode in chunks, retained per-item planning and a fused capture and plan (designs, not built) |
| [fifo-and-native-worker.md](fifo-and-native-worker.md) | Where the FIFO thread's time goes, and the native worker's synchronous poll |
| [performance-history.md](performance-history.md) | What each measured round showed, as conclusions |

**Status.** Most of the adapter code these notes describe still lives in the separate adapter
checkout until it moves into `crates/mag-render-adapter-*` (see
[render-frontend.md](../../render-frontend.md)). The parts that are already public are the native
crate's position reservations and resident upload arenas (`crates/mag-render-native`), and, in the
pinned ps3recomp runtime (`mag-recomp/runtime-2026.11.08`), the FIFO busy accounting, the present
thread, the guest write watch and its preservation sessions. Every mechanism here is opt-in behind
an environment variable and changes nothing when it is unset.

Measurements come from timed rounds of a fixed hosted scenario on one Apple Silicon Mac, and from
a Training scenario on one Linux x86_64 host. Numbers are medians across arms of the same round
unless a note says otherwise. Compare arms only within a round: server and route changes between
rounds move the baseline.
