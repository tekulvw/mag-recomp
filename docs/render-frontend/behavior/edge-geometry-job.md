# Edge geometry child job

## Scope

The streamed WWS job that turns one mesh partition into RSX draw commands on the retail v1.0
build. Parent render jobs (Main, Depth, Shadow and Other) queue one invocation of this job per
drawn partition. Each invocation:

1. decodes the partition's positions, and its other attributes when it produces vertices;
2. optionally deforms them: instancing with wind, wind, ripple or palette skinning;
3. projects positions to the screen and removes triangles that cannot produce pixels;
4. writes the surviving 16-bit index list, and any produced vertices, into an output ring in RSX
   local memory;
5. fills a command region (the *hole*) that the parent reserved in its command ring with vertex
   array setup and an indexed draw;
6. hands the invocation back to the job manager.

This document specifies the guest-visible result of one invocation so that a native
implementation can replace it on all hosts, with the default SDK RSX renderer as well as the
optional frontend. It also states the integration contract, the acceptance gates and a performance
plan for that replacement.

Out of scope:

- how parents choose items and build the child descriptor ([shadow-pass.md](shadow-pass.md) and the
  Main specifications);
- the WWS job manager itself, except for the four job API services an invocation uses;
- the vertex and fragment programs that later consume the output;
- the Rust frontend's own geometry path, which does not run this job.

### Identity

| Item | Value |
| --- | --- |
| Module guest address | `0x01963A00` (lifted as `mag_wwsjob_01963a00`) |
| Module image size | `0x1B870` bytes, loaded at local store `0x4000` |
| Module image SHA-256 | `e6e888bb338de47fb7508ba8082845044e8cb8c560dd660760649b0d7c9949ce` |
| Job entry | local store `0x15898` |
| Dispatching policy | WWS policy SHA-256 `6a3af90518af72ed790b36b2081ab2040237d378bf3644d4461356b44bcfc58a`; the payload call at `0x325C` returns to `0x3260` (the seam `tools/graphics_payload_hooks.py` instruments) |

### Evidence

Statements marked *(verified)* were checked against complete job I/O recordings of this job, made
with the default renderer:

- 800 from two Training samples;
- 6,000 from hosted Sabotage at spawn, in three windows, covering Main and both shadow cascades;
- 1,623 from Training with camera movement, in three windows. These cover triangles that reach
  behind the camera, group-11 skinning in the Main view, groups 12 and 13, and a 160 × 160 view.

Behaviour for triangles that reach behind the camera was also measured directly. The original job
was replayed on the SDK interpreter with about 5,000 synthetic triangles substituted into a
recorded invocation.

Every recorded invocation was compared with a black-box model written from this document.

- **Training:** the model reproduces the culled index list exactly in 792 of 794 checkable
  invocations. It also reproduces all 800 holes byte for byte, and every output size and ring
  update.
- **Sabotage:** it reproduces 5,921 of the checkable invocations exactly.
- **Training with movement:** it reproduces 1,452 of 1,461 checkable invocations exactly.

Every remaining difference, in all three sets, is a borderline triangle as defined under
Tolerance. Statements marked *(observed)* hold in every recording but were not
varied. Statements marked *(unconfirmed)* have no evidence and must be checked with the oracle (see
Integration contract) before a native path relies on them.

## Feature inventory (retail v1.0)

Sources: one Sabotage Main frame and two Shadow cascades (child descriptors written by the parent
jobs), and the two Training samples (complete job recordings).

### Invocations per frame

| Parent | EdgeGeom children | Other children |
| --- | ---: | ---: |
| Main (Sabotage, one frame) | 2,163 | 245 material-binding jobs |
| Shadow cascade 0 / 1 | 366 / 999 | 36 / 48 |
| Depth | not recorded; same descriptor format | |

With Depth, a Sabotage frame runs roughly 4,000 to 4,600 invocations. Earlier timing put the mean
lifted invocation at about 23 µs of host CPU, which is consistent with the measured 49 ms (macOS)
to 130 ms (Linux) of SPU-thread CPU per frame.

### Kinds of invocation

The *kind* is selected by the userdata mode word, cull flavour and material group (Inputs).

| Kind | Mode / flavour | Groups | Main share | Shadow share | Output |
| --- | --- | --- | ---: | ---: | --- |
| Static | `0x08` or `0x0A` / 2 | 1, 2, 8, 12, 13, 15 (also 3 without deformation); any group with an empty output layout | about 70 % | about 80 % | indices only |
| Instanced wind | `0x28` / 1 | 0 | about 15 % | 0 | positions (12 bytes per vertex) and indices |
| Wind | `0x28` / 1 | 3 | about 14 % | about 13 % | 20-byte vertices and indices |
| Ripple | `0x28` / 1 | 4 | 0.6 % | 0.2 % | 28-byte vertices and indices *(unconfirmed layout)* |
| Skinned | `0x08` or `0x0A` / 2 | 7, 11 (group 11: the first-person view model in Main, about 10 per 1,600 Training invocations) | 0.4 % | about 3 % | 24-byte vertices and indices |

Typical sizes (Training sample, vertices `V` / indices `K`, medians): static 30 / 54 (largest 886 /
4,866), instanced 24 / 36 with 1 to 88 instances, wind 42 / 108, skinned characters 710 / 3,930
(largest 1,112 / 4,602). The skinned invocations are the most expensive per call; the static ones
dominate by count.

### Formats in use

| Feature | Seen | Not seen |
| --- | --- | --- |
| Position codecs | three `f32` (codec 0), three rebased halves (descriptor codec 3), packed 11/11/10 (descriptor codec 6, group 0 only) | anything else |
| Other input attributes | packed spherical normal (codec 9), four rebased halves for tangents (codec 3), four unsigned bytes for weights (codec 4) | |
| Index format | raw big-endian `u16` (configuration byte 6 high nibble 1) | compressed indices |
| Skinning | four (weight byte, joint byte) pairs, 3×4 `f32` palette, palettes of 14 to a few dozen matrices | other influence layouts or matrix formats |
| Blend shapes | never (userdata count is always 0) | any |
| Culling | frustum sides, near plane (shadow cascades), back-face (flavour 2), zero area (flavour 1), small primitive | far plane as the only reason; software depth testing |
| Output layouts | empty (index only), built-in layout 0, custom layouts of 20 and 24 bytes | built-in layouts 1–3 |
| Instance counts | 1 to 88 | |

## Inputs

All guest data is big-endian.

### Child descriptor (464 bytes, guest memory)

Written by the parent; the job manager reads it.

| Offset | Size | Meaning |
| --- | --- | --- |
| `+0x000` | 64 | WWS job command words. Word `+0x0C` is the module address `0x01963A00`. Not read by the job itself. |
| `+0x040` | 192 | userdata (below) |
| `+0x100` | 192 | input list: 24 entries of (`u32` byte length, `u32` guest address), one per stream slot |
| `+0x1C0` | 16 | zero |

### Userdata (192 bytes)

| Offset | Type | Meaning | Status |
| --- | --- | --- | --- |
| `+0x20` | `f32` ×4 | material row `M0` | verified (wind and instancing) |
| `+0x30` | `f32` ×4 | material row `M1` | verified |
| `+0x40` | `f32` ×4 | material row `M2` | ripple only; unconfirmed |
| `+0x50` | `f32` ×2 | instancing fade multiplier `φm`, fade start `φs` (already decoded to `f32`) | verified |
| `+0x60` | `f32` ×4 | object quaternion `(x, y, z, w)`, used as stored | verified |
| `+0x70` | `f32` ×3 | object translation `t` | verified |
| `+0x7C` | `f32` | object scale `σ` | verified |
| `+0x80` | `f32` ×4 | wind factors `(ω0, ω1, ω2, ω3)`; `ω1` unused | verified |
| `+0x90` | `f32` | wind phase `θ` | verified |
| `+0x94` | `u32` | output ring table address `R` | verified |
| `+0x98` | `u32` | hole guest address `H` | verified |
| `+0x9C` | `u32` | hole size in 16-byte units (always equal to configuration byte 1) | observed |
| `+0xA4` | `u8` | blend-shape count; 0 in every observation | observed |
| `+0xA5` | `u8` | cull flavour: 1 two-sided, 2 back-face | verified |
| `+0xA6` | `u8` | not used by any verified behaviour (values 2, 3, 5 seen) | |
| `+0xA7` | `u8` | material group 0–15 | verified |
| `+0xAC` | `u32` | mode word: `0x08`, `0x0A` or `0x28` | verified |
| `+0xB0` | `u8` | instance count `I` (1 unless group 0) | verified |

The other bytes (`+0x00..+0x1F`, `+0x58..+0x5F`, `+0xA0..+0xA3`, `+0xA8..+0xAB`, `+0xB1..+0xBF`)
affect no verified output. A native path must not depend on them. Mode `0x0A` behaved exactly
like `0x08` in every observation.

These fields are copies made by the parent: `M0`, `M1` and `M2` come from the primary material,
`+0x60..+0x7F` from the object transform record, and `+0x80..+0x90` from the pass record
`+0x260..+0x270`. They match the inputs of the deformation specifications listed under Behaviour.

### Stream slots

Slot roles are as in [geometry-streams.md](geometry-streams.md). Every length is a multiple of 16
*(observed)*.

**Long streams continue in the next slot.** A single list entry carries at most 16 KiB. When the
index list or the primary vertex data is longer than that, the stream is split:

- slot 1 carries the first `0x4000` bytes of the index list and slot 2 the rest;
- slot 7 carries the first `0x4000` bytes of the primary vertex data and slot 8 the rest.

The two parts are contiguous in guest memory: slot 2's address is slot 1's address plus `0x4000`,
and likewise for slots 7 and 8. Because buffer 1 packs the slots in slot order, they are also
contiguous in local store. Read each stream as one run across both slots. This is *verified*:
7 invocations, all reproduced exactly when the run is read that way. The `0x4000` length here is
the same quantity as the large-stream control in geometry-streams.md. The parent does not
replace it; it adds the second slot instead.

| Slot | Content here |
| --- | --- |
| 0 | output layout (descriptor table; zero declarations means *index only*) |
| 1 | index list, `K` raw `u16` (first 16 KiB of it, when slot 2 is non-empty) |
| 2 | continuation of slot 1 |
| 3, 4 | skinning palette segments, concatenated |
| 5 | skinning influences, 8 bytes per vertex |
| 7 | primary vertex data (first 16 KiB of it, when slot 8 is non-empty) |
| 8 | continuation of slot 7 |
| 10 | secondary vertex data |
| 13 | instance rows, 16 bytes each (group 0) |
| 17 | view record, 128 bytes (below) |
| 19 | configuration, 16 bytes |
| 20, 21 | primary and secondary descriptor tables |

Slots 9, 11 and 12 are non-empty in at least one recording, but no verified output depends on
them.

### Configuration (slot 19)

The field meanings are in [geometry-streams.md](geometry-streams.md). Values seen here:

| Byte | Values seen | Meaning here |
| --- | --- | --- |
| 0 | `0x81`, `0x83` | stream set (one primary stream; or primary plus secondary) |
| 1 | 7–15 | hole size in 16-byte units |
| 2 | `0x00`, `0xFF` | position codec: three `f32`, or the slot-20 table |
| 3 | `0xFF` | secondary attributes from the slot-21 table |
| 4 | `0x00`, `0xFF` | output layout: built-in layout 0, or the slot-0 table |
| 5 | `0xFF` | no vertex-delta stream |
| 6 | `0x10`, `0x11` | high nibble 1: raw `u16` indices; low nibble 1: skinned with four byte pairs |
| 7 | `0x00` | palette of 3×4 `f32` matrices |
| 8–9 | | `V` |
| 10–11 | | `K` |
| 12–15 | `0xFFFFFFFF` | |

### View record (slot 17, 128 bytes)

One record per pass view, shared by every child of that view.

| Offset | Type | Meaning |
| --- | --- | --- |
| `+0x00` | `u16` ×2 | viewport origin x, y in pixels |
| `+0x04` | `u16` ×2 | viewport width, height (1280 × 720 for Main, 1024 × 1024 for each shadow cascade) |
| `+0x08` | `f32` ×2 | depth range (0, 1) |
| `+0x10` | `f32` ×16 | clip matrix `C`, four rows of four, row-major, applied to column vectors |
| `+0x50` | `f32` ×3 | viewport scale `(kx, ky, kz)`; `ky` is negative |
| `+0x60` | `f32` ×3 | viewport offset `(ox, oy, oz)`, which includes the origin |
| `+0x70` | `u8` | projection kind; always 3 |

Only `C`, the scale and the offset affect verified behaviour.

### Output ring record (guest memory, 128 bytes)

There is one record per virtual SPU. The record for the SPU running the invocation is at
`R + 0x80·(s + 1)`, where `s` is that SPU's number (0–4 observed). `s` is word `+0x00` of the job
API context, and the virtual SPU number the runtime keeps for the context.

| Offset | Type | Meaning |
| --- | --- | --- |
| `+0x00` | `u32` | ring start `S` (guest address in RSX local memory, 128-byte aligned) |
| `+0x04` | `u32` | ring start as an RSX local-memory offset |
| `+0x08` | `u32` | ring end `E` |
| `+0x0C` | `u32` | top `T`: next free byte |
| `+0x10` | `u32` | 0 |
| `+0x14` | `u32` | label address `L` (a 4-byte RSX semaphore) |
| `+0x18` | `u32` | cached consumed position `U` |
| `+0x1C..+0x7F` | | zero |

## Behaviour

### 1. Admission (decided before any guest-visible effect)

A native implementation handles an invocation only if every check below passes. Otherwise it leaves
the invocation to the original job and makes no guest-visible change.

1. Module, entry and policy identities match the table above.
2. Mode word `0x08` or `0x0A` with flavour 2, or mode word `0x28` with flavour 1. The mode word
   and flavour select culling only; vertex production is selected by group and output layout
   (section 3).
3. Blend-shape count 0.
4. Configuration byte 6 is `0x10` or `0x11`, byte 7 is 0, byte 5 is `0xFF`, and bytes 12–15 are
   `0xFFFFFFFF`.
5. Hole size: configuration byte 1 is non-zero and equals userdata `+0x9C`. `H` is non-zero and
   16-byte aligned.
6. **Stream lengths.** The listed lengths, counting slots 2 and 8 as continuations, must cover
   what the counts need:
   - slots 1 + 2 have at least `2K` bytes;
   - slots 7 + 8 have at least `V` times the primary stride;
   - slot 13 has at least `16I` bytes;
   - slot 17 has 128 bytes;
   - slot 19 has 16 bytes.

   One exception is not refused: an index-only invocation whose index or vertex run is still
   short. The original draws nothing for it (*verified*, 128 of 128 recordings): no allocation,
   no data, and a hole of only the four invalidate packets. A native path produces exactly that.

   Any other short invocation (produced kinds: groups 0, 3, 11) is refused. In such invocations
   the original reads on past the listed length into whatever bytes follow in local store, so the
   result depends on the neighbouring streams and on stale data (open question 3).
7. The production rule below is one of the supported kinds. Unsupported kinds (ripple, any
   built-in output layout other than 0, any undecodable codec) are refused until the oracle
   confirms them.
8. Every index is below `V`. When the lengths are sufficient but an index is `V` or more, the
   original still draws. It reads that vertex's position from the bytes after the vertex stream
   (2 recordings), so the invocation is refused.
9. The hole content (section 7) fits in the hole.
10. Ring allocation (section 6) succeeds.

Checks 1–9 depend only on the invocation's inputs. So a given partition, mode and pass inputs
always get the same decision. Check 10 depends on ring state; when it fails the original job runs,
with the same output as a native success except for where the data is placed.

### 2. Decoding

Positions and attributes decode as in [geometry-streams.md](geometry-streams.md) and
[geometry-normals.md](geometry-normals.md). With configuration byte 2 = 0, positions are `V` rows of
three `f32` in slot 7. Otherwise they come from the slot-20 table over slot 7. Secondary
attributes come from the slot-21 table over slot 10.

### 3. Vertex production

Let `P` be the decoded positions and `N`, `Tg`, `W` the normal, tangent and weight banks.

| Kind | Rule | Produced vertices `Vout` |
| --- | --- | --- |
| Index only: the output layout has no declarations and `I = 1`, whatever the group (groups 1, 2, 3, 8, 12, 13 and 15 observed) | no vertices are written. Culling uses `P`. | none |
| Group 0, mode `0x28` | [group-zero-deformation.md](group-zero-deformation.md), "Per instance" and "Per vertex", with the userdata mapping above and the game's wind values below. `E3` is view record row `+0x40`, instance rows are slot 13, `φm` and `φs` are userdata `+0x50` and `+0x54`, and `T` is the object translation. The colour rows of that specification are not produced here. | `V·I`, instance-major |
| Group 3 with a non-empty output layout, either mode word (`0x28` with flavour 1, or `0x08` with flavour 2) | [noninstanced-deformation.md](noninstanced-deformation.md), group 3, with `Σ = t.x + t.y + t.z` and the game's wind values below. The mode-`0x08` form is wind with back-face culling, not a pass-through: its positions are displaced exactly like mode `0x28` (verified, 6 Sabotage invocations). Group 3 with an empty output layout is index only. | `V` |
| Skinned (byte 6 low nibble 1), groups 7 and 11 | [skinning-deformation.md](skinning-deformation.md) in full: palette blend of positions, normals and tangents, then normals and tangents (not positions) turned by the object direction matrix `O = σ·R(q)` from the userdata quaternion and scale. The palette is slots 3 and 4 concatenated, the influences slot 5. Verified for group 11 with non-identity object rotations: positions within 1e-6, normal fields within 1 unit, tangents within binary16 rounding, tangent handedness lane exact. An empty palette (slots 3 and 4 both empty) is refused, as that specification requires. | `V` |

The produced positions stay in the object's frame. They are not moved by the object transform.

#### Wind values (game-supplied)

This job uses the game's own wind coefficients. The two deformation specifications keep this
project's own tuning values for the frontend; this job does not use those. The coefficients are
game-chosen values, stated here as observed behaviour under the public-specification ruling
for game state values (2026-11-03).

They were measured black-box. First the recorded outputs were fitted by least squares. Then the
original job was replayed on the SDK interpreter with the wind phase `θ` stepped through 24–40
values, and with one material amplitude at a time set to zero to separate the two terms. No
original code or constant table was consulted. Use each value as the binary32 nearest the
decimal shown.

| Coefficient | Role | Group 0 | Group 3 |
| --- | --- | --- | --- |
| `KA1` | phase rate of the slow vertical wave | 0.375 | 0.375 |
| `KA2` | phase rate of the second vertical wave | (none) | 0.193 |
| `KB1` | phase rate of the fast horizontal wave | 1.975 | 1.975 |
| `KB2` | phase rate of the second horizontal wave | (none) | 0.793 |
| `KC`, `KC3` | weight of the summed position in the horizontal phase | 0.33 | 0.33 |

Everything else in the two specifications' formulas is unchanged. With these values the
specification formulas reproduce the recorded outputs:

- group 0: all 15,936 recorded instance vertices to 1.5e-5 absolute;
- group 3: all 5,769 Training vertices to 1.9e-4 absolute (99th percentile 2.6e-5). The worst
  Sabotage error is 0.0045 units, where `Σ ≈ 545`. That is within the tolerance formula of
  [noninstanced-deformation.md](noninstanced-deformation.md), at most 0.85 of its bound.

The group-3 horizontal phase is ill-conditioned. Its coupling multiplies the object-translation
sum, which is in the hundreds to thousands in the recordings, so binary32 rounding alone moves the
phase by hundredths of a cycle. The error is therefore bounded by the horizontal sway amplitude,
which the noninstanced tolerance already covers, and not by the coefficients.

### 4. Projection

For every vertex that culling reads (each produced vertex, or each decoded position when index
only):

1. World position `w = σ·R(q)·p + t`. `R(q)` is the quaternion matrix of
   [skinning-deformation.md](skinning-deformation.md), step 5, applied to the stored quaternion.
2. Clip position `c = C·(w, 1)`.
3. Screen position `sx = (c.x / c.w)·kx + ox`, `sy = (c.y / c.w)·ky + oy`.

### 5. Triangle culling

The index list is read as `K/3` triangles `(a, b, c)`, in order. For group 0, the list is repeated
for each instance `i` from 0 to `I − 1`, with `i·V` added to every index. A triangle is removed
when any of these holds (verified, all flavours unless stated):

1. **Outside one frustum side.** Either all three vertices are in front of the near plane, or all
   three lie beyond the same side of the viewport.
   - *Near plane:* all three have `c.z < −c.w`. In Sabotage shadow cascades, all 469 triangles
     that only this plane separates were removed.
   - *Viewport sides:* these are tested on the side position
     `(ux, uy) = ((c.x·kx + c.w·ox) / |c.w|, (c.y·ky + c.w·oy) / |c.w|)`.
     The removal conditions, each over all three vertices, are:
     - `ux < ox − |kx|`;
     - `ux > ox + |kx|`;
     - `uy < oy − |ky|`;
     - `uy > oy + |ky|`.

     The bounds are the viewport rectangle. Note that the division is by `|c.w|`, not `c.w`:
     - For `c.w > 0`, `(ux, uy)` is the screen position `(sx, sy)` and the test is the ordinary
       one.
     - For `c.w < 0` it is not a projection, and the four sides are no longer symmetric. With the
       Main viewport and `c.w = −1`, a vertex counts as left of the viewport when `c.x < 1`, but
       right of it only when `c.x > 3`.

     This was measured by replaying the original on synthetic triangles. It is the rule that
     separates kept from removed triangles that reach behind the camera. Plain clip comparisons
     and divided (NDC) comparisons each disagree with the original on hundreds of them.
2. **Facing.** Let `A = (sx_b − sx_a)·(sy_c − sy_a) − (sx_c − sx_a)·(sy_b − sy_a)`.
   - Flavour 2: removed unless `A < 0`. Zero and positive areas are removed.
   - Flavour 1: removed only if `A = 0`.
3. **Small primitive.** All three vertices have the same `floor(4·sx)`, or all three have the same
   `floor(4·sy)`. That is, the triangle lies inside one quarter-pixel column or one quarter-pixel
   row.

The rules are applied in the order listed. Only whether a triangle survives is visible, not which
rule removed it. Surviving triangles keep their order and their vertex order.

**Crossing the near plane.** A triangle that rule 1 does not remove, but that has at least one
vertex with `c.z < −c.w` or `c.w ≤ 0`, is kept. Rules 2 and 3 are not applied to it. Verified in
two ways:

- 141 of 141 partly-near triangles in Sabotage were kept, whatever their facing or quarter-pixel
  extent;
- synthetic crossing triangles confined to one quarter-pixel column were kept, 141 of 141.

The GPU clips such a triangle, so keeping it never hides geometry.

A vertex with `c.w = 0` exactly was not measured. An implementation may treat it as `c.w < 0` with
`|c.w|` replaced by the smallest positive normal binary32.

**Far plane.** No vertex beyond the far plane (`c.z > c.w`) appeared in any recording. An
implementation may apply the far-plane side test or not.

`K'` is the number of surviving indices (three per surviving triangle).

### 6. Output allocation

When `K' = 0`, nothing is allocated, no data is written and the draw is omitted (section 7).

Otherwise:

- index bytes `bi = 2·roundup(K', 8)`;
- vertex bytes `bv = roundup(stride·Vout, 16)`, or 0 when index only;
- request `n = roundup(bi + bv, 128)`.

The ring record is updated atomically on its 128-byte line. Its in-use region runs from `U` up to
`T`, wrapping at `E` back to `S`:

1. Candidate start `X = T` if `T + n ≤ E`, otherwise `X = S` (wrap).
2. The candidate is free if `[X, X + n)` does not reach into the in-use region:
   - if `U > T`, the free bytes are `[T, U)`;
   - if `U ≤ T`, the free bytes are `[T, E)` and `[S, U)`.

   The allocation must end strictly before `U` when it is placed below `U` (open question 4).
3. If the candidate is not free, refresh `U` from the 4-byte value at `L` and try again. A
   refresh writes the new `U` to `+0x18` when the record is updated.
4. On success, `T` becomes `X + n` and the record is written back. All other fields are unchanged.
5. If the candidate is still not free after one refresh, the native path refuses the invocation
   (admission check 10) without touching the record. Waiting is not specified; the original's
   behaviour in this case was not observed.

The record line is shared with the original job running on the same virtual SPU in other
invocations. Native and original invocations must therefore follow exactly this protocol, so that
either can run next.

### 7. Output data

At `X`, written in this order:

1. `K'` surviving indices as big-endian `u16`, then padding to `bi` bytes. The padding values are
   unspecified.
2. At `X + bi`, the `Vout` produced vertices in the output layout, then padding to `bv` bytes,
   values unspecified.

Output layouts:

| Layout | Stride | Fields |
| --- | --- | --- |
| Built-in 0 (group 0) | 12 | position as three `f32` |
| Custom, group 3 (observed) | 20 | position three `f32` at 0; normal as RSX CMP at 12; weights as four bytes at 16 |
| Custom, skinned (observed) | 24 | position three `f32` at 0; normal as RSX CMP at 12; tangent as four binary16 at 16 |

A custom layout is the slot-0 table read as in [geometry-streams.md](geometry-streams.md). Byte 1
of each declaration is the RSX vertex format, byte 2 the component count, byte 3 the semantic,
byte 5 the RSX attribute slot. The custom layouts above are the ones observed. Implementations
encode per declaration and refuse formats they do not implement.

Field encodings:

- **Position:** the produced `f32` values.
- **Weights (four bytes):** the input bytes, copied unchanged (verified bit-exact).
- **Normal (RSX CMP):** x in bits 0–10, y in bits 11–21 (signed 11-bit), z in bits 22–31 (signed
  10-bit). The reference quantization is `floor(v·1023.5 − 0.5)` for x and y and
  `floor(v·511.5 − 0.5)` for z, clamped to the field range. The original agrees with this
  reference to within one unit in 99.9 % of fields.
- **Tangent (four binary16):** each lane converted to binary16. Lane 3 (handedness ±1) is exact.

### 8. Hole content

The hole is `16·h` bytes at `H` (`h` = configuration byte 1). The parent filled it beforehand
with a jump to itself at its start and at every 128-byte line boundary inside it. The RSX may
already be waiting on those jumps when the invocation runs.

The invocation writes the whole hole. The *logical* method list, in order:

1. `0x1710 ← 0`, then `0x1714 ← 0` three times. These are always present, also when nothing is
   drawn.
2. If `K' > 0`:
   1. When vertices were produced: for each output-layout declaration in table order, set the
      attribute slot's array format (`0x1740 + 4·slot ← stride·256 + count·16 + format`), then
      the array offset (`0x1680 + 4·slot ← RSX offset of (X + bi + declaration offset)`, with
      location bit 31 = 0 for local memory). Built-in layout 0 is one declaration: slot 0, format
      2, count 3.
   2. `0x1714 ← 0`.
   3. `0x181C ← RSX offset of X`, `0x1820 ← 0x10` (local memory, 16-bit indices), as one
      two-argument packet.
   4. `0x1808 ← 5` (triangles).
   5. `0x1824` draw batches as one non-incrementing packet (at most 2,047 arguments per packet).
      Each argument covers up to 256 indices: `(count − 1)·2^24 + first`, with `first` running
      from 0 in steps of 256. `X` is always 128-byte aligned, so no alignment batch is needed.
   6. `0x1808 ← 0`.
   7. `0x1D6C ← L − 0xB0000000` (the label's semaphore offset; the semaphore area is at guest
      `0xB0000000` in every recording), then `0x1D74 ← X + n` (the new top). The RSX writes the
      new top to the label once it has passed this point. That is the `U` a later allocation
      reads.

**Packing rule.** No packet may cross a 128-byte line of guest memory. At each packet, let `r` be
the number of whole words left in the current line, minus one for the header.

- If `r = 0`, write one zero word (a no-op) and continue on the next line.
- If the packet has more arguments than `r`, write the first `r` under a header with count `r`,
  then continue the rest under a new header on the next line. The method advances by `4r` for an
  incrementing packet and stays the same for a non-incrementing one.

All remaining words up to `16·h` bytes are zero. If the packed content does not fit, the invocation
is refused at admission.

The original produces exactly this byte sequence in all 800 recordings. A native implementation
should produce it too: the hole carries no floating-point values, so byte equality costs nothing
and makes oracle comparison exact.

### 9. Ordering and completion

What other agents can observe, and when:

1. **Data before hole.** Every byte of the index and vertex data must be visible to the RSX before
   any word of the hole.
2. **Line by line.** For each 128-byte line of the hole, the line's first word (which replaces the
   parent's jump-to-self) must become visible no earlier than the rest of that line. Lines may
   complete in any order.
3. **Hole before hand-back.** The hole must be complete before the invocation is handed back to
   the job manager. The parent, and through it the PPU, treats the job manager's completion as "the
   child is done".
4. **Ring record.** The ring update (section 6) is a single atomic update of its 128-byte line,
   made with the same reservation semantics as an SPU atomic store, so that any agent holding a
   reservation on the line loses it. It happens before the data writes.
5. **Guest writes go through the runtime.** Data, hole and ring-record writes must have every
   side effect that the runtime's MFC put path has: reservation loss on the lines written,
   invalidation of code or overlay residency, and the RSX-visible memory ordering the SDK
   renderer relies on.
6. **Hand-back.** Afterwards the invocation calls the job manager exactly as the original does,
   through the job API context passed in `r3` at entry. That context's function slots are an
   interface between the job manager and every WWS job:
   - at start, *get buffer* (slot `+0x34`) for buffer 1 (inputs) and buffer 3 (output and scratch);
   - at the end, *release buffer* (slot `+0x48`) for buffer 3, then *finish* (slot `+0x44`).

   Then return to the dispatcher with `r1` and `r80`–`r127` as at entry. No return value is
   defined ("Return value" under Integration contract). *Finish* is a scheduling point and is the
   invocation's last act ("Finish" under Integration contract). The job manager's own effects
   (the queue line update and the prefetch of the next descriptor) follow from those calls. The
   oracle compares them (Integration contract); they are not re-specified here.

No other guest memory is written. In particular the invocation writes nothing back to the
descriptor or the input streams *(observed)*.

## Edge cases and errors

- `K = 0`, or every triangle culled: no allocation, no data, a hole of the four invalidate packets
  followed by zeros (verified).
- An index of `V` or more, a stream shorter than its counts need (except the index-only empty
  draw in admission check 6), an undecodable codec or selector,
  a palette joint index out of range (see [skinning-deformation.md](skinning-deformation.md)), or a
  non-finite input lane: refused before any effect.
- Hole size 0, hole address 0 or misaligned, or content that does not fit: refused.
- `Vout` above 65,536 (indices must fit in 16 bits): refused.
- Ring allocation not satisfiable after one refresh: refused before any effect.
- Unknown mode word, flavour, group or configuration byte: refused.

Every refusal is counted per reason and reported by the implementation. Unsupported behaviour
stays visible, never silently skipped.

## Tolerance

Native arithmetic is used. The reference is exact real arithmetic on the decoded inputs.

| Output | Tolerance |
| --- | --- |
| Produced positions | as in the deformation specification that produces them, with the game's wind values. In recordings: skinned positions within 7e-7 absolute, group 0 within 1.5e-5, group 3 within 1.9e-4 |
| Weights, hole words, index values, sizes, ring fields | exact |
| Normal fields | within 1 unit of the reference quantization |
| Tangent fields | within 1 binary16 ulp; magnitudes below the smallest normal binary16 may become zero |
| Culling decisions | Exact, except for borderline triangles, which may go either way (see the definition after this table). In the recordings every difference was borderline. The largest Sabotage cases were zero-area group-0 triangles from collapsed instances, and quarter-pixel distances below `1e-4` pixel. The largest Training-with-movement cases were group-11 slivers about 0.4 units from the camera, at world coordinates near 1,000. |

**Borderline triangle.** For each vertex `v`, let

- `R_v` be the largest magnitude among the vertex's world coordinates and the object translation,
  and
- `δ_v = 2^-8 + 2^-22 · R_v · max(|kx|, |ky|) / |c.w|`, in pixels.

`δ_v` is the screen-position uncertainty, at a few binary32 ulps of the world coordinates. Let
`δ` be the largest `δ_v` over the triangle and `L` its longest screen edge. The triangle is
borderline if any of these holds:

- `|A| < 2^-8 + 2·L·δ` square pixels;
- a vertex lies within `δ` pixel of a quarter-pixel line;
- a vertex lies within `δ` of a viewport side (in the units of `(ux, uy)`), or within
  `2^-8·|c.w|` of the near plane.

The `2^-22·R` term is what makes the rule scale. With world coordinates near 1,000, as on the
Training map, and the first-person view model 0.4 units from the camera, binary32 rounding alone
moves screen positions by about a tenth of a pixel.

An implementation may evaluate the projection in binary64, or relative to the camera position.
That reduces its own error, but it does not remove the original's, so the tolerance stays.

### Consequences of non-bit-exact output

- **Depth equality between passes.** Depth pre-pass and Main draw the same partitions and compare
  depths with EQUAL. For index-only kinds the drawn positions are the original buffer's, so only
  borderline triangle choices can differ, and those triangles cover no sample. For produced kinds,
  Main and Depth must take their positions from the same producer. Admission must therefore be a
  pure function of the invocation's inputs (checks 1–9), and enabling the native path must be a
  per-run setting, never toggled within a frame. Under these conditions only a ring-allocation
  refusal can mix producers. When that happens, the next invocation on that SPU may instead run
  the original. Implementations should avoid it by making native admission take the same
  allocation decision the original would.
- **Wind parity.** With the game's wind values, produced positions match the original to within
  the stated tolerance. A frame that mixes producers for one partition therefore differs only by
  rounding, not by a visible sway difference. Depth EQUAL ties at that level are already
  tolerated between hosts.
- **Caches.** Any cache of decoded or produced vertices must key on source bytes, codec
  parameters and every userdata and view input it reads, never on output values.

## Integration contract

### Hook point

Replace the whole invocation at the WWS policy's payload dispatch. That is the seam
`tools/graphics_payload_hooks.py` already instruments, at the call at `0x325C` with continuation
`0x3260`. When the target is `0x15898` and the resident module image is the one identified above,
the hook asks the native implementation to run the invocation. On success it skips the call
and continues as if the job had returned. On refusal it runs the lifted job unchanged.

Why this seam:

- It is the only point where whole-job replacement needs no knowledge of the job's internal
  local-store layout. Inputs are the descriptor's userdata and input list. Outputs are guest
  memory, the hole and the four job API calls.
- It works with the default SDK renderer and with the frontend alike, on all three hosts, because it
  sits below both.
- Mid-job seams were tried before. They reuse the original's internal layout, which this method
  forbids.

Extension-point shape (keeps AGENTS.md's inert-by-default rule):

- A generic, optional `payload_override` slot in `MagExtensionCallbacks` (`src/mag_extension.h`).
  It receives the SPU context and the payload target and returns *handled* or *not handled*.
  With no extension registered, the dispatch code path and its outputs are unchanged.
- The in-repository native implementation is default-off and registers through that slot, like
  the render adapter. A single environment or build option enables it, and the receipt records it.
- The payload-hook lift step must be part of the normal build for the override to work. While it
  is off, the native path is unavailable, not silently skipped. Report that in the receipt.

### Job API calling contract

These are interface facts between the WWS job manager and every job it runs. They are observed
on the retail build by tracing the original job on the SDK interpreter. They are not this job's
internals.

**Context.** At entry, `r3` holds the local-store address of the job API context (`0x1330` in
every recording). Context fields used here:

| Field | Meaning |
| --- | --- |
| `+0x00` | virtual SPU number `s` (selects the output ring record) |
| `+0x20` | local-store address of this job's 192-byte userdata copy (`0xC40` in every recording) |
| `+0x34` | service slot *get buffer* |
| `+0x44` | service slot *finish* |
| `+0x48` | service slot *release buffer* |

**Service slots.**

- A slot word is the local-store address of a job-manager function. Its low 18 bits are the
  address.
- Call it as an ordinary SPU subroutine:
  - link in `r0`;
  - `r1` a valid, 16-byte aligned stack pointer whose 32 bytes from `r1` upward belong to the
    caller (the callee saves its link at `r1 + 16` and builds its frame below `r1`);
  - arguments in `r3` upward;
  - result in `r3`;
  - `r80`–`r127` preserved.
- The job makes each call with SPU interrupts disabled and re-enables them on return. A native
  caller does the same.
- Any otherwise-unused local-store address may serve as the link when the call is driven from host
  code with the runtime's call-and-drain mechanism. The entry address `0x15898` is a safe choice: no
  service executes it.
- For stack, use the stack pointer from entry minus at least one 512-byte frame. The job's stack
  region at the top of local store is unused by the services' data.

| Service | Arguments | Result | Observed effects |
| --- | --- | --- | --- |
| get buffer | `r3 = 0`; `r4` = buffer number; `r5 = 0`; `r6` = 1 for buffer 1, 0 for buffer 3 | `r3` = 16-byte buffer descriptor | buffer 1 call: writes two quadwords of job-manager bookkeeping; buffer 3 call: none |
| release buffer | `r3 = 0`; `r4 = 3`; `r5 = 0` | not used | no guest-memory transfer |
| finish | `r3` and `r4` are not read | `r3`; not used by the job afterwards | completion published to the job queue, next-descriptor prefetch, and possibly a pass through the SPURS kernel's workload selection (below) |

**Buffer descriptor.** 16 bytes:

| Bytes | Meaning |
| --- | --- |
| 0 | buffer number |
| 4–5 | start / 4 |
| 6–7 | length / 4 |
| 8–11 | for buffer 1, the guest address of the input list (child descriptor `+0x100`) |
| 12–15 | for buffer 1, the list length (192) |

- **Buffer 1 (inputs).** The job manager has already filled it by DMA. The streams are packed in
  slot order from the buffer's start, each taking its listed length. The input list itself is the
  buffer's last 192 bytes. Buffer 1 started at `0x1FC00` in every recording, and its length
  varied (`0xC00`–`0xB000`).
- **Buffer 3 (output and scratch)** follows buffer 1 directly.

Both buffers belong to the invocation until *release buffer*. A native path may stage outgoing data
in either, with each DMA at most 16 KiB. Releasing buffer 3 writes nothing back to guest memory.

Staging may begin only after the last point at which the invocation can still be refused, which
is the ring allocation (admission check 10). Buffer 1 holds the lifted job's inputs. Any byte
written to either buffer before that point corrupts the fallback: a refused invocation hands the
lifted job modified inputs, and the job then derails. Scratch state needed before that point,
such as the ring line and the label value, belongs in host memory or in the native code's own
stack frame.

**Where the list is not.** Local store at userdata `+0xC0` (`0xD00`) holds the job manager's queue
state, not the input list. Without *get buffer* the list cannot be located from fixed context
fields.

**Admission and get buffer.** *Get buffer* is not proven free of side effects: the buffer-1 call
writes job-manager bookkeeping that depends only on the current job. The supported sequence is:

1. Call *get buffer* for buffers 1 and 3 once, as the job does.
2. Run admission on the buffers.
3. On success, run natively, then call *release buffer* (3) and *finish*. Return to the dispatcher
   continuation with `r1` and `r80`–`r127` as at entry. `r3` is not defined.
4. On refusal, run the lifted job from its entry. It repeats the *get buffer* calls.

This path is now verified. The implementer's replay used the real runtime and job-manager code.
In it, *get buffer* was called, the invocation was refused, and the lifted job then ran from its
entry. Of 40 cases, 37 left the job manager's local store, registers and transfers equal to the
original alone. The other 3 differed only in a timer word, in runs where *finish* entered the
kernel during replay. So the repeated *get buffer* calls are equivalent, provided the buffers are
untouched (above). Oracle runs should keep a forced-refusal case for every kind.

**Identity check at run time.** These facts are sufficient; no per-call hashing is needed:

1. The policy image is the pinned, instrumented one. The payload-hook lift step checks this at build
   time against `POLICY_SHA` and `FUNCTION_SHA` in `tools/graphics_payload_hooks.py`.
2. The runtime's residency record for local store `0x4000` names the verified region registered
   for source address `0x01963A00`. Its span is the lifted code only (from `0x4000` to the lift's
   code end, as `tools/wws_job_lift.py` registers it), not the whole `0x1B870`-byte image: the job
   uses the rest of the image area for its own data. That region's code segments were generated
   from the image with the SHA-256 above, and the runtime compares them once per load. Ask the
   runtime's own lifted-code lookup whether the loaded code is verified; do not compare spans
   against the image size.
3. The payload target is `0x15898`.

If the region is resident but not yet verified when the payload call is reached (the first
dispatch after a load), either run the runtime's own one-time comparison or refuse. The lifted
dispatch then verifies it.

**Return value.** The dispatcher does not consume any value the job returns:

- After the payload call returns, the policy saves `r0`, `r3` and `r4` into three job-manager
  local-store save slots (`0x1630`, `0x1640`, `0x1650`), runs a bookkeeping helper, and restores
  them.
- On both paths it can then take, each register is overwritten before any read.

So the value left in `r3` (and `r0`, `r4` and the other volatile registers `r2`–`r79`) at
hand-back has no guest-visible effect. It survives only as the content of those three save slots
until the next payload overwrites them.

- The original job leaves an incidental value: neither the *finish* result nor its low half.
  Recordings and replays disagree about it because the recorder snapshots registers at a
  different moment.
- A native path may leave any value. Returning the *finish* result unchanged is a reasonable
  choice.
- Oracle comparisons must exclude `r0`, `r2`–`r79` and those three save slots.

**Finish.** *Finish* ends the invocation from the job manager's point of view:

- It publishes the job's completion to the job queue with an atomic update. From that moment the
  parent and, through it, the PPU may treat the child as done.
- It may prefetch the next job's descriptor into the job manager's local store.
- It may pass through the SPURS kernel's workload-selection service. In the runtime's HLE that
  service lives at local store `0x9D0`; the replay harness stops at `0x9D4` because it does not
  model the kernel. Control may then run other job-manager or workload code, on its own stack,
  before or instead of coming back to the caller in the same host call.

"*Finish* may not return to its caller promptly, or at all within the current host frame" is
therefore expected behaviour of the contract, not an error. In game, every recorded invocation
eventually reached the dispatcher continuation `0x3260`.

Before calling *finish*, a native implementation must guarantee:

1. **Outputs visible.** Every guest-visible effect of the invocation is complete and visible to
   the RSX and to other agents: ring record, index and vertex data, the whole hole (ordering rules
   1–5 of section 9).
2. **Transfers drained.** Any MFC command it issued has completed (tag 31 drained).
3. **Buffers given back.** *Release buffer* (3) has already been called. Buffers 1 and 3, the
   staging data in them and the job's stack area may be reused by the next job as soon as *finish*
   runs.
4. **Bookkeeping closed.** The invocation is counted as handled (and any per-kind statistics
   updated) before the call. No host lock is held across it, and no host object needs the call
   to return in order to be released.
5. **Bracketed call.** The call goes through the runtime's ordinary lifted-call mechanism (link
   register set, the call driven until control comes back to the link). The runtime can then
   carry the SPU through the kernel service and any other code that runs in between.
6. **Nothing read afterwards.** If and when the call returns, the native code reads nothing from
   local store or volatile registers that belonged to the invocation. It restores `r1` and
   `r80`–`r127` from values it saved itself, then continues at the dispatcher continuation.

**MFC tags.** The original job issues all of its own transfers on tag 31: data, read-back,
hole, ring line and label read. The job manager's transfers in the recordings use tag 0. A native
path that issues MFC commands uses tag 31 and has all of them complete before it calls *release
buffer*. Host-side writes made through the runtime's write path use no tag.

**Ring protocol.**

- The original reads the ring line with a reservation (GETLLAR) and writes it back
  unconditionally (PUTLLUC). That is safe only because the record belongs to the running virtual
  SPU.
- A GETLLAR/PUTLLC loop that retries on reservation loss is equivalent and acceptable.
- So is the 4-byte read of `L` (on tag 31) for the refresh in section 6, which is what the original
  does.

### Per-invocation fallback

Admission runs to completion before any effect, and a refusal leaves no trace. Neither the ring
record nor any byte of buffers 1 and 3 has been written. The fallback is the unmodified lifted job. Refusal reasons are counted per kind and
reason, and periodically reported when diagnostics are enabled.

### Differential oracle (outside this repository)

The oracle reuses the job I/O recordings and the replay approach of the private `jobio_replay`
harness. That harness serves every GET and atomic load from a recording on the SDK interpreter
and compares every PUT.

1. Record N invocations (`MAG_JOB_IO_ENTRY=15898`, complete v1 mode) across Training, Sabotage and
   each pass, including shadow cascades.
2. For each recording, replay the original on the interpreter to confirm it reproduces itself (the
   harness's own check).
3. Run the native implementation on the same entry state, with an MFC model that serves reads from
   the recording.
4. Compare:
   - the ring record write, the index values (`K'` entries), the hole bytes and the job API call
     sequence: exact;
   - produced vertices: within Tolerance;
   - the culled list: exact, or differing only by triangles the oracle confirms are borderline
     (recompute rules 2 and 3 for each differing triangle and check the boundary distance);
   - exit `r1` and `r80`–`r127`, and the job manager's local-store state after the finish call,
     excluding the three save slots named under "Return value": exact. `r0` and `r2`–`r79` are not
     compared.
   - Replays that reach the kernel's workload-selection service end there (the harness has no
     kernel). Compare them up to that point: every transfer before it, and the job manager's local
     store at that point.
5. Synthetic inputs (this document's vectors and generated ones) run in the public repository
   without game data.

### Acceptance gates before default-on

1. **Oracle.** At least 20,000 recorded invocations across Training and Sabotage (Main, Depth,
   both shadow cascades, Other) pass, with every admitted kind represented at least 200 times.
2. **Refusal rate.** Under 2 % of invocations by count in Sabotage, and no unexplained reason.
3. **Visual parity.** Same-scene captures of Training and Sabotage with the native path on and off
   on all three hosts. There must be no missing geometry, holes, z-fighting or visible sway
   difference.
4. **Performance.** Same-binary A/B on macOS, Windows and Linux (Training and the
   `sabotage-a-static` scenario). Lifted EdgeGeom CPU per frame must fall by at least 5×, and
   frame time must not regress on any host.
5. **Robustness.** One hour of hosted play per host without asserts, halts or a ring stall.

## Performance plan

**Parallelism across invocations.** Invocations are independent of each other. The one shared
structure each touches is its SPU's ring record, which only that SPU uses. So the existing SPU
worker threads already run invocations in parallel, and the native path keeps that unchanged. Do
not move invocations to other threads: the ring record belongs to the running virtual SPU.

**Parallelism within one invocation.** Static items are too small to split (median 30 vertices).
Skinned characters, with several hundred to over a thousand vertices and up to about 1,500
triangles, could split by vertex range and triangle range on a pool. Do that only if
single-threaded native SIMD leaves characters on the critical path.

**SIMD.**

- Structure-of-arrays decode (halves, packed normals) into lanes of 4 (NEON) or 8 (AVX2).
- One 3×4 object matrix composed per invocation from `q`, `σ`, `t`, premultiplied by `C` into a
  single 4×4. Projection is then one matrix-vector product per vertex.
- Culling evaluates rules 1–3 for 4 or 8 triangles at a time and compacts the survivors with a
  mask.
- The skinning blend vectorizes across vertices.

**Allocation-free.** Use per-thread scratch buffers sized by the largest observed `V` and `K`.
Grow them only on refusal paths.

**Expected cost.** Simple per-element estimates:

| Kind | Native estimate |
| --- | --- |
| Static, 30 vertices / 18 triangles | 0.5–1 µs including the write path |
| Character, 710 vertices / 1,310 triangles | 5–15 µs |

For a Sabotage frame (about 2,800 static, 600 instanced, 500 wind and 50 skinned invocations)
that is roughly 5–10 ms of SPU-thread CPU per frame, against today's 49 ms (macOS), 74 ms (Windows)
and 130 ms (Linux, before register promotion). The job manager's own dispatch per invocation remains
lifted and is not included. Measure it separately after the first A/B.

**Caching.** Not needed for the first version. Decoded static positions are tiny, and hashing them
would cost as much as decoding them.

## Synthetic test vectors

Unless stated: view record with `C` = identity, scale `(640, −360, 0.5)`, offset `(640, 360, 0.5)`;
object quaternion `(0, 0, 0, 1)`, `σ = 1`, `t = 0`; flavour 2; positions as three `f32`.

### ET-1: facing

Positions `p0 = (0, 0, 0)`, `p1 = (0.1, 0, 0)`, `p2 = (0, 0.1, 0)`. Screen positions
`(640, 360)`, `(704, 360)`, `(640, 324)`. Triangle `(0, 1, 2)` has `A = −2304`: kept with flavours 1
and 2. Triangle `(0, 2, 1)` has `A = +2304`: removed with flavour 2, kept with flavour 1.

### ET-2: frustum

`p0 = (1.5, 0, 0)`, `p1 = (2, 0, 0)`, `p2 = (1.5, 0.1, 0)`: every `c.x > c.w = 1`, so the
triangle is removed. Moving `p0` to `(0.9, 0, 0)` keeps it (`A = −25344`).

### ET-3: small primitive

`p0 = (0, 0, 0)`, `p1 = (0.0003, −0.1, 0)`, `p2 = (0.0001, 0.1, 0)` give screen x of 640, 640.192 and
640.064 (one quarter-pixel column). `A = −9.216`, but the triangle is removed. With
`p1 = (0.0005, −0.1, 0)` (x = 640.32) it is kept.

### ET-4: zero area with flavour 1

`p0 = (0, 0, 0)`, `p1 = (0.1, 0, 0)`, `p2 = (0.2, 0, 0)`, flavour 1: removed, because `A = 0`
(rule 3 removes it too).

### ET-5: ring

`S = 0xC1000000`, `E = 0xC1010000`, `T = 0xC100FF00`, `U = 0xC1008000`, `n = 0x200`.
`T + n > E`, so the candidate is `S`; `[S, S + 0x200)` lies below `U`, so it is free. The new
`T` is `0xC1000200`, `U` is unchanged and the label is not read. Variant: with `U = 0xC1000100`
the candidate is not free. After a refresh that reads `0xC100F000` from `L`, `U` becomes
`0xC100F000` and the allocation succeeds at `S`.

### ET-6: hole without a draw

`h = 7` (112 bytes), `H` 128-byte aligned, `K' = 0`. Words: `0x00041710 0`, `0x00041714 0`,
`0x00041714 0`, `0x00041714 0`, then 20 zero words.

### ET-7: index-only draw

`H = 0x70000000`, `h = 7`, `K' = 36`, `X = 0xCF000000` (RSX offset `0x0F000000`),
`L = 0xB00005D0`, `n = 128`. Words:

```
00041710 00000000 00041714 00000000 00041714 00000000 00041714 00000000
00041714 00000000 0008181C 0F000000 00000010 00041808 00000005 40041824
23000000 00041808 00000000 00041D6C 000005D0 00041D74 CF000080 00000000
00000000 00000000 00000000 00000000
```

### ET-8: line-boundary split and no-op

As ET-7 but `H = 0x70000050` (`h = 8`): 12 words remain in the first line. The prefix uses 8 and
`0x1714` uses 2. The index-array packet has room for one argument, so the line ends with
`0x0004181C 0x0F000000` and the next line starts `0x00041820 0x00000010`. With `H = 0x70000054`
(11 words in the first line) the packet has no room: the first line ends with one zero word,
and the next starts `0x0008181C 0x0F000000 0x00000010`.

### ET-9: instance expansion

Group 0, `V = 3`, `K = 3` with indices `(0, 1, 2)`, `I = 2`, nothing culled: surviving list
`0, 1, 2, 3, 4, 5`, `Vout = 6`, vertex bytes `roundup(6·12, 16) = 80`, index bytes 16, `n = 128`.

### ET-10: a triangle reaching behind the camera

Main viewport (scale `(640, −360)`, offset `(640, 360)`). Clip coordinates are given directly.
Two vertices are in front: `(2, 0.5, 0.8, 1)` and `(2.5, −0.5, 0.8, 1)`, with `ux` = 1920 and
2240. The third vertex is behind the camera at `(x0, 0, −1.2, −1)`, with `ux = 640·x0 − 640`.

- `x0 = 0`: `ux = −640`, which is not beyond the right side. The triangle is kept, even though
  every vertex has `c.x > c.w`. It crosses the near plane, so facing and small-primitive tests
  are skipped.
- `x0 = 3.5`: `ux = 1600`, beyond the right side. Removed.

Mirror case, with front vertices `(−2, 0.5, 0.8, 1)` and `(−2.5, −0.5, 0.8, 1)`:

- `x0 = 0.5`: `ux = −320`, left of the viewport, as are the other two. Removed.
- `x0 = 1.5`: `ux = 320`. Kept.

The original agrees with all four cases. Its measured switching points are `x0` between 2.96
and 3.07 on the right, and between 0.5 and 1.0 on the left, as the formula predicts.

## Open questions

1. **Wind tuning values.** Resolved. This job uses the game's values, measured black-box ("Wind
   values (game-supplied)"). The frontend's deformation specifications keep their own values;
   whether they adopt these is a separate decision for the frontend.
2. **Ripple.** Group 4 (ripple) has not been recorded. It stays refused until a recording and the
   oracle confirm it. Group 11 is resolved: it is the full skinning specification, including the
   object direction matrix (section 3).
3. **Short streams.** Part resolved.
   - Long streams are split across slots 1 + 2 and 7 + 8 (Stream slots).
   - Index-only invocations that are still short draw nothing (128 of 128).
   - For produced kinds the original reads on past the listed length. 5 of the 5 short produced
     invocations that drew are reproduced exactly by reading local store contiguously from each
     stream's packed position. Another 15 drew nothing, and that is not explained.

   These invocations stay refused. Why the parent sends short lists at all (streaming not yet
   complete, or a level-of-detail switch) is not established.
4. **Ring boundary cases.** Whether an allocation may end exactly at `U`, and what the job does
   when a refreshed `U` still leaves no room (wait or omit the draw), were not observed. The
   native path refuses the second case.
5. **Depth pass.** Depth children were not recorded. Their descriptor format is the same, but
   their kinds and frequencies are not measured.
6. **Userdata bytes without a verified meaning** (`+0x00..+0x1F`, `+0x58..+0x5F`, `+0xA6`,
   `+0xB1..+0xBF`). The camera position appears at `+0x10`; nothing verified depends on these
   bytes.
7. **Software depth.** The module also contains a coarse software depth test and update keyed by the
   view record's origin and size. No recorded invocation used it, and the native path does not
   implement it. How the job selects it is not established. If it can be selected by the inputs,
   admission must refuse it once identified.
