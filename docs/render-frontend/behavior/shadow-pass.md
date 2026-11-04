# Shadow pass

## Scope

Each update the game renders shadow-caster depth for a directional light into a shadow map. It
does this with the MaterialFlushShadow SPU job, which it runs twice per update (once per cascade)
and waits on both times. Later, the Main lighting producer ([main-lighting.md](main-lighting.md))
samples the map. This document specifies what a native replacement for those two job runs must
produce:

- the two cascade views it renders from;
- which items it draws, and with what geometry, programs and fixed-function state;
- the shadow map it writes and how that map is consumed;
- the guest-visible obligations that must still be met when the original job is suppressed.

Applies to retail v1.0. Identities: the job is the material-flush module whose parent object is at
`0x1F6BAD0`. Its image base is `0x192BF80` and its payload entry is local-store `0x12F68`.

**Status: partial.** The structure below comes from:

- the job's construction and its input format (shared with Main and Depth);
- the cascade-view behaviour;
- the job's set of emitted RSX methods;
- a job-IO recording of four runs, which settles the pass state, the command output and the terminal
  segment.

The render target, its clear, inherited stencil and depth bias, the light-view construction and the
per-item child work are not yet established, and are listed in Open questions. An implementation
must not guess them. It may implement the established parts behind a default-off switch.

Out of scope: how the game picks the cascade distances and light direction, the cull and sort jobs
that build the caster lists ([scene-plan.md](scene-plan.md) consumes the same kind of lists for
Main), and the lighting producer's own math.

## Inputs

### Invocation

Per update there are two Shadow job runs, *cascade 0* and *cascade 1*. Both are built by the same
material-flush job constructor used for Main and Depth, so they take the same root input format as
those jobs:

| Run | Family | Bucket | Enabled-group mask |
| --- | --- | --- | --- |
| cascade 0 | 1 | 1 | `0xFE` |
| cascade 1 | 2 | 2 | `0xFE` |

The enabled-group mask has bit `g` set when mesh group `g` takes part. `0xFE` selects groups 1–7.
Items in group 0 (instanced foliage) and in groups 8–15 are never drawn into the shadow map by these
runs. A mask of 0 is a legitimate empty run: no item is drawn, but the run's terminal obligations
(below) are still met.

The game passes the mask in when it builds the run. A replacement must read the mask from the run's
input, not assume `0xFE`.

### Caster lists

Each run consumes a visibility cohort: the sorted per-group item lists that the game's sort job
produced for that run's family. Cascade 0 uses cohort camera slot 1 and cascade 1 uses camera slot 2
(Main uses slot 3). The cohort, item, draw-batch (MBI) and transform formats are the
same as Main's ([scene-plan.md](scene-plan.md), [material-sources.md](material-sources.md),
`mag-render-adapter-core::cohort`). The run's item budget is the sum of the cohort's group counts
for the enabled groups only. Items are drawn in cohort order: ascending group, then sorted order
within the group.

### Mesh, material and geometry sources

For every item the mesh record, primary material, technique chunk, fragment microcode, partition
streams and attribute codecs are located exactly as for Main ([material-sources.md](material-sources.md),
[geometry-streams.md](geometry-streams.md)). Deformation follows the same group rules:

- group 3 wind and group 4 ripple, as in [noninstanced-deformation.md](noninstanced-deformation.md);
- palette-skinned group-7 partitions, which use the same palette skinning as group 11
  ([skinning-deformation.md](skinning-deformation.md)) without group 11's Main-only depth-axis
  adjustment. Group 7 admission is not yet in a public spec (open question 10). The recorded runs drew group-7
  items;
- static partitions for the other enabled groups.

The *pass object* is the shadow run's own pass record. It has the same layout as Main's pass record,
including the render mode at `+0xDC`, the technique override mask at `+0x194` and the geometry
resource base at `+0x284`. It is a different object from Main's, so each run must be given its own
pass object. Technique selection uses that pass object's render mode, so a material can select a
different (shadow) technique than it does in Main.

### Cascade views

The two cascades come from the game's shadow view preparation. That preparation is already
reimplemented as owned native state in `mag-render-adapter-core` (`views`, `shadow_owner`,
`shadow_call`). Restated here as behaviour:

1. **Inputs.** Three positive plane distances `d0 < d1 < d2`, the camera basis `X`, `Y`, `Z` and
   position `P`, a vertical half-extent scale `v` and an aspect scale `r`, and a directional-light
   vector `L`.
2. **Corners.** For each distance `d`: the half-height is `h = d·v` and the half-width is
   `w = h·r`. The plane centre is `C = P − Z·d`. The four corners are `C ± X·w ± Y·h`, in the order
   (+X,+Y), (−X,+Y), (−X,−Y), (+X,−Y). There are also two axial centres, on the backward axis at
   distances `(d0+d1)/2` and `(d1+d2)/2`.
3. **Cascade bounding sphere.** Cascade `k` covers distances `n = d_k` to `f = d_(k+1)`. Let
   `E(d) = (d·v·r)² + (d·v)²` be the squared radial extent of the plane at distance `d`, and
   `D = f − n`. The sphere centre lies on the backward axis at distance
   `c = min(f, n + (D² + E(f) − E(n)) / (2D))`, so the near and far corners are equidistant when the
   clamp does not apply. The radius is the distance from the centre `P − Z·c` to the far (+X,+Y)
   corner. For an orthonormal basis that is `√((f − c)² + E(f))`.
4. **Refresh.** The two fits persist across updates. Cascade 0 is refitted when `d0` or `d1`
   changes. Cascade 1 is refitted only when `d2` changes, so a change to `d1` alone leaves
   cascade 1's old fit in place. Camera pose, `v` and `r` changes alone do not refit. A real
   (re)initialisation of the shadow configuration invalidates both fits.
5. **Light view.** Each cascade's light view looks along `L` and encloses its sphere. Its exact
   orthographic projection, texel snapping and near/far placement are not yet specified (open
   question 2).

### Shadow map texture

Main's lighting producer samples two depth sources. They are different things:

- **Unit 0** is the *scene* depth: Main's own depth buffer, read through the descriptor at
  `RENDERER+0x3120` or `+0x3140` as a packed Z24S8 alias ([main-producer-inputs.md](main-producer-inputs.md)).
  It is not a cascade map.
- **Unit 1** is the *shadow map*: the full descriptor at `PARAMS+0x1520`, sampled with depth
  comparison. Its two per-cascade transforms project the reconstructed scene position into it.

Whether the image `PARAMS+0x1520` names is exactly the depth target the two Shadow runs render
into is the key edge between this pass and Main. It is expected but not yet confirmed (open
question 11). Until it is confirmed, a native Shadow pass must not replace the image that unit 1
imports.

The map is described by the full texture descriptor at `PARAMS+0x1520`
([material-producers.md](material-producers.md)), where `PARAMS` is the shadow/parameter object.
The two shadow texture transforms at `PARAMS+0xD0` and `PARAMS+0xB50` are one per cascade. The
lighting producer combines each with a scale-bias matrix whose horizontal offset differs between the
cascades ([main-lighting.md](main-lighting.md)). The working hypothesis is that both cascades share
one depth texture, side by side horizontally. That is not yet confirmed (open question 3).

## Outputs

Facts marked *(observed)* come from a job-IO recording of four consecutive runs, two updates times
two cascades, in a populated outdoor scene. The recording itself stays private; only the behaviour
is stated here. Facts marked *(static)* come from the job's set of emitted methods, but no recorded
run used them.

### Which run is which

Runs alternate per update: cascade 0, then cascade 1. Each run's progress semaphore offset
identifies it *(observed)*: `0x490` for cascade 0 and `0x4A0` for cascade 1. Cascade 1 draws more
items than cascade 0 and takes correspondingly longer.

### Command output

Each run writes its commands into the shared command ring *(observed)*:

- in segments of at most 8 KB, each ending in a jump to the next;
- one 32-byte segment descriptor per segment, written into the writer's descriptor table;
- usually several segments per run, and more for cascade 1.

1. **Opening.** The first segment begins with a progress marker: semaphore offset (`0x1D6C`), then a
   texture-read semaphore release (`0x1D74`) whose value is the ring offset of the next word with
   bit 31 set *(observed)*.
2. **Pass state, once per run** *(observed)*:
   - shader packer `0`;
   - depth test on;
   - depth write on;
   - depth function less-or-equal (`0x0203`);
   - blending off;
   - alpha test off;
   - colour mask all channels, then immediately none (`0x01010101`, then `0`);
   - MRT colour mask `0xFFF0`, then `0`.

   The net effect is depth-only rendering: no colour channel is written.
3. **Vertex arrays, once per run** *(observed)*: arrays 1–15 are disabled (format `0x2`: float, size
   0) and the frequency divider is cleared.
4. **Per draw** *(observed)*:
   - vertex array 0 offset and format, for positions only. Formats seen: three floats (stride 12,
     20, 24 or 28) and three half floats (stride 6 or 22), as the partition's stream dictates;
   - vertex constant rows 3–6, the draw's four transform rows;
   - when it changes, vertex constant row 24, the binding row of [material-values.md](material-values.md).
     Observed values have a denominator of 15, and lane 3 differs per cascade;
   - when it changes, vertex constant row 42, one row whose meaning is not yet established
     (question 5);
   - a reserved command region (*hole*) jumped over by the job, which the item's child job fills.
5. **Per-item child jobs** *(observed)*. Each drawn item gets one 464-byte child descriptor. Two
   child kinds appear, one for most items and the other for a small share. The children fill the
   reserved regions, so the item's draw calls and per-material state are not in the Shadow job's own
   output.
6. **Draws the job issues itself** *(observed)*. For a minority of items the job writes the draw
   directly: begin/end with triangles (`5`), an index array in local memory with 16-bit indices
   (`0x181C` with format word `0x10`), and one or more `0x1824` index ranges.
7. **Not emitted** *(observed)*:
   - surface, viewport, scissor or clear methods;
   - stencil or polygon offset;
   - cull state;
   - texture units;
   - any report.

   The render target, its clear, depth bias and stencil are therefore whatever was in effect when
   the run started (question 3). Stencil and polygon-offset methods exist in the job *(static)* but
   no recorded run used them.
8. **Closing.** The last segment is a 128-byte terminal segment *(observed)*: no-operation, the
   semaphore offset, a texture-read semaphore release, then a jump to the continuation.
9. **Progress release values.** Every texture-read semaphore release the run writes has the ring
   offset of the word after it in its low 30 bits. Bits 30–31 carry the *ring generation* in force
   when the release is written. The generation is a 2-bit counter kept in the writer's epoch word.
   It advances by one, modulo 4, each time the ring wraps back to its start. So a run that wraps
   writes its earlier releases with the old generation and its later ones with the new. A run that
   does not wrap leaves the epoch word unchanged. Readers of the semaphore compare both parts: an
   offset alone is ambiguous across laps.
10. **Continuation.** The closing jump targets the writer's address base plus the run's
    continuation offset, which is part of the run's input. Shadow and Depth write through side
    slots 1 and 2, and always return through that continuation, even when the run wrapped the ring.
    Main's slot 0 differs: it jumps to the ring start only on a wrap. That is the shared ring rule;
    see the native ring writer.
11. **Writer publication** *(observed)*: after each batch of segments the run writes:
   - the new ring write offset to the put word;
   - the writer slot's word with value `offset | 2`;
   - the epoch word holding the current ring generation;
   - a 16-byte next-writer-state record (begin, end, current, `0xFFFFFFFF`).

### Shadow map contents

For each cascade: the depth of every drawn item's geometry, as seen from that cascade's light view,
written into the bound depth target with less-or-equal testing and writes on. Colour is masked off.

### Comparison with the Depth job

The two jobs share the input format and most per-item work. In the recorded scene the Shadow job
emitted no stencil, polygon offset or report. The Depth job writes report clears and reports. Both
use per-item child jobs, vertex constant uploads and progress semaphores in the shared ring. The
semaphore offsets are `0x490` and `0x4A0` for Shadow and `0x4B0` for Depth.

## Behaviour required of a native replacement

1. For each cascade run, select the items of the enabled groups from that run's cohort. Prepare each
   item's geometry and technique exactly as Main does, using the run's own pass object.
2. Render the items, in cohort order, depth-only into the bound shadow target, from the cascade's
   light view. Use depth test on, depth write on, less-or-equal, blending and alpha test off and
   colour writes off. Take stencil and depth bias as inherited from the state at run start.
3. Order the result before Main's lighting producer samples the map, through the native equivalent
   of the texture-read semaphore.
4. Meet the run's guest-visible obligations even though the original job does not run. This mirrors
   the native Depth cutover:
   - write the run's command-ring segment progress semaphores and continuation, and its final
     transition state;
   - publish the run's fence and put pointer and its writer state;
   - perform the ready-work notification;
   - let the PPU wait on each run return only after the native result is published.

   The terminal segment shape is in "Command output" step 8. The child jobs must not be created,
   because their work is part of the native draw. Releases carry the ring generation and the
   closing jump uses the side-slot continuation, as in steps 9 and 10. Keep the original job for any
   update that is not wholly owned.
5. A frame whose Main is not natively owned keeps the original Shadow job.

## Edge cases and errors

- An enabled-group mask of 0 is an empty run, not an error.
- An item that any shared spec refuses (geometry, material or technique) is refused for the shadow
  run in the same way. The run is then not natively ownable for that update, and the whole update
  falls back to the original job.
- A cascade whose distances are not strictly increasing and positive, or whose fit is not finite, is
  declined. No empty cascade is fabricated.

## Tolerance

| Output | Tolerance | Reason |
| --- | --- | --- |
| Cascade sphere centre distance and radius | relative `2^-20` | Shadow coverage changes by far less than a texel. |
| Corner and axial-centre positions | relative `2^-21` of the largest coordinate magnitude | |
| Shadow map depth per texel | within the depth format's quantum plus the depth bias tolerance | Shadow tests compare against the map with a bias. |
| Copied guest words | bit-exact | data movement |

**Consequences of non-bit-exact output.** Shadow tests in Main's lighting producer compare
reconstructed depth against the map. Native positions that differ slightly from the original's
cause acne or peter-panning only where the depth bias does not absorb the difference. Compare
against the original visually, or with a per-texel tolerance of a few depth quanta, not bit for
bit. A cascade fit computed natively may differ in its last bits from the game's own fit. Because
fits persist across updates, the native fit must be the only one used once the native path owns the
pass.

## Synthetic test vectors

1. **Group mask.** Group counts `[5, 3, 0, 2, 0, 0, 0, 4, 9, …]` with mask `0xFE`: items from groups
   1, 3 and 7 only, 9 in total. Mask `0`: no items, terminal obligations still met.
2. **Sphere fit, unclamped.** `n = 10`, `f = 30`, `v = 0.1`, `r = 1`: `E(n) = 2`, `E(f) = 18`,
   `c = 20.4`, radius `√110.16 ≈ 10.495713`. The near corner is at the same distance.
3. **Sphere fit, clamped.** `n = 1`, `f = 3`, `v = 0.5`, `r = 2`: the unclamped centre would be 4.5,
   so `c = 3` and the radius is `√11.25 ≈ 3.354102`.
4. **Sphere fit, third.** `n = 5`, `f = 20`, `v = 0.25`, `r = 1.5`: `c = 15.0390625`, radius
   `≈ 10.288873`.
5. **Refresh rule.** After fits at `(10, 30, 60)`, a change to `(10, 31, 60)` refits cascade 0 only.
   A change to `(10, 30, 61)` refits cascade 1 only. A change to `(11, 30, 60)` refits cascade 0
   only.
6. **Corners.** `P = 0`, `X = (1,0,0)`, `Y = (0,1,0)`, `Z = (0,0,1)`, `d = 4`, `v = 0.5`, `r = 2`:
   `h = 2`, `w = 4`, centre `(0, 0, −4)`, corners `(4, 2, −4)`, `(−4, 2, −4)`, `(−4, −2, −4)`,
   `(4, −2, −4)`.
7. **Ring generation.** Epoch generation `0b10` and no wrap during the run: every release has bits
   30–31 `0b10`, and the epoch word is unchanged. Generation `0b11` and one wrap: releases written
   before the wrap carry `0b11`, those after it carry `0b00`, and the epoch word becomes `0b00`. A
   release for ring offset `0x001230` under generation `0b11` has the value `0xC0001230`.

## Open questions

**Settled by the recording.**

- Pairing: the runs alternate cascade 0, then cascade 1, and the semaphore offset (`0x490` or
  `0x4A0`) identifies each.
- State values: depth-only, less-or-equal with writes, no blend, no alpha test, no colour (former
  question 1, except stencil and bias, which no run emitted).
- Colour target writes: masked off (former question 4).
- Terminal segment shape and writer publication (former question 6).
- The release flag is the ring generation, and the continuation follows the side-slot rule (former
  question 6, settled by checking the recordings against the shared ring writer).
- Disjointness: the two cascades' lists are *not* disjoint. Most of cascade 0's items also appear in
  cascade 1 in the same update (former question 8).

**Still open.**

1. **Stencil and depth bias.** No recorded run emitted stencil or polygon offset, so both come from
   the state in effect at run start. Which earlier work sets them, and with what values, needs the
   FIFO segment just before each run.
2. **Light view and projection.** This is PPU-side work. Rows 3–6 carry each draw's transform, but
   their construction from the cascade sphere (orientation, depth range, snapping) still needs the
   shadow owner's view construction traced.
3. **Target, layout and clear.** The job sets no surface, viewport, scissor or clear. Recording the
   FIFO just before each run settles these, together with question 11: both cascades in one texture
   or not, size, format, viewport and clear value.
5. **Row 42.** What the one-row upload at vertex constant row 42 holds.
7. **Child work.** Technique selection, programs and per-material state are in the per-item child
   jobs, not in the Shadow job. They need recordings of the material-binding child (image entry
   `0x105F8`) and the geometry child (image entry `0x15898`) during shadow runs.
9. **Groups outside the mask.** Whether any setting enables groups outside `0xFE`. The recorded runs
   drew items of groups 1, 2, 3, 4 and 7 only.
10. **Group 7 skinning admission.** `skinning-deformation.md` admits group 11 only. The group-7
    admission conditions (geometry mode, worker control, palette inputs) need their own statement
    before group-7 casters can be prepared natively.
11. **Unit-1 import identity.** Compare the shadow map descriptor at `PARAMS+0x1520` (location,
    offset, pitch, size, format) against the depth surface bound for the two Shadow runs, in the
    same update. If it matches, the native Shadow pass writes the image that Main's lighting unit 1
    imports. Also check whether the game's post-join cascade-target record and handle write change
    which image that descriptor names. The job recording cannot settle this, because the job sets no
    surface.

Observed cost, for orientation: in Training the PPU waits about 1.8 ms on cascade 0 and about 4.3 ms
on cascade 1, about 6.1 ms of a 29 ms update.
