# Performance history

Conclusions from timed rounds of the opt-in native Main path on one Apple Silicon Mac, in a fixed
hosted scenario. Each round compares arms of the same build. Numbers are median milliseconds per
update unless stated otherwise. Compare only within a round: server and route changes between
rounds move the baseline.

| Round | Question | Conclusion |
| --- | --- | --- |
| R89 | Where does the FIFO thread's time go? Do the experimental tier-1 build and SPU task pool help? | Main is about 70% of FIFO busy time. Tier-1 and the task pool are within noise. |
| R90 | Does chunked planning (K=4) help? Does a present thread? Does state-first? | K=4 cuts Main's FIFO time by about 2.4 ms and puts FIFO busy under 16.7 ms; frame rate is unchanged because the game thread sets the pace. The present thread alone is neutral. State-first is worse (31–32 fps against 37–38). |
| R91 | Early completion on top of chunked planning | The game-thread wait moves onto the FIFO (FIFO busy 22–24 ms). K=6 with early completion is the best arm (about 39 fps). |
| R92 | Do capture splits (chunked materials, materials first) shorten the capture? | No: capture time stays at 8.7 ms or gets longer. The cause is a per-read global lock, fixed for R93. Torn captures equal declined commits, which is the black flicker. |
| R93 | The torn re-commit; copy-on-write capture; whether the lock fix cures the split; chunked resources; state-first with early completion | The re-commit removes every declined commit, with no measurable cost. Copy-on-write does not reduce tears (they are at per-update addresses) and lengthens the capture. With the lock fix, the split capture shortens to about 7.5 ms and gives the best update (about 23.2 ms). Chunked resources also help. State-first stays worse (about 29 ms). |
| R94 | Chunked resources with the split capture; the packet retire thread; the cost split; diagnostics off | Split capture plus chunked resources is the new best (about 21.7–22.3 ms). The retire thread and the cost split are within noise on Metal, though the cost split gives the best p95 (balance 0.88–0.93). Turning off the timing and log flags saves about 1.2 ms. |

The Linux findings (resident arenas fixing the native worker's poll growth, and the cost split's
balance) are in [fifo-and-native-worker.md](fifo-and-native-worker.md) and
[main-chunked-planning.md](main-chunked-planning.md).
