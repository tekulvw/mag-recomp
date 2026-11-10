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

Admitted kinds: `index_only` (static items), `instanced_wind` (group 0), `wind` (group 3) and
`skinned` (group 7). The group and a non-empty output layout select what is produced, and the mode
word and flavour select culling only. Group 3 with produced vertices is therefore the wind under
mode `0x28` (two-sided) and under `0x08`/`0x0A` (back-face culled) alike. Group 4 (ripple), group 11 (skinned in Main) and anything else the
specification marks unconfirmed is refused with `kind`.

| Reason | Meaning |
| --- | --- |
| `identity` | The module's residency record at LS `0x4000` is missing, or the dispatcher's resolver (`spu_lifted_lookup`) does not accept its loaded code. With the counters on, the first refusals also print the residency records |
| `service` | A transfer was pending when the invocation started, so no service call was made |
| `mode`, `blend_shapes`, `configuration`, `hole` | Inputs outside the specified values (admission checks 2–5) |
| `stream_short`, `index_range` | Input streams shorter than their counts need, or an index of `V` or more |
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

- `tests/test_native_edgegeom.py` builds two synthetic fixtures; neither needs game files.
  - The core fixture checks the specification's vectors ET-1 to ET-9 and the codec and
    deformation specifications' vectors.
  - The shim fixture runs against a stand-in job manager and MFC. It checks the call order,
    refusals that leave the entry state, the ring retry and refresh, and the write order.
- Culling also applies the near plane: a triangle with every vertex at `c.z < −c.w` is removed,
  and one that only reaches past the near plane is kept without the facing and quarter-pixel
  rules, the same treatment as a vertex at `w ≤ 0`. The Sabotage shadow-cascade recordings show
  the original job doing both, and the specification states both rules.
- A triangle that crosses the eye plane (some but not all vertices at `c.w ≤ 0`) is always kept
  for the GPU to clip. Recordings with camera movement show the original drawing some such
  triangles that plain clip comparisons would remove, and no rule found so far reproduces its
  decisions; keeping them can never hide geometry. The specification is being revised.
- A private differential comparison against recorded original invocations runs outside this
  repository. On 800 Training recordings, every hole, ring update and index list matches; the
  only exceptions are 2 invocations whose culled lists differ by triangles within 2^-8 pixel of a
  decision boundary, which the specification's tolerance allows. Every produced vertex is within
  the specification's tolerance.
- Before it could become a default, the native path must pass the specification's acceptance
  gates (oracle coverage, refusal rate, visual parity, performance and robustness on all three
  hosts).
