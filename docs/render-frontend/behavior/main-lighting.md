# Main lighting producer (initial slot 1)

## Scope

The lighting producer runs before the first Main item when the Main setup record's setup
reference 1 is nonzero (event order: [scene-plan.md](scene-plan.md)). It draws one or two
full-screen triangles that write only the alpha channel of the Main lighting targets, gated by a
depth-bounds test on the stored depth. This document specifies its constants, textures, fixed
state, draws and the state it leaves behind.

Inputs are from [material-producers.md](material-producers.md); surface selection, viewports and
texture associations are from [main-producer-inputs.md](main-producer-inputs.md). Applies to
retail v1.0.

Out of scope: the GPU program decode and compilation contracts, and the opt-in diagnostic mode that
omits the draws.

## Inputs

- The camera matrix `M` (projection block `+0x70`), and the projection fields `+0x54`
  (`p_scale`), `+0x58` (`p_far`), `+0x60` (`p_slope`) and `+0xA24` (`p_aspect`).
- The renderer depth-distance fields `r_a = f32 RENDERER+0x34` and `r_b = f32 RENDERER+0x40`.
- The parameter transforms `P0 = PARAMS+0xD0` and `P1 = PARAMS+0xB50` (row-major `4×4`).
- `o = V_ONE` (four lanes), `s_d = S_DEPTH`, and `a = BASIS_A[0]`, `b = BASIS_B[0]`,
  `c = BASIS_C[0]`.
- Scalars `Z0`, `Z1`, `ZH`, `PA`, `PB`, `PC`, `PD`, `LOFF`, `T_LO`, `T_HI`, `T_FAR`, `T_UVFAR`.
- `W = u32 RENDERER+0x3164`, `H = u32 RENDERER+0x3168`.
- The lighting programs: vertex registry index `0x12`, fragment registry index `0x44`.
- The two lighting texture sources ([main-producer-inputs.md](main-producer-inputs.md)).

Matrices use the row-vector convention: a point `p` (row vector) maps to `p·M`. Row 3 of an affine
matrix is the translation. "Row-major" means the 16 floats are stored row 0 first.

## Outputs

1. Vertex constant row 4, then the lighting vertex program's static constants (which win over row
   4 if they also write it).
2. Fragment program constant rows 0–7.
3. Texture units 0 and 1 descriptors and images.
4. Fixed-function state for the first draw, a modified state for the optional second draw, and a
   final state left for later Main work.
5. One or two draws, each in its own pass, of the same three-vertex triangle.
6. The *current* (constant) values of vertex attributes 0 and 8 after the draws.
7. A "lighting ran" flag that the fog producer reads ([main-fog.md](main-fog.md)).

## Behaviour

### Camera inverse

Let `A` be the upper-left `3×3` of `M` and `t = (M[3][0], M[3][1], M[3][2])`. Column 3 of `M` is
ignored (treated as `(0, 0, 0, 1)`). With `det = det(A)`:

```
Inv[i][j] = (A⁻¹)[i][j] · o[j]                         for i, j in 0..2
Inv[3][j] = −(t · A⁻¹)[j] · o[j]                       for j in 0..2
Inv[i][3] = 0 for i in 0..2;  Inv[3][3] = 1
```

That is the ordinary affine inverse, with column `j` (rows 0–3) scaled by `o[j]`. In the game data
`o` is expected to be all ones, which makes `Inv` exactly the affine inverse. `o[3]` is unused.

### Shadow/parameter texture transforms

For `k = 0, 1`:

```
T_k = | c        0  0  0 |
      | 0        b  0  0 |
      | 0        0  a  0 |
      | e_k      a  a  1 |       where e_0 = c and e_1 = 0

F_k = Inv · P_k · T_k          (ordinary 4×4 matrix products)
```

Fragment constant row `4k + r` is row `r` of `F_k`, as four `f32` lanes in column order.

### Reconstruction extent

```
height = p_far · p_slope
width  = height · p_aspect
vertex constant row 4 = (width, height, +0, +0)
```

### Depth bounds

Define the window-depth map `D(z) = ZH · ((PA / z + PB + PC) / PD) + ZH`. Then

```
distance = (r_a + r_b) / (p_scale · s_d)
upper    = D(distance + LOFF)
bounds   = [Z0, upper]
```

### Full-screen triangle

Three vertices, each `(position x, y, z, w, texcoord s, t, r, q)`, in this order:

| Vertex | Position | Texcoord |
| --- | --- | --- |
| 0 | `(T_LO, T_LO, T_LO, 1)` | `(+0, T_HI, +0, 1)` |
| 1 | `(T_FAR, T_LO, T_LO, 1)` | `(T_UVFAR, T_HI, +0, 1)` |
| 2 | `(T_LO, T_FAR, T_LO, 1)` | `(+0, T_LO, +0, 1)` |

Attribute 0 is the position and attribute 8 the texture coordinate. The draw is a non-indexed
triangle list of these three vertices. The constants are copied bit for bit.

### Textures

- Unit 0: the compact lighting source descriptor (`RENDERER+0x3120` if bank zero is enabled,
  otherwise `+0x3140`).
- Unit 1: the full descriptor at `PARAMS+0x1520` (twelve words), after which unit 1's border colour
  is forced to `0xFFFFFFFF`.
- Any image previously associated with units 0 and 1 is forgotten. Each unit the lighting fragment
  program samples must be bound to an image whose provenance matches that unit's (source address,
  descriptor) exactly. That image must be a single-sample, single-layer 2D image with the
  descriptor's size and mip count. Binding is purely by provenance: the producer does not classify
  depth itself. The list of producer texture bindings already contains, for a depth-classified
  request, a colour image derived from the depth plane ("Producer texture bindings" in
  [main-producer-inputs.md](main-producer-inputs.md)). This applies to both units, including unit
  1's full descriptor, which is matched by its compact form. A binding whose image has a depth
  packet format is a decline.
- The lighting fragment program may sample only units 0 and 1. It may not discard fragments, and
  its header flags (bits 1–3 of the header `+8` word) must be zero.

### Fixed-function state before the first draw

Precondition (the *stencil-write precondition*): the stencil test is known disabled, or it is enabled and no stencil face that can be used
has writes. Precisely: the stencil enable must be known, and when it is on the front face must be
known with write mask 0. The back face is required (known, write mask 0) only when two-sided
stencil is on. When two-sided stencil is off, an unknown back face is accepted, but a *known* back
face with a nonzero write mask is still a decline, so that later enabling two-sided stencil cannot
expose writes. The two-sided flag must be known when stencil is on.
Otherwise decline, because a depth-bounds rejection must not be able to change stencil.
Also decline if the bounds are not finite or `bounds[0] > bounds[1]`.

Then set: depth-bounds test on with range `bounds`; depth test off; depth write off; alpha test
off; blending off; colour write mask alpha only, on all targets; face culling off; sRGB output
conversion off.

### Draws

- **Draw 0** targets `select(1, 1)` with the explicit producer viewport (`W`, `H`, `Z0`, `Z1`).
- **Draw 1** happens only when bank zero is enabled. Before it: the depth-bounds test is turned off,
  and on both stencil faces the comparison becomes "less", the reference 0x80 and the read mask
  0xFF. The stencil operations and write masks are unchanged. It targets `select(1, 0)` with that
  surface's source viewport. The record's origin must be zero.

Each draw's target must already have initialised colour (its existing contents are kept, not
cleared) and an initialised depth plane, plus an initialised stencil plane if it has one.
The bound colour and depth images must match the surface's format and size.

Each draw's raster state is translated as in [raster-state.md](raster-state.md), with triangles,
one sample, colour attachment 0, no bias clamp and an unobserved blend constant. Every known-field
bit must be set and no obligation may remain; otherwise decline. Given the writes above, the fields
this producer leaves to inherited state, which must therefore be known, are:

- front face (needed for the cull bit, even with culling off);
- stencil test enable; if it is enabled, the two-sided flag and the stencil faces the raster
  rules need (front face only when two-sided is off, both faces when it is on), including their
  write masks for the stencil-write precondition above;
- polygon-offset fill enable, which must be known *disabled*, because no bias clamp is supplied
  and so an enabled offset can never be fully known.

Every other raster field (cull enable, depth test, depth write, blending, colour mask, depth bounds,
topology, samples) is written by this producer. The shader controls additionally use the alpha-test
and sRGB-output state, which this producer writes, and texture units 0 and 1, which it sets.

### State after the producer

Depth-bounds test off, two-sided stencil off, stencil test off. The current value of attribute 0
becomes vertex 2's position and attribute 8 becomes vertex 2's texture coordinate. The "lighting
ran" flag is set.

## Edge cases and errors

Decline, with no draw and no state change visible to later Main work, when:

- the projection block or the parameter object address is not 16-byte aligned, or any of `V_ONE`,
  `BASIS_A`, `BASIS_B`, `BASIS_C` is not 16-byte aligned;
- any of the 16 stored elements of `M`, `P0` or `P1`, or any of the four lanes of `o`, is not
  finite. This is intentional: column 3 of `M` and `o[3]` do not affect any output, but a
  non-finite value there signals corrupt input, and the conservative choice is to decline;
- `det = 0`, or any element of `Inv` is not finite (singular or overflowing camera);
- any fragment constant, `width`, `height` or bound is not finite;
- any triangle constant is not finite;
- a lighting program is absent, discards, or exports depth;
- a required image binding is missing or mismatched, or the target is not initialised;
- the stencil precondition above fails.

Lanes 1–3 of `BASIS_A`, `BASIS_B` and `BASIS_C` are not read and are not checked. Lane 0 of each
is checked only through the finiteness of the results. Signed zero inputs are accepted. Subnormal inputs are accepted and treated as
their values; the tolerance below applies.

## Tolerance

Implementations use ordinary native arithmetic in any evaluation order, at binary32 or wider
precision.

| Output | Tolerance | Reason |
| --- | --- | --- |
| Fragment rows 0–7 (`F_0`, `F_1`) | each element within `2^-18 · max(1, maxabs(F_k))` of the exact value, where `maxabs` is the largest absolute element of the same matrix | These rows project reconstructed positions into shadow/parameter texture space. A relative error of `2^-18` is under 1/60 of a texel at 4096 texels, and under 1/1000 of a depth step at the shadow-map precision. |
| Vertex row 4 | within 2 ulp per lane | two products |
| `upper` depth bound | within `2^-22` absolute of the exact value | At most 4 steps of a 24-bit depth buffer. This only changes which pixels exactly at the bound are lit. |
| `bounds[0]` | bit-exact copy of `Z0` | data movement |
| Triangle and texture words | bit-exact copies | data movement |

The exact value is the real-number result of the formulas above applied to the binary32 inputs.

**Consequences of non-bit-exact output.**

- Fragment constant values may feed a compiled-program or pipeline cache key if the implementation
  folds constants into programs. Small differences then create distinct cache entries for the same
  frame content. They must not create incorrect cache hits.
- The `bounds[0] ≤ bounds[1]` decline uses the computed bound. When the exact `upper` lies within
  tolerance of `Z0`, either outcome is acceptable.
- Differential tests against the original must compare with these tolerances, not bit equality.
- No later producer compares these values for equality.

## Synthetic test vectors

All use `o = (1, 1, 1, 1)`, `a = 0.5`, `b = −0.5`, `c = 0.5`.

**L1: inverse and transforms.** `M` rows: `(0, 2, 0, 0)`, `(−4, 0, 0, 0)`, `(0, 0, 0.5, 0)`,
`(1, 2, 3, 1)`. Then `Inv` rows are `(0, −1/4, 0, 0)`, `(1/2, 0, 0, 0)`, `(0, 0, 2, 0)` and
`(−1, 1/4, −6, 1)`.

- `P0 = I`. Rows 0–3: `(0, 1/8, 0, 0)`, `(1/4, 0, 0, 0)`, `(0, 0, 1, 0)`, `(0, 3/8, −5/2, 1)`.
- `P1` rows `(2, 0, 0, 0)`, `(0, 2, 0, 0)`, `(0, 0, 2, 0)`, `(1/4, 1/2, 3/4, 1)`. Rows 4–7:
  `(0, 1/4, 0, 0)`, `(1/2, 0, 0, 0)`, `(0, 0, 2, 0)`, `(−7/8, 0, −41/8, 1)`.

All of these are exactly representable, so a correct implementation reproduces them exactly.

**L2: lane scale.** As L1 with `o = (2, 1, 1, 1)`. Column 0 of `Inv` doubles: rows
`(0, −1/4, 0, 0)`, `(1, 0, 0, 0)`, `(0, 0, 2, 0)`, `(−2, 1/4, −6, 1)`.

**L3: singular camera.** `M` row 2 `= (0, 0, 0, 0)`. Expected: decline.

**L4: checked and ignored lanes.** As L1 with `M[0][3] = NaN`: decline. As L1 with `o[3] = ∞`:
decline. As L1 with lane 1 of `BASIS_A` = NaN: same outputs as L1.

**L5: extent.** `p_far = 100`, `p_slope = 0.5`, `p_aspect = 1.75`. Row 4 = `(87.5, 50, +0, +0)`.

**L6: depth bound.** `ZH = 0.5`, `PA = −2`, `PB = 1`, `PC = 0`, `PD = 1`, `Z0 = 0`.

| `r_a` | `r_b` | `p_scale` | `s_d` | `LOFF` | `distance` | `upper` (hex) |
| --- | --- | --- | --- | --- | --- | --- |
| 3 | 5 | 2 | 1 | 0 | 4 | 0.75 (`0x3F400000`) |
| 1.5 | 1.5 | 0.5 | 3 | 0 | 2 | 0.5 (`0x3F000000`) |
| 1.5 | 1.5 | 0.5 | 3 | 2 | 2 | 0.75 (`0x3F400000`) |

**L7: inverted bounds.** As the first L6 row but `Z0 = 0.9`. Expected: decline.

**L8: zero distance.** `r_a = r_b = 0`, `LOFF = 0`. `PA / 0` is infinite, so the bound is not
finite. Expected: decline.

**L9: stencil precondition.** Stencil enabled, two-sided on, front write mask 0, back write mask
0xFF: decline. Both masks 0: accepted. Two-sided off, front write mask 0, back face unknown:
accepted. Two-sided off, front write mask 0, back face known with write mask 0xFF: decline.
Two-sided on, back face unknown: decline. Stencil enable unknown: decline.

**L10: draw count.** Bank zero disabled: one draw, target `select(1, 1)` (which resolves to role
2), and the state after the producer has stencil disabled. Bank zero enabled: two draws, and the
second has depth bounds off and stencil "less" with reference 0x80 on both faces.

**L11: triangle.** `T_LO = −1`, `T_HI = 1`, `T_FAR = 3`, `T_UVFAR = 2`. Vertex data in order:
`(−1, −1, −1, 1, 0, 1, 0, 1)`, `(3, −1, −1, 1, 2, 1, 0, 1)`, `(−1, 3, −1, 1, 0, −1, 0, 1)`.
After the producer, attribute 0 = `(−1, 3, −1, 1)` and attribute 8 = `(0, −1, 0, 1)`.

## Open questions

1. `V_ONE` is assumed to hold ones. The spec keeps the general lane scaling so a different value is
   still reproduced, but no frame with other values has been examined.
2. The meanings given to the projection fields (`p_far`, `p_slope`, `p_aspect`, `p_scale`) and to
   `r_a`, `r_b` are inferred from how they combine. The behaviour does not depend on the names.
3. The derivation of the lighting fragment program's use of rows 0–7 (which target texture each
   reconstructs into) is not needed for this producer and has not been established.
