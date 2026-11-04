# Group-11 palette skinning

## Scope

Main-pass preparation of vertices for skinned items of material group 11 on the retail v1.0
build. Each vertex is blended from up to four palette matrices. Positions come out in the frame
the palette maps to. Normals and tangents are blended the same way without translation, then turned
by the object's rotation-scale matrix.

Out of scope: animation (who writes the palette), the vertex program, the group-11 depth-axis
adjustment, culling, and other palette formats or options (refused, see Admission).

## Admission

| Condition | Data |
| --- | --- |
| Group 11 | mesh record `+0xBC` (`u16`) is 11 and equals the high nibble of draw-batch byte `+0x00` |
| Not static | mesh record `u32 +0x9C` bit `0x1000` is clear |
| Skinned source | object transform byte `+0x6F` is not 1 and primary material `u32 +0x58` bit `0x20` is set; otherwise refuse "palette inputs" |
| Pass present | the item's pass record address is non-zero |
| No optional per-vertex post-processing | as for groups 3 and 4: `draw-batch byte +0x18 >> 2` is 0, or draw-batch byte `+0x1B` bit 0 is set |
| Geometry mode | `(pass +0x230 OR (material +0x58 bit 0 ? 0x20 : 0)) AND NOT 0x2` is 8. Bit 1 is ignored, so a raw mode of 8 or 10 qualifies. |
| Worker control | `min(pass +0x22C, material +0x58 bit 0 ? 1 : 2)` equals 2 |
| Standard skinning configuration | the partition's configuration stream (stream 19) has at least 16 bytes, byte 0 is `0x83`, byte 6 is 17, byte 7 is 0, and bytes 12 to 15 are `0xFF` |
| Palette lengths fit 16 bits | high 16 bits of partition `u32 +0x48` and `u32 +0x50` are zero; otherwise refuse "unsupported control" for that slot |
| At least one matrix | see "Palette" below |

Byte 6 = 17 selects "four (weight, index) byte pairs per vertex"; byte 7 = 0 selects packed
3×4 single-precision matrices and the standard direction transform. Other values are other
skinning formats that have not been specified.

## Inputs

### Palette

- Palette base `PB`: if draw-batch byte `+0x1B` bit 0 is clear, `PB = (draw-batch u32 +0x14) AND
  NOT 0xF`; otherwise `PB = 0` (open question 2).
- Two segments: segment 0 at `PB + (partition u16 +0x14)` with `L0 = partition u32 +0x48` bytes;
  segment 1 at `PB + (partition u16 +0x16)` with `L1 = partition u32 +0x50` bytes. A zero-length
  segment is not read.
- The two segments are concatenated, segment 0 first. The matrix count is
  `J = floor((L0 + L1) / 48)`. Trailing bytes beyond `48·J` are ignored. A matrix may straddle the
  join between the segments.
- Each 48-byte matrix is twelve `f32` in row order: row `r` (r = 0, 1, 2) is
  `(m[r][0], m[r][1], m[r][2], m[r][3])`, where columns 0 to 2 are the linear part and column 3 is
  the translation.

### Influences

The partition's stream 5 holds `8·V` bytes, one 8-byte record per vertex: four pairs
`(weight byte b_k, joint index j_k)` for `k = 0..3`. Bytes after `8·V` are not read.

### Vertex banks and object

| Input | Meaning |
| --- | --- |
| positions (semantic 1) | `p`, four lanes |
| normals (semantic 2) | `n`, four lanes |
| tangents (semantic 3) | `t`, four lanes |
| object transform `+0x00` `f32` ×4 | object quaternion `(x, y, z, w)`, used as stored (not normalised) |
| object transform `+0x10..+0x18` `f32` ×3 | object translation: not used, but must be finite |
| object transform `+0x1C` `f32` | object scale `σ` |

## Behaviour

Per vertex:

1. Weights `w_k = b_k / 255`. They are not renormalised: if the four bytes do not sum to 255 the
   blend is scaled accordingly.
2. Blended matrix `B = Σ_k w_k · m_{j_k}` (all four lanes, including lanes with weight 0).
3. Position: `p'.xyz = B_lin · p.xyz + B_t`, `p'.w = p.w`.
4. Normal and tangent first stage: `n1.xyz = B_lin · n.xyz`, `n1.w = n.w`; the same for `t`.
5. Object direction matrix `O = σ · R̃`, where `R̃` is built from the stored quaternion with the
   usual unit-quaternion formula, applied literally even if the quaternion is not unit length:

   ```
   | 1-2(y²+z²)   2(xy-zw)     2(xz+yw)   |
   | 2(xy+zw)     1-2(x²+z²)   2(yz-xw)   |
   | 2(xz-yw)     2(yz+xw)     1-2(x²+y²) |
   ```

6. Final normal `n'.xyz = O · n1.xyz`, `n'.w = n1.w = n.w`; the same for the tangent.

Positions are not affected by the object quaternion or scale. The output has no weight bank. A
later consumer that asks for semantic 9 on this item must refuse rather than invent weights.

Outputs are three banks of `V` four-lane rows: positions, normals and tangents, in vertex order.

## Edge cases and errors

All-or-nothing. In this order:

1. normal bank length ≠ `V` → population (semantic 2);
2. tangent bank length ≠ `V` → population (semantic 3);
3. influence stream shorter than `8·V` bytes → source extent;
4. `J = 0` → empty palette ("palette inputs");
5. output exceeds the implementation's resource budget → budget;
6. any lane of any position, normal, tangent, palette matrix, or the eight object-transform words
   `+0x00..+0x1C` is NaN or infinite → non-finite, reporting the word;
7. any joint index `j_k ≥ J`, **including lanes whose weight byte is 0** → joint, reporting
   vertex, lane and index;
8. any output lane not finite in binary32 → non-finite.

## Tolerance

Implementations use ordinary native arithmetic. The reference is exact real arithmetic.

Write `|m|` for a matrix with every entry replaced by its absolute value, and `|v|` likewise for a
vector.

- **Positions, xyz.** Allowed absolute error per lane `r`:
  `2^-18 · Σ_k w_k · (Σ_c |m_{j_k}[r][c]|·|p_c| + |m_{j_k}[r][3]|)`.
- **Normals and tangents, xyz.** Let `u = Σ_k w_k·|m_{j_k}|_lin·|n.xyz|` (the size of the first
  stage). Allowed absolute error per lane `r`: `2^-18 · (|O|·u)_r`.
- **w lanes.** Exact copies.

*Justification:* `2^-18` of the size of the contributing terms is below 4 parts per million, far
below one pixel for positions and far below any visible shading change for directions. It allows
any evaluation order and correctly rounded `1/255`.

### Consequences of non-bit-exact output

The list in [group-zero-deformation.md](group-zero-deformation.md#consequences-of-non-bit-exact-output)
applies. Skinned characters are drawn in Main, Depth and Shadow views. Any pass whose depth must
coincide with Main (an EQUAL test) must use the same produced positions. Inline and deferred
preparation must agree bit-for-bit.

## Synthetic test vectors

Influence records are written as four `(b, j)` pairs. Unless stated: one matrix, the object
quaternion is `(0, 0, 0, 1)`, `σ = 1`, translation `(0, 0, 0)`.

### SK-1: single rotation-translation matrix

Matrix 0 rows `(0, -1, 0, 1)`, `(1, 0, 0, 2)`, `(0, 0, 1, 3)` (90° about z, then translate
`(1, 2, 3)`). Influence `(255, 0) (0, 0) (0, 0) (0, 0)`.

| Input | Expected |
| --- | --- |
| `p = (1, 0, 0, 1)` | `(1, 3, 3, 1)` |
| `n = (1, 0, 0, 0)` | `(0, 1, 0, 0)` |
| `t = (0, 1, 0, -1)` | `(-1, 0, 0, -1)` |

### SK-2: two-matrix blend

Matrix 0 is the identity with zero translation. Matrix 1 is twice the identity with translation
`(4, 0, 0)`. Influence `(51, 0) (204, 1) (0, 0) (0, 0)`, so `w = (0.2, 0.8, 0, 0)`,
`B_lin = 1.8·I`, `B_t = (3.2, 0, 0)`.

| Input | Expected |
| --- | --- |
| `p = (1, 1, 1, 1)` | `(5, 1.8, 1.8, 1)` |
| `n = (0, 0, 1, 0)` | `(0, 0, 1.8, 0)` (not renormalised) |

### SK-3: weights are not renormalised

Matrix 0 is the identity with translation `(255, 0, 0)`. Influence `(100, 0) (0, 0) (0, 0) (0, 0)`.
`p = (1, 0, 0, 1)` → `(100.392157, 0, 0, 1)` (that is, `(100/255)·256`).

### SK-4: object direction matrix with a non-unit quaternion

Identity matrix with zero translation, influence `(255, 0)` and zeros. Object quaternion
`(0, 0, 1, 1)` (length √2, used as stored), `σ = 1`, so `R̃` has rows `(-1, -2, 0)`, `(2, -1, 0)`,
`(0, 0, 1)`.

| Input | Expected |
| --- | --- |
| `p = (1, 2, 3, 1)` | `(1, 2, 3, 1)` (positions ignore the object matrix) |
| `n = (1, 0, 0, 0)` | `(-1, 2, 0, 0)` |
| `t = (0, 1, 0, 1)` | `(-2, -1, 0, 1)` |

### SK-5: unit quaternion and scale

As SK-4 but quaternion `(0, 0, √½, √½)` (90° about z) and `σ = 3`:
`n = (1, 0, 0, 0)` → `(0, 3, 0, 0)`; `t = (0, 1, 0, -1)` → `(-3, 0, 0, -1)`.

### SK-6: palette straddling the segment join

`L0 = 40`, `L1 = 60`, so the joined stream is 100 bytes and `J = floor(100/48) = 2`. Matrix 0 is
segment-0 bytes 0..39 followed by segment-1 bytes 0..7. Matrix 1 is segment-1 bytes 8..55.
Segment-1 bytes 56..59 are ignored. Write the identity matrix with translation `(1, 2, 3)` as
matrix 1 and use influence `(255, 1)` and zeros: `p = (0, 0, 0, 1)` → `(1, 2, 3, 1)`.

### SK-7: refusals

- Influence `(255, 0) (0, 1) (0, 0) (0, 0)` with `J = 1` → joint (vertex 0, lane 1, index 1), even
  though lane 1's weight is 0.
- Empty tangent bank → population (semantic 3).
- Influence stream of `8·V - 1` bytes → source extent.
- Object quaternion x = NaN → non-finite.
- Object translation x = infinity → non-finite, even though translation is not used.

## Open questions

1. **Other skinning configurations.** Configuration-stream values other than those in Admission
   (other influence layouts, other palette formats, the alternative direction transform) are
   refused. They have not been specified.
2. **Palette base when draw-batch byte `+0x1B` bit 0 is set.** The current reference uses base 0,
   which makes the two partition offsets absolute guest addresses. Whether the original program
   does the same, or takes the palette from somewhere else in this case, is unconfirmed. An
   implementation may refuse this case until it is confirmed.
3. **Weight constant.** The weight scale is stated as exactly `1/255`. The retail program's value
   differs from the correctly rounded `1/255` by about one binary32 ULP. That is inside the
   tolerance and needs no ruling unless bit-exact output is ever required.
4. **Coupling.** The stream descriptor convention (where stream 5 and stream 19 are found in the
   partition record) and the attribute bank decoders are shared with the geometry stream
   specifications.
