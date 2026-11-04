# Main material value composition

## Scope

This specification covers how the values a Main draw's programs read are built and carried from item
to item:

- the 512-row vertex constant bank and what is known about each row;
- the 16 current (constant) vertex attributes;
- the fragment constant row writes;
- the per-window staging rows;
- the *binding action* that decides whether a material's values are reapplied.

The planner calls this once per prepared item and once per update-only item, in item order
([scene-plan.md](scene-plan.md), sections 3 and 5). Applies to retail v1.0.

Out of scope:

- technique selection and the guest location of the cohort item, mesh and technique objects
  (material-source specification, not yet public);
- fragment built-in rows, which this specification takes as an input list (built-in
  specification, not yet public);
- the program decode that yields a vertex program's static constant rows.

## Inputs

### Guest objects

All fields are big-endian. `PASS` is the Main pass object ([material-setup.md](material-setup.md)).

| Object | Field | Meaning |
| --- | --- | --- |
| material | `u16 +0x44` | byte offset of the parameter records |
| material | `u16 +0x46` | byte offset of the parameter values |
| material | `u16 +0x48` | byte offset of the technique table (32-byte records) |
| material | `u16 +0x4C` | parameter record count |
| material | `u8 +0x56` | material class |
| material | `u32 +0x58` | material flags; bit `0x10` atlas allowed, bit `0x8000` forced distances |
| technique record (at `material + u16(+0x48) + 32·selector`) | `u8 +1` | membership bit index `m` (must be below 32) |
| technique record | `u8 +3` | first parameter index |
| technique record | `u8 +5`, `u8 +6` | first staging row `f` and staging row count `n` |
| technique record | `u64 +8` | technique flags (bits used: 6, 7, 23, 26, 27, 29) |
| parameter record (16 bytes, at `material + u16(+0x44) + 16·i`) | `u32 +4` | membership mask: the record belongs to the technique when bit `m` is set |
| parameter record | `u16 +8` | value index `v`: values start at `material + u16(+0x46) + 4v` |
| parameter record | `u8 +10` | value type (below) |
| parameter record | `u16 +12` | byte offset, inside the selected technique's program chunk, of the record's destination word |
| destination word (in the chunk) | bits 24–27 | destination kind: 1 current attribute, 2 vertex constants, 3 fragment constants, other ignored |
| destination word | bits 14–23 | first destination slot |
| cohort item | `transform`, 112 bytes | `+0x00`–`+0x1F` quaternion `(x, y, z, w)`, translation `(tx, ty, tz)`, scale `s`; `+0x20`–`+0x5F` a `4×4` matrix `P` (row-major); `+0x60`, `+0x64` two words `e0`, `e1`; byte `+0x68` an enable |
| cohort item | `mbi`, 32 bytes | per-item bytes; `mbi[4]` bits 0–2 distance category; `u32 mbi+24` packed atlas and parameter fields; `u32` at byte offset 9 (unaligned) an atlas record address |
| mesh | material group `g` (0–15) and `+0x20` | a 16-byte mesh constant row |
| `PASS` | `+0x68`, `+0x1DC` | depth anchor `a` and depth scale `k` (group 11) |
| `PASS` | `+0x80` | `4×4` world-to-view matrix `W`, row-major |
| `PASS` | `+0xD0` | binding lane 3 word |
| `PASS` | `+0x118`… | distance words; `+0x124`… denominators |
| `PASS` | `+0x140` | parameter-column table base |

### Value types

| Type | Words | Meaning |
| --- | --- | --- |
| 1, 2 | 1 | signed 32-bit integer, converted to `f32` rounding toward zero |
| 3 | 0 | no operation |
| 4–7 | 1–4 | `f32` scalar or vector |
| 8–10 | 8, 12, 16 | 2, 3 or 4 rows of four `f32` |

Any other type is an error.

### Other inputs

- The selected technique's identity: its program chunk address, its selector, and the material
  address.
- The vertex program's static constant rows `(row, four words)`, in program order.
- The fragment built-in rows, as an ordered list of `(fragment row, four words)` writes.
- The carried state from the previous item.

## Outputs

- **Vertex bank**: 512 rows of four words, each row *known* or *unknown*.
- **Current attributes**: 16 entries, each a known four-word value or unknown.
- **Fragment writes**: the ordered fragment row writes of the current binding.
- **Binding action**: `Full`, `MaterialOnly` or `Unchanged`.
- **Completeness**: whether the fragment writes omit computed rows. Only the opt-in
  reduced-fidelity mode omits rows; see Open questions.

## Behaviour

### Ordinary parameters

For the selected technique, walk parameter records from its first parameter index upwards while
records remain and each record's membership mask has bit `m` set. Stop at the first record that
does not. If the first index is at or beyond the record count, there are no parameters. For each
record in the run:

- Type 3 contributes nothing.
- Otherwise read its words (types 1 and 2 convert the integer), and its destination word from the
  chunk.
- **Vertex or fragment constants**: split the words into rows of four, starting at the destination
  slot. A short final row is padded with `(0, 0, 0, 1)` in the missing lanes (so a 1-word value
  becomes `(v, 0, 0, 1)`). The last row must stay below 512 for vertex constants and below 1024 for
  fragment constants (`Mapping` error otherwise).
- **Current attribute**: the slot must be below 16. Types 1, 2 and 4 give one lane, 5 two, 6 three
  and 7 four, padded with `(0, 0, 0, 1)`. Matrix types are refused (unqualified).
- Other kinds are ignored.

Within each destination the writes keep record order. A later write to the same row wins.

### Window start

When a Main window begins, every staging row 0–47 becomes known with value `(0, 0, 0, 0)`, and the
remembered binding identity and bound selector are cleared. The vertex bank and current
attributes are not reset; they carry knowledge from before Main.

### Binding action

An item's *identity* is (material address, technique chunk address, selector, group). Compared
with the remembered identity:

- `technique changed` holds when the group is 13, or there is no remembered identity, or the
  technique address or group differs, or the bound selector differs from the item's selector.
- `material changed` holds when the group is 13, or there is no remembered identity, or the
  material address differs.

The action is `Full` if the technique changed, else `MaterialOnly` if the material changed, else
`Unchanged`.

### Material binding (actions Full and MaterialOnly)

Applied in this order, so that later writes win:

1. Fragment writes become the ordinary fragment rows followed by the built-in rows. They replace
   the previous binding's writes.
2. Current attribute updates are applied.
3. Ordinary vertex constant rows are written, then the vertex program's static constant rows.
4. Vertex row 24 is written with the *binding row* (below).

With `Unchanged`, none of this happens and the previous fragment writes stay in force.

### Per-item draw values (every prepared item)

Groups 14 and 15 are refused (unqualified). Let `F` be the technique flags. Steps, in order:

1. **Parameter column** (when `F` bit 27): `idx = (u32 mbi+24 >> 15) & 0x7FF`. If `idx = 0` or
   `u32 PASS+0x140 = 0`, the values are `(0, 0, 0, 0.001)` and `(1, 0, 0, 0)`. Otherwise read the
   128 bytes at `base + 128·⌊(idx−1)/4⌋` as eight rows of four words, and take column
   `(idx−1) mod 4` of each row. That gives eight words; vertex row 44 is the first four and row 45
   the last four. If bit 27 is set but the column is "unused", the item fails (`MissingParameters`).
2. **Mesh constant** (bit 26): staging row 13 := the mesh constant row.
3. **Atlas** (bit 29): requires material flag `0x10` and bit 0 of `u32 mbi+24` (otherwise
   `MissingAtlas`). Let `A` = (the `u32` at `mbi` byte 9) with its low four bits cleared, and let
   `WH = u32 at A+72`; width `Wa = WH >> 16`, height `Ha = WH & 0xFFFF`. With `b = mbi+24`:
   - `n0 = (⌊b[0]/4⌋ + 1)·16`
   - `n1 = ((u16(b[0..1]) & 0x3F0) | 15) + 1`
   - `n2 = ((u32 b >> 14) & 63)·16`
   - `n3 = (b[2] & 63)·16`

   Staging row 18 := `(n0/Wa, n1/Ha, n2/Wa, n3/Ha)`.
4. **Extra row** (bit 23 and transform byte `+0x68 ≠ 0`): staging row 12 := `(e0, e1, 0, 0)`.
5. **Staging copy**: if `n > 0`, `f + n` must be at most 48 (`SourceBounds` otherwise). Vertex rows
   `f … f+n−1` := staging rows `f … f+n−1`, including their knowledge.
6. **Transform rows**: let `T(q, t, s)` be the `3×4` matrix whose left `3×3` is `s·R(q)` and whose
   column 3 is `t`:

   ```
   R(q) = | 1−2y²−2z²   2xy−2zw     2xz+2yw   |
          | 2xy+2zw     1−2z²−2x²   2yz−2xw   |
          | 2xz−2yw     2yz+2xw     1−2x²−2y² |
   ```

   The quaternion is used as stored, without normalising it. Then:
   - If `F` bit 7: vertex rows 0–2 := the rows of `T`.
   - Vertex rows 3–6 := the rows of `P`, adjusted for group 11 (below).
   - If `F` bit 6: vertex rows 7–9 := rows 0–2 of `Wᵀ · T₄`, where `T₄` is `T` extended with the
     row `(0, 0, 0, 1)`. Element `(c, l)` is `Σ_k W[k][c] · T₄[k][l]`.

   **Group 11 adjustment**: for each row `r` of `P`, lane 2 becomes `P[r][2]·k + P[r][3]·(a·k − a)`.
   The other lanes are unchanged. This scales clip-space depth by `k` about the anchor `a`.

### Binding row (vertex row 24)

- Category `c = min(mbi[4] & 7, 2)`.
- The class is *alternate* when the material class is 1 or 2.
- Distance `d` = the word at `PASS + 0x118 + 4c` (`PASS + 0x11C + 4c` when alternate).
- Denominator `q` = the word at `PASS + 0x124 + 4c`.
- *Forced* (alternate and material flag `0x8000`): `d = (30, 100, 200)[c]` and `q = 15`.
- Inverse `r` = `100.0` if not forced and `q` is negative, zero or subnormal; otherwise `1/q`.
- Lane 2 is `1.0` for groups 8, 9, 12 and 13, and `0` for groups 0 and 10. For other groups it is
  `0` unless the pass control is 5, in which case it is `1.0`.
- Row 24 = `(d, r, lane 2, u32 PASS+0xD0)`.

### Update-only items

A group-0 item routed as update-only records its identity as the remembered identity. It does not
change the bound selector, values or writes. Any other group is refused (`UnqualifiedGroup`).

### Vertex bank operations

- **Write**: the row must be below 512 (`VertexConstantRange`); it becomes known.
- **Batch write**: every row is range-checked first; then all are written, so a range error writes
  nothing.
- **Invalidate**: with no list, every row becomes unknown. With a list, those rows become unknown,
  but any listed index of 512 or more makes every row unknown.
- **Read for a draw**: given the rows a program reads and whether it indexes constants dynamically:
  - with dynamic indexing every row must be known, and the error names the lowest unknown row;
  - otherwise each listed row must be in range and known.
  The uniform image is all 512 rows, with unknown rows as zero words. Zero padding never makes a
  row known.

### Atomicity

A composition that fails leaves the carried state unchanged. A caller that discards the whole plan
on error may update in place instead.

## Edge cases and errors

- Missing transform, view (bit 6), atlas or group-11 depth adjustment: error.
- Groups 14 and 15 in a draw: error. Update-only items outside group 0: error.
- A parameter value type outside the table, an out-of-range destination, or a current-attribute
  slot of 16 or more: error.
- Arithmetic inputs (quaternion, scale, `W`, `P`, `a`, `k`, `q`): a subnormal input is treated as
  zero. A value whose exponent field is all ones is outside the supported domain and fails the
  item. Any result whose exact magnitude exceeds the binary32 maximum fails the item.
- Copied words (translation, `e0`, `e1`, mesh constant, parameter column, distance, lane 3, static
  rows, parameter values) are copied bit for bit, with no domain check.

## Tolerance

Native arithmetic, any evaluation order, binary32 or wider.

| Output | Tolerance |
| --- | --- |
| `s·R(q)` elements | absolute error at most `2^-21 · |s| · (1 + x² + y² + z² + w²)` |
| View rows (bit 6) | element `(c, l)` within `2^-20 · Σ_k |W[k][c]|·|T₄[k][l]|` |
| Group-11 lane 2 | within `2^-21 · (|P[r][2]·k| + |P[r][3]|·(|a·k| + |a|))` |
| Atlas lanes `n/W`, `n/H` | within 1 ulp |
| `1/q` | within 1 ulp |
| Results with exact magnitude below `2^-126` | may be returned as zero of either sign |
| Everything else | exact (copies, integer conversion toward zero, the fixed values) |

These bounds allow any reasonable ordering of a handful of binary32 operations. They correspond to
sub-micrometre placement error at game scale.

### Consequences of non-bit-exact output

- **Depth-equal tests.** Group 1 can test depth *equal* against depth written earlier in the frame
  ([main-group-transitions.md](main-group-transitions.md)). If the depth was written by a pass that
  computed its transform rows differently (for example the original depth job or a GPU kernel),
  positions can differ in the last bit and the equal test fails per pixel. The depth producer and
  Main must use identical transform rows (the same implementation and inputs), or the comparison
  must be relaxed explicitly.
- **Determinism.** The same inputs must give the same bits within a build, so that repeated or
  retried plans match.
- **Differential tests.** Comparisons with the original program must use these tolerances.
- **Caches.** Constant values are uniform data, not part of program cache keys. No cache depends on
  them being bit-exact.

## Synthetic test vectors

1. **Rotation about z by a half turn.** `q = (0, 0, 1, 0)`, `t = (4, 5, 6)`, `s = 3`, `F` bit 7.
   Rows 0–2: `(−3, 0, 0, 4)`, `(0, −3, 0, 5)`, `(0, 0, 3, 6)`.
2. **Third turn about the diagonal.** `q = (0.5, 0.5, 0.5, 0.5)`, `t = 0`, `s = 1`: rows
   `(0, 0, 1, 0)`, `(1, 0, 0, 0)`, `(0, 1, 0, 0)`.
3. **No normalisation.** `q = (0, 0, 2, 0)`, `s = 1`: row 0 lane 0 = `1 − 8 = −7`.
4. **View.** `W` = identity with row 3 `(10, 20, 30, 1)`; `q = (0, 0, 0, 1)`, `t = 0`, `s = 1`,
   `F` bits 6 and 7. Rows 7–9: `(1, 0, 0, 10)`, `(0, 1, 0, 20)`, `(0, 0, 1, 30)`. With bit 6 and
   without bit 7, rows 0–2 are not written, but rows 7–9 are the same.
5. **Group 11.** `P = I`, `a = 2`, `k = 0.5`: row 5 = `(0, 0, 0.5, 0)`, row 6 = `(0, 0, −1, 1)`,
   rows 3 and 4 unchanged.
6. **Atlas.** `mbi+24..27 = 0C 5F 83 00`, `Wa = 256`, `Ha = 128`: numerators `(64, 96, 992, 48)`,
   row 18 = `(0.25, 0.75, 3.875, 0.375)`.
7. **Parameter column.** `u32 mbi+24 = 0x00030000` gives `idx = 6`, so read the 128 bytes at
   `base + 128`, column 1. `idx = 0`, or base 0: rows 44 and 45 = `(0, 0, 0, 0.001)` and
   `(1, 0, 0, 0)`.
8. **Binding row.** `mbi[4] = 1`, class 0, group 5, pass control 3. `PASS+0x11C = 50.0`,
   `PASS+0x128 = 4.0`, `PASS+0xD0 = 7.0`: row 24 = `(50, 0.25, 0, 7)`. Class 1, flag `0x8000`,
   `mbi[4] = 6`: `(200, 1/15 (0x3D888889 ±1 ulp), 0, 7)`. Not forced with `q = −2`: `r = 100`.
   Group 12: lane 2 = 1.0.
9. **Binding action.** Items A (material M1, technique T1, selector 0, group 2), B (M2, T1, 0, 2),
   C (M2, T1, 0, 2), D (M2, T1, 0, 13): actions Full, MaterialOnly, Unchanged, Full.
10. **Update-only then prepared.** Update-only item (M1, T1, sel 0, group 0), then a prepared item
    (M1, T1, sel 0, group 0) at window start: Full, because the bound selector is unset.
11. **Scalar parameter.** Type 1 with integer `−7` to vertex slot 30: row 30 = `(−7.0, 0, 0, 1)`.
    Type 1 with `16777217`: `16777216.0` (toward zero). Type 9 (12 words) at fragment slot 1023:
    `Mapping` error.
12. **Staging.** `f = 12`, `n = 7` after window start, bit 29 with vector 6: vertex rows 12–17 are
    `(0, 0, 0, 0)` and row 18 is the atlas row. `f = 40`, `n = 9`: `SourceBounds`.
13. **Bank read.** Rows 0–9 known. Required `[3, 9]`: success, and row 10 reads as zero. Required
    `[10]`: `UnknownVertexConstant(10)`. Dynamic indexing: `UnknownVertexConstant(10)`.
14. **Invalidate.** Invalidate `[5, 600]`: every row becomes unknown.

## Open questions

1. **Game-chosen values.** These are stated as observable behaviour:
   - the null parameter column `(0, 0, 0, 0.001)`, `(1, 0, 0, 0)`;
   - the forced distances 30, 100 and 200 and the denominator 15;
   - the fallback inverse 100.

   They are covered by the same public-specification ruling as the other game state values.
2. **Domain rule.** The game's own arithmetic treats an all-ones exponent as a large finite number,
   rounds by truncation and saturates on overflow. This specification instead refuses such
   inputs, as the native-arithmetic ruling requires. No observed frame uses such values; confirm
   that refusing them is acceptable.
3. **Reduced-fidelity omission.** An opt-in diagnostic mode drops two kinds of computed fragment
   built-in rows that have no consumer in a flat program. Any later exact fragment program then
   forces a fresh binding. This mode depends on the unpublished built-in specification and is out
   of scope here.
4. **Matrix current attributes.** Parameters of types 8–10 aimed at current attributes are refused.
   Their register addressing has not been established.
