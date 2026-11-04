# Main fragment built-in rows

## Scope

A material can ask for fragment constant rows that the frontend computes, instead of rows copied from
its ordinary parameter values. This specification defines those *built-in* rows: which requests
exist, how a material's data names them, what guest data they read, the values they produce, and
where they sit among the other fragment writes. The result is the ordered list of
`(logical slot, four words)` writes that [material-values.md](material-values.md) takes as its
"fragment built-in rows" input. Applies to retail v1.0.

Out of scope:

- where the material, its technique record and the technique's program chunk live, and how the
  technique is selected ([material-sources.md](material-sources.md));
- how logical slots map onto a fragment program's inline constants
  ([program-compilation.md](program-compilation.md));
- the opt-in reduced-fidelity mode that drops animated built-in rows for flat programs. It is a
  diagnostic policy, not behaviour.

## Inputs

All fields are big-endian. `MAT` is the item's material: the selected material, which for every
qualified item is also the primary material ([material-sources.md](material-sources.md)). `PASS` is
the Main pass object ([material-setup.md](material-setup.md)). `CHUNK` is the selected technique's
program chunk, and `sel` the technique selector.

### Material fields

| Field | Meaning |
| --- | --- |
| `u16 MAT+0x42` | byte offset, inside `MAT`, of the built-in record table |
| `u8 MAT+0x53` | number of built-in records |
| `u16 MAT+0x44` | byte offset of the parameter records (16 bytes each) |
| `u16 MAT+0x46` | byte offset of the parameter values (`u32` words) |
| `u8 MAT+0x52` | number of parameter records scanned for *role values* (below) |
| `u16 MAT+0x48` | byte offset of the technique table (32-byte records) |
| `u8 MAT+0x5C` | the primary material's alpha byte `α` |

The technique record is at `MAT + u16(MAT+0x48) + 32·sel`. Its byte `+1` is the membership bit index
`m`, the same one the ordinary parameters use ([material-values.md](material-values.md)).

### Built-in record (16 bytes)

| Offset | Type | Meaning |
| --- | --- | --- |
| `+4` | `u32` | built-in kind `K` |
| `+8` | `u32` | membership mask; the record applies to the technique when bit `m` is set |
| `+12` | `u32` | byte offset, inside `CHUNK`, of the record's *destination word* |

The destination word has the layout used by ordinary parameters: bits 24–27 are the destination
kind (1 current attribute, 2 vertex constants, 3 fragment constants, anything else ignored), and
bits 14–23 are the first destination slot `L`.

### Parameter record fields used for roles

For parameter record `i`, at `MAT + u16(MAT+0x44) + 16·i`: `u16 +8` is the value index `v`, with
values starting at `MAT + u16(MAT+0x46) + 4v`, and `u16 +14` is the record's *role* `r`.

### Pass values

| Name | Source | Used by |
| --- | --- | --- |
| `X` | sixteen `u32` words at `PASS+0x00` (four rows of four) | kind 4 |
| `μ` | `f32` at `PASS+0xD8`, the alpha modulation scalar | kind 3 |
| `t` | `f32` at `PASS+0x1E0`, the animation time | kinds 31 and 32 |

These are the built-in pass values. The material capture requires that `u32 PASS+0x1FC` equals
`PASS` (the pass is its own binding payload); otherwise the capture is invalid
([material-sources.md](material-sources.md)). A pass value is read only when some item in the window
needs it. Every item in one source window sees the same value.

## Outputs

An ordered list of fragment row writes `(logical slot, four u32 words)`, or an error.

## Behaviour

### Collecting requests

1. If `m ≥ 32`, the result is a *mapping* error.
2. If the built-in record count is zero, the result is the empty list, and nothing else is read.
3. Visit the built-in records in table order. A record whose membership mask lacks bit `m` is
   skipped; the scan continues with the next record.
4. A record whose kind is not 3, 4, 31 or 32 is a no-op. Its destination word is not read, so an
   out-of-range destination offset is harmless.
5. For kinds 3, 4, 31 and 32, read the destination word. If its destination kind is not 3
   (fragment constants), the record writes nothing. This is not an error. Those destinations have
   their own writers elsewhere.
6. Otherwise the record produces the rows below, starting at logical slot `L`, in ascending slot
   order.

Requests keep record order. A later write to a slot replaces an earlier one.

### Kind 3: alpha transfer

One row at slot `L`. With `â = α / 255` and `μ' = â·μ`:

```
row = ( 1/â        if α > 0,          otherwise 1,
        1/(1 − μ') if μ' < 1,         otherwise 1,
        μ',
        1 )
```

Lane 0 is the reciprocal of the material alpha, lane 1 the reciprocal of the transmission left after
modulation, and lane 2 the modulated alpha. The guarded cases avoid division by zero.

### Kind 4: pass matrix

Four rows at slots `L`, `L+1`, `L+2`, `L+3`: row `j` is words `4j … 4j+3` of `X`, copied bit for bit.
`L > 1020` is a *mapping* error.

### Role values (kinds 31 and 32)

Scan parameter records `0 … u8(MAT+0x52) − 1` in order. Note that this count is separate from the
ordinary parameter record count. For each record with role `r < 8`, read its value words:

- role 0: two words;
- role 6: three words;
- every other role: one word.

Unread words are zero. A later record with the same role replaces an earlier one. Records with
`r ≥ 8` are ignored. `R_r` denotes role `r`'s words as `f32`. A role that no record carries is
*missing*.

### Kind 31: animated scroll

Four rows at `L … L+3`. Let `(p, q) = R_0`, `d = R_1[0]` and `w = R_2[0]`. Then

```
u = t·p/d        s = t·q/d        θ = π·w
a = s·cos θ      b = s·sin θ

row L   = ( −(√3/2)·u + a,   u/2 + b,   a,   −u + b )
row L+1 = (  (√3/2)·u + a,   u/2 + b,   a,   −u + b )
row L+2 = (  d,              d,         −K1·d,   K1·d )
row L+3 = ( −K2·d,           K2·d,      −d,      −d )
```

Rows `L` and `L+1` each pack two 2-D offsets. Together they give three scroll directions 120° apart
(at 150°, 270° and 30°), each moving by `u`, plus a common offset `(a, b)` of length `s` at angle
`θ`. Rows `L+2` and `L+3` are scale rows proportional to `d`. `K1 = 0.91` and `K2 = 0.95` are two
fixed scale factors, each used as the binary32 value nearest the decimal shown. They are this
implementation's own tuning values, chosen for visual equivalence with the game's scroll; they are
not taken from the game. Comparisons of rows `L+2` and `L+3` against the original program are
therefore visual (scale factors within 3%), not numeric.

### Kind 32: animated products

Two rows at `L` and `L+1`:

```
row L   = ( R_3[0]·R_4[0],   R_5[0]·t,         0,                0 )
row L+1 = ( R_6[0]·R_7[0],   R_6[1]·R_7[0],    R_6[2]·R_7[0],    0 )
```

For kinds 31 and 32, `L + rows > 1024` is a *mapping* error.

### Place among the fragment writes

The material's fragment writes are its ordinary fragment rows followed by these built-in rows, in
the order above ([material-values.md](material-values.md), "Material binding"). Built-in rows
therefore win over ordinary rows at the same slot.

## Edge cases and errors

| Condition | Result |
| --- | --- |
| Any read outside `MAT` or `CHUNK` (tables, records, destination word, role values) | *bounds* error |
| `m ≥ 32`; kind 4 with `L > 1020`; kind 31 or 32 overrunning slot 1023 | *mapping* error |
| Kind 31 or 32 with a missing role, or with `t` unavailable | *unqualified kind 31/32* error for the item |
| A pass value that cannot be read | the item's capture fails |
| Kind 3, 4, 31 or 32 aimed at a non-fragment destination | no write, no error |
| Any other kind | no-op, no read, no error |

**Numeric domain.** `μ`, `t` and the role words used by kinds 31 and 32 are arithmetic inputs. They
follow the domain rule of [material-values.md](material-values.md): a subnormal input is treated
as zero; an all-ones exponent (infinity or NaN) fails the item; and a computed result whose exact
value is not finite fails the item. For kind 31 that includes `d = 0`. Kind 4 words are copied and
never checked.

## Tolerance

Native arithmetic, any evaluation order, binary32 or wider. Exact values are the formulas above in
real arithmetic on the binary32 inputs.

| Output | Tolerance |
| --- | --- |
| Kind 3 lanes 0–2 | within 2 ulp; lane 3 exactly 1 |
| Kind 4 | bit-exact copies |
| Kind 31 rows `L`, `L+1` | each lane within `2^-18·(|u| + |s|) + 2^-20·|s|·(1 + |θ|)` absolute |
| Kind 31 rows `L+2`, `L+3` | within 1 ulp of the exact product |
| Kind 32 products | within 1 ulp; the zero lanes are exactly `+0` |

*Justification:* these rows drive texture-coordinate animation and alpha weighting. An error of
`2^-18` of the scroll distance is far below one texel at any texture size the game uses. The
`|θ|` term lets an implementation reduce a large angle in binary32, which only shifts a rotating
offset by an invisible amount. The kind-3 bounds keep alpha weights accurate to well under one
8-bit step.

### Consequences of non-bit-exact output

- These rows enter only the fragment uniform block, never a program or cache key
  ([program-compilation.md](program-compilation.md)). A tolerance difference never selects a
  different compiled program.
- A lane near the sanitise threshold may land on either side of it. Built-in values are far from it
  in practice.
- Differential tests against the original program must compare computed lanes with these
  tolerances. Kind 4 rows and the fixed lanes (`1`, `0`) are compared exactly.

## Synthetic test vectors

Common setup: `m = 0`, every built-in record has membership mask `1`, and each destination word is
`(3 << 24) | (L << 14)`.

1. **Alpha transfer.**

   | `α` | `μ` | row |
   | --- | --- | --- |
   | 51 | 0.5 | `(5, 1.1111111, 0.1, 1)` |
   | 0 | 0.5 | `(1, 1, 0, 1)` |
   | 255 | 1 | `(1, 1, 1, 1)` (the `μ' < 1` guard fails) |
   | 255 | 2 | `(1, 1, 2, 1)` |
   | 102 | 0.25 | `(2.5, 1.1111111, 0.1, 1)` |

2. **Pass matrix.** `X = 1.0, 2.0, …, 16.0`, `L = 40`: rows 40–43 = `(1, 2, 3, 4)`, `(5, 6, 7, 8)`,
   `(9, 10, 11, 12)`, `(13, 14, 15, 16)`. With `L = 1021`: *mapping* error.
3. **Kind 31, quarter turn.** `R_0 = (2, 6)`, `R_1 = 4`, `R_2 = 0.5`, `t = 2`. Then `u = 1`,
   `s = 3`, `θ = π/2`, `a = 0` and `b = 3` (within tolerance). Row `L` = `(−0.8660254, 3.5, 0, 2)`,
   row `L+1` = `(0.8660254, 3.5, 0, 2)`, row `L+2` = `(4, 4, −3.64, 3.64)`, row `L+3` =
   `(−3.8, 3.8, −4, −4)`.
4. **Kind 31, no turn.** As vector 3 with `R_2 = 0`: `θ = 0`, `a = 3`, `b = 0`. Row `L` =
   `(2.1339746, 0.5, 3, −1)`, row `L+1` = `(3.8660254, 0.5, 3, −1)`.
5. **Kind 32.** `R_3 = 1.5`, `R_4 = 4`, `R_5 = −2`, `R_6 = (0.5, −1, 2)`, `R_7 = 3`, `t = 0.25`,
   `L = 10`. Row 10 = `(6, −0.5, 0, 0)`, row 11 = `(1.5, −3, 6, 0)`. With `L = 1023`: *mapping*
   error.
6. **Role precedence.** Two role-1 records, with values 9 and then 2: `R_1 = 2`. A record with role
   9 is ignored. With role 0 present but role 2 absent, kind 31 is the *unqualified kind 31* error.
7. **Membership.** Three records: kind 3 with mask 1, kind 3 with mask 2, kind 3 with mask 1.
   Requests come from records 0 and 2 only, and record 2's row is written after record 0's.
   `m = 32`: *mapping* error.
8. **Ignored kinds.** A kind-5 record with destination offset `0xFFFFFFFF`: no read, no row, no
   error. The same offset on a kind-3 record: *bounds* error.
9. **Other destinations.** Kind 4 with destination word `(2 << 24) | (8 << 14)`: no rows, no
   error, and `X` is not needed.
10. **Order against ordinary rows.** An ordinary parameter writes slot 8 with `(7, 7, 7, 7)`. A
    kind-3 built-in with `L = 8`, `α = 51`, `μ = 0.5`: slot 8 ends as `(5, 1.1111111, 0.1, 1)`.
11. **Domain.** Kind 31 with `R_1 = 0`: the item fails. Kind 3 with `μ` = NaN: the item fails.
    Kind 32 with `R_5` subnormal: `R_5` is treated as zero, so row `L` lane 1 = `0`.

## Open questions

1. **Kind-31 direction constant.** The spec uses the exact `√3/2` (cos 30°). The game's own value
   is a short decimal rounding of it; the difference is about `3·10^-5` relative, inside the
   tolerance of rows `L` and `L+1`.
2. **Non-finite results.** The game computes with a saturating arithmetic. Its results for `d = 0`
   or a non-finite `μ` would be large finite numbers that compilation later sanitises to zero. This
   specification refuses those items instead, following the native-arithmetic ruling. No observed
   material produces them.
3. **Selected and primary materials.** The alpha byte is read from the primary material and the
   records from the selected material. These are the same material for every qualified item. If a
   future route lets them differ, which object supplies each field must be confirmed.
