# Packed spherical normal decoding (attribute codec 9)

## Scope

Decoding of one vertex attribute format used by retail v1.0 partition streams: a 3-byte packed
unit direction with a handedness sign, referred to here as codec 9. The decoder produces a four-lane
value: a unit vector in lanes 0 to 2 and ±1 in lane 3. It is used for normals and tangents in the
primary and secondary attribute banks that feed every deformation specification.

Out of scope: how a stream, stride and per-vertex offset are located (geometry stream
specification), and the component-default policy applied when a position-semantic consumer reads
fewer than four components (lanes past the declared count become 0, and lane 3 becomes 1).

## Inputs

Three bytes, read big-endian as a 24-bit value `n`:

| Bits of `n` | Field |
| --- | --- |
| 14..23 | `u`, 10-bit unsigned |
| 4..13 | `v`, 10-bit unsigned |
| 0..3 | selector `sel` |

The selector splits into: `axis = floor(sel / 4)` (0, 1 or 2 for valid selectors), bit 0 = sign of
the reconstructed component (set = positive), bit 1 = handedness (set = +1).

## Behaviour

1. If `sel ≥ 12`, refuse: unqualified selector, reporting `sel`.
2. Planar coordinates `a = L(u)`, `b = L(v)` with the linear map

   `L(q) = -1/√2 + q·√2/1023`, mapping 0 to about -0.7071 and 1023 to about +0.7071.

   The retail coefficients differ from these ideal values by less than `8e-5` at every `q` (open
   question 1); the tolerance below covers that.
3. Remainder `r = 1 - (a² + b²)`. If `r < 0`, use `r = 0` (open question 2).
4. Reconstructed component `c = +√r` if `sel` bit 0 is set, otherwise `c = -√r`.
5. Output lanes:
   - lane `axis` is `c`;
   - the other two of lanes 0 to 2 are `a` then `b`, in ascending lane order;
   - lane 3 is `+1` if `sel` bit 1 is set, otherwise `-1`.

| `sel` | lane 0 | lane 1 | lane 2 | lane 3 |
| --- | --- | --- | --- | --- |
| 0 | `-√r` | `a` | `b` | -1 |
| 1 | `+√r` | `a` | `b` | -1 |
| 2 | `-√r` | `a` | `b` | +1 |
| 3 | `+√r` | `a` | `b` | +1 |
| 4..7 | `a` | `±√r` | `b` | as above |
| 8..11 | `a` | `b` | `±√r` | as above |

## Edge cases and errors

- Selectors 12 to 15 are refused. They are not declared malformed; their meaning is unknown.
- With the reference map, `a² + b² < 1` for every pair of 10-bit inputs, so `r` is always positive
  and no input is refused for its magnitude. An implementation using the ideal map can produce
  `a² + b²` slightly above 1 at the four corner codes (`u` and `v` each 0 or 1023). It must clamp
  `r` to 0 there, not refuse.
- When `r = 0` and `sel` bit 0 is clear, lane `axis` may be `-0` or `+0`.

## Tolerance

Implementations use ordinary native arithmetic.

- **Planar lanes `a`, `b`.** Within `1e-4` absolute of the reference map. *Justification:* this
  admits the ideal `±1/√2` map; an angular error of 1e-4 radian is invisible in shading.
- **Reconstructed lane.** The sign must be exact. The magnitude `|c|` must satisfy
  `|c² - c_ref²| ≤ 4e-4`. For every code whose reconstructed component is the largest of the three
  (`c_ref² ≥ 1/3`), this is `|c - c_ref| ≤ 3.5e-4`. *Justification:* the bound follows from the
  planar tolerance through `r`. Near the corner codes, where `c` is close to zero, it allows an
  angular difference of about 1.2°. Those codes describe directions almost in the plane of the
  other two axes. An encoder that always drops the largest component would never produce them.
- **Lane 3.** Exact.

### Consequences of non-bit-exact output

- Decoded normals and tangents are inputs to the deformation formulas (group 0 wind, group 3 and 4
  displacement, skinning). Deformed positions therefore inherit this tolerance. The deformation
  tolerances are stated relative to the decoded values each implementation actually used. Two
  passes that must produce identical positions (see
  [group-zero-deformation.md](group-zero-deformation.md#consequences-of-non-bit-exact-output))
  must use the same decoder.
- A cache that keys decoded banks on the source bytes is unaffected. A cache keyed on decoded
  values must not mix decoders.
- Differential tests against the original decoder must compare planar and lane-3 values within
  the tolerances above, not bit for bit.

## Synthetic test vectors

Bytes are given in stream order. Expected values use the ideal map. The tolerances above apply.

| Bytes | `u`, `v`, `sel` | Expected (lane 0, 1, 2, 3) |
| --- | --- | --- |
| `80 12 C5` | 512, 300, 5 | (0.000691, 0.956302, -0.292381, -1) |
| `C8 06 4A` | 800, 100, 10 | (0.398828, -0.568865, -0.719256, +1) |
| `7F DF F0` | 511, 511, 0 | (-1.000000, -0.000691, -0.000691, -1) |
| `4B 2B CB` | 300, 700, 11 | (-0.292381, 0.260586, 0.920113, +1) |
| `FF C0 03` | 1023, 0, 3 | (c, 0.707107, -0.707107, +1) with `0 ≤ c ≤ 0.025` (corner code) |
| `00 00 06` | 0, 0, 6 | (-0.707107, c, -0.707107, +1) with `-0.025 ≤ c ≤ 0` (corner code) |
| `00 00 0C` | 0, 0, 12 | refuse: unqualified selector 12 |
| `00 00 0F` | 0, 0, 15 | refuse: unqualified selector 15 |

Two checks per selector, for every `sel` in 0..11 with `u = v = 512`: lane `axis` has magnitude
about 0.9999995 and the sign given by bit 0; lane 3 follows bit 1; the other two lanes are both
about 0.000691.

## Open questions

1. **Exact map coefficients.** The retail decoder's end points differ slightly from `∓1/√2`: the
   value at `q = 0` is a short binary fraction just above `-1/√2`, and the step is slightly smaller
   than `√2/1023`. This specification states only the ideal map and a tolerance that covers the
   difference. If a later ruling requires closer agreement, the two coefficients need to be
   published or obtained from the retail program.
2. **Negative remainder.** Unreachable with the reference map. The current reference refuses
   rather than clamping. This specification asks for clamping, because the ideal map can produce
   a tiny negative remainder at the corners.
