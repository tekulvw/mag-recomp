# Group-zero instance and wind deformation

## Scope

Main-pass preparation of vertices for items whose material group is 0 on the retail v1.0 build.
These items draw one shared partition mesh many times ("logical instances"), each instance with its
own offset, scale and orientation, plus a per-vertex wind sway and a distance-based shrink ("fade").
The preparation expands every instance into its own copy of the partition's vertices and produces:

- one deformed position per (instance, vertex);
- one colour row per (instance, vertex) for vertex attribute slot 12;
- a 32-bit index list covering all instances.

Out of scope: the vertex program that later consumes these arrays, culling and software depth
outputs, other attribute slots (they are expanded by the generic auxiliary-attribute rules), and
every other material group. Groups 3 and 4 are in
[noninstanced-deformation.md](noninstanced-deformation.md); group 11 is in
[skinning-deformation.md](skinning-deformation.md).

## Admission

The behaviour applies only when all of the following hold. Any other combination is a visible,
typed refusal of this preparation (the item stays in the scene's accounting as unprepared); it is
not evidence of malformed data.

| Condition | Data |
| --- | --- |
| The item's draw-batch record says group 0 | high nibble of byte `+0x00` of the 32-byte draw-batch record is 0 |
| The pass is not an update-only pass | pass record `+0x144` (`u32`) is greater than 2. Values 0, 1 and 2 mean the pass only updates state for group 0; no geometry is prepared |
| Geometry mode is the instanced-wind mode | `(pass +0x230 OR (primary material +0x58 bit 0 ? 0x20 : 0)) AND NOT 0x2` equals `0x28` |
| One worker | `min(pass +0x22C, primary material +0x58 bit 0 ? 1 : 2)` equals 1 |
| Output selector 0 | byte 4 of the partition's configuration stream (stream 19) is 0 |
| At least one instance | object transform byte `+0x6C` is non-zero |
| Instance rows are in the geometry resource | object transform `+0x60` bit 31 is set |
| No other writer of slot 12 | none of the item's auxiliary attribute bindings (successful or failed) targets slot 12 |
| Colour frequency at least 2 | mesh record `+0x34` low 16 bits ≥ 2 (see open question 2) |
| Batch maximum at least 1 | partition record `+0xFC` ≥ 1 |

If the primary material copy is unavailable, 96 bytes are read from the 24-bit guest address in
bytes `+0x06..+0x08` of the draw-batch record (low byte zero), provided
`(draw-batch u32 +0x1C AND 0x1FF) × 16 ≥ 96`; otherwise the preparation is refused as
"missing material".

## Inputs

All fields are big-endian.

| Object | Field | Meaning |
| --- | --- | --- |
| Pass record (688 bytes) | `+0x238` `u32` | guest address of the 128-byte view environment record |
| | `+0x260` `f32` | wind frequency factor `ω0` |
| | `+0x264` `f32` | unused by this behaviour |
| | `+0x268` `f32` | first wind amplitude factor `ω2` |
| | `+0x26C` `f32` | second wind amplitude factor `ω3` |
| | `+0x270` `f32` | wind phase `θ` (advances with time) |
| Primary material (first 48 bytes) | `+0x00` `f32` ×4 | row `M0`; lanes x, y are used |
| | `+0x10` `f32` ×4 | row `M1`; lane x is used |
| Object transform record (112 bytes) | `+0x00` `f32` ×4 | object quaternion (not used by any output; must be finite) |
| | `+0x10` `f32` ×3 | object translation `T` |
| | `+0x1C` `f32` | object scale (not used by any output; must be finite) |
| | `+0x60` `u32` | bit 31: must be set; bits 0..30: byte offset of the instance rows from the geometry resource base |
| | `+0x64` `u32` | bit 31: colour pool location (0 = local video memory, 1 = main memory); bits 0..30: offset of the colour pool in that location |
| | `+0x68` `u16` | fade multiplier `φm`, half-float (decoding below) |
| | `+0x6A` `u16` | fade start `φs`, half-float |
| | `+0x6C` `u8` | instance count `I` |
| View environment record (128 bytes) | `+0x40` `f32` ×4 | row `E3` (x, y, z, w). Rows at `+0x10`, `+0x20`, `+0x30` do not affect any output but must be finite |
| Mesh record (192 bytes) | `+0x34` `u32` | low 16 bits: colour frequency `F` |
| Partition record (544 bytes) | `+0xFC` `u32` | batch maximum `Bmax` |
| Instance rows | `16 × I` bytes at resource base + offset | one row per instance (decoding below) |
| Colour pool | `4 × C` bytes | `C` entries of four unsigned bytes R, G, B, A (`C` defined below) |
| Partition vertex banks | semantics 1, 2, 9 | positions `P` (xyz), normals `N` (xyz), weights `W` (lane 3 = `w3`), each with `V` entries, decoded by the geometry stream codecs |
| Partition index list | `u16` × `K` | triangle indices into `0..V-1` |

When a semantic is declared in both the primary and the secondary declaration lists, the first
primary declaration wins, including when its decode failed (then this preparation is refused).

### Instance row decoding

Each 16-byte row holds eight 16-bit fields `h0..h7`. Each field decodes as a "rebased half":

- if `h AND 0x7FFF` is 0, the value is zero with the sign of bit 15;
- otherwise, with `s` = bit 15, `E` = bits 10..14, `m` = bits 0..9, the value is
  `(-1)^s × 2^(E-15) × (1 + m/1024)`. Every exponent, including 0 and 31, uses this normal form:
  there are no subnormal, infinite or NaN encodings.

Fields `h0, h1, h2` are the instance offset `o = (ox, oy, oz)`, `h3` is the instance scale `σ`, and
`h4..h7` are the instance quaternion `q = (qx, qy, qz, qw)`.

### Fade half decoding

`φm` and `φs` use binary16 layout (`s`, 5-bit `E`, 10-bit `m`):

- `E` from 1 to 31: `(-1)^s × 2^(E-15) × (1 + m/1024)`. `E = 31` is an ordinary finite value
  (up to just under 2^17), not infinity or NaN;
- `E = 0`, `m = 0`: signed zero;
- `E = 0`, `m ≠ 0`: `(-1)^s × m × 2^-25` in the current reference. This is half of the IEEE
  binary16 subnormal value; see open question 1.

### Colour decoding

Each pool entry converts to `(R/255, G/255, B/255, A/255)`.

## Behaviour

### Helper functions

- `frac(x) = x - floor(x)`, in `[0, 1)`. For negative `x` this is not `x - trunc(x)`; for example
  `frac(-0.25) = 0.75`.
- `tri(x) = |2·frac(x) - 1|`, a triangle wave in `[0, 1]` with period 1, equal to 1 at integers
  and 0 at half-integers.
- `S(t) = t²·(3 - 2t)`, the cubic smoothstep, mapping `[0, 1]` onto `[0, 1]`.
- `wave(φ; f) = S(tri(f·(2·frac(φ) - 1) + 1/2))`.

`wave` is continuous in `φ` everywhere (the jump of `2·frac(φ) - 1` at integers is absorbed by the
symmetry of `tri`), its values lie in `[0, 1]`, and its slope with respect to `φ` is at most `6·|f|`.

### Per-item constants

- `f = ω0 · M1.x` (wind frequency)
- `A1 = M0.x · ω2` and `A2 = M0.y · ω3` (wind amplitudes)
- three wind coefficients `KA1`, `KC` and `KB1`. They are fixed positive constants of the retail
  program, each between 0.1 and 2, and are not stored in guest data. See open question 3.

### Per instance `i` (0-based, in row order)

1. Normalise the quaternion: `q̂ = q / |q|`. If `|q| = 0`, `q̂ = (0, 0, 0, 0)` and no repair is
   made.
2. Rotation matrix `R` from `q̂ = (x, y, z, w)`, acting on column vectors:

   ```
   | 1-2(y²+z²)   2(xy-wz)     2(xz+wy)   |
   | 2(xy+wz)     1-2(x²+z²)   2(yz-wx)   |
   | 2(xz-wy)     2(yz+wx)     1-2(x²+y²) |
   ```

   A zero quaternion gives the identity matrix.
3. Instance centre `c = T + o`.
4. View depth `d = E3.x·c.x + E3.y·c.y + E3.z·c.z + E3.w`.
5. Fade `φ = clamp((d - φs)·φm, 0, 1)`.
6. Effective scale `s = σ·(1 - φ)`. A fully faded instance (`φ = 1`) collapses every vertex onto
   `o` before wind is added.

The object's quaternion and scale do not influence `d`: the object's own rotation and scale act
only on lanes that the fade does not read. They are still read and must be finite.

### Per vertex `v` of instance `i`

1. Placed position `B = s·(R·P_v) + o`. The object translation `T` is not added: outputs stay in
   the object's frame and the vertex program applies the object transform.
2. Two phases:
   - `α = KA1 · (θ + B.x + B.y)`
   - `β = KB1 · (θ + KC·(B.x + B.y + B.z))`
3. Displacement `D = (A1·wave(β; f) + A2·wave(α; f)) · (1 - w3)`.
4. Output position `(B.x + D·N.x, B.y + D·N.y, B.z + D·N.x, 1)`.

   The z lane uses the normal's x component, not its z component. This is the observed behaviour
   and must be reproduced; it is not a typo.

### Colour rows (slot 12)

The colour pool is indexed per draw batch. Instances are split into consecutive batches of at most
`Bmax` instances. For instance `i` and vertex `v`:

- `b = floor(i / Bmax) · Bmax` (the first instance of the batch);
- `colour index = b + floor(((i - b)·V + v) / F)`.

The slot-12 row for `(i, v)` is the decoded pool entry at that index. The pool length `C` that must
be read is the largest index used plus one:

`C = max over batches with first instance b and n instances (n·V > 0) of b + floor((n·V - 1) / F) + 1`.

Slot 12 is enabled for these draws even if the mesh's vertex format record disables it, with the
format "four unsigned normalised bytes, stride 4".

### Indices

For instance `i`, each base index `k` becomes `i·V + k`, a `u32`. Instance index lists are
concatenated in instance order.

### Output order

Positions and colour rows are instance-major: all `V` vertices of instance 0, then all of instance
1, and so on. Output count is `I·V`; index count is `I·K`.

## Edge cases and errors

The preparation is all-or-nothing. A refusal produces no partial output.

| Case | Result |
| --- | --- |
| `I = 0` | refuse: instance population |
| normal or weight bank length differs from `V` | refuse: normal population / weight population |
| `F < 2` | refuse: colour frequency (see open question 2) |
| `Bmax = 0` | refuse: batch maximum |
| a base index `≥ V` | refuse: index, reporting the index |
| `I·V` exceeds `2^32 - 1`, or the output would exceed the implementation's resource budget | refuse: budget. An implementation must refuse rather than truncate |
| any lane of the object quaternion, translation, scale, the four environment rows or the decoded fade values is not finite | refuse: unqualified float |
| any instance offset, scale or quaternion lane not finite | cannot occur with the rebased-half decoding |
| `(d - φs)·φm` exceeds the binary32 range | refuse: unqualified float |
| any output position lane not finite in binary32 | refuse: unqualified float |
| a guest read fails or is out of range | refuse: source |

Signed zeros in inputs need no special handling. Inputs are never NaN or infinite after the checks
above.

## Tolerance

Implementations use ordinary native arithmetic. The reference is the formulas above evaluated in
exact real arithmetic. Test vectors in this document are given to at least six significant digits.

- **Positions, x/y/z lanes.** Let `M = |s|·(|P.x| + |P.y| + |P.z|) + |o_lane|` (the size of the
  placed position) and `δ = 2^-20 · (1 + |θ| + 4·M)` (the allowed phase error). The allowed absolute
  error per lane is `2^-18·M + (|A1| + |A2|)·|1 - w3|·|N_lane|·(2^-18 + 6·|f|·δ)`, where `N_lane`
  is `N.x` for the x and z lanes and `N.y` for the y lane.
  *Justification:* `2^-18` of an object's size (under 4 µm on a 1 m object) is far below one pixel
  at any usable distance. The phase term lets an implementation evaluate a large elapsed phase in
  binary32; it equals a timing jitter of about one millionth of the elapsed phase, which is not
  visible in a sway animation.
- **Position, w lane.** Exactly 1.0.
- **Colour rows.** Within 1 ULP of `byte/255`. *Justification:* this is a format conversion; there
  is no reason to deviate.
- **Indices.** Exact.

### Consequences of non-bit-exact output

These places previously relied on, or could rely on, exact equality of produced positions:

1. **Depth EQUAL against a separate depth pre-pass.** Main draws that test depth EQUAL rely on an
   earlier pass having written the same depths. If that pass uses a different evaluation of these
   formulas (the recompiled original Depth/DepthNormal job, the GPU kernel variant, or any other
   code path), pixels will fail the test and the surface will show holes or z-fighting. Every pass
   whose depth must coincide with Main for the same item and view must consume the same produced
   position array, or the comparison must be relaxed explicitly (for example to LEQUAL) and the
   relaxation recorded as a fidelity reduction.
2. **Shadow passes.** Shadow maps rendered from another evaluation differ by up to the tolerance.
   The existing depth bias normally absorbs this; check for acne in acceptance runs.
3. **Determinism.** The same inputs must give the same output bits within one build on one
   platform, independent of threading, batching, deferral or job order. Inline and deferred
   preparation of the same item must agree bit-for-bit, because (1) depends on it.
4. **CPU and GPU variants.** A CPU and a GPU implementation may legitimately differ within the
   tolerance. They must not be mixed for one item across passes that rely on (1).
5. **Differential tests.** Comparisons against the original program (kept outside this
   repository) must use the tolerances above, not bit equality, for positions. Colour rows and
   indices remain exact comparisons.
6. **Caches and deduplication.** No current cache keys on produced vertex values. Any future
   content hash or deduplication over produced positions must not assume equality between results
   from different implementations or code paths.

## Synthetic test vectors

Unless stated, `ω0..ω3`, `θ`, `M0`, `M1` are zero (no wind), the object quaternion is (0, 0, 0, 1),
`T = (0, 0, 0)`, object scale 1, environment rows are the identity rows
(`E0 = (1,0,0,0)` … `E3 = (0,0,0,1)`), `φm = φs = 0` (so `φ = 0`), and `Bmax = 255`.

### GZ-1: instance placement and rotation

- `V = 2`: `P0 = (1, 0, 0)`, `P1 = (0, 2, 0)`; normals `(0, 0, 1)`; weights `(0, 0, 0, 1)`.
- Indices `[0, 1, 1]`. `F = 3`, `Bmax = 1`, pool of 2 colours `c0`, `c1`.
- Instance 0: `o = (3, 0, 0)`, `σ = 1`, `q = (0, 0, 0, 1)`.
- Instance 1: `o = (0, 0, -1)`, `σ = 0.5`, `q = (0, 0, 1, 1)` (normalises to a 90° turn about z).

Expected positions: `(4, 0, 0, 1)`, `(3, 2, 0, 1)`, `(0, 0.5, -1, 1)`, `(-1, 0, -1, 1)`.
Indices: `[0, 1, 1, 2, 3, 3]`. Colours: `c0, c0, c1, c1`. Required pool length `C = 2`.

### GZ-2: fade

One vertex `P = (1, 1, 1)`, one instance `o = (0, 0, 0)`, `σ = 2`, identity quaternion, no wind.
`E3 = (0, 0, 1, 0)` so `d = c.z = T.z`. `φm` half `0x3800` (0.5), `φs` half `0x4000` (2).

| `T.z` | `d` | `φ` | expected position |
| --- | --- | --- | --- |
| 0 | 0 | 0 (clamped from -1) | (2, 2, 2, 1) |
| 3 | 3 | 0.5 | (1, 1, 1, 1) |
| 10 | 10 | 1 (clamped from 4) | (0, 0, 0, 1) |

### GZ-3: wind with phases that do not depend on the constants

One vertex at `P = (0, 0, 0)`, one instance at `o = (0, 0, 0)`, `σ = 1`, `θ = 0`. Then `B = 0` and
`α = β = 0` whatever `KA1`, `KB1`, `KC` are. `N = (1, 2, 3)`, `w3 = 0.5`, `M0 = (0.5, 1, ·, ·)`,
`ω2 = 0.5`, `ω3 = 0.5`, `ω0 = 1`.

| `M1.x` | `f` | `wave(0; f)` | `D` | expected position |
| --- | --- | --- | --- | --- |
| 0.5 | 0.5 | 1 | 0.375 | (0.375, 0.75, 0.375, 1) |
| 1 | 1 | 0 | 0 | (0, 0, 0, 1) |

The first row also checks that z is displaced by `N.x`, not `N.z`.

### GZ-4: wave function

| `φ` | `f` | `wave(φ; f)` |
| --- | --- | --- |
| 0 | 1 | 0 |
| 0.25 | 1 | 1 |
| 0.125 | 1 | 0.5 |
| -0.375 | 2 | 1 |
| 1000000.25 | 1 | 1 |
| 3.7 | 0.3 | 0.145152 |
| -2.6 | 1.5 | 0.648 |

These rows test the helper alone. Inputs are the binary32 values nearest the decimals shown.
Allowed error: `1e-5` absolute. The `1000000.25` row checks that the fraction of a large
but representable phase is kept. Inside the full preparation, a phase that large carries the phase
term of the position tolerance instead.

### GZ-5: colour batches

`V = 3`, `I = 5`, `Bmax = 2`, `F = 4`. Colour indices per instance:
`[0,0,0]`, `[0,1,1]`, `[2,2,2]`, `[2,3,3]`, `[4,4,4]`. Required pool length `C = 5`.

`V = 4`, `I = 3`, `Bmax = 255`, `F = 6`: `[0,0,0,0]`, `[0,0,1,1]`, `[1,1,1,1]`; `C = 2`.

### GZ-6: closed form with the wind constants

`P = (1, 0, 0)`, `o = 0`, `σ = 1`, identity quaternion, `θ = 0`, `w3 = 0`, `N = (1, 0, 0)`. Then
`B = (1, 0, 0)`, `α = KA1`, `β = KB1·KC`, and the expected position is
`(1 + D, 0, D, 1)` with `D = A1·wave(KB1·KC; f) + A2·wave(KA1; f)`. Use it once the constants are
ruled on (open question 3).

### GZ-7: refusals

- `I = 0` → instance population.
- Two vertices, one normal → normal population.
- Index list `[2]` with `V = 2` → index 2.
- `F = 1` → colour frequency.
- Environment row `E1.y` = NaN → unqualified float, even though `E1` does not affect any output.

## Open questions

1. **Subnormal fade halves.** The current reference decodes a subnormal half-float (`E = 0`,
   `m ≠ 0`) as `m × 2^-25`, half of the IEEE value `m × 2^-24`. This needs confirmation against
   the original program. Until then, implement the stated value. The difference only matters for
   fade multipliers or starts below `2^-14`.
2. **Colour frequency 0 and 1.** For these values the colour index depends on a base index
   inherited from earlier state, which has not been specified. They are refused.
3. **Wind constants `KA1`, `KC`, `KB1`.** These are fixed values of the retail program with no
   guest-data location an implementation could read. They are decimal tuning values, not evident
   mathematical constants, so this specification does not state them. A user ruling is needed on
   whether their values may be published in this specification. (`KA1` and `KB1` are shared with
   group 3; `KC` equals group 3's `KC3` to within one binary32 ULP.)
4. **Coupling to other specifications.** The geometry stream codecs (positions, normals, weights,
   instance-row location), the auxiliary attribute expansion for the other slots, and the pass
   route rule (`+0x144`) are specified elsewhere or still need specifications.
