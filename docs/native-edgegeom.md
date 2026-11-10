# Native Edge geometry job (opt-in)

The streamed Edge geometry job is the largest SPU cost in a frame. It turns one mesh partition
into an RSX index list, any produced vertices, and the draw commands in the parent's command
hole. `src/native_edgegeom/` reimplements it natively from
[render-frontend/behavior/edge-geometry-job.md](render-frontend/behavior/edge-geometry-job.md),
under [the clean-room method](native-graphics-reimplementation-method.md).

It is off by default, and it changes nothing unless both the build and the run opt in. An
invocation it does not handle runs the lifted job unchanged.

## Building with it

1. Apply the payload-override lift step to the engine SPU lift. Run it after
   `tools/wws_job_lift.py`; if you also use `tools/graphics_payload_hooks.py`, run this step after
   that one:

   ```sh
   python3 tools/payload_override_hooks.py --directory generated/retail-v1.0/spu-engine-retained
   ```

   This guards the WWS policy's payload call with `mag_payload_override` (`src/mag_extension.h`)
   and records `payload_override_hooks` in that directory's `provenance.json`.
2. Configure the engine with `-DMAG_NATIVE_EDGEGEOM=ON`. Configuring fails if the SPU lift lacks
   the step, because the native path could never run. The receipt lists the implementation's
   sources under `extensions` (`native-edgegeom-sources`, `native-edgegeom-build`).

The same binary runs the lifted job unless the run opts in, so one build serves an A/B.

## Running

| Variable | Effect |
| --- | --- |
| `MAG_NATIVE_EDGEGEOM=1` | Registers the payload override at start (`[native-edgegeom] enabled` on stderr). Any other value, or none, leaves the job lifted. Set it for the whole run; never toggle it within a run (the Depth and Main passes must take positions from the same producer). |
| `MAG_NATIVE_EDGEGEOM_TRACE=N` | Prints the first N job-manager service calls: slot, entry, arguments, context state before the call (status, interrupt enable, interrupt save and frame, pending transfer, host depth, policy mode, image, stack) and the result after it. Diagnostic only. |
| `MAG_NATIVE_EDGEGEOM_FORCE_REFUSE=1` | Diagnostic: refuses every invocation at the latest refusal point (buffers acquired, ring line reserved), reason `forced`, so the fallback to the lifted job can be exercised in game. |
| `MAG_NATIVE_EDGEGEOM_STATS=1` | Every 5 seconds and at exit, prints the number of invocations, handled and refused, the mean native time per handled invocation, and the counts per kind and reason. |

Example report line: `[native-edgegeom]   kind=index_only handled=48211`.

### Kinds and refusal reasons

Admitted kinds: `index_only` (an empty output layout, any group), `instanced_wind` (group 0),
`wind` (group 3), `ripple` (group 4) and `skinned` (groups 7 and 11; normals and tangents are
also turned by the object's rotation and scale). The group and a non-empty output layout select what is produced, and the mode
word and flavour select culling only. Group 3 with produced vertices is therefore the wind under
mode `0x28` (two-sided) and under `0x08`/`0x0A` (back-face culled) alike. Produced vertices the specification does
not cover are refused with `kind`.

| Reason | Meaning |
| --- | --- |
| `identity` | The module's residency record at LS `0x4000` is missing, or the dispatcher's resolver (`spu_lifted_lookup`) does not accept its loaded code. With the counters on, the first refusals also print the residency records |
| `service` | A transfer was pending when the invocation started, so no service call was made |
| `mode`, `blend_shapes`, `configuration`, `hole` | Inputs outside the specified values (admission checks 2–5) |
| `stream_short`, `index_range` | An input stream shorter than its counts need, or an index of `V` or more. Slots 2 and 8 continue slots 1 and 7 past 16 KiB and are counted |
| `kind`, `codec`, `output_format` | A production rule, input codec or output layout this implementation does not implement |
| `non_finite`, `joint`, `budget` | Non-finite inputs or outputs, a palette joint out of range, or more than 65,536 produced vertices |
| `hole_overflow` | The draw does not fit in the parent's hole |
| `ring_full` | The output ring has no room after one refresh of the consumed position |
| `inputs` | The buffers could not be read or staged |

A refusal decided before the job-manager services are called leaves no trace at all. A later
refusal hands the lifted job its exact entry registers and untouched buffers (the ring line and
label word are staged in the invocation's own stack frame), and that job repeats the
buffer-acquisition calls, as the specification's integration contract allows. A private replay
that runs the job manager and the original job from recorded local store confirms that a refusal
at the latest point followed by the lifted job leaves the same job-manager state and transfers as
the lifted job alone.

## How it works

- `edgegeom_core.cpp` is a pure function of the invocation's inputs. It covers admission,
  decoding, deformation, projection and culling, the output data, ring allocation and the hole
  layout. Culled indices, produced vertices and draw parameters (`Prepared`) are kept separate
  from the hole encoder (`Engine::build_hole`).
- `edgegeom_runtime.cpp` is the engine shim:
  - checks the resident module and the entry. The runtime records a verified streamed region
    with the span of its lifted code (as `tools/wws_job_lift.py` registers it), so the record
    must cover the entry. Whether the loaded code matches is the dispatcher's own resolver's
    answer: it compares a newly loaded region once, as the lifted dispatch would;
  - calls the job manager's *get buffer*, *release buffer* and *finish* services the way a
    lifted job's own call does: link in `r0`, a nested host frame, the runtime's dispatch and
    drain up to the link, interrupts disabled around each call. (The runtime's
    `spu_native_service_call` scope requires a RUNNING context status, which the SPURS policy
    contexts that run every WWS job do not have, so it refused every call in game.);
  - writes guest memory only through the runtime's MFC channels on tag 31, in this order: the
    ring record (GETLLAR/PUTLLC), the data, then the hole, with each 128-byte line's first word
    last;
  - calls *finish* last, after every output is visible, tag 31 has drained, buffer 3 is
    released and the invocation is counted. Finish may run other job-manager or workload code,
    including another invocation on the same thread, before it returns. The shim keeps its saved
    registers on its own frame and reads nothing of the invocation afterwards;
  - returns to the policy continuation with `r1` and `r80`–`r127` as at entry. No return value
    is defined; `r3` holds finish's result.
- Work stays on the calling SPU thread. Each thread reuses its own scratch memory.
- The core is compiled without floating-point contraction. The facing rule must see an exact
  zero area for coincident screen positions.

## Verification

### Synthetic tests

`tests/test_native_edgegeom.py` builds two fixtures; neither needs game files. CI runs them on
Linux, macOS and Windows.

- The core fixture checks the specification's vectors ET-1 to ET-10 and the vectors of the codec
  and deformation specifications (normals, rebased halves, wave, GZ-1, GZ-3, NI-1, NI-3, SK-1, SK-2,
  SK-7), plus near-plane, continuation-slot, group-11 and refusal cases.
- The shim fixture runs against a stand-in job manager and MFC. It checks:
  - identity against the runtime's residency records;
  - the service calls and their order, with SPURS policy contexts that are not RUNNING;
  - that refusals leave the entry registers and every buffer byte untouched;
  - the ring retry and refresh;
  - data before hole, with each hole line's first word last, for holes of any size;
  - another invocation running inside *finish*;
  - an inert seam when the switch is unset.

### Culling

Culling follows the specification's rule 1: the near plane, and the four viewport sides tested on
positions divided by `|c.w|`, which reproduces the original for triangles reaching behind the
camera. A triangle that survives rule 1 with a vertex past the near plane or at `c.w ≤ 0` is kept
without the facing and quarter-pixel rules.

### Differential comparison (private, outside this repository)

Recorded original invocations (job I/O recordings, native off) are compared output by output:
hole bytes, ring update, index list and produced vertices.

- **Recordings:** 12,423 invocations from Training (standing and on a walk route) and hosted
  Sabotage (7 sessions), covering every kind:
  - index-only for groups 1, 2, 3, 8, 12, 13 and 15;
  - instanced wind;
  - wind under both mode words;
  - ripple;
  - skinned groups 7 and 11;
  - Main, shadow-cascade and 160 × 160 views.
- **Handled invocations:** every one conforms to the specification. Each either matches exactly,
  differs only by triangles the specification's tolerance classes as borderline, or (group 3)
  differs only in vertex positions within its tolerance.
- **Refusals:** 10 — short streams of produced kinds, and indices of `V` or more.
- **Refusal-after-acquisition:** a replay that runs the job manager's own code from recorded
  local store confirms it is equivalent to the lifted job alone. The run is forced to refuse at
  the latest point, then the original job runs; job-manager state and every transfer are
  unchanged.

### In game (acceptance runs so far)

| Host | Scene | Build | Result |
|---|---|---|---|
| macOS | Training | `c4205e7` | +4.7% fps; SPU workers about 17% less busy; no visual difference |
| macOS | Training walk route | `e1dd9c4`, `ff794c2` | no missing near-camera geometry, holes or z-fighting |
| macOS | hosted Sabotage | `9923544`, `ff794c2` | +6.5% and +7.1% fps; no freeze; the forced-refusal run plays normally |
| Linux | Training | `9923544`, `edcd663` | the job's CPU falls from about 70 ms to about 5 ms per frame; fps flat on the RSX-bound main runtime, with process CPU −20% |

Native work averages 4–5 µs per invocation. Refusals in game are expected `ring_full` cases
(the ring is momentarily full; the lifted job then waits) and a handful of unrecognised mode
words, which `MAG_NATIVE_EDGEGEOM_STATS=1` prints.

The native path stays opt-in. Making it the default is a separate decision, after the
specification's acceptance gates pass on all three hosts at the current head.
