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

**Status: partial.** The structure below comes from the job's construction, its input format (shared
with Main and Depth), the cascade-view behaviour and the job's set of emitted RSX methods. The values
of most per-pass state words, the render target setup and the clear behaviour have not been
established. They are listed in Open questions together with the recording that would settle them.
An implementation must not guess those values. Until they are settled, it may only implement the
established parts behind a default-off switch.

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
produced for that run's family. The cohort, item, draw-batch (MBI) and transform formats are the
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
  adjustment. Group 7 admission is not yet in a public spec (open question 10);
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

The map is described by the full texture descriptor at `PARAMS+0x1520`
([material-producers.md](material-producers.md)), where `PARAMS` is the shadow/parameter object.
The two shadow texture transforms at `PARAMS+0xD0` and `PARAMS+0xB50` are one per cascade. The
lighting producer combines each with a scale-bias matrix whose horizontal offset differs between the
cascades ([main-lighting.md](main-lighting.md)). The working hypothesis is that both cascades share
one depth texture, side by side horizontally. That is not yet confirmed (open question 3).

## Outputs

1. **Shadow map contents.** For each cascade, the depth of every drawn item's geometry as seen from
   that cascade's light view, written into the cascade's region of the shadow map. Colour output,
   if any target is bound, is not consumed (open question 4).
2. **Fixed-function state per draw.** The run builds the following state itself, in addition to
   whatever its copied setup banks contain. The methods are public NV4097 interface facts; the values
   are open (question 1):
   - alpha test enable (`0x0304`), blend enable (`0x0310`), colour mask (`0x0324`) and the MRT colour
     mask (`0x0370`);
   - stencil: enable (`0x0328`), front write mask (`0x032C`), front function, reference and read
     mask (`0x0330`–`0x0338`), front operations (`0x033C`–`0x0344`), back write mask (`0x034C`),
     back function, reference and read mask (`0x0350`–`0x0358`) and back operations
     (`0x035C`–`0x0364`);
   - depth function (`0x0A6C`), depth write mask (`0x0A70`) and depth test enable (`0x0A74`);
   - polygon offset scale factor and bias (`0x0A78`, `0x0A7C`), the shadow depth bias;
   - shader packer (`0x1FEC`).
3. **Per-item geometry and programs.** Vertex array offsets and formats for slots 6 and 12
   (`0x1698`, `0x16B0`, `0x1758`, `0x1770`); the current value of attribute 6 (`0x1C60`); vertex
   constant uploads of one, two and eight rows (`0x1EFC` with 4, 8 and 32 data words); and
   indexed draws (`0x1808` begin/end, `0x181C` index array address and format). The eight-row upload
   is the per-cascade light transform (open question 5).
4. **Completion.** After the last draw of a run, a texture-read semaphore release (`0x1D6C`
   semaphore offset, then `0x1D74`) tells later texture reads that the shadow map is complete. Main's
   lighting producer must not sample the map before this point. Unlike the Depth job, the Shadow job
   writes no report (`0x17C8`/`0x1800`).

### Comparison with the Depth job

The two jobs share the input format and most per-item work. The Shadow job adds stencil state,
polygon offset and the texture-read semaphore. It omits the Depth job's report clear and report
write. Both upload vertex constants, set vertex arrays 6 and 12 and attribute 6, and set alpha test,
blend, colour mask and depth state.

## Behaviour required of a native replacement

1. For each cascade run, select the items of the enabled groups from that run's cohort. Prepare each
   item's geometry and technique exactly as Main does, using the run's own pass object.
2. Render the items, in cohort order, depth-only into the cascade's region of the shadow map, from
   the cascade's light view, with the run's fixed-function state and depth bias.
3. Order the result before Main's lighting producer samples the map, through the native equivalent
   of the texture-read semaphore.
4. Meet the run's guest-visible obligations even though the original job does not run. This mirrors
   the native Depth cutover:
   - write the run's command-ring segment progress semaphores and continuation, and its final
     transition state;
   - publish the run's fence and put pointer and its writer state;
   - perform the ready-work notification;
   - let the PPU wait on each run return only after the native result is published.

   The exact terminal segment of an empty Shadow run is not yet specified (open question 6). Until
   it is, the original job must keep running.
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

## Open questions

Questions 1, 4, 5, 6, 7 and 8 can all be settled by one job-IO recording of the Shadow job, taken
the same way as the Main recording: set `MAG_JOB_IO_ENTRY` to the payload entry `0x12F68`, and
record both runs of several updates in a scene with visible shadows. Run the recording through
`tools/jobio.py`, `tools/jobio_provenance.py` and `tools/jobio_rules.py`. That yields the emitted
command words with their input provenance, and so the state values and their item-level rules.

1. **State values.** The values the run writes for alpha test, blend, colour masks, stencil (both
   faces), depth function, depth write and test, polygon offset factor and bias, and shader packer.
   Also whether they are fixed per run, set per group, or taken from the material.
2. **Light view and projection.** How each cascade's sphere becomes an orthographic light view: its
   orientation (the up vector), depth range, texel snapping, and where the eight-row transform comes
   from. This is PPU-side work and needs a PPU-side trace of the shadow owner's view construction,
   or the recording's eight-row constant values matched against the sphere fit.
3. **Map layout.** Whether both cascades share one texture (and if so how it is split), the map's
   size and format, and the viewport and scissor of each run. Surface, viewport and clear methods
   are not among the state the run builds itself. The target is therefore set either by work
   outside the job (the PPU, or the FIFO just before the run) or by a setup bank the run copies
   unchanged. The recording plus the FIFO segment just before each run settles which.
4. **Colour target.** Whether a colour target is bound during the shadow runs, and if so whether
   anything reads it.
5. **Constant rows.** Which vertex constant rows the one-, two- and eight-row uploads write, and
   their meaning.
6. **Terminal segment.** The exact command-ring segment an empty Shadow run writes: progress
   semaphores, final state, continuation, and the texture-read semaphore offset and value. The
   native Depth cutover needed the equivalent for Depth.
7. **Technique selection.** Which technique the shadow pass object's render mode selects for each
   enabled group, and whether alpha-tested casters (foliage cards in groups 3 and 4) use a fragment
   program that discards.
8. **Families and cameras.** Which cohort camera index each run's caster list uses, and whether the
   two runs always draw disjoint lists.
9. **Groups outside the mask.** Whether any later setting enables groups outside `0xFE` (for example
   group 11 characters), or whether characters cast shadows by another route.
10. **Group 7 skinning admission.** `skinning-deformation.md` admits group 11 only. The group-7
    admission conditions (geometry mode, worker control, palette inputs) need their own statement
    before group-7 casters can be prepared natively.
