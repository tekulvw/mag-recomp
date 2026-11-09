# Verifying a run

A game run produces logs, traces and counters. Most of them can be misread. This page says what a
run has to show before it supports a claim, how frame rate is counted, and which offline tools in
`tools/` read the opt-in diagnostics. Everything a run produces stays in the Git-ignored `runs/`
directory: logs, traces, captures and frame dumps are never committed.

## What a run can and cannot show

Keep these four kinds of result apart. Each needs its own evidence, and none implies another.

| Result | Shown by | Not shown by |
| --- | --- | --- |
| Navigation | The game reaching an explicit stage (main menu, mission loaded, spawned, HUD up) | A HUD asset appearing in a log; a process still running |
| Frame rate | Unique guest frames per second over a stated window (below) | Display presentations per second, the window title's FPS, a CPU timing |
| Rendering | A person looking at the screen, or a capture compared against a reference | A capture file existing; draw counters; GPU completions |
| Audio | A person listening with the real output driver | Copy and consumption counters, which can show stutter risk but not audible quality |

A tool that exits 0 has produced its report without detecting an error in its inputs. It has not
passed the game, the sound or the rendering. Read the report's qualification and error fields.

## Counting frame rate

- **Count unique guest frames.** The host may present the same guest frame more than once; in one
  measured Training window about a third of all presentations repeated an already shown frame. The
  display rate is therefore higher than the rate at which the game produces new frames. Report
  unique guest frames per second, and give the display rate only as a separate number.
- **Physical timing needs physical timestamps.** A presentation without a positive display
  timestamp cannot be counted. On a locked macOS session the system supplies none, so a run there
  can show world progress but no displayed frame rate.
- **Use whole windows.** Measure fixed windows (60 s is typical) after a warm-up, and state the
  window. Sparse one-second traces taken inside a window describe those samples only; do not scale
  their counts up to the whole window.
- **Expect spread.** An unchanged build varied by about ±4 ms per update between runs on the same
  machine. A smaller difference from one pair of runs is not a result.
- **Keep observers out of timing windows.** Diagnostics cost time: a residency statistics switch
  once accounted for about 6% of SPU worker time, a per-prepare CPU clock read cost about 3% of the
  FIFO thread, and an external stack sampler slowed updates by about 8%. Profile in a separate
  window from the one you time, and turn off observers the measurement does not use.

## Running a comparison

- Build each arm from recorded sources, keep its build receipt next to the binary, and check that
  the binary you launch is the one the receipt names.
- Launch the exact process yourself and stop it yourself when done, then confirm it has exited.
  Do not attach to, or stop, a process somebody else started.
- Use the same scene, the same route and the same settings in both arms. An A/B inside one process
  (switching an opt-in stage on part-way) removes most scene and machine variation, but its
  transition period belongs to neither arm.
- Trace buffers have a fixed capacity. A trace that dropped events is incomplete, so shorten the
  window rather than accept it. As the engine got faster, the usable trace window shrank from 2.5 s
  to 1 s.
- Read logs at a fixed cut-off (a byte prefix with its digest) rather than while they are still
  being written, and write each report to a new file instead of overwriting an earlier one.
- Keep failed runs. A run that stopped at a failed check is evidence of that failure; it is not
  rerun quietly and replaced.

## Offline assessors

These tools read evidence a run already wrote. They never start, control or attach to a process.
Give each report a new path: most of them refuse to overwrite an existing file
(`graphics_critical_assess` does not check, and `audio_chronology_assess` prints to standard output
without `--output`). Run them as modules from the repository root. Each has synthetic tests under
`tests/`.

| Tool | Reads | Reports |
| --- | --- | --- |
| `tools.audio_chronology_assess` | The SDK's opt-in audio chronology log | Copy and consumption rates, dropped, pending and overlapping records, runs of zero-magnitude blocks, stutter-risk spans |
| `tools.ppu_callspan_assess` | A fixed log prefix with PPU call-span records and audio records | Call durations grouped by entry flags, with unmatched starts kept separate; missing span data is an error, never zero calls |
| `tools.ppu_wait_notes_assess` | The same prefix plus PPU wait notes | Sleep brackets inside completed spans, and workload queries during them |
| `tools.draw_outcomes_assess` | The draw engine's opt-in per-draw observer output (`RSX_ENGINE_DRAW_OBSERVER`), optionally with the launch environment | Per-frame draw outcomes and program-content groups for complete interior frames; boundary frames and capped tails stay partial |
| `tools.graphics_critical_assess` | A critical-path trace capture (JSON events plus drop counts) | PPU scope intervals, report and flush totals, presentation timing, and the joins it could not prove; a trace with lost events is never complete |

```sh
python3 -m tools.audio_chronology_assess --log runs/<run>/audio.log --output runs/<run>/audio-report.json
python3 -m tools.draw_outcomes_assess runs/<run>/draws.jsonl --output runs/<run>/draws-report.json
```

Their reports carry the same limits as the table above. A matching callback rate does not prove
which job owns a callback, a content hash does not prove that a GPU used the program, and a wait
inside a span is wall time, not CPU time.
