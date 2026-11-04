# Main scene planning

## Scope

The Main planner turns one owned Main source window into one scene packet: an ordered list of
passes, item records and material draws with their programs, textures, samplers, raster state and
vertex data. This specification covers the planner's own behaviour:

- association checks between the cohort, material sources, prepared geometry and plan inputs;
- the per-item traversal: repeat detection, group order and item routes;
- the ordered producer events (setup banks, group-12 textures, group-13/15 work, finish);
- pass opening;
- per-item material state updates and texture-unit binding;
- per-draw choice of program variant, shader controls, packet textures and draw state;
- the plan statistics;
- replay of a retained clear-only setup bank as native clear passes.

It applies to retail v1.0.

These behaviours are used here but specified elsewhere or not yet specified:

| Behaviour | Where |
| --- | --- |
| Fixed-function state at group boundaries and per primary material | [main-group-transitions.md](main-group-transitions.md) |
| Setup-bank copy and register knowledge | [material-setup.md](material-setup.md) |
| Texture-unit decode, native sampler, view remap, depth comparison | [texture-unit-controls.md](texture-unit-controls.md) |
| Producer events (what each one records) | [main-effects.md](main-effects.md) and the producer specifications it lists |
| Material value composition (vertex/fragment constants, current attributes, update-only observation) | [material-values.md](material-values.md) |
| Program compilation, vertex-input packing, effective vertex-array mask | [program-compilation.md](program-compilation.md), [vertex-inputs.md](vertex-inputs.md) |
| Raster-state translation from pipeline state | [raster-state.md](raster-state.md) |
| Viewport mapping (viewport, shader scale/offset, sample grid) | [main-producer-inputs.md](main-producer-inputs.md), "Mapping a source viewport to a native target" |

## Inputs

### Plan inputs

| Field | Meaning |
| --- | --- |
| `source` | (engine update, cohort, publication, pass occurrence) identity of this window |
| `viewport` | viewport mapping: packet viewport, shader scale and offset, sample grid |
| `sample_grid` | `[x, y]` samples, must equal `viewport.sample_grid` |
| `source_color_format` | RSX surface colour-format code of the Main target |
| `alpha_window_tail` | three raw words appended to the fragment alpha constant |
| `raster` | raster inputs for raster-state translation |
| `color_format`, `depth_format` | native attachment formats |
| `vertex_byte_budget` | maximum vertex bytes per draw |

### Cohort

A header (`source_invocation`, `execution_update`, `pass_occurrence`) and an ordered list of
items. Each item has:

- `mbi`: a 32-byte item record. Bits 7..4 of byte 0 are the item's *census group*. Bytes 12..15
  are a big-endian mesh index; the item's *mesh key* is `index * 16` modulo 2^32.
- `transform`: an optional 112-byte transform record. Bytes 0..31 are eight big-endian `f32`
  words used for repeat detection. Byte 111 equal to 1 disables repeat detection for this item.

### Material sources

One entry per item, in item order, each carrying its own item ordinal and either a refusal or:
material address, technique selector, draw sources (mesh group, a copy of the item transform, the
pass control word), primary material state, ordinary texture bindings, producer texture bindings,
the geometry attribute-6 source (none, a known current value, or a vertex stream), programs and
fragment template.

For the whole window: the pass control word (`u32` at Main pass object `+0x144`), the render mode
(`i32` at pass `+0xdc`), the setup sources of [material-setup.md](material-setup.md) and
[main-group-transitions.md](main-group-transitions.md), and the built-in pass values used by the
reduced-fidelity policy.

### Prepared geometry

One entry per item, in item order, each with its ordinal, a list of draws (one per static
partition, each with a partition ordinal) and a list of omissions (geometry the preparer could not
produce).

### Initial state

A `MainState`: material value state, pipeline state, sixteen texture units, and per unit an
optional CPU image (or the error from producing it), an optional *packet image binding* and an
optional descriptor selection (resident object, fallback, producer, or producer asset).

### Policy

| Policy | Default | Effect |
| --- | --- | --- |
| reduced-fidelity fragments | off | fragment shading may be replaced by base-colour or flat programs; vertex programs, geometry, ordering and depth/raster remain exact |
| try source materials | off | with reduced fidelity on, first try the exact source material and fall back only on a classified coverage gap |
| current-attribute-6 default | on (only matters with reduced fidelity) | see "Attribute 6" |
| relax depth-equal | off | with reduced fidelity on, a depth comparison of equal becomes less-or-equal |
| gamma-off unit mask | 0 | diagnostic: clears the gamma mask of the named units |
| vertex packing | deferred, up to 4 threads | must produce a byte-identical packet to serial packing |

All non-default settings are explicit opt-in diagnostics or experiments and must be visible in
the statistics.

## Outputs

On success: the packet, the final `MainState` and `MainPlanStats`. On failure: an error and *no*
packet; the input packet is consumed. No partial packet can escape. The planner never submits GPU
work, runs original code or declares original work removed.

## Behaviour

### 1. Association

Refuse with `Association` unless: `source.cohort == header.source_invocation`,
`source.engine_update == header.execution_update`, `source.pass_occurrence ==
header.pass_occurrence`, `source.publication != 0`, `sample_grid == viewport.sample_grid`, and
the material and geometry lists both have one entry per item. Per item, the material and geometry
entries must carry that item's ordinal (`Association`). The setup sources must be present
(`Source("Main setup sources")`).

At the start, the material value state begins a new Main window (its per-window globals reset; this
is part of material value composition).

### 2. Initial producer events

If lists are enabled, for each initial setup reference slot 0..2 that is nonzero, emit
`Initial { slot, target }`. This happens even when the cohort has no items.

### 3. Item traversal

A traversal remembers the last *committed* (mesh key, transform) and the current group (initially
−1). For each item in order:

**Lists disabled.** Open a pass if none is open, add a no-draw record `ListsDisabled` for the item,
count it, and continue. Nothing is committed.

**Lists enabled.**

1. The material entry must not be a refusal (`Source("material item")`), its draw sources must be
   present (`MaterialValues` error, stage `captured-draw-source`), the item must have a transform
   (`Source("transform")`), and the draw sources' transform and pass control must equal the item's
   transform and the window's pass control (`Association`).
2. Decide the item route, with `group` = the draw sources' mesh group:
   - census group ≠ `group` → `GroupRelationship { census, effective }` error;
   - else if byte 111 of the transform is not 1, a previous item was committed, its mesh key
     equals this item's, and each of the eight leading transform words is *equal-or-both-tiny* to
     the committed one → **Repeated**. Two words are equal-or-both-tiny when their bit patterns are
     identical or both have a zero exponent field (zero or subnormal, of either sign);
   - else if `group` < current group → `GroupOrder` error;
   - else by group and pass control `c` (unsigned):
     - group 0 with `c <= 2` → **UpdateOnly**;
     - groups 0..13 → **Prepare**;
     - group 14 → special route 14; group 15 → special route 15;
     - anything else → `Source("group")`.
3. Unless Repeated, apply the group transition from the current group to `group` (section 4).
4. Ask the effects for the current plan inputs (a producer may have changed targets). The returned
   sample grid must equal its viewport's sample grid and its source identity must be unchanged
   (`Association`).
5. Open a pass if none is open, with the current viewport.
6. Record the item: Repeated and UpdateOnly add a no-draw record with that reason; other routes add
   an item record with zero unresolved obligations.
7. By route:
   - Repeated: count; skip step 8 (nothing is committed).
   - UpdateOnly: the material value state observes the item (binding identity update only); count.
   - Special routes: `Unsupported("special Main caller")`.
   - Prepare: section 5.
8. Commit: the traversal remembers this item's mesh key and transform and sets the current group to
   `group`.

After all items, emit `Finish { previous_group }` with the current group (−1 if nothing was
committed). This is emitted in every case, including lists disabled.

### 4. Group transition with producer events

Transition from `old` (current group) to `new`:

- If `old == -1`, apply the start-of-Main state transition (−1 → 0).
- For each group `g` from `max(old + 1, 1)` to `new` inclusive:
  - if lists are enabled:
    - `g == 12`: emit `Group12Textures`, then write the two water-constant rows into vertex
      constant rows 46 and 47;
    - `g == 13` with a nonzero group-13 reference: emit `Group13 { target }`;
    - `g == 15`: for each group-15 slot 0..1 with a nonzero reference, emit
      `Group15 { slot, target }`;
  - apply the state transition `g − 1 → g` of [main-group-transitions.md](main-group-transitions.md),
    using a copy of the setup sources whose initial, group-13 and group-15 references are cleared
    (their effects have been emitted as events).

Emitting any producer event closes the open pass: the next item that needs a pass asks the effects
to open a new one (with restored targets and the current viewport). A producer event that the
effects do not implement fails the plan with `Producer(event)`.

### 5. Prepared item

1. The item must have at least one geometry draw (`Source("missing prepared geometry")`). The
   effects check the geometry; by default any omission fails with `Geometry { ordinal, omission }`
   (the first omission).
2. Compose the item's material values (external). This yields a binding action (unchanged or
   changed) and a count of computed fragment built-ins omitted under the reduced-fidelity policy.
   Errors fail with `MaterialValues` (stage `compose-material-and-draw`). The effects then check the
   composed vertex constants.
3. If the binding action is not "unchanged": apply the primary material state with the window's
   render mode, then bind each ordinary texture binding in record order (section 7). A failed
   binding fails with `MaterialTexture` (stage `ordinary` or `ordinary-record`, with the record
   index).
4. Bind every producer texture binding (always, regardless of binding action).
5. If the item's attribute-6 source is a known current value, it becomes the carried current value
   of attribute 6. A refused attribute-6 source fails with `GeometryCurrentSix`.
6. Determine the fragment texture units the technique samples (`used`, a 16-bit mask), unless
   reduced fidelity is on without "try source", in which case `used = 0`.
7. Build shader controls (section 6) for `used` and the alpha constant, then overwrite its words
   1..3 with `alpha_window_tail`. Under "try source", a *control gap* (below) instead selects the
   flat controls and records the gap as the item's fallback reason; other errors fail the plan.
   Under "try source" without a fallback so far, a *texture gap* check runs over `used`.
8. Under "try source", when `used` contains unit 0 and some other unit, a *base-colour* candidate is
   prepared from controls for unit 0 alone, provided unit 0 has no control or texture gap.
9. Translate the pipeline state into raster state. It must be fully known with no unresolved
   obligations (`Source("effective raster state")`). With reduced fidelity and relax-depth-equal on,
   an enabled depth test with comparison *equal* becomes *less-or-equal*.
10. For each geometry draw, in partition order: section 8.

Coverage gaps (only consulted under "try source"):

| Gap reason | Raised by |
| --- | --- |
| texture capability | texture alpha kill, texture dimension ≠ 2, sampling a disabled unit, a CPU image failing for a depth format (`0x90..=0x93`) or a 3-D image |
| sampler capability | unsupported addressing (clamp or mirror-once), unsupported anisotropy, missing texture state, missing border colour |
| texture source | a producer-selected unit without a packet image binding, no CPU image, or a CPU image refused as missing source |
| computed fragment values | the composed fragment values are incomplete |
| current attribute | a vertex input the program reads is neither an enabled array nor a known current value |
| vertex constants | a vertex constant the program reads is unknown (all 512 rows for indexed access) |
| vertex arrays | a required vertex array is missing or its primary semantic is unprepared |
| shader capability | the program uses an unsupported feature or an unqualified texture coordinate |

Any other error fails the plan even under "try source". A CPU image whose descriptor differs from the
unit's current words is an `Association` error.

### 6. Shader controls

For a unit mask `used`:

- alpha function: the alpha-test enable must be known (`Source("alpha enable")`). Off → none.
  On → the comparison must be known (`Source("alpha comparison")`) and the function is
  `0x200 + code` (never 0 … always 7).
- alpha constant word 0: 0 when the function is none, never or always; otherwise the alpha reference
  (must be known, `Source("alpha reference")`) converted for `source_color_format` by the public
  `mag_render_programs::fragment_alpha_reference`; that conversion accepts colour formats 1..5 and
  8..16 only.
- sRGB output must be known (`Source("sRGB output")`).
- sample grid from the plan inputs; vertex texture mask 0; enabled arrays 0 (set per draw).
- for each unit `u` in `used`, decode it ([texture-unit-controls.md](texture-unit-controls.md));
  a disabled unit is `Unsupported("sampled disabled texture")`, a decode error is `Sampler(error)`,
  `alpha_kill` is `Unsupported("texture alpha kill")`, `dimension != 2` is
  `Unsupported("texture dimension")`. Otherwise set the cube and unnormalised mask bits, the gamma
  mask (zero if the unit is in the gamma-off diagnostic mask), the expand mask, the texture
  dimensions `[w[6] >> 16, w[6] & 0xffff]` and the depth-comparison request.
- units outside `used` keep zero masks and dimensions `[1, 1]`.

The *flat controls* are the same structure with no alpha function, sRGB off, all masks zero and
the plan's sample grid.

### 7. Texture-unit binding

`bind(binding)` for a binding with unit `u`, selection, 8-word descriptor, optional inherited border
and payload (a CPU image result, or "native producer"):

- `u >= 16` → `Source("texture unit")`.
- Producer-asset selection: the descriptor's format (without the linear bit) must be `0x86`, `0x87`
  or `0x88` and the payload must be a CPU image; when that image was produced, its descriptor must
  equal the binding's and its format must be BC1, BC2 or BC3 respectively. Otherwise
  `Association`.
- A native-producer payload is allowed only with a producer selection (`Association` otherwise).
- On success: the unit's compact descriptor becomes the binding's descriptor; its packet image
  binding is cleared; its border becomes the inherited border when one is given (otherwise
  unchanged); its CPU image becomes the payload result (none for native producer); its selection is
  recorded. All checks happen before any change.

`bind_packet_image(u, packet, descriptor, image)` attaches an image already in this packet:

- `u >= 16` → `Association`. The unit's packet and CPU images are cleared first, so a failed attach
  never leaves an older image usable.
- The image must exist and pass the *resolve* check below, and the unit's current compact
  descriptor must be known and have the same *storage words* as `descriptor`. The storage words are
  `w[0]`, `w[1]`, `w[6]`, `w[7]`; wrap, filter, remap, LOD and enable changes do not change storage.

*Resolve* of a packet image binding against descriptor `w` in a packet succeeds when the image
exists in that packet, the packet identity and source identity are those recorded at attach time,
the storage words equal those recorded, the image format equals the one recorded, and the image has
1 sample, depth 1, dimension 4 and 6 layers for a cube (bit 2 of `w[1]`) or dimension 2 and 1 layer
otherwise, `mip_count == w[1] >> 16`, width `w[6] >> 16` and height `w[6] & 0xffff`. Otherwise
`Association`. A binding therefore never crosses packets, retries or frames.

### 8. One draw

1. Enabled arrays = the effective vertex-array mask of this draw (external). Errors fail with
   `MaterialCompile` (stage `effective-array-mask`).
2. Attribute 6: if the item's attribute-6 source is a stream and array 6 is not enabled for this
   draw, the compile view of the state treats current attribute 6 as unknown (the carried value is
   not changed). With reduced fidelity and the attribute-6 default on, if array 6 is not enabled,
   current attribute 6 is unknown in the compile view, and the program reads input 6, the compile
   view uses `[0.0, 1.0, 0.0, 0.5]` (words `0, 0x3f800000, 0, 0x3f000000`) for this draw only.
3. Under "try source" without a fallback so far, check computed fragment values, current attributes
   and vertex constants for gaps (section 5 table).
4. Choose programs, first that applies:
   1. **Source** variant with the shader controls (and this draw's enabled arrays), when reduced
      fidelity is off, or under "try source" with no fallback. Under "try source", a classified
      compile gap records the fallback and moves on; otherwise any error fails the plan
      (`MaterialCompile`, stages `compile-programs` or `pack-vertex-inputs`).
   2. **Base-colour** variant with the unit-0 controls, when a base-colour candidate exists and the
      fragment values are complete. A classified gap moves on (reported); other errors fail.
   3. **Flat** variant with the flat controls (reduced fidelity only). Any error fails.
   Program values for all variants: the compile view's vertex constants, current attributes and
   fragment rows, the viewport shader scale and offset, and the variant's alpha constant (zero for
   flat).
5. A compiled vertex program that samples textures fails with `Unsupported("vertex texture source")`.
6. Textures for the draw: `used` for Source (exact or restored), `1` for base colour, `0` for flat.
   For each such unit in ascending order:
   - decode the unit (must be enabled);
   - image: a packet image binding resolved against the unit's words; else, if the selection is a
     producer, `Unsupported("producer texture requires native image")`; else the CPU image, which
     must be present (`Source("texture image carry")`), produced (`Source("texture payload")`) and
     carry the unit's exact words (`Source("texture image association")`), added to the packet
     once per distinct image;
   - add a view and a native sampler (both de-duplicated by the packet);
   - add a texture layout and texture use for the fragment stage (stage 1) with texture slot and
     sampler slot both `u`; the layout format is the packet image's format code (RGBA8 1, R8 4,
     RG8 5, BC1 6, BC2 7, BC3 8, R16 9, RG16 10, RGBA16F 11, RGBA32F 12, R32F 13, RG16F 14).
7. Add the material draw to the open pass with draw state: item ordinal, partition ordinal, zero
   unresolved obligations, colour and depth formats, raster state, texture layouts and uses. Vertex
   bytes may be written later (deferred packing) but the packet must be byte-identical to writing
   them immediately.
8. Mark base-colour draws and restored source draws with the material address and technique
   selector for diagnostics, and update the statistics.

### 9. Statistics

`intended` = item count; `source_disabled`, `repeated`, `update_only`, `emitted_items` (prepared
items) and `draws` count those outcomes. Per draw: approximated fragment draws (reduced fidelity
and not restored), restored source draws, base-colour draws, restored draws whose fragment program
can discard, one counter per fallback reason, approximated vertex outputs, approximated position
binding, attribute-6 approximations (and those whose source was a stream), owned stream-6 draws and
relaxed depth-equal draws. Omitted computed built-in requests are summed, and distinct
(material address, technique selector) pairs with omissions are counted.

### 10. Retained clear replay

`replay(bank, target, packet)` turns a copied setup bank that only clears into native clear passes.
`target` has the source identity, colour and depth image ids, width, height, source colour format,
depth format and viewport. The clear state carries the last colour clear value and depth/stencil
clear value, each unknown until written.

- Refuse `Unsupported("scissored retained clear")` unless width and height are nonzero and the
  viewport's scissor is exactly `(0, 0, width, height)`.
- Process all methods first, on a copy of the clear state:
  - nonzero subchannel → `Unsupported("non-clear retained bank effect")`;
  - method 0 with value 0 → ignore;
  - `0x1d90` → colour clear value; `0x1d8c` → depth/stencil clear value;
  - `0x1d94` with mask `m`: refuse `Unsupported("partial color/stencil clear")` if `m` has bits
    outside `0xf1` or its colour bits `m & 0xf0` are neither 0 nor `0xf0`. `m == 0` → nothing.
    Colour bits set → the source colour format must be 8 (`Unsupported("source clear color format")`)
    and the colour value must be known (`Source("clear color")`); clear colour =
    `[R, G, B, A] / 255` from the ARGB word. Bit 0 set → the depth format must be 2
    (`Unsupported("source clear depth format")`) and the depth/stencil value must be known
    (`Source("clear depth")`); clear depth = `(value >> 8) / 16777215`;
  - any other method → `Unsupported("non-clear retained bank effect")`.
- Then, for each clear in order, add a pass with the target's viewport: colour attachment loads
  (`load = 1`) when not cleared, else clears to the colour; depth attachment likewise; both store.
- Commit the clear state and return the number of passes. On any refusal no pass is added and the
  clear state is unchanged.

## Edge cases and errors

- Repeat detection compares against the last *committed* item only; Repeated and lists-disabled
  items never commit, so a run of identical items is all Repeated after the first.
- A Repeated item skips the group-order check and the transition, even if its group is lower.
- Group 0 pass-control comparison is unsigned: `0x80000000` and `0xffffffff` prepare.
- Every refusal is a plan error with the item ordinal and, where available, partition, material
  address, technique selector and stage. There is no silent skip.
- On failure, implementations may emit read-only diagnostic summaries; they must not change the
  outcome.

## Tolerance

The planner's own floating-point outputs are:

| Output | Mathematical value | Tolerance |
| --- | --- | --- |
| sampler fields | see [texture-unit-controls.md](texture-unit-controls.md) | as stated there |
| clear colour channel | `n / 255`, `n` in 0..255 | ≤ 1 ULP; `round(x * 255) == n` must hold |
| clear depth | `d / 16777215`, `d` in 0..16777215 | ≤ 1 ULP; `round(x * 16777215) == d` must hold, and `d == 16777215` must give exactly 1.0 and `d == 0` exactly 0.0 |
| attribute-6 default | `[0, 1, 0, 0.5]` | constants, exact |

Viewport, shader scale and offset, alpha reference conversion and all constant and vertex data are
taken from their producers unchanged.

### Consequences of non-bit-exact output

- The packet de-duplicates samplers by the bits of every field (including border and LOD floats)
  and compares viewports by bits. Within one implementation this is deterministic. Cross-
  implementation packet comparisons (differential tests, packet fingerprints) must compare these
  floats within tolerance.
- Clear depth: later draws that test depth *equal* against a cleared value would be sensitive to the
  exact cleared depth. The endpoints 0.0 and 1.0 are exact under the stated tolerance; any other
  clear depth used with an equal test must be reported as a precision risk.
- Cross-plan reuse caches and the "byte-identical packet" property between serial and deferred
  vertex packing are within-implementation guarantees and remain exact.

## Synthetic test vectors

Traversal (transform `T(x)` = 112 bytes, first word `x`, rest zero; mesh key 16):

1. Commit (16, `T(0x3f800000)`, group 0). Next item census 1, group 1, `T(0x3f800000)`: Repeated;
   current group stays 0. Same item with `T(0x40000000)`: Prepare.
2. Commit (1, `T(0x00000001)`, group 2). Next (1, `T(0x807fffff)`, group 2): Repeated (both
   subnormal). Same with byte 111 = 1: Prepare. Next (1, `T(0x00800000)`): Prepare.
3. Census 1, group 3: `GroupRelationship { census: 1, effective: 3 }`.
4. Committed group 5; next item group 4, not repeated: `GroupOrder`. Same item but repeated: Repeated.
5. Routes: (group 0, control 2) UpdateOnly; (group 0, control 3) Prepare; (group 0, control
   `0x80000000`) Prepare; (group 7, control 0) Prepare; (group 14) special → plan error
   `Unsupported("special Main caller")`.

Event order. Lists enabled; initial references `[0xa00, 0, 0xb00]`; group-13 reference `0xc00`;
group-15 references `[0, 0xd00]`; items with groups 0 (control 3), 12, 15 (none repeated):

6. Events: `Initial{0, 0xa00}`, `Initial{2, 0xb00}`, `Group12Textures` (then vertex rows 46/47 =
   water rows), `Group13{0xc00}`, `Group15{1, 0xd00}`, `Finish{15}`. Passes: one opened before item
   0, one before item 1 (after `Group12Textures`), one before item 2.
7. Same sources with an empty cohort: `Initial{0, 0xa00}`, `Initial{2, 0xb00}`, `Finish{-1}`.
8. Lists disabled with two items: two `ListsDisabled` records in one pass, then `Finish{-1}`; no
   initial events.

Shader controls:

9. Alpha test on, comparison greater-or-equal, reference raw word R, colour format 8: function
   `0x206`, constant word 0 = `fragment_alpha_reference(R, 8)`, words 1..3 = `alpha_window_tail`.
   Alpha test on with comparison always: function `0x207`, word 0 = 0. Alpha test unknown:
   `Source("alpha enable")`.

Texture binding:

10. Unit 3 with known compact descriptor `D` = the words of
    [texture-unit-controls.md](texture-unit-controls.md) vector 1 with `w[6] = 0x00080008`;
    packet image 0 of format RGBA8, 2-D, 8 × 8, 1 mip, 1 layer, depth 1, 1 sample. `bind_packet_image(3, P, D,
    0)` succeeds; textures for `used = 1 << 3` give layout `{stage 1, slot 3, sampler 3, format 1}`.
    The same binding in another packet: `Association`. `bind_packet_image(3, P, D, 99)`:
    `Association`, and unit 3 has no packet image afterwards.
11. A producer-asset binding with descriptor format `0x85`: `Association`. With format `0x86` and a
    produced BC1 image with the same descriptor: accepted. With a native-producer payload and a
    resident-object selection: `Association`.

Clear replay (target 8 × 8, colour format 8, depth format 2, scissor full):

12. Methods `0x1d90 = 0x40ff8000`, `0x1d8c = 0xffffff00`, `0x1d94 = 0xf1`, `0x1d94 = 0x01`:
    two passes. Pass 1: colour clear `[1.0, 0.5019608, 0.0, 0.2509804]`, depth clear 1.0.
    Pass 2: colour load, depth clear 1.0.
13. `0x1d8c = 0x00000100`, `0x1d94 = 0x01`: depth clear `1/16777215` ≈ `5.9604652e-8`.
14. `0x1d94 = 0x02` (stencil): `Unsupported("partial color/stencil clear")`. `0x1d94 = 0x31`:
    same. `0x1d94 = 0xf0` with no colour value written: `Source("clear color")`. Method `0x1814`:
    `Unsupported("non-clear retained bank effect")`. Scissor `(0, 0, 4, 8)`:
    `Unsupported("scissored retained clear")`. In every refusal no pass is added.

## Open questions

- The attribute-6 default `[0, 1, 0, 0.5]` is the value Main uses when no attribute-6 stream or
  current value is supplied. It is stated as observed behaviour; whether game-chosen defaults of
  this kind may appear in public specifications needs the same ruling as the group-transition
  constants.
- Special routes 14 and 15 are refused. Their behaviour is not specified.
- Material value composition, program compilation, vertex-input packing, raster translation and the
  viewport mapping are prerequisites of this planner and have no public specification yet.
