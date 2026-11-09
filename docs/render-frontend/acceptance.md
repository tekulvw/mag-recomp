# Frontend scope and acceptance

What the frontend has to take over, how we prove that original work no longer runs, and what has
to hold before the frontend may become a default. The architecture is in
[architecture.md](architecture.md). Nothing here claims that any of it is met yet: every SPU job
still runs as the recompiled original program by default, and the frontend is opt-in.

## Responsibilities

These are the render responsibilities the frontend must eventually own, stated as obligations.
The list is a floor, not a proof that every format, role or occurrence has been found; the
inventory of the retail build decides that.

| Responsibility | What it consumes and produces | Condition for taking it over |
| --- | --- | --- |
| World publication | Scene, transforms and palettes, view and camera, effects, lights and streaming changes | Gameplay and network consumers keep their own timing. Source-owned publications must exist before any render-only descendant is replaced. |
| Scene traversal | Scene and level populations, walked into renderer lists | Replace the render-only walk without disturbing streaming or gameplay consumers, and without inventing population counts. |
| Occlusion | The occluder population and the renderer's frusta | A real empty population publishes a fresh zero; stale data is not reused when the count is zero. |
| Culling and level of detail | Mode, view, objects and their streams; produces selected items and transforms, and in some modes persistent feedback read by animation | Each mode is closed separately before its output is called render-only. |
| Sorting | Items with material, camera and transform validity, and completed query reports; produces the ordered selection for every pass | Visibility values are never faked and report dependencies are kept. Equal-key and transparency order is preserved. |
| Materials | Shader, layout and state, role and technique, textures and samplers, ordinary and built-in constants, target state | Full, inherited and incremental bindings are all covered. |
| Geometry | Packed streams, descriptors, topology and partition rules, consumed attributes, resource versions | No shape, attribute or format allow-list may stand in for full coverage. Empty and culled branches need evidence. |
| Characters | Animation, palettes and transforms; render-only skinning, deformation and morphs feed every pass that uses them | Animation results gameplay needs stay with the game. |
| Particles and effects | Separate render preparation, simulation and admission populations | Collision and gameplay simulation stay. Smoke, repair lights and muzzle flashes are correctness requirements. |
| Lights | Dynamic light streams and their generation; lighting constants and volumes | An empty light stream needs evidence; lights are never dropped from the inventory. |
| Main, DepthNormal, Shadow, Other | Every selected item with its material, geometry, view and state, per pass occurrence | Each occurrence has its own identity: Shadow runs more than once per update. Real guest readers of depth and normal surfaces keep correct generations and aliasing. |
| Auxiliary views and UI | The UI and auxiliary producers, their order, targets and resources | Kept as explicit scope; UI is not assumed to be ordinary Other work. |
| Post-processing | Input surfaces, formats, alias generations, order and output attachments | CPU reads that are really needed stay explicit. |
| Render transport | Original material and geometry construction, patching, guest packets and reparsing below the semantic hand-off | The representation is replaced, not only a leaf job. Host resource publication, encoding and pipeline creation remain measured costs. |
| External completion | Guest reports, labels and markers, semaphores, resource retirement and failure state | Published only on the real condition: source consumed, GPU complete, presented and retired are separate facts. |

The dependencies run from world publication through traversal, view generation and particle
input, to culling, sorting and pass selection, then materials and deformation, then the passes,
the GPU backend, and finally guest-visible results and retirement. Query reports produced by the
GPU feed back into sorting, and culling's level-of-detail and animation feedback goes back to the
CPU.

**Pass and branch coverage.** Every pass occurrence has its own `(frame, view, family,
occurrence)` identity. Observed minimum counts per update (for example one Main and two Shadow
occurrences in Training) are sanity checks, not caps. Menus, loading, respawn, streaming and other
render modes count even when they look unlike Training. Material role and mask alternatives,
inherited state, every geometry codec and layout, deformation, wind and morphs, particle and
effect routes, optional lighting, multiple views, UI and post-processing are all branch
dimensions to enumerate.

**Cadence.** The game limits its update delta, but that limit is not evidence of a fixed render
rate, and no timer is changed. Rendering faster than the game updates would need an owned scene
and camera publication contract with defined view interpolation, keeping the original simulation
and network timing. Re-presenting a frame under a new number does not count as a new frame.

## Proving original work is removed

Two independent accounting paths are kept, so the frontend cannot certify itself:

1. **Obligations recorded before interception.** For each frame, an upstream observer records the
   work keys `(frame, pass occurrence, source item, family, stage)` before any original work is
   suppressed, so a suppressed hook cannot shrink the denominator. An item that fans out to
   several passes has one obligation per pass.
2. **What actually ran.** Counters on original producer entries and on the runtime's execution of
   original jobs, plus the replacement's outputs and GPU completions.

Each obligation must reconcile to exactly one outcome: a replacement output (tied to a
submission), culled or empty with independent visibility evidence, an original fallback, or a
failure. An implementation cannot pass by declaring every item culled.

Original entries are counted on four routes, all required: the PPU entry, the runtime's compiled
job body, the runtime's interpreter, and anything that runs outside an active transaction
(including late work drained after the window). A route's receipt records the span it covered, the
point it drained to, its entries, lost records, overflow and whether it was enabled. A zero from a
disabled or missing observer proves nothing, and lost records, overflow or unrecognised routing
invalidate the proof. For every retired family, the qualified window needs zero original entries
globally, not only inside one hook. Non-render SPU work that is deliberately kept is attributed
separately.

`crates/mag-render` implements the verifier (`accounting.rs`, types in `types.rs`): it takes a
scope manifest fixed before interception and an immutable event stream, and checks frame identity,
obligations, outcomes, observer receipts, GPU completions and presentations. Synthetic negative
tests must fail it: one injected original dispatch, a duplicated or dropped outcome, a suppressed
producer hook, hidden preparation, and a false completion. A pass is a consistency proof over the
receipts it is given, not authentication of a real device run.

The private qualification that runs the original game stays as it is; the frontend gets its own
qualification path rather than loosened original-job predicates.

## Milestones

| Milestone | Deliverable | Removal proof |
| --- | --- | --- |
| M0 | Responsibility and dependency map, baseline, cadence and reference machines | None claimed; original-mode counters see the expected workload and reject deliberate omissions |
| M1 | Rust contracts, resource ownership and the independent verifier | None claimed; hidden original work and false completion fail synthetic checks |
| M2 | Semantic passes and portable compute on real Metal, Vulkan and D3D12 devices | Synthetic scenes render without guest command preparation or presentation readback |
| M3 | Complete material and state preparation, rigid and instanced geometry, connected Depth, Main and Shadow | Zero original preparation and children for the cut-over families; the rest listed |
| M4 | Traversal, visibility, level of detail, sorting and multiple views from early scene inputs | Zero original render-only traversal, culling and sorting for that scope, CPU consumers kept |
| M5 | Deformation, characters, effects, particles, lights, transparency, Other, auxiliary, UI and post-processing | Every inventoried family supported; zero unknown families, normal fallback or targeted original entries |
| M6 | Complete live frontend, real-match correctness and removal on every OS | Zero targeted original execution through qualified match windows; no visual or gameplay regressions |
| M7 | Sustained 60 fps on declared reference machines across the match matrix | Removal and correctness still hold while the performance gates pass |
| M8 | Dead-code retirement and reproducible source and build integration | Superseded providers no longer linked; checks still pass after deletion |

The first live removal checkpoint is a complete, connected pass ownership unit, not another
shape or attribute allow-list. Each milestone is recorded per platform; a Mac pass does not
complete Windows or Linux.

## Performance and correctness gates (proposed)

These are engineering proposals, set before measurement and revised only explicitly, never after
a failing result.

**Profile.** 1920×1080 physical render size at 100% scale, retail-equivalent effects, visibility
and shadows, with no dynamic resolution, frame generation, dropped populations or disabled guest
audio. A 1280×720 result is kept for history but does not substitute. Each run records CPU, GPU,
memory, OS and driver, display refresh, backend, build, runtime and toolchain identities, and
settings.

**Windows of play.** On each reference machine, at least three ten-minute windows of real
multiplayer play: dense combat with many characters, effects-heavy objective combat, and
traversal, streaming and respawn. Record the map and mode, player and bot populations, visible
actors and effects. Training or a paused scene does not qualify. A population or map that cannot
be played is marked pending, not assumed.

**Gates, per run:**
- At least 60 newly rendered frames per second over the whole window and in each ten-second
  bucket; time between completions at p95 ≤ 16.67 ms and p99 ≤ 25 ms.
- Scene-ready to GPU-complete latency at p95 ≤ 33.34 ms with no growing backlog; at most two
  submitted frames awaiting completion to start with.
- Paced display within 0.2% of the measured refresh (59.94 Hz counts as correct), with at most
  0.1% repeated or missed presentations after warm-up.
- No unexplained gameplay stall over 100 ms; stalls over 50 ms, p99.9 and the maximum are
  reported, with loading and menu transitions reported separately.
- Simulation updates, rendered frames, GPU submissions and displayed frames are separate metrics;
  overlapping CPU and GPU scopes are never summed into a serial budget.

**Correctness** combines exact synthetic tests of topology, order, resources and completion;
private differential tests against the original program's observable outputs; initialised
offscreen colour, depth and stencil comparisons; and live checks of movement, firing, aiming,
recoil, damage indicators, smoke, repair lights, muzzle flashes, shadows, transparency,
streaming, death and respawn, and map changes. Pixel tolerances are set per shader and format
from the reference's own repeatability; no global tolerance is raised to hide a missing effect.
Retail v1.0 behaviour is the reference, not an existing recompilation bug.

## Cleanup and release

Proven-dead experiment code may be removed early, after checking its real compile, link and
reference dependencies. Active fallback providers are removed only after their replacement gates
pass, and a buildable original reference is kept through M7. Compiled SPU programs that do
non-render work are never removed because render jobs were replaced. The frontend's defaults
change only together with complete build recipes, after acceptance, and releases remain a manual
human decision after a live-match review.
