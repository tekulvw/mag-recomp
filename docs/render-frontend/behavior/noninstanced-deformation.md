# Group-3 wind and group-4 ripple deformation

## Scope

Main-pass preparation of vertex positions for non-instanced items whose material group is 3
("wind") or 4 ("ripple") on the retail v1.0 build. Each partition is drawn once. Positions are
displaced along the vertex normal (group 4) or along a mix of normal lanes and a fixed vertical
lane (group 3). Normals, weights and, for group 4, tangents pass through unchanged.

Out of scope: the vertex program, culling, and the other groups. Group 0 is in
[group-zero-deformation.md](group-zero-deformation.md), group 11 in
[skinning-deformation.md](skinning-deformation.md). The waveform helpers `frac`, `tri`, `S` and
`wave` are defined in the group-zero specification and are used here unchanged.

## Admission

| Condition | Data |
| --- | --- |
| Group 3 or 4 | mesh record `+0xBC` (`u16`) is 3 or 4 and equals the high nibble of draw-batch record byte `+0x00` |
| Pass present | the item's pass record address is non-zero |
| No optional per-vertex post-processing | if draw-batch byte `+0x1B` bit 0 is clear, `draw-batch byte +0x18 >> 2` must be 0; if the bit is set the count is 0 |
| No skinning palette | partition record `u32 +0x48` and `u32 +0x50` are both 0 |
| Geometry mode | `mode = (pass +0x230 OR (primary material +0x58 bit 0 ? 0x20 : 0)) AND NOT 0x2` is 8, 10 or `0x28` |
| Worker control | `min(pass +0x22C, primary material +0x58 bit 0 ? 1 : 2)` equals 1 when `mode = 0x28` and 2 otherwise |
| Group 4 has tangents | a tangent bank (semantic 3) exists |

The primary material fallback (96 bytes from the draw-batch 24-bit address when no material copy
exists) is the same as for group 0. Any failed condition is a visible, typed refusal of this
preparation.

## Inputs

| Object | Field | Meaning |
| --- | --- | --- |
| Pass record | `+0x260` `f32` | wind frequency factor `ω0` (group 3) |
| | `+0x264` `f32` | unused |
| | `+0x268`, `+0x26C` `f32` | wind amplitude factors `ω2`, `ω3` (group 3) |
| | `+0x270` `f32` | phase `θ` (both groups) |
| Primary material | `+0x00` `f32` ×4 | row `M0` |
| | `+0x10` `f32` ×4 | row `M1` |
| | `+0x20` `f32` ×4 | row `M2` |
| Object transform record | `+0x10`, `+0x14`, `+0x18` `f32` | object origin `T`; only `Σ = T.x + T.y + T.z` is used |
| Partition vertex banks | semantics 1, 2, 9, 3 | positions `P` (four lanes), normals `N`, weights `W`, tangents `Tg` (group 4), each with `V` entries |

Lanes used, per group:

| Input | Group 3 | Group 4 |
| --- | --- | --- |
| `M0` | x, y | x, y, z, w |
| `M1` | x | x, y, z |
| `M2` | none | x, y, z |
| wind words | `ω0`, `ω2`, `ω3`, `θ` | `θ` |
| `N` | x, y | x, y, z |
| `W` | x, y, z | x, y, z |
| `Tg` | not read | x, y, z, w |
| `P` | x, y, z, w | x, y, z, w |

Lanes not listed are not read as numbers. They may hold arbitrary bit patterns (not necessarily
valid floats) and must not cause a refusal.

## Behaviour

### Group 3 (wind)

Per item:

- `f = M1.x · ω0`
- `A1 = M0.x · ω2`, `A2 = M0.y · ω3`
- five positive constants `KA1`, `KA2`, `KB1`, `KB2`, `KC3`, each between 0.1 and 2, fixed in the
  retail program and not stored in guest data (open question 1). `KA1` and `KB1` are the same
  constants as in group 0.

Per vertex:

1. `t = W.y + Σ`
2. `a = θ + t`
3. `b = θ + (KC3 + t)·(P.x + P.y + P.z)`
4. `wb = wave(KB1·b; f) + wave(KB2·b; f)` and `wa = wave(KA1·a; f) + wave(KA2·a; f)`, each in `[0, 2]`
5. `g = W.x · A1`
6. Output position:
   - `x' = P.x + wb · g · N.x`
   - `y' = P.y + wa · (1 - W.z) · A2`
   - `z' = P.z + wb · g · N.y`
   - `w' = P.w`

The y displacement does not involve the normal. The z displacement uses the normal's y component.
Both are observed behaviour and must be reproduced.

Group 3 produces no tangent bank.

### Group 4 (ripple)

Helper: `ripple(φ) = 2·S(tri(φ)) - 1`, in `[-1, 1]`, continuous, slope at most 6.

Per item:

- `o = frac(Σ)`
- `r1 = 1 / M1.z` and `r2 = 1 / M2.z`

Per vertex, with tangent `Tg = (t, h)` where `t` is the xyz part and `h` the w lane:

1. Bitangent `bt = h · (N × t)`, the cross product of the normal's and tangent's xyz parts, scaled
   by the handedness lane.
2. Two directions: `d1 = M0.x·t + M0.y·bt` and `d2 = M0.z·t + M0.w·bt`.
3. For `k = 1, 2`: phase `φk = rk·(P.xyz · dk) + o - Mk.y·θ + W.z + 1/2`, using `M1` for `k = 1`
   and `M2` for `k = 2`.
4. `Dk = ripple(φk) · Mk.x · wk`, where `w1 = W.x` and `w2 = W.y`.
5. `D = D1 + D2`.
6. Output position `(P.x + D·N.x, P.y + D·N.y, P.z + D·N.z, P.w)`.

### Pass-through banks

The output normal, weight and (group 4) tangent banks are bit-exact copies of the inputs, all four
lanes, including lanes never read as numbers.

### Output w lane

The position's w lane equals the input w lane. An implementation may turn a negative zero or a
subnormal w into +0.

## Edge cases and errors

All-or-nothing. Checks that do not need arithmetic come first, in this order:

1. group not 3 or 4 → group;
2. normal bank length ≠ `V` → population (semantic 2);
3. weight bank length ≠ `V` → population (semantic 9);
4. group 4 without tangents → missing tangent;
5. tangent bank present with length ≠ `V` → population (semantic 3);
6. output would exceed the implementation's resource budget → budget;
7. any lane listed as used in the table above is NaN or infinite, or any of the three origin lanes
   is → non-finite, reporting the word.

Then arithmetic refusals:

- group 4: `r1` or `r2` not finite in binary32 (`M1.z` or `M2.z` is zero, or so small that the
  reciprocal overflows) → non-finite;
- any output position lane not finite in binary32 → non-finite.

A refusal that happens after work was deferred to a worker must still be reported for this item;
deferral must not change the result or the refusal.

## Tolerance

Implementations use ordinary native arithmetic. The reference is the formulas above in exact real
arithmetic.

- **Group 3 positions.** Let `Mp = |P.x| + |P.y| + |P.z|`, phase bound
  `Φ = |θ| + |W.y| + |Σ| + (2 + |W.y| + |Σ|)·Mp`, and `δ = 2^-20·(1 + 2·Φ)`. Per lane the allowed
  absolute error is
  - x: `2^-18·|P.x| + 2·|g·N.x|·(2^-18 + 6·|f|·δ)`
  - y: `2^-18·|P.y| + 2·|(1 - W.z)·A2|·(2^-18 + 6·|f|·δ)`
  - z: `2^-18·|P.z| + 2·|g·N.y|·(2^-18 + 6·|f|·δ)`
- **Group 4 positions.** Let `ak`, `bk` be the two `M0` lanes that build `dk` (x and y for
  `k = 1`, z and w for `k = 2`), `nmax = max(|N.x|, |N.y|, |N.z|)`, `tmax = max(|t.x|, |t.y|, |t.z|)`,
  `Gk = (|ak| + 2·|bk|·|h|·nmax)·tmax`, `Φk = |rk|·Mp·Gk + |Mk.y·θ| + |W.z| + 2` and
  `δk = 2^-20·Φk`. Allowed absolute error per lane:
  `2^-18·|P_lane| + |N_lane|·Σk |Mk.x·wk|·(2^-18 + 6·δk)`.
- **w lane, pass-through banks.** Exact (data movement), except the w-lane zero/subnormal freedom
  above.

*Justification:* the same as for group 0. `2^-18` of the vertex's coordinate size is far below one
pixel, and the phase allowance amounts to an invisible timing jitter in an animated sway or
ripple while letting an implementation evaluate phases in binary32.

### Consequences of non-bit-exact output

The list in [group-zero-deformation.md](group-zero-deformation.md#consequences-of-non-bit-exact-output)
applies unchanged: depth EQUAL against a pre-pass computed by a different evaluation, shadow-pass
consistency, the determinism requirement (inline and deferred preparation must agree bit-for-bit),
differential tests with tolerances, and no caching on produced vertex values. In addition, the
pass-through banks must stay bit-exact, because other passes may decode the same banks
independently and must see identical normals and tangents.

## Synthetic test vectors

Unspecified inputs are zero. `P.w = 1` in every vector.

### NI-1: group 3 with phases that do not depend on the constants

`θ = 0`, `T = (0, 0, 0)`, `W = (1, 0, 0.5, ·)`, so `t = 0` and `a = 0`. `P = (1, -1, 0, 1)`, so
`P.x + P.y + P.z = 0` and `b = 0`. `M0 = (0.5, 1, ·, ·)`, `ω2 = 1`, `ω3 = 0.25`, so `A1 = 0.5`,
`A2 = 0.25`, `g = 0.5`. `N = (0.5, -1, 9)`. `ω0 = 1`.

| `M1.x` | `f` | `wa`, `wb` | expected position |
| --- | --- | --- | --- |
| 0.5 | 0.5 | 2, 2 | (1.5, -0.75, -1, 1) |
| 1 | 1 | 0, 0 | (1, -1, 0, 1) |

The first row checks that z uses `N.y` (here -1) and not `N.z` (here 9).

### NI-2: group 3 closed form

`θ = 0`, `T = (0, 0, 0)`, `W = (1, 0, 0, ·)`, `P = (1, 0, 0, 1)`, `N = (1, 0, 0)`, `A1 = A2 = 1`,
`f` arbitrary. Then `a = 0`, `b = KC3`, and the expected position is `(1 + wb, wa, 0, 1)` with
`wb = wave(KB1·KC3; f) + wave(KB2·KC3; f)` and `wa = 2·wave(0; f)` (so `wa = 2` for `f = 0.5`).
Use it once the constants are ruled on.

### NI-3: group 4, simple ripple

`Tg = (1, 0, 0, 1)`, `N = (0, 0, 1)`, so `bt = (0, 1, 0)`. `M0 = (1, 0, 0, 1)`, so `d1 = (1, 0, 0)`,
`d2 = (0, 1, 0)`. `M1 = (0.5, 0, 2, ·)` (`r1 = 0.5`), `M2 = (0.25, 1, 4, ·)` (`r2 = 0.25`).
`θ = 0.25`, `T = (0, 0, 0)`, `W = (1, 1, 0, ·)`, `P = (0.5, 1, 0, 1)`.

- `φ1 = 0.5·0.5 + 0 - 0 + 0 + 0.5 = 0.75`, `ripple = 0`, `D1 = 0`.
- `φ2 = 0.25·1 + 0 - 0.25 + 0 + 0.5 = 0.5`, `ripple = -1`, `D2 = -0.25`.

Expected position `(0.5, 1, -0.25, 1)`.

With the handedness flipped (`Tg.w = -1`): `bt = (0, -1, 0)`, `φ2 = 0`, `ripple = 1`, `D2 = 0.25`,
expected position `(0.5, 1, 0.25, 1)`.

### NI-4: group 4, general case

`Tg = (0, 1, 0, 1)`, `N = (1, 0, 0)`, so `bt = (0, 0, 1)`. `M0 = (2, 1, 0, 3)`,
`M1 = (0.5, 0.25, 4, ·)`, `M2 = (-1, 0, 0.5, ·)`, `θ = 1.5`, `T = (0.3, 0.5, 0.45)` (so `o = 0.25`),
`W = (0.6, 0.2, 0.1, ·)`, `P = (2, 0.125, -0.25, 1)`.

- `d1 = (0, 2, 1)`, `d2 = (0, 0, 3)`
- `φ1 = 0.475`, `ripple(φ1) = -0.9855`, `D1 = -0.29565`
- `φ2 = -0.65`, `ripple(φ2) = -0.568`, `D2 = 0.1136`
- expected position `(1.81795, 0.125, -0.25, 1)`, allowed error about `1e-5` on x (the inputs
  are the binary32 values nearest the decimals)

### NI-5: ripple helper

| `φ` | `ripple(φ)` |
| --- | --- |
| 0 | 1 |
| 0.25 | 0 |
| 0.5 | -1 |
| 0.75 | 0 |
| -0.1 | 0.792 |
| 2.3 | -0.296 |

### NI-6: refusals and unused lanes

- Group 11 → group.
- Group 4 without tangents → missing tangent.
- Group 3 with an empty normal bank → population (semantic 2).
- `θ` = NaN → non-finite (both groups).
- Group 4 with `M1.z = 0` → non-finite.
- Group 3 with `N.z`, `N.w`, `W.w`, `M0.z`, `M0.w`, `M1.y..w`, all of `M2` and `ω1` set to
  `0xFFFFFFFF`: the output position equals the result without those changes, and the normal and
  weight banks are returned with those words unchanged.
- Group 4 with `M1.w`, `M2.w`, `ω0..ω3` set to `0xFFFFFFFF`: same as above.

## Open questions

1. **Group-3 constants `KA1`, `KA2`, `KB1`, `KB2`, `KC3`.** Fixed values of the retail program with
   no guest-data location. They are decimal tuning values, so this specification does not state
   them. A user ruling is needed on whether they may be published here. `KC3` is within one
   binary32 ULP of group 0's `KC`, so one shared value is within tolerance.
2. **Optional per-vertex post-processing and palette inputs.** Items with a non-zero optional count
   or a non-zero palette length are refused. Their behaviour has not been specified.
3. **Coupling.** The attribute bank decoders (including the spherical normal codec in
   [geometry-normals.md](geometry-normals.md)) and the primary-before-secondary declaration rule are
   shared with the other geometry specifications.
