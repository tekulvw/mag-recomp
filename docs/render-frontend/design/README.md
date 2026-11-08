# Render frontend design notes

These notes describe mechanisms of the game adapter's Main path and what measurements taught us
about them. They are our own designs; none of them describes the original program.

| Note | Covers |
| --- | --- |
| [main-chunked-planning.md](main-chunked-planning.md) | Planning Main's items in parallel ranges, and choosing the ranges |
| [main-early-completion.md](main-early-completion.md) | Releasing the game before the capture ends, the torn-capture guard, the re-commit, and why copy-on-write did not pay |
| [main-state-first.md](main-state-first.md) | Publishing Main's state early and its encode late |
| [fifo-and-native-worker.md](fifo-and-native-worker.md) | Where the FIFO thread's time goes, and the native worker's synchronous poll |
| [performance-history.md](performance-history.md) | What each measured round showed, as conclusions |

Status: the code these notes describe lives in the separate adapter checkout until it moves into
`crates/mag-render-adapter-*` (see [render-frontend.md](../../render-frontend.md)). Every
mechanism here is opt-in behind an environment variable and changes nothing when it is unset.

Measurements come from timed rounds of a fixed hosted scenario on one Apple Silicon Mac, and from
a Training scenario on one Linux x86_64 host. Numbers are medians across arms of the same round
unless a note says otherwise. Compare arms only within a round: server and route changes between
rounds move the baseline.
