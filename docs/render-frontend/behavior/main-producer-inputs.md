# Main producer surfaces, viewports and texture associations

## Scope

The shared rules every Main producer uses to pick render targets, build viewports, decide which
guest image a texture descriptor refers to, and list which surfaces Main needs before it starts.
Guest data comes from the input set in [material-producers.md](material-producers.md); names such
as `RENDERER`, `VP_HX` and *bank zero enabled* are defined there.

Applies to retail v1.0. Out of scope: allocating, importing or initialising the native images
(adapter lifetime rules), and the general Main draw planner ([scene-plan.md](scene-plan.md)).

## Inputs

### Surface record (84 bytes)

A surface record holds the arguments of the RSX surface setup methods for one render target.
Fields are big-endian `u32`.

| Offset | Meaning |
| --- | --- |
| `+0x04` | `SURFACE_CLIP_HORIZONTAL`: width in bits 16–31, x origin in bits 0–15 |
| `+0x08` | `SURFACE_CLIP_VERTICAL`: height in bits 16–31, y origin in bits 0–15 |
| `+0x0C` | `SURFACE_FORMAT`: colour format bits 0–4, depth format bits 5–7, type bits 8–11, anti-aliasing bits 12–15 |
| `+0x10` | colour A pitch |
| `+0x20` | colour A offset |
| `+0x30` | colour A DMA context: `0xFEED0000` local memory, `0xFEED0001` main memory |
| `+0x40` | depth/stencil offset |
| `+0x44` | depth/stencil pitch |
| `+0x48` | depth/stencil DMA context (same encoding) |
| `+0x4C` | window offset (not a target selector) |

Accepted colour formats: 8 (A8R8G8B8), 11 (four half floats), 12 (four floats), 13 (one float).
Accepted depth formats: 0 (none), 1 (Z16), 2 (Z24S8). Anti-aliasing modes map to a *sample grid*:
0 → 1×1, 3 → 2×1, 4 and 5 → 2×2; any other mode is unsupported. The *physical size* of a record
is `(width × grid_x, height × grid_y)`.

The *target uses* of a record are colour, plus depth and stencil for depth format 2, plus depth
only for depth format 1.

### Surface record table

`RENDERER+0x2C10` holds eight records: role `r` (0–3), index `i` (0–1) is at
`RENDERER + 0x2C10 + 0xA8·r + 0x54·i`.

### Compact texture descriptor (eight words)

Words, in order, are the arguments of the RSX texture methods `OFFSET`, `FORMAT`, `ADDRESS`,
`CONTROL0`, `CONTROL1`, `FILTER`, `IMAGE_RECT` and `CONTROL3`. Relevant fields:

- `FORMAT`: location bits 0–1 (1 local, 2 main), cube flag bit 2, dimension bits 4–7, texel
  format bits 8–15 (compare after masking with `0x9F`), mip count bits 16–31.
- `ADDRESS` bits 20–23: per-channel gamma (sRGB) decode enables.
- `CONTROL0` bit 31: unit enable.
- `FILTER` bits 16–19: minification filter; bits 24–27: magnification filter.
- `IMAGE_RECT`: width bits 16–31, height bits 0–15.
- `CONTROL3`: pitch bits 0–19, depth bits 20–31.

A *full* descriptor has twelve words: words 0–6 as above, word 7 the border colour, word 8
`CONTROL3`, word 9 the texture-coordinate control and word 10 the vertex LOD control. Applying a
full descriptor to a unit sets its compact descriptor to words 0–6 plus word 8, and also sets the
unit's border colour, texture-coordinate control and vertex LOD control.

The compact descriptor that belongs to surface record (role `r`, index `i`) is at
`RENDERER + 0x2FC0 + 32·(2r + i)`.

### Invocation values

Two values come with each Main invocation:

- the *initial-producer enable*: its low eight bits are nonzero when the initial producer banks
  run;
- the *target index*: a signed value; negative means "use the current index", the `u32` at
  `RENDERER+0x2FB0`.

## Behaviour

### Surface selection

`select(i, r)` returns the record for index `i` (0 or 1) and requested role `r` (0–3); other
arguments are an error. The *effective role* is 2 when any of these hold, otherwise `r`:

- bank zero is not enabled and `r ≤ 1`;
- the anti-aliasing byte (`RENDERER+0x329F`) is zero and `r = 3`;
- role forcing is on.

The result is the record at index `i` and the effective role, together with its guest address.

### Source viewport for a surface

For a record `s` with width `w`, height `h` and anti-aliasing mode `a` (the x and y origins must be
zero, otherwise decline):

- jitter: `jx = f32 at JITTER+4`, `jy = f32 at JITTER+8`. If `a ≠ 0`, `jx ← jx·VP_HX`. If
  `a ∉ {0, 3}`, `jy ← jy·VP_HX`.
- scale `= (w·VP_HX, h·VP_HY, VP_HX, 0)`;
- offset `= (w·VP_HX + jx, h·VP_HX + jy, VP_HX, 0)`;
- clip range `[CLIP_MIN, CLIP_MAX]`;
- scissor `(0, 0, w, h)`.

`w` and `h` are converted to `f32` exactly (they are below `2^16`).

### Explicit producer viewport

Some producer draws use the explicit viewport instead. Let `W = u32 RENDERER+0x3164` and
`H = u32 RENDERER+0x3168`:

- scale `= (W/2, −H/2, (Z1 − Z0)/2, 0)`;
- offset `= (W/2, H/2, (Z1 + Z0)/2, 0)`;
- clip range `[Z0, Z1]`;
- scissor `(0, 0, W, H)`.

### Mapping a source viewport to a native target

Given a source viewport, the target's physical size `(PW, PH)` and the record's sample grid
`(gx, gy)`:

- Decline unless the clip range is exactly `[0, 1]` and every scale and offset lane is finite.
- Native viewport: `x = (ox − sx)·gx`, `y = (oy + sy)·gy`, `width = 2·sx·gx`,
  `height = −2·sy·gy`, depth range `[0, 1]`.
- Native scissor: each scissor value multiplied by the matching grid factor.
- Decline unless `0 < width ≤ 16384`, `0 < height ≤ 16384`, `x ≥ −32768`, `y ≥ −32768`,
  `x + width ≤ 32767`, `y + height ≤ 32767`, and the scissor lies inside `(PW, PH)`.
- The native viewport carries the x/y transform, including jitter. The source z scale and z offset
  remain a shader-side transform, applied once.

When a viewport is built for a surface, the physical size must be at least the record's physical
size, otherwise decline.

### Descriptor-to-surface association (colour)

Given a compact descriptor `d` and a set of candidate records:

1. Map the masked texel format: `0x85` → colour format 8, `0x9A` → 11, `0x9B` → 12, `0x9C` → 13.
   Formats `0x90`–`0x93` (depth formats) are an *unsupported depth-as-colour* error. Anything else
   means "no association".
2. Require dimension 2, no cube flag, mip count 1 and `CONTROL3` depth at most 1; otherwise "no
   association".
3. Location 1 means local and 2 means main; anything else is an error.
4. A candidate matches when its colour format equals the mapped format, its physical size equals
   the descriptor's `IMAGE_RECT`, and its colour (location, offset, pitch) equals the
   descriptor's (location, `OFFSET`, `CONTROL3` pitch).
5. No match means "no association". Matches that disagree on (location, offset, pitch, format,
   size) are an *ambiguous source* error. Otherwise the result is the first matching record.

### Depth classification of the lighting source

The lighting producer's unit-0 descriptor can name a depth buffer. Classify a descriptor `d` read
from address `A`:

- **Packed alias**: masked format `0x85` and `A` is `RENDERER+0x3120` or `RENDERER+0x3140`.
- **Native depth**: masked format `0x90`.
- **Unsupported**: masked format `0x91`–`0x93`.
- **Not depth**: anything else.

For the first two, require dimension 2, no cube flag, mip count 1, `CONTROL3` depth at most 1 and
an `IMAGE_RECT` with both sides in 1–16384; otherwise unsupported.

- Packed alias from `+0x3120`: the owning record is the raw table record at role 1, index 0
  (`RENDERER+0x2CB8`), without role forcing. From `+0x3140`: the owning record is the downsample
  depth record at `RENDERER+0x2BAC`. The owning record's depth format must be 2 (Z24S8) and its
  depth (location, offset, pitch) must equal the descriptor's. The image is that record's
  depth/stencil plane, read as packed A8R8G8B8 texels that carry the Z24S8 bits.
- Native depth: location must be 1 or 2, pitch at least `4·width` and a multiple of 4. The image is
  a Z24S8 depth plane at that (location, offset, pitch) with the descriptor's size, read as
  normalised 24-bit depth.

### Lighting texture sources

Lighting uses two descriptors:

- unit 0: the compact descriptor at `RENDERER+0x3120` when bank zero is enabled, otherwise at
  `RENDERER+0x3140`;
- unit 1: the full descriptor at `PARAMS+0x1520`. Its compact form is words 0–6 and word 8.

### Required surfaces before Main

Main declines with *unsupported* unless: the Main setup record has lists enabled, its setup
reference 1 is nonzero, and the initial-producer enable is set. (The setup record is specified in
[material-setup.md](material-setup.md).)

Let `k` be the target index. The required set is built from these rules, in this order. Each entry
names a record and whether it is an *attachment* (written) or a *prior input* (its content must
exist before Main writes anything):

1. `select(k, 1)`: attachment, target uses. This is the Main target.
2. If setup reference 0 is nonzero, bank zero is enabled and both stencil-prepass programs are
   present: `select(0, 1)` as a colour prior input, and `select(1, 0)` as a depth and stencil
   attachment.
3. `select(1, 1)`: attachment, target uses.
4. If bank zero is enabled: `select(1, 0)`: attachment, target uses.
5. If the group-13 reference is nonzero, the fog vertex program and at least one fog fragment
   program are present, role forcing is off and bank zero is enabled: `select(k, 0)`: attachment.
6. Let `k' = k XOR 1` if group-15 reference 0 is nonzero, bank zero is enabled and both quincunx
   programs are present; otherwise `k' = k`.
7. If any group-15 reference is nonzero and bank zero is enabled: if reference 0 and the quincunx
   programs, `select(k', 2)` as an attachment. If reference 1 and both downsample programs: the
   downsample depth record as a depth attachment, and `select(k', 2)` as an attachment.
8. If the restore reference is nonzero: `select(k', 2)`: attachment.
9. The lighting fragment program may sample only units 0 and 1; any other unit is *unsupported*.
   For each unit it samples, mark its lighting texture source as a prior input:
   - a depth format (`0x90`–`0x93`) marks the depth plane of every set entry that has depth use,
     a local or main depth DMA context matching the descriptor's location, and the same depth
     offset and pitch;
   - otherwise the colour association above, over the set, marks that record's colour plane.

Entries are merged by record address: uses are combined. Two entries with the same address but
different record contents are an error.

A *retained lighting input* also exists. If texture unit 10's compact descriptor is known in the
state inherited from before Main, and it associates (colour rules) with `select(1, 1)`, then
`select(1, 1)` is added as a colour prior input and moved first in the set.

## Edge cases and errors

- Nonzero x or y origin in a record used for a viewport or an image key: decline.
- Unsupported anti-aliasing mode: decline.
- A mapped viewport outside the limits above, or with a clip range other than `[0, 1]`: decline.
- Ambiguous colour association: error. A depth descriptor never takes the colour path.

## Tolerance

The viewport values are single products or sums of exactly representable values. Each must be
within 1 ulp of the exact result; a correctly rounded implementation is exact. Native viewport
rectangles are compared with the integer scissor; a 1 ulp difference in `x`, `y`, `width` or
`height` moves rasterisation by far less than one sample.

**Consequences of non-bit-exact output.** None of these values feeds a hash key or an equality
check, except that the clip range must equal `[0, 1]` exactly. That check is on guest constants,
not on computed values.

## Synthetic test vectors

1. **Role forcing.** Bank zero disabled. `select(1, 1)` → record at table offset
   `0xA8·2 + 0x54 = 0x1A4`. `select(1, 3)` with AA byte nonzero → offset `0xA8·3 + 0x54 = 0x228`;
   with AA byte zero → `0x1A4`.
2. **Surface viewport.** Record 1280×720, AA 0. `VP_HX = 0.5`, `VP_HY = −0.5`, jitter `(0.25, −0.25)`,
   `CLIP_MIN = 0`, `CLIP_MAX = 1`. Scale `(640, −360, 0.5, 0)`, offset `(640.25, 359.75, 0.5, 0)`.
   Mapped on a 1280×720 target with grid 1×1: `x = 0.25`, `y = −0.25`, `width = 1280`,
   `height = 720`, scissor `(0, 0, 1280, 720)`.
3. **AA jitter.** Same, AA mode 3 (grid 2×1): `jx = 0.125`, `jy = −0.25`. Offset
   `(640.125, 359.75, 0.5, 0)`. Mapped on 2560×720: `x = 0.25`, `width = 2560`,
   scissor `(0, 0, 2560, 720)`.
4. **AA mode 4.** `jx = 0.125`, `jy = −0.125`.
5. **Explicit viewport.** `W = 640`, `H = 360`, `Z0 = 0`, `Z1 = 1`: scale `(320, −180, 0.5, 0)`,
   offset `(320, 180, 0.5, 0)`.
6. **Clip range refusal.** `CLIP_MAX = 0.999`: mapping declines.
7. **Colour association.** Record: colour format 8, 1280×720, AA 3, colour offset `0x7C0000`, pitch
   10240, DMA `0xFEED0000`. Descriptor: `OFFSET = 0x7C0000`, location 1, format `0xA5` (masked
   `0x85`), dimension 2, mip 1, `IMAGE_RECT = 2560<<16 | 720`, pitch 10240. Expected: matches.
   `IMAGE_RECT` 1280×720: no association. Pitch 5120: no association. Location 2: no association.
8. **Depth-as-colour.** Same descriptor, masked format `0x90`: the colour path is an error, and the
   depth classification is "native depth".
9. **Prior depth input.** Record with depth use, depth offset `0x40000`, pitch 5120, DMA
   `0xFEED0000`. Lighting descriptor format `0x90`, location 1, offset `0x40000`, pitch 5120.
   Expected: depth plane marked as a prior input; colour plane not marked. Offset `0x41000`: no
   mark.
10. **Merge.** Prior colour input on record X, then X as a colour and depth attachment: one entry,
    uses = colour+depth, prior = colour, attachment = colour+depth. The same address with a
    different record byte: error.

## Open questions

1. The two invocation values (initial-producer enable and target index) are currently observed as
   values passed into the game's Main preparation. Their guest-memory origin has not been
   established as a data fact. Until it is, a replacement must receive them with the invocation.
2. The packed-alias rule ties the `+0x3120` descriptor to the role 1, index 0 record without role
   forcing. That matches all observed frames but has not been proven for every mode.
