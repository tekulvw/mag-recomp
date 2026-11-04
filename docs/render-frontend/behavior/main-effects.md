# Main producer effects: framework and stencil-mask producer

## Scope

This specification covers the part of the native Main plan that runs *producer* work: the
full-screen passes the game inserts before, between and after the ordinary Main material draws.
It defines:

- how producer work is associated with guest render-target records ("surfaces") and with packet
  images;
- which producer runs for each producer event, and when an event is a no-op or an error;
- the common contract that every full-screen producer draw follows;
- the stencil-mask producer (the first initial producer) in full;
- the group-twelve texture association and the Main pass opening rules.

The other producers have their own documents:

| Producer event | Document |
| --- | --- |
| Initial producer 0 | this document, "Stencil-mask producer" |
| Initial producer 1 | `main-lighting.md` |
| Initial producer 2 | none: always a no-op (see "Producer events") |
| Group 12 entry | `main-group-twelve.md` (unit values); this document (image association) |
| Group 13 entry | `main-fog.md`, `main-fog-values.md` |
| Group 15 entry, slot 0 | `main-quincunx.md` |
| Group 15 entry, slot 1 | `main-downsample.md` |
| End of Main | `main-finish.md` |

Guest inputs are read as described in `material-producers.md` and `main-producer-inputs.md`. The
material-group walk that raises producer events is in `scene-plan.md`; the setup references that
decide which events exist are in `material-setup.md`. Geometry qualification for ordinary Main
items is out of scope here.

Applies to retail v1.0 only.

## Inputs

### Renderer object

`R` is the guest address of the renderer object (obtained as described in
`material-producers.md`). Fields used by this group of producers:

| Location | Type | Meaning |
| --- | --- | --- |
| `R+0x2BAC` | 84-byte surface record | Dedicated depth-downsample target (see `main-downsample.md`). |
| `R+0x2C10` | 8 x 84-byte surface records | Surface table, see below. |
| `R+0x2FC0` | 8 x 32-byte texture descriptors | Per-surface texture descriptor table, see below. |
| `R+0x3120` | 32-byte texture descriptor | Depth-source descriptor used when bank zero is enabled (`main-lighting.md`). |
| `R+0x3140` | 32-byte texture descriptor | Depth-source descriptor used when bank zero is disabled. |
| `R+0x3160` | `u32` | Surface branch word (non-zero = set). |
| `R+0x3174` | `u32` (first of 3) | Selection override word (non-zero forces role 2). |
| `R+0x329C` | `u8` | sRGB control (non-zero = on). |
| `R+0x329D` | `u8` | Bank-zero control (non-zero = set). |
| `R+0x329F` | `u8` | AA-surfaces control (non-zero = AA role-3 surfaces exist). |

**Bank zero enabled** means `u8 R+0x329D != 0` and `u32 R+0x3160 != 0`.

### Surface records

A surface record is 84 bytes. Fields used here:

| Offset | Type | Meaning |
| --- | --- | --- |
| `+0x04` | `u32` | Width in the upper 16 bits; the lower 16 bits (x origin) must be zero. |
| `+0x08` | `u32` | Height in the upper 16 bits; the lower 16 bits (y origin) must be zero. |
| `+0x0C` | `u32` | RSX `SET_SURFACE_FORMAT` word: bits 0-4 colour format, bits 5-7 depth format (2 = Z24S8), bits 12-15 anti-aliasing mode. |

Width and height are the *logical* (resolved) size. The anti-aliasing mode gives the sample grid:

| AA mode (bits 12-15) | Sample grid `(gx, gy)` |
| --- | --- |
| 0 (centered, 1 sample) | (1, 1) |
| 3 (diagonal, 2 samples) | (2, 1) |
| 4 or 5 (square, 4 samples) | (2, 2) |
| anything else | unsupported: decline |

The *physical* size of a surface is `(width * gx, height * gy)`.

The surface table at `R+0x2C10` is indexed by role `0..3` and index `0..1`: the record for
`(role, index)` is at `R+0x2C10 + role*0xA8 + index*0x54`.

**Surface selection** `select(index, role)` (authoritative definition in `material-producers.md`;
summarised here): `index` must be 0 or 1 and `role` 0 to 3, otherwise error. The role actually
used is 2 when any of these hold, and `role` otherwise:

- bank zero is disabled and `role <= 1`;
- the AA-surfaces control byte is zero and `role == 3`;
- the selection override word is non-zero.

The result is the record at the forced `(role', index)` and its guest address.

### Per-surface texture descriptors

The texture descriptor for a selected surface record at table position `(role', index)` is the
eight `u32` words at `R+0x2FC0 + (role'*2 + index)*32`. `role'` is the role actually selected,
recovered from the record address (`(address - (R+0x2C10)) / 0xA8`).

A compact texture descriptor is eight RSX texture register values of one unit: `OFFSET`,
`FORMAT`, `ADDRESS`, `CONTROL0`, `CONTROL1`, `FILTER`, `IMAGE_RECT` (NV4097 methods `0x1A00` to
`0x1A18` for unit 0), then `CONTROL3` (method `0x1840` for unit 0: pitch in bits 0-19, depth in
bits 20-31). The border colour is not part of the compact form; see `main-producer-inputs.md` for
the twelve-word full form. Bit fields used in the producer documents:

| Word | Bits | Meaning |
| --- | --- | --- |
| `FORMAT` | 2 | Cube map. |
| `FORMAT` | 8-15 | Texel format; masked with `0x9F` it gives the base format (`0x85` = A8R8G8B8, `0x90`-`0x93` = depth formats). |
| `FORMAT` | 16-31 | Mip level count. |
| `ADDRESS` | 20-23 | Per-channel gamma (sRGB) enable. |
| `CONTROL0` | 31 | Unit enable. `CONTROL0 = 0` disables the unit. |
| `CONTROL0` | 4-6 | Maximum anisotropy. |
| `FILTER` | 0-12 | LOD bias. |
| `FILTER` | 13-15 | Convolution kernel. |
| `FILTER` | 16-19 | Minification filter. |
| `FILTER` | 24-27 | Magnification filter. |
| `IMAGE_RECT` | 16-31 / 0-15 | Width / height. |

"Set min/mag to NEAREST" means `FILTER = (FILTER & ~0x0F0F0000) | 0x01010000`; every other bit is
kept.

### Module constants

These are `f32` words in the retail module's constant data, located at fixed negative offsets
from the module's table-of-contents base `T` (supplied with the producer source observation; see
`material-producers.md`):

| Name | Location | Use |
| --- | --- | --- |
| `L` | `T-0x754` | Full-screen geometry low coordinate. |
| `H` | `T-0x750` | Full-screen geometry high coordinate. |
| `F` | `T-0x748` | Full-screen triangle far coordinate. |
| `G` | `T-0x74C` | Full-screen triangle far texture coordinate. |
| `Zmin` | `T-0x22A0` | Producer depth-range minimum. |
| `Zmax` | `T-0x22A8` | Producer depth-range maximum. |

They are read from guest memory at plan time, never hard-coded. A missing word is an error.
`material-producers.md` calls the same six constants `T_LO`, `T_HI`, `T_FAR`, `T_UVFAR`, `Z0` and
`Z1`.

### Supplied associations

Besides guest memory, the planner receives from its caller:

- **Surface bindings**: 1 to 8 entries, each naming a surface-record guest address and the packet
  images that back it: physical dimensions, colour and depth packet formats, colour, depth and
  stencil attachments, and whether depth and stencil content is already initialised. Addresses
  must be non-zero and unique, dimensions non-zero, and one entry must name the current Main
  surface. Otherwise the whole plan fails.
- **The current Main surface**: the surface-record address the Main geometry currently renders to.
- **The active colour-target selector** (RSX `SET_SURFACE_COLOR_TARGET` value currently in
  effect). Only the value 1 (colour target A alone) is supported.
- **Producer texture bindings**: at most 5 entries, each associating a guest texture descriptor
  (its guest address and its eight words) with a packet image, packet format and dimensions.
- **The producer-generation flag** for this Main invocation (see "Generation guard").

A surface binding is *consistent* with a record when its physical dimensions equal the record's
physical size exactly. Every producer use of a binding checks this and fails on mismatch.

## Outputs

- Packet passes, each with colour, depth and stencil attachments, a viewport and an associated
  surface `(record address, physical clip size)`.
- One packet item per producer draw, using a packet-local identity with the top bit set.
- Material draws: compiled vertex and fragment programs, vertex data, raster state and texture
  uses.
- Updates to the carried Main state: RSX pipeline state, texture unit words, current vertex
  attribute values, the vertex constant bank, the current surface and its viewport.

## Behaviour

### Current surface

The current surface is a record in the surface table. From its address `A` and the table base
`B = R+0x2C10`: `A - B` must be below `8*84` and a multiple of 84, otherwise error. Its
*index* is `((A-B)/84) mod 2` and its *role* is `(A-B)/168`.

The planner also carries a *current viewport* for the current surface. Setting the current surface
to a record with a viewport mapping requires that the mapping's sample grid equals the record's
grid and that the absolute viewport width and height do not exceed the binding's physical
dimensions.

### Generation guard

The producer-generation flag says whether the game generates the initial producer banks for this
Main invocation. Its low 8 bits non-zero means *enabled*. The flag must be supplied together with
`R` and `T` that match the producer inputs and a non-zero source-window token; otherwise the plan
fails.

When the flag is enabled, the current viewport is initialised to the jittered surface viewport of
the current surface (`main-producer-inputs.md`, "Surface viewport").

### Producer events

Each event is validated, then routed:

1. **Reference validation.** An initial event `(slot, target)` requires `target != 0` and
   `target` equal to setup reference `slot`. A group-13 event requires `target != 0` and equal
   to the group-13 reference. A group-15 event `(slot, target)` requires `target != 0` and
   equal to group-15 reference `slot`. References come from `material-setup.md`. Any mismatch is
   an error.
2. **Guard.** Initial events for slots 0 and 1, and all group-13 and group-15 events, require a
   known generation flag (error otherwise). When the flag is disabled they are no-ops. The guard
   deliberately does not cover the group-12 event or the finish. Group-12 setup belongs to the
   material lists, not to the producer banks the flag describes, so it runs whatever the flag
   says. The finish always runs; it consults the flag only to decide whether to restore the role-2
   surface (`main-finish.md`). Initial slot 2 is a no-op either way.
3. **Routing.** See the table in "Scope". Initial slot 2 is a no-op regardless of the guard: its
   bank exists but contains no work. Any other event (including auxiliary group-15 item events)
   is an error: the plan declines rather than skip unimplemented producer work.

### Common producer draw contract

Every full-screen producer draw (stencil mask, quincunx, downsample, and the lighting and fog
producers) satisfies the following.

**Programs.** The vertex and fragment programs are read from the program registries named in
`material-producers.md`. A missing program makes the producer a no-op, *not* an error. Programs
are decoded and translated by the ordinary program translation rules. The vertex program's
embedded constant rows are written into the carried vertex constant bank at their row numbers
before compilation; no other material constants are applied. After translation:

- the fragment program may sample only the units the producer document allows (error
  otherwise);
- the vertex program may read only vertex inputs 0 (position) and 8 (texture coordinate 0) and
  may not sample textures (error otherwise).

**Vertex data.** Vertices have two attributes: position `(x, y, z, w)` at input 0 and texture
coordinate `(s, t, r, q)` at input 8. In the packet they are interleaved with a 32-byte stride:
position at byte 0, texture coordinate at byte 16, each lane a little-endian `f32`. The enabled
vertex array mask is `0x101` (inputs 0 and 8).

The full-screen **quad** (four vertices, indices `0,1,2, 0,2,3`):

| Vertex | Position | Texture coordinate |
| --- | --- | --- |
| 0 | `(L, L, L, 1)` | `(0, H, 0, 1)` |
| 1 | `(H, L, L, 1)` | `(H, H, 0, 1)` |
| 2 | `(H, H, L, 1)` | `(H, 0, 0, 1)` |
| 3 | `(L, H, L, 1)` | `(0, 0, 0, 1)` |

The full-screen **triangle** (three vertices, indices `0,1,2`):

| Vertex | Position | Texture coordinate |
| --- | --- | --- |
| 0 | `(L, L, L, 1)` | `(0, H, 0, 1)` |
| 1 | `(F, L, L, 1)` | `(G, H, 0, 1)` |
| 2 | `(L, F, L, 1)` | `(0, L, 0, 1)` |

`0` and `1` are the exact values +0.0 and 1.0. Each lane is a copy of the guest word, not a
recomputed value. A non-finite `L`, `H`, `F` or `G` is an error.

Because the game issues these vertices as immediate data, the RSX current vertex attribute values
afterwards equal the *last* vertex. After a quad draw: current attribute 0 is `(L, H, L, 1)` and
current attribute 8 is `(0, 0, 0, 1)`. After a triangle draw: current attribute 0 is
`(L, F, L, 1)` and current attribute 8 is `(0, L, 0, 1)`.

**Producer viewport.** For a target record of logical size `(W, Hs)` and sample grid `(gx, gy)`,
with physical binding size `(Pw, Ph)`, the RSX viewport the game programs is

- scale `(W/2, -Hs/2, (Zmax - Zmin)/2, 0)`,
- offset `(W/2, Hs/2, (Zmax + Zmin)/2, 0)`,
- clip range `[Zmin, Zmax]`,
- scissor `(0, 0, W, Hs)`.

The native mapping of an RSX viewport scale `s` and offset `o` with grid `(gx, gy)` is:

- `x = (o.x - s.x) * gx`, `y = (o.y + s.y) * gy`,
- `width = 2 * s.x * gx`, `height = -2 * s.y * gy`,
- depth range `[0, 1]`,
- scissor `(sx*gx, sy*gy, sw*gx, sh*gy)` from the RSX scissor `(sx, sy, sw, sh)`,
- shader viewport scale `(1, 1, s.z, s.w)` and shader viewport offset `(0, 0, o.z, o.w)` (the
  native viewport owns X and Y; the translated vertex program keeps the Z transform once).

For the producer viewport this gives `x = y = 0`, `width = Pw`, `height = Ph` (when the binding is
consistent), scissor `(0, 0, Pw, Ph)`, shader scale `(1, 1, (Zmax-Zmin)/2, 0)` and offset
`(0, 0, (Zmax+Zmin)/2, 0)`.

The mapping fails (and so the producer fails) when: any scale or offset lane is non-finite; the
clip range is not exactly `[0, 1]` (so `Zmin` must be ±0 and `Zmax` must be 1); the grid is not
one of the three supported grids; the target is empty; width or height is not in `(0, 16384]`;
`x` or `y` is below -32768 or `x+width`, `y+height` exceeds 32767; or the scissor does not fit
the target.

**Raster state.** Each producer document lists the pipeline fields it writes. All other fields
are inherited from the carried Main state. Every raster field the packet records must be known
after the producer's writes; an unknown inherited field is an error. Topology is triangles and
the sample count is 1.

**Shader controls.** Translation uses the carried alpha-test, sRGB-output and texture-unit state
(the ordinary Main rules in `scene-plan.md`), the producer's sample grid, and the target's colour
format.

**Pass and item.** Each producer draw opens its own pass on the target attachments, associates
the pass with the target record address and its physical clip size, and adds one producer item
whose identity has the top bit set. Identities are packet-local; they must be unique within a
pass and must never be confused with cohort item identities.

### Stencil-mask producer (initial slot 0)

Purpose: write a stencil reference value over the target's stencil plane wherever the fragment
program does not discard, using a colour surface as the mask source.

Conditions, in addition to the event rules:

- If bank zero is disabled: no-op.
- Sampled surface `S = select(0, 1)`; target surface `D = select(1, 0)`.
- If the stencil-mask vertex or fragment program is missing: no-op.
- `S` and `D` must have consistent bindings.
- `D`'s binding must have a depth image, its packet depth format must be D24S8 (15) or D32S8 (16),
  and `D`'s RSX depth format field must be Z24S8 (2). Otherwise error.
- The fragment program may sample only unit 0.

Texture unit 0:

- Descriptor: the per-surface texture descriptor of `S`, with min/mag set to NEAREST.
- Unit 0's words become that descriptor, and any image previously associated with unit 0 is
  dropped.
- If the fragment program samples unit 0, the unit must be enabled, 2D, not a cube map, and its
  `IMAGE_RECT` must equal `S`'s physical binding size (error otherwise). The pass then samples
  `S`'s colour image through a view of mip 0 and layer 0 only, with the component remap from
  `CONTROL1` and the sampler derived from the unit words (`scene-plan.md`), in fragment texture
  slot 0 and sampler slot 0, with `S`'s colour packet format.

Pipeline writes (before the draw):

| Field | Value |
| --- | --- |
| Stencil test | enabled, two-sided |
| Both faces | function ALWAYS, reference `0x81`, read mask `0x00`, write mask `0xFF`, fail/depth-fail/pass all REPLACE |
| Depth test | disabled |
| Depth write | disabled |
| Alpha test | disabled |
| Blending | disabled |
| Colour write mask | all channels off |
| Face culling | disabled |

The draw uses the full-screen quad and the producer viewport of `D`.

Pass attachments:

- colour: none;
- depth: `D`'s depth image, load = *load* if `D`'s depth is initialised, otherwise *don't care*;
  store = *store*;
- stencil: the same image, load = *load* if `D`'s stencil is initialised, otherwise *don't care*;
  store = *store*; clear value 0; no full-plane-write flag.

The producer never asks for a clear: six indices covering the viewport do not prove that every
stencil texel is written.

State after the draw:

- current attributes as after a quad draw;
- unit 0 `CONTROL0 = 0`, all other unit 0 words kept;
- both stencil faces' write mask = 0; every other stencil field (including enable) kept.

### Group-twelve texture association

At the group-12 transition, `main-group-twelve.md` produces a list of texture-unit requests, each a
unit number and a guest texture descriptor (address and words). The group-twelve source capture
must exist and belong to the same Main pass, and the producer full descriptor source for unit 11
must exist; otherwise error. For each request, in order:

1. If a producer texture binding has the same descriptor address and the same eight words, its
   image must have the binding's format and dimensions in the packet (error otherwise), and that
   image is associated with the unit.
2. Otherwise no image is associated here; the unit's words still carry (and a draw that samples
   the unit must find its image by the ordinary rules or fail).

Optional approximation (off unless the opt-in diagnostic unlit mode is on, and then on unless
disabled): if the request's base format is A8R8G8B8 and the request's storage is the colour plane
of the current Main surface (association rules in `main-producer-inputs.md`), a native pass cannot
sample its own attachment. The current colour image is copied into a new packet image at this
point and the copy is associated with the unit instead. Later group-12 writes are then not
visible to these reads, which differs from the original and must remain labelled as an
approximation.

### Producer texture binding validation

Each supplied producer texture binding must have: a real image; a packet format 1 to 16;
non-zero dimensions equal to the descriptor's `IMAGE_RECT`; a `(descriptor address, words)` pair
equal to one of the known producer texture sources (the two depth-source descriptors in
`main-lighting.md`, and the group-twelve requests when those sources exist); and a descriptor
address not used by an earlier binding. Any violation fails the whole plan.

### Opening a Main pass

When the ordinary Main draws need a pass:

- the colour-target selector must be 1 (colour target A); another value is unsupported and an
  unknown value is an error;
- the current Main surface's binding must have colour load = *load*; if it has depth, depth load
  = *load* and initialised; if it has stencil, stencil load = *load* and initialised. A reopened
  Main pass preserves earlier results and never repeats an initial clear.

The pass uses the binding's attachments and is associated with the current surface record.

Optional approximation (only in the opt-in diagnostic unlit mode, on by default there): the first
Main pass clears colour to `(0, 0, 0, 0.125)` instead of loading. This is not original behaviour.

### Main draw inputs after producers

Ordinary Main draws take their viewport, sample grid, raster colour format (`SET_SURFACE_FORMAT`
bits 0-4 of the current record), packet colour format and packet depth format from the *current*
surface and current viewport, which producers may have changed. The current viewport must exist
and belong to the current Main surface (error otherwise). If the colour format changes and the
blend constant was known, the blend constant becomes unknown, because its meaning depends on the
colour format.

## Edge cases and errors

- Any error fails the whole plan: no partial packet escapes.
- Short guest captures (surface table not exactly `8*84` bytes, selection words not 12 bytes,
  bank-zero control bytes not 3 bytes, surface branch not 4 bytes) fail at construction.
- A surface record with non-zero x or y origin is an error where a surface viewport is derived.
- An unsupported AA mode is an error wherever the grid is needed.
- `Zmin`/`Zmax` other than 0 and 1, or non-finite full-screen constants, are errors.
- NaN, infinite or denormal module constants: non-finite are errors; denormal `L`, `H`, `F`, `G`
  are copied unchanged.

## Tolerance

The only computed floating-point values here are the viewport mapping and the shader viewport
Z terms.

| Output | Maximum error | Justification |
| --- | --- | --- |
| Native viewport `x`, `y`, `width`, `height` | 2^-8 pixel absolute | Below rasteriser sub-pixel precision. |
| Shader viewport Z scale and offset | 2^-22 relative | Keeps depth error below one D24 step. |
| Vertex data, descriptor words, stencil values | none (copied or set) | Data movement. |

With the supported inputs (integer sizes below 2^24, `Zmin` = 0, `Zmax` = 1) ordinary arithmetic
produces the exact values, and the test vectors use exact values.

### Consequences of non-bit-exact output

- The compiled-program cache key includes the shader viewport scale and offset. A different
  rounding only changes cache hits, not correctness, as long as one plan computes them
  consistently.
- "Set current surface" compares the viewport extent with the binding size. An extent rounded
  above the physical size would fail; compute the extent from integers.
- Packet pass/viewport de-duplication, if any, compares viewport values for equality.

## Synthetic test vectors

Common fixture: `R = 0x30000`, `T = 0x20000`, `L = -0.75`, `H = 0.5`, `F = 2.5`, `G = 1.75`,
`Zmin = 0`, `Zmax = 1`. Every record in the surface table has width 320, height 180, depth format
2 and colour format 8.

1. **Selection.** With bank zero enabled (`R+0x329D = 1`, `R+0x3160 = 1`), AA control 0,
   override 0: `select(0, 1)` is at `0x30000+0x2C10+0xA8 = 0x32CB8`; `select(1, 0)` at `0x32C64`;
   `select(1, 3)` is forced to role 2: `0x32D60+0x54 = 0x32DB4`. With bank zero disabled,
   `select(0, 1)` is `0x32D60`.
2. **Current index and role.** Current surface `0x32C10 + 252`: index 1, role 1. Current surface
   `0x32C10 + 250`: error (not a multiple of 84).
3. **Producer viewport, AA mode 3.** Record `D` has AA mode 3 (grid 2x1); binding 640x180.
   Viewport `x=0, y=0, width=640, height=180`, depth `[0,1]`, scissor `(0,0,640,180)`, shader
   scale `(1, 1, 0.5, 0)`, shader offset `(0, 0, 0.5, 0)`, pass clip `(640, 180)`.
4. **Bad depth range.** As 3 with `Zmax = 0.5`: the producer fails.
5. **Inconsistent binding.** As 3 with a binding of 320x180: the producer fails.
6. **Quad.** Vertex bytes (little-endian `f32`) are rows
   `[-0.75,-0.75,-0.75,1, 0,0.5,0,1]`, `[0.5,-0.75,-0.75,1, 0.5,0.5,0,1]`,
   `[0.5,0.5,-0.75,1, 0.5,0,0,1]`, `[-0.75,0.5,-0.75,1, 0,0,0,1]`; indices `0,1,2,0,2,3`.
   Afterwards current attribute 0 is `(-0.75, 0.5, -0.75, 1)` and attribute 8 is `(0,0,0,1)`.
7. **Triangle.** Rows `[-0.75,-0.75,-0.75,1, 0,0.5,0,1]`, `[2.5,-0.75,-0.75,1, 1.75,0.5,0,1]`,
   `[-0.75,2.5,-0.75,1, 0,-0.75,0,1]`. Afterwards attribute 0 `(-0.75, 2.5, -0.75, 1)`, attribute 8
   `(0, -0.75, 0, 1)`.
8. **Stencil-mask descriptor.** `S = select(0, 1)` is at table position `1*2 + 0 = 2`, so its
   descriptor is at `R+0x3000`:
   `[0x1000, 0x00018522, 0x00030303, 0x80000000, 0x0000AAE4, 0x02024ABC, 0x014000B4, 0]`. Unit 0
   during the draw: same words with `FILTER = 0x01014ABC`. After the draw: `CONTROL0 = 0`, other
   words unchanged. With `S` bound at 320x180 and a fragment program sampling unit 0 the draw
   succeeds; with `IMAGE_RECT = 0x01400168` (320x360) it fails the sampled-size check.
9. **Stencil-mask pipeline (state writes only).** This vector checks only which fields the
   producer writes, not whether its draw is admitted. Starting from a state with depth bounds on,
   polygon-offset fill on and front face counter-clockwise: after the producer, depth bounds, offset
   fill and front face are unchanged; stencil enabled, two-sided, both faces ALWAYS/`0x81`/read `0`/write `0`/
   REPLACE x3; depth test and depth write off; colour mask all off.
   A draw from this exact state is refused: with polygon-offset fill on and no bias clamp supplied,
   the bias field can never be fully known ([raster-state.md](raster-state.md)), so the raster
   state is incomplete. An admitted stencil-mask draw needs offset fill known *disabled*, a known
   front face and known depth bounds.
10. **Stencil-mask attachments.** `D` binding depth not initialised, stencil not initialised:
    pass depth load *don't care*, stencil load *don't care*, colour none, one draw with 6
    indices.
11. **Wrong depth format.** `D`'s record depth field = 1 (Z16): error.
12. **Guard.** Generation flag `0x100` (low byte zero): initial slot 0 is a no-op and leaves
    unit 0 words unknown. Flag absent: initial slot 0 is an error.
13. **References.** Setup references `[0x400, 0x404, 0x408]`: event initial `(2, 0x408)` is a
    no-op; initial `(2, 0x404)` is an error; initial `(0, 0)` is an error.
14. **Main draw inputs.** Current surface changed from a binding (colour 1, depth 15, colour
    format 8, grid 1x1) to one with colour 13, depth 16, record colour format 13, AA mode 3, and a
    known blend constant: the Main draw inputs report colour 13, depth 16, raster colour format
    13, grid 2x1, and the blend constant becomes unknown.
15. **Colour target.** Selector 1: a Main pass may open. Selectors 0, 2, `0x13`: unsupported.
    Unknown: error.

## Open questions

1. **Generation flag source.** The flag is currently obtained by observing a value the game
   passes when it begins producer generation. The guest-memory datum it is derived from has not
   been identified as a data fact. Until it is, the flag must come from an observer at that
   point, and the replacement cannot compute it from memory alone.
2. **`T`.** The table-of-contents base is a property of the retail module; it is currently taken
   from the same observation. If it is constant for retail v1.0 it could be fixed in the build
   identity instead.
3. **Purpose naming.** The stencil reference `0x81` and the role 1 index 0 mask source are stated
   as observed data. The visual purpose (which later Main draws test this stencil value) is not
   established here.
4. **Producer item identities.** Existing packets use fixed high identities per producer. Any
   scheme with the top bit set and per-pass uniqueness is acceptable; whether a downstream consumer
   depends on specific values should be confirmed against `scene-plan.md` and the finaliser.
