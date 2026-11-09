# Performance history

Conclusions from timed rounds of the opt-in native Main path on one Apple Silicon Mac, in a fixed
hosted Sabotage scenario (20-second windows, two runs per arm). Each round compares arms of the
same build. Numbers are median milliseconds per update unless stated otherwise. Compare only
within a round: server and route changes between rounds move the baseline.

"FIFO busy" is the RSX FIFO thread's busy time per update from the runtime's opt-in busy
accounting (`PS3RECOMP_RSX_FIFO_BUSY=1`, in the pinned runtime); "Main" is the part of it spent in
Main's hand-off, including any wait for the capture under early completion. See
[fifo-and-native-worker.md](fifo-and-native-worker.md).

## Before R89

Earlier rounds established that the native Main path is faster than the original renderer in the
same scenario on the same Mac (about 27–28 fps against about 17 fps, once transparent and skinned
groups were covered). From R89 on, the question was where the remaining time goes and how much
of it the FIFO thread can shed.

## R89 to R94

| Round | Question | Conclusion |
| --- | --- | --- |
| R89 | Where does the FIFO thread's time go? Do the experimental tier-1 build and the runtime's SPU task pool help? | Main is about 70% of FIFO busy time (about 12 of 17.5 ms); draws, drains and presents together are about 3.3 ms. Tier-1 and the task pool are within noise. |
| R90 | Does chunked planning (K=4) help? Does a present thread? Does state-first? | K=4 cuts Main's FIFO time by about 2.4 ms and puts FIFO busy under 16.7 ms; frame rate is unchanged because the game thread sets the pace (FIFO idle time grows by the same amount). The present thread alone is neutral. State-first is worse (31–32 fps against 37–38). |
| R91 | Early completion on top of chunked planning | The game-thread wait moves onto the FIFO: FIFO idle falls from about 11 ms to 2 ms and FIFO busy rises to 22–24 ms. K=6 with early completion is the best arm (about 39 fps); K=6 plans about 0.6 ms faster than K=4. |
| R92 | Do capture splits (chunked materials, materials first) shorten the capture? | No: the capture stays at about 8.7–8.9 ms, or grows to about 10.1 ms with materials first. The cause is a per-read global lock, fixed for R93. Variants that change geometry ranges tear more. Torn captures equal declined commits in every early-completion arm, which is the black flicker. |
| R93 | The torn re-commit; copy-on-write capture; whether the lock fix cures the split; chunked resources; state-first with early completion | Control about 25.7–26.7 ms. The re-commit removes every declined commit, with no measurable cost. Copy-on-write does not reduce tears (they are at per-update addresses) and makes the update slower (about 25–26 ms against 23.7–24.6 ms). With the lock fix, the split capture shortens to about 7.5 ms and gives the best update (about 23.2–24.2 ms). Chunked resources also help (about 23.4–24.1 ms). State-first stays worse (about 29 ms). |
| R94 | Chunked resources with the split capture; the packet retire thread; the cost split; diagnostics off | Control about 25.7–26.1 ms. Split capture plus chunked resources is the new best (about 21.7–22.3 ms, 43–44 fps). The retire thread and the cost split are within noise on Metal, though the cost split gives the best p95 (balance 0.88–0.93). Turning off the timing and log flags saves about 1.2 ms on the control. |

## Where that leaves the Main path

- The measured wins are chunked planning (K=6), early completion with the torn re-commit, and a
  shorter capture (materials captured in ranges alongside geometry, and resources in ranges).
- State-first and copy-on-write capture are parked, off by default.
- The FIFO thread is no longer the limit on its own: with early completion it mostly waits for
  the capture, so the capture's length is the lever. Streaming Main's encode
  ([main-late-encode.md](main-late-encode.md)) is the next design if the FIFO becomes the limit
  again.

The Linux findings (resident arenas fixing the native worker's poll growth, and the cost split's
balance) are in [fifo-and-native-worker.md](fifo-and-native-worker.md) and
[main-chunked-planning.md](main-chunked-planning.md).
