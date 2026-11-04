# Texture unit decoding and native sampler translation

## Scope

How the frontend turns the RSX fragment texture-unit registers of one unit into decoded sampling
controls, a native sampler, an image-view component remap and a depth-comparison request. The
rules are RSX hardware interface facts plus the frontend's own policy for what it can and cannot
translate. They are used by [material-setup.md](material-setup.md) (register tracking) and
[scene-plan.md](scene-plan.md) (packet texture binding). Applies to retail v1.0; nothing here is
build-specific.

Out of scope: decoding texture pixel data, vertex texture units, and the order in which the game
writes these registers.

## Inputs

A *texture unit record* for fragment unit `u` (0..15) consists of:

| Field | RSX method | Meaning |
| --- | --- | --- |
| `w[0]` | `0x1a00 + 32u` | texture offset |
| `w[1]` | `0x1a04 + 32u` | format word |
| `w[2]` | `0x1a08 + 32u` | address word |
| `w[3]` | `0x1a0c + 32u` | control 0 |
| `w[4]` | `0x1a10 + 32u` | control 1 (component remap) |
| `w[5]` | `0x1a14 + 32u` | filter word |
| `w[6]` | `0x1a18 + 32u` | image rectangle: width in bits 31..16, height in bits 15..0 |
| `w[7]` | `0x1840 + 4u` | control 3 (pitch and depth) |
| `border` | `0x1a1c + 32u` | border colour, ARGB8888, optional |
| `texcoord_control` | `0x0b00 + 4u` | optional, carried only |
| `vertex_lod_control` | `0x1d00 + 4u` | optional, carried only |

The eight words `w[0..7]` are the *compact descriptor*. A unit is *known* only when all eight are
known; `border`, `texcoord_control` and `vertex_lod_control` are each independently known or
unknown. Some guest material records carry a 48-byte descriptor of twelve words; it maps to the
record as `w[0..6] = d[0..6]`, `border = d[7]`, `w[7] = d[8]`, `texcoord_control = d[9]`,
`vertex_lod_control = d[10]` (`d[11]` is not used). A 32-byte descriptor of eight words is the
compact descriptor and leaves the three optional fields unchanged.

## Outputs

`decode(record)` returns one of:

- `MissingState` error, if the compact descriptor is unknown;
- `Disabled(w)`, if bit 31 of `w[3]` is clear;
- `Enabled(controls)`, with the fields below;
- a decode error (`Wrap(code)`, `MinFilter(code)`, `MagFilter(code)`, `Remap(op)`).

The checks are made in this order and the first failure is reported: unknown compact descriptor
(`MissingState`); unit disabled (`Disabled`, which is not an error and makes no further check); the
wrap codes of the observed axes in the order s, t, r; the minification code; the magnification code;
then the remap operations of the output channels in the order A, R, G, B. For example, a unit with
an invalid s wrap code and an invalid magnification code reports `Wrap`.

`controls` fields:

| Field | Value |
| --- | --- |
| `format` | `(w[1] >> 8) & 0xff` |
| `base` | `format & 0x9f` (format without its linear-layout bit `0x20` and unnormalised bit `0x40`) |
| `dimension` | `(w[1] >> 4) & 0xf` |
| `cube` | bit 2 of `w[1]` |
| `mip_count` | `w[1] >> 16` |
| `unnormalized` | bit 6 of `format` (`format & 0x40`) |
| `wrap[s,t,r]` | see "Addressing" |
| `min`, `mip`, `mag` | see "Filtering" |
| `min_lod_q8` | `(w[3] >> 19) & 0xfff` (unsigned, 8 fractional bits) |
| `max_lod_q8` | `(w[3] >> 7) & 0xfff` |
| `anisotropy_code` | `(w[3] >> 4) & 7` |
| `alpha_kill` | bit 2 of `w[3]` |
| `lod_bias_q8` | bits 12..0 of `w[5]` as a 13-bit two's-complement integer (8 fractional bits) |
| `compare_function` | `(w[2] >> 28) & 0xf` |
| `remap[r,g,b,a]` | see "Component remap" |
| `gamma_mask`, `expand_mask` | see "Gamma and sign expansion" |
| `words` | `w`, unchanged |
| `border` | `border`, unchanged (may be unknown) |

## Behaviour

### Addressing

A wrap code `c` (4 bits) means: 1 repeat, 2 mirror, 3 clamp to edge, 4 clamp to border, 5 clamp
(half-border), 6 mirror once to edge, 7 mirror once to border, 8 mirror once with clamp. Any other
code is a `Wrap(c)` error, but only for an *observed* axis.

- `s` uses bits 3..0 of `w[2]` and is always observed.
- `t` uses bits 11..8. It is *unobserved* when `dimension == 1` and the unit is not a cube.
- `r` uses bits 19..16. It is unobserved when `dimension < 3` and the unit is not a cube.

An unobserved axis keeps its raw code but never fails and never observes the border.
Codes 4, 5, 7 and 8 *observe the border*.

### Filtering

Minification code `(w[5] >> 16) & 7`: 1 nearest/no mip, 2 linear/no mip, 3 nearest/nearest mip,
4 linear/nearest mip, 5 nearest/linear mip, 6 linear/linear mip; others are `MinFilter(code)`.
Magnification code `(w[5] >> 24) & 7`: 1 nearest, 2 linear; others are `MagFilter(code)`.

### Component remap

Let `ctl = w[4]`. For `base` in {`0x95`, `0x98`, `0x99`} (two- and four-component 16/32-bit
formats) the low byte of `ctl` is first replaced, leaving the rest of `ctl` unchanged:

| Low byte | Replacement when `ctl >> 16 != 0` | Replacement otherwise |
| --- | --- | --- |
| `0xe4` | `0x56` | `0x66` |
| `0x4e` | `0xa9` | `0x99` |
| `0xee` | `0xaa` | `0xaa` |
| `0x44` | `0x55` | `0x55` |
| other | unchanged | unchanged |

The *input lanes* are, by `base`: `0x81` and `0x94` → (one, red, red, red); `0x8b` → (green, red,
green, red); every other format → (alpha, red, green, blue).

For each output channel `k` in A, R, G, B order (`k` = 0..3), let `sel = (ctl >> 2k) & 3` and
`op = (ctl >> (8 + 2k)) & 3`. The channel's source is zero when `op == 0`, one when `op == 1`, input
lane `sel` when `op == 2`; `op == 3` is a `Remap(3)` error. The result is stored in RGBA order:
output A goes to `remap[a]`, R to `remap[r]`, G to `remap[g]`, B to `remap[b]`. Zero `ctl` is not an
identity swizzle; it really selects zero for every channel.

### Gamma and sign expansion

- `gamma_mask` starts as `(w[2] >> 20) & 0xf` (bit 0 R, 1 G, 2 B, 3 A in RGBA order) and is forced
  to zero unless `base` is in `0x81..=0x88`, `0x8b`, `0x8d..=0x8f`, `0x97`, `0x9d` or `0x9e`.
- A channel is *physical* when its resolved source is a colour component: red, green, blue or
  alpha. It is not physical when its operation selects constant zero or constant one, or when it
  selects an input lane that is itself the constant one (lane 0 of formats `0x81` and `0x94`). A physical
  channel is *signed* when bit `28 + sel` of `w[5]` is set (`sel` being that channel's lane
  selector). The gamma bit of every physical signed channel is cleared.
- `expand_mask` bit for channel `c` is set when `base` is in `0x81..=0x88`, `0x8b`, `0x8d..=0x95`,
  `0x97`, `0x98`, `0x9d` or `0x9e`, bit 12 of `w[2]` is set, and the channel is physical and not
  signed. If any expand bit is set and `base` is `0x94` or `0x95`, bit 4 (`16`) is also set.

### Native sampler

`native_sampler(controls)` produces a packet sampler (`mag_render_abi::packet::Sampler`):

| Field | Value |
| --- | --- |
| `known_fields` | `63` |
| `min_filter` | 1 if minification is linear, else 0 |
| `mag_filter` | 1 if magnification is linear, else 0 |
| `mip_filter` | 0 none, 1 nearest, 2 linear |
| `wrap_s/t/r` | unobserved or clamp to edge → 0; repeat → 1; mirror → 2; clamp to border → 3 |
| `comparison_enabled`, `comparison` | 0 (depth comparison is performed in the shader) |
| `lod_min` | `min_lod_q8 / 256` |
| `lod_max` | `max_lod_q8 / 256` |
| `lod_bias` | `lod_bias_q8 / 256` |
| `max_anisotropy` | `[1, 2, 4, 6, 8, 10, 12, 16][anisotropy_code]` |
| `border` | `[R, G, B, A] / 255` from the ARGB border word, or all zero when no axis observes the border |

Refusals:

- A wrap of clamp (5) or any mirror-once mode (6, 7, 8) on an *observed* axis is refused as
  unsupported addressing. An unobserved axis is never refused, whatever its raw code (including
  5–8 and invalid codes), and is encoded as clamp to edge (0).
- If any axis observes the border and the border colour is unknown, the result is a `MissingBorder`
  error. No black border is invented. When no axis observes the border, an unknown border is
  harmless.

### Image view remap

The packet view remap encodes each RGBA output as 0 zero, 1 one, 2 red, 3 green, 4 blue, 5 alpha.
The view uses `base_mip = 0`, `mip_count = w[1] >> 16`, `base_layer = 0` and `layer_count = 6` for
a cube or 1 otherwise.

### Depth comparison request

`depth_compare(controls)` decides whether the shader must compare a fetched depth against the
coordinate's third component (the RSX behaviour for integer depth formats with a comparison
function). Let `base` and `f = compare_function` be as above.

- If `base` is not in `0x90..=0x93`, or `f` is 0 (never) or 7 (always): no comparison.
- Otherwise the request is *qualified* when `base` is `0x90` (depth24/stencil8) or `0x92`
  (depth16), `f < 7`, `dimension == 2`, the unit is not a cube, `mip_count <= 1` and minification
  and magnification are both linear or both nearest. A qualified request yields
  `{enabled: 1, function: f, linear: 1 if minification is linear else 0, unnormalized}`.
- Any other combination (float depth formats, cube or non-2D units, mip chains, mixed
  linearity, `f >= 8`) yields no comparison and must be reported as a visible diagnostic, at most
  once per distinct (`base`, `f`, `dimension`, `cube`, `mip_count`, min linear, mag linear)
  signature and at most 16 times in total.

## Edge cases and errors

- `alpha_kill` and `dimension != 2` are decoded faithfully; refusing them is the planner's
  decision (see [scene-plan.md](scene-plan.md)).
- `Disabled` units keep their words so a later enable can be tracked; sampling a disabled unit is a
  planner refusal.
- Anisotropy codes are three bits, so the table lookup cannot fail for decoded values; a code
  outside 0..7 supplied by another path must be refused, never mapped to 1x.

## Tolerance

All decoded fields are integers and bit fields copied from guest words. The sampler floats are
quotients of integers:

| Output | Tolerance |
| --- | --- |
| `lod_min`, `lod_max`, `lod_bias` | at most 0.5 ULP of the exact quotient. The quotient of a 13-bit integer by 256 is representable in binary32, so ordinary division gives the exact value. |
| `max_anisotropy` | exact small integer |
| `border[i]` | at most 1 ULP from `n / 255`; functionally `round(border[i] * 255) == n` must hold for every channel. |

### Consequences of non-bit-exact output

The packet de-duplicates samplers by the bit patterns of all sampler fields, including `lod_*`,
`max_anisotropy` and `border`. Within one implementation the same input always gives the same bits,
so de-duplication is unaffected. Comparisons of sampler tables produced by different
implementations (differential tests, plan-reuse caches keyed across implementations) must compare
border colours within the stated tolerance rather than by bits.

## Synthetic test vectors

Common words unless stated: `w[0] = 0x00010000`, `w[4] = 0x0000aae4` (identity remap),
`w[6] = 0x01000080` (256 × 128), `w[7] = 0x00000400`, border unknown.

1. Enabled linear-mip A8R8G8B8 unit.
   `w[1] = 0x00018521` (1 mip, format `0x85`, 2-D, location 1), `w[2] = 0x00000301` (s repeat,
   t edge, r code 0), `w[3] = 0x80068030` (enabled, min LOD 0, max LOD q8 `0xd00`, anisotropy
   code 3), `w[5] = 0x02061f80` (mag linear, min linear/linear mip, bias `-128`).
   Decode: `dimension 2`, not cube, `wrap = [repeat, edge, unobserved(0)]`, `min linear`,
   `mip linear`, `mag linear`, `min_lod_q8 0`, `max_lod_q8 3328`, `lod_bias_q8 -128`,
   `anisotropy_code 3`, `remap = [red, green, blue, alpha]`, `gamma_mask 0`, `expand_mask 0`.
   Native sampler: filters (1, 1, 2), wraps (1, 0, 0), `lod_min 0.0`, `lod_max 13.0`,
   `lod_bias -0.5`, `max_anisotropy 6.0`, `border [0, 0, 0, 0]`.
2. Same as 1 with `w[3] = 0x00068030`: `Disabled(w)`.
3. Same as 1 with `w[2] = 0x00000304` (s border) and border unknown: `MissingBorder`. With border
   `0x80ff4000`: `border = [1.0, 0.2509804, 0.0, 0.5019608]` (bits `3f800000 3e808081 00000000
   3f008081` for ordinary division).
4. Same as 1 with `w[2] = 0x00000305` (s clamp): unsupported addressing. Same as 1 with
   `w[2] = 0x00060301` (r mirror once to edge on a 2-D unit, so r is unobserved): accepted, and the
   native r wrap is 0. Same as 1 with `w[2] = 0x00000309` (invalid s code 9) and `w[5] = 0x07061f80`
   (invalid magnification 7): `Wrap(9)`, because wrap codes are checked first.
5. Same as 1 with `w[2] = 0x00000309`: `Wrap(9)` error. With `w[1] = 0x00018511` (1-D) and
   `w[2] = 0x00000901`: t is unobserved, so no error.
6. Bias sign: `w[5] = 0x02061000` gives `lod_bias_q8 -4096`, `lod_bias -16.0`; `w[5] =
   0x02060fff` gives `4095`, `15.99609375`.
7. Remap: `w[4] = 0x00000000` gives every channel zero. `w[4] = 0x0000a91b`: A op 1 → one;
   R op 2 sel 2 → green; G op 2 sel 1 → red; B op 2 sel 0 → alpha; result
   `remap = [green, red, alpha, one]`.
8. Gamma: format `0x85`, `w[2] = 0x00f00301`, identity remap, `w[5]` signs clear: `gamma_mask 15`.
   With `w[5] |= 0x20000000` (lane 1, red, signed): `gamma_mask 14`.
9. Expand: format `0x85`, `w[2] = 0x00001301`, identity remap, no signs: `expand_mask 15`.
   Format `0x94` (`w[1] = 0x00019421`), same `w[2]`, `w[4] = 0x0000aa00` (all lanes select
   lane 0 = one) → no physical channel → `expand_mask 0`.
10. Depth comparison: `w[1] = 0x00019221` (depth16, 1 mip, 2-D), `w[2] = 0x60000303` (function 6),
    `w[5] = 0x02020000` (both linear): `{1, 6, 1, 0}`. Same with `w[5] = 0x01010000`:
    `{1, 6, 0, 0}`. Same with `w[1] = 0x00029221` (2 mips): no comparison, one diagnostic.
    Same with `w[2] = 0x00000303` (function 0): no comparison, no diagnostic. Format `0x91`:
    no comparison, diagnostic.

## Open questions

- The remap low-byte replacement for formats `0x95`, `0x98` and `0x99` reproduces how these
  formats are known to behave on hardware. It is not yet confirmed by a hardware-independent
  reference for every combination of `ctl >> 16`; a differential test against the RSX reference
  emulation should confirm it.
- Clamp (half-border) and the mirror-once modes have no native translation. Whether any retail
  Main material uses them is unmeasured; they are refused visibly.
