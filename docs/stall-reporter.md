# Match stall reporter

When a match freezes (the window stays open, but no new frame is drawn and nothing more is
logged), the engine log used to end with no clue about why. The stall reporter writes, into the
same log, what every guest thread was doing when the frames stopped. A frozen session's report
(its `engine.log` tail) then names the wait.

## Behaviour

- **On by default, silent while the game draws.** A background thread in the engine looks once a
  second at the session monitor's latest presented guest frame (`src/mag_session_monitor.h`). That
  costs one clock read and two relaxed loads per second. Nothing is added to the frame path.
- **Only in a match.** The stall must be in the match phase, with the local player on foot,
  when a threshold passes. Loading screens and menus can pause for a long time legitimately, so
  they are never reported. A frozen game does not change phase, so a freeze in a match stays a
  match stall.
- **Two reports per stall**, at 10 s and at 30 s without a frame. Each one starts with
  `[mag-stall] no guest frame for <s> s in a match (report <n>)`. It then lists:
  - every guest thread inside a firmware (HLE) call or an lv2 syscall, by guest thread id, with
    the call's name. Syscalls the threads most likely wait in are named: waits, `sys_fs_*` and
    sleeps. A stalled file read shows as `sys_fs_read`, and a stalled streamed read
    (`cellFsRead`, a `.skx` stream) shows the same way;
  - `no guest thread is inside a firmware call or syscall` when every thread is running guest code;
  - the process CPU time used since the last frame. A spin burns CPU; a deadlock uses almost none.
    Two reports 20 s apart tell them apart;
  - the host audio counters, when the runtime provides them, which show whether the audio path
    still runs;
  - the runtime's per-thread guest link register listing, `[WATCHDOG] guest-tid N lr=… r3=… sp=…`.
    The `lr` value is the last direct guest call site. For a thread inside a wait, `r3` is the
    object it waits on.
- **One line when frames come back:** `[mag-stall] guest frames resumed after <s> s`. A stall
  that recovers is then visible too.
- **A new stall** is reported again from the first threshold.

## It must not freeze too

A wedged thread may be holding the C library's lock on standard error. The report is therefore
formatted in memory and written with one raw `write` to a duplicate of the error stream's
descriptor, which takes no stdio lock. The runtime's link-register listing comes last because it
uses stdio. If that listing blocks, only the reporter's own thread waits; the report lines are
already in the log.

## Switch

`MAG_STALL_REPORT_SECONDS`: a comma-separated, ascending list of thresholds in seconds (default
`10,30`). `off` or `0` disables the reporter. A malformed value keeps the default.

## Code and tests

- `src/mag_stall_reporter.{h,cpp}`: the decisions (`Watch`), the report text and the thread.
- `src/mag_stall_probe.cpp`: the engine's probe. It reads the runtime's `g_hle_inflight` and
  `g_sc_inflight` records and its `ppu_report_guest_lrs` listing, the process CPU time and the audio
  counters. It is linked only into the game executable.
- `tests/test_stall_reporter.py` covers threshold parsing, when to report (never while drawing,
  never outside a match, once per threshold, again for a new stall, resumed only after a report)
  and the report text. It also runs the real thread against a synthetic clock and probe, with
  frames frozen and then resumed.
