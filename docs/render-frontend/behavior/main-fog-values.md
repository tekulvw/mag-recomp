# Main fog constants and depth bounds

## Scope

The values that drive the group-13 fog producer ([main-fog.md](main-fog.md)): which fog fragment
program variant is used, the fog vertex constant rows, the fog fragment constant rows, and the
depth-bounds range. Applies to retail v1.0.

Out of scope: how the game derives the seven "prepared rows" below from scene and environment data.
That derivation is still done by the game; see Open questions.

## Inputs

### Prepared rows

Seven `vec4` rows (four `u32` words each) that the game prepares for the fog pass in the same frame:

- `V0`, `V1`: the values the game uploads as fog vertex constant rows 0 and 1;
- `R0` … `R4`: the values the game places in fog fragment constant rows 0–4.

Two lanes have a defined use here:

- `tint = f32 R0.w`: the tint amount;
- `atm = f32 R2.w`: the atmosphere distance limit.

All other lanes are copied without interpretation.

### Fog parameter block

`B[0..15]`: sixteen `f32` words at `RENDERER+0x50`. Lanes used in arithmetic: `B[4]` (fog distance)
and `B[5]` (fog range). Lanes copied: `B[0]`, `B[1]`, `B[2]`, `B[3]`, `B[4]`, `B[9]`–`B[14]`.
`B[6]`–`B[8]` and `B[15]` are unused.

### Camera

The global at `TOC−0x223C` holds the address of a pointer variable; that variable holds the camera
object address (null is an error). Two `f32` fields are read: `cs = camera+0x60` (vertical extent
slope) and `ca = camera+0xA24` (aspect ratio). These are the same offsets as in the projection
block of [material-producers.md](material-producers.md).

### Constants

`Z0`, `Z1`, `PA`, `PB`, `PC`, `PD`, `ZH`, `FTOP` from [material-producers.md](material-producers.md).

## Outputs

- `tinted`: a boolean that selects the fragment program variant.
- Vertex constant rows 0 and 1.
- Fragment constant rows 0–6, plus row 7 when `tinted`.
- `bounds = [lower, FTOP]`: the depth-bounds range.

## Behaviour

```
tinted = tint > Z0

fragment rows 0..4 = R0..R4                         (copied)
fragment row 5     = (B[4], B[12], Z1 / B[5], B[13])
fragment row 6     = (B[0], B[1],  B[2],      B[14])
fragment row 7     = (B[9], B[10], B[11],     B[3])  only when tinted
vertex rows 0, 1   = V0, V1                          (copied)

d      = min(B[4], atm)                (B[4] when B[4] < atm, otherwise atm)
z      = d / sqrt(cs² · (ca² + Z1) + Z1)
lower  = ZH · ((PA / z + PB + PC) / PD) + ZH
bounds = [lower, FTOP]
```

Geometrically, with `Z1 = 1`, `z` is the view-axis depth of a point at Euclidean distance `d` along
the frustum's corner ray `(cs·ca, cs, 1)`. `lower` is that depth mapped into window depth by the
same map `D` the lighting producer uses ([main-lighting.md](main-lighting.md)). The fog draw
therefore affects only pixels whose stored depth is at least the depth of the fog start.

Words that are copied are copied bit for bit, including NaN payloads and signed zeros.

## Edge cases and errors

Decline (no fog draw) when any of these is not finite: `tint`, `atm`, `B[4]`, `B[5]`, `cs`, `ca`,
or any of the eight constants. No other lane is checked, because copied lanes are passed through.

Also decline when `Z1 / B[5]` is not finite (this covers `B[5] = 0`), when `lower` is not finite
(this covers `d = 0`, which makes `PA / z` infinite), or when `lower > FTOP`.

A negative `d` (a negative `B[4]` or `atm`) makes `z` negative. That is not an error; `lower` is
computed as written.

## Tolerance

Native arithmetic, any evaluation order, binary32 or wider.

| Output | Tolerance | Reason |
| --- | --- | --- |
| Fragment row 5 lane 2 (`Z1 / B[5]`) | within 1 ulp | a single quotient; it scales the fog ramp |
| `lower` | within `2^-22` absolute of the exact value | at most 4 steps of a 24-bit depth buffer at the fog start boundary |
| `tinted` | exact | comparison of copied inputs |
| All copied words and `FTOP` | bit-exact | data movement |

**Consequences of non-bit-exact output.**

- The `lower > FTOP` decline uses the computed `lower`. When the exact value is within tolerance of
  `FTOP`, either outcome is acceptable. Test vectors avoid that band.
- The depth-bounds range is part of the fixed-function state of the fog draw. If an implementation
  keys pipeline or pass caches on the bounds, small differences produce distinct keys, not wrong
  hits.
- Differential tests against the original must compare `lower` and row 5 lane 2 with these
  tolerances.

## Synthetic test vectors

Constants for all vectors: `Z0 = 0`, `Z1 = 1`, `ZH = 0.5`, `PA = −2`, `PB = 1`, `PC = 0`, `PD = 1`,
`FTOP = 1`. Unless stated, `B[k] = 1` for every lane, then the overrides listed.

| # | `B[4]` | `B[5]` | `atm` | `cs` | `ca` | `tint` | `d` | `z` | `lower` | row 5 lane 2 | `tinted` |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| F1 | 4 | 2 | 2 | 0 | 0 | 0 | 2 | 2 | 0.5 (`0x3F000000`) | 0.5 | false |
| F2 | 4 | 2 | 2.5 | 0.75 | 0 | 0.25 | 2.5 | 2 | 0.5 | 0.5 | true |
| F3 | 3 | 8 | 10 | 0.5 | 0.75 | 0 | 3 | 2.5439949… | 0.606917… (`0x3F1B5EF1` ±tolerance) | 0.125 | false |

Further vectors:

- **F4 (row layout).** F2 with `B[9] = 0.75`. Fragment rows: eight rows. Row 7 =
  `(0.75, B[10], B[11], B[3])`. Row 6 = `(B[0], B[1], B[2], B[14])`. Row 5 =
  `(4, B[12], 0.5, B[13])`.
- **F5 (copy of NaN).** F1 with `V0.x` = the bits of a quiet NaN and `B[15] = NaN`. Expected:
  success; vertex row 0 lane 0 carries the same NaN bits.
- **F6 (zero range).** F1 with `B[5] = 0`. Expected: decline.
- **F7 (zero distance).** F1 with `atm = 0`. Expected: decline (the bound is not finite).
- **F8 (non-finite camera).** F1 with `cs = NaN`. Expected: decline.
- **F9 (bound above top).** F1 with `FTOP = 0.25`. `lower = 0.5 > 0.25`. Expected: decline.
- **F10 (minimum rule).** `B[4] = 2`, `atm = 2`: `d = 2` (the `atm` branch; same value).
  `B[4] = 1.5`, `atm = 2`: `d = 1.5`.

## Open questions

1. **Prepared rows.** `V0`, `V1` and `R0`–`R4` are still produced by the game's own environment and
   atmosphere computations, and observed when the game finishes preparing them for the fog pass.
   Specifying that derivation (light direction, tint and atmosphere data) from scene data is
   outstanding. Until then, a replacement needs those seven rows supplied by the game as data, or
   recovered from the fog pass's own constant uploads.
2. The camera reached through `TOC−0x223C` is assumed to be the active scene camera. It has not been
   shown to be the same object as the renderer's projection block.
