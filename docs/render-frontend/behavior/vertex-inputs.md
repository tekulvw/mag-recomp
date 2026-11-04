# Main vertex-input packing

## Scope

How the frontend turns one prepared geometry draw into the native vertex inputs of a material draw:
which vertex-array slots are enabled, which source rows feed each slot the compiled vertex program
reads, how those rows are interleaved, and how indices are carried. It also defines the *effective
vertex-array mask*, which program compilation ([program-compilation.md](program-compilation.md))
and the Main planner ([scene-plan.md](scene-plan.md)) use.

Applies to retail v1.0. Out of scope:

- how attribute banks are decoded from partition streams ([geometry-streams.md](geometry-streams.md)).
  The primary and secondary banks used below follow its "Looking up a semantic" rule;
- the deformation that produces prepared banks ([group-zero-deformation.md](group-zero-deformation.md),
  [noninstanced-deformation.md](noninstanced-deformation.md),
  [skinning-deformation.md](skinning-deformation.md));
- the diagnostic lane-reversal and reduced-fidelity position-approximation modes, both opt-in and
  off by default. Each is mentioned only where it changes a rule.

## Inputs

### Vertex bindings

A *binding* says which source attribute feeds a vertex-array slot. It is decoded from a mesh's
vertex declaration rows and carries:

| Field | Meaning |
| --- | --- |
| `declaration` | ordinal of the declaration row (its position in declaration order) |
| `semantic` | attribute meaning: 1 position, 2 normal, 3 tangent, 4 secondary direction, 9 skinning weights; others are opaque |
| `slot` | vertex-array slot, 0–15 (a value of 16 or more is an error) |
| `format` | RSX vertex data type: 1 signed-normalised 16-bit, 2 32-bit float, 3 16-bit float, 4 unsigned-normalised byte, 5 signed 16-bit integer, 6 packed 11/11/10 (`CMP`), 7 unsigned byte integer |
| `components` | component count, 0–4; 0 means the array is declared but disabled |

The source attribute values arrive already decoded as rows of four `u32` words (binary32 bit
patterns), one row per vertex. Format and component count describe how they were encoded; they do
not change the row values here.

### One draw

A draw supplies:

- **output bindings**: the bindings for the mesh's primary output declaration. Absent (an error)
  when the declaration could not be resolved;
- **auxiliary attributes**: bindings whose rows were decoded successfully, each with its row bank,
  and separately the auxiliary bindings whose decode *failed*;
- **primary and secondary attribute banks**: decoded banks by semantic. A bank may itself be a
  decode failure;
- **positions**: the decoded position bank;
- **raw indices**: 16-bit indices from the partition;
- at most one **preparation**, which replaces some of the above:
  - *direct*: a prepared direct draw with its own auxiliary attributes, vertex count and 32-bit
    indices. It has no output bindings;
  - *group-zero*: prepared positions, 32-bit indices, slot-12 colour rows and its own auxiliary
    attributes. It may instead be GPU-produced (below);
  - *non-instanced*: prepared positions, normals, weights and optionally tangents, plus the
    preparation's mesh group, geometry mode and static-source facts.

**Skinned items** (group 11, [skinning-deformation.md](skinning-deformation.md)) have no
preparation kind of their own. A successful skinning result enters vertex-input packing as a
*non-instanced* preparation with:

- mesh group 11 and the item's geometry mode;
- positions: the skinned positions;
- normals: the skinned normals;
- tangents: the skinned tangents, always present;
- weights: empty. Skinning consumes the source weights, so a program that reads semantic 9 gets a
  *missing semantic* error for that slot;
- no static-source facts. A program that reads semantic 4 gets an *unprepared primary semantic*
  error, because the static-source conditions cannot hold.

A refused skinning result is a refused non-instanced preparation. The skinned banks must not be
replaced by the raw source banks, and the normals and tangents must not be left empty when the
skinning result has them.

A preparation may itself be a recorded refusal. Using a refused preparation is an error.

### Request

- `required`: a 16-bit mask of the slots the compiled vertex program reads. Each such slot needs a
  row source.
- `budget`: the maximum number of bytes for vertices plus indices.

## Outputs

`NativeVertexInputs`:

| Field | Meaning |
| --- | --- |
| `array_mask` | effective vertex-array mask (all enabled slots, read or not) |
| `stride` | bytes per vertex: 16 × (number of slots in `required`) |
| `attributes` | `(location, offset)` per required slot, ascending by slot, offsets 0, 16, 32 … |
| `vertices` | interleaved bytes; each lane is a little-endian `u32` |
| `indices` | 32-bit indices |
| `produced` | for GPU-produced group-zero vertices only: the GPU layout plan, with `vertices` empty |

Every native attribute is four 32-bit floats.

## Behaviour

### Slot selection

Build a table of 16 optional sources:

1. With a group-zero preparation, slot 12 starts as its prepared colour rows. They count as a
   binding with format 2, four components and declaration ordinal 0. Any auxiliary binding to slot
   12, successful or failed, therefore replaces them, and an output binding always does.
2. Take the auxiliary attributes: the preparation's when a direct or group-zero preparation is
   present, otherwise the draw's own. If the draw's own auxiliary attributes are unavailable, that
   is a *source bindings* error. Walk the successful auxiliary bindings, then the failed ones. A
   binding replaces the current occupant of its slot when the slot is empty or the occupant's
   declaration ordinal is at most this one's. The highest declaration ordinal therefore wins, and
   on a tie the failed binding wins. A slot of 16 or more is an error.
3. The output bindings then overwrite their slots unconditionally. A direct preparation has no
   output bindings.

The **effective vertex-array mask** has bit `s` set when slot `s` has a source whose `components`
is nonzero.

Preparation consistency, checked first:

- a direct preparation together with a group-zero or non-instanced preparation is *conflicting
  preparation*;
- a group-zero and a non-instanced preparation together are also *conflicting preparation*;
- a refused group-zero or non-instanced preparation is an error of that kind;
- missing output bindings, without a direct preparation, are a *source bindings* error.

A direct preparation is either present or absent here. A direct draw whose preparation was refused
never reaches vertex-input packing, because the direct-draw geometry stage refuses the item first.
So there is no direct-preparation refusal error at this step.

For a GPU-produced group-zero preparation the mask is the one the GPU plan declares, and the table
above is not built.

### Row sources

Every slot in `required` must be in the effective mask. Otherwise the error is *missing array*,
naming the lowest missing slot. For each required slot, in ascending order:

- `components` must be 1–4 and `format` 1–7, otherwise an error naming the offending value;
- a successful auxiliary binding uses its own rows;
- prepared slot-12 colours use the prepared rows;
- a failed auxiliary binding is a *failed auxiliary* error for that slot;
- an output (primary) binding uses, by case:
  - **non-instanced preparation present**:
    - semantic 1 → prepared positions;
    - semantic 2 → prepared normals (missing if empty);
    - semantic 9 → prepared weights (missing if empty);
    - semantic 3 → prepared tangents (missing if absent);
    - semantic 4 → the static secondary-direction bank, but only on an undeformed static source.
      That means geometry mode 8 or 10, mesh group not 0, 3, 4 or 6, configuration-stream byte 6
      low nibble zero, and both palette control words (partition `+0x48` and `+0x50`) zero. The
      bank is the first semantic-4 bank in the primary list, otherwise in the secondary list. A
      failed bank is a *failed semantic* error. Every row's first three lanes must be finite with
      magnitude at most 1; lane 3 is not checked;
      If those static-source conditions do not hold, semantic 4 is an *unprepared primary
      semantic* error. If they hold but neither list has a semantic-4 bank, it is a *missing
      semantic* error;
    - any other semantic → *unprepared primary semantic*;
  - **no non-instanced preparation, semantic not 1** → *unprepared primary semantic*. A raw
    normal, tangent or weight bank is not the post-deformation value the program reads;
  - **semantic 1 with a group-zero preparation** → prepared positions;
  - **semantic 1 otherwise** → the first position bank in the primary attribute list. If that list
    is absent, the decoded positions are used. A failed list or bank is a *failed semantic* error;
    a missing bank is a *missing semantic* error.

Each selected bank must have exactly `count` rows (see below), otherwise *vertex count*.

### Vertex count and indices

- `count` is the direct preparation's vertex count. Without a direct preparation it is the length
  of the position bank in use: non-instanced prepared positions, else group-zero prepared
  positions, else decoded positions.
- Indices are the direct preparation's, else the group-zero preparation's, else the raw 16-bit
  indices widened to 32 bits. Every index must be less than `count`; otherwise *index* error
  (reporting the first bad index).
- Budget: `count × stride + 4 × index_count` must not exceed `budget` and must not overflow.
  Otherwise *budget*.

### Interleaving

For vertex `v` and the `k`-th required slot (in ascending slot order), bytes
`v·stride + 16k … +15` hold four little-endian words:

- lanes `0 … n−1` are the source row's lanes, where `n` is `components`, except format 6, where
  `n = 3`;
- the remaining lanes default to `(0, 0, 0, 0x3F800000)`: zero for lanes 1–2 and 1.0 for lane 3.

`attributes[k] = (slot, 16k)`.

Slots that are enabled but not in `required` contribute nothing to the bytes. They stay in
`array_mask`, because enabled arrays decide whether a program input comes from an array or from a
current attribute value.

### GPU-produced group-zero vertices

When the group-zero preparation is GPU-produced, its layout plan supplies the stride and one field
per required slot. `attributes[k] = (field slot, 16k)`. The indices are the preparation's,
`vertices` is empty, and `produced` carries the plan. A layout failure is a group-zero preparation
error.

### Deferred writing

An implementation may validate first and write the interleaved bytes later, even on another thread.
The bytes must be identical to writing them immediately, and every error must be raised during
validation.

## Edge cases and errors

Errors: source bindings; group-zero preparation; non-instanced preparation; conflicting preparation;
slot; missing array (slot); unprepared primary semantic (slot, semantic); missing semantic (slot,
semantic); failed semantic (slot, semantic); failed auxiliary (slot); components (value); format
(value); vertex count (slot, expected, actual); index (index, count); budget; allocation.

- `required = 0`: stride 0, no attributes and empty vertex bytes. Indices are still validated and
  carried.
- An enabled slot with `components = 0` is not in the mask, so requiring it is *missing array*.
- Opt-in reduced-fidelity position approximation: when output bindings are missing only because an
  empty custom declaration left them unresolved, the draw may use a single synthetic binding
  (semantic 1, slot 0, float, three components). The use is reported as an approximation. It is off
  by default.

## Tolerance

No arithmetic is performed. Every lane is a bit-exact copy of its source row word or one of the
stated defaults.

**Consequences of non-bit-exact output.** None arise here. Prepared banks from deformation carry
their own tolerances. This packing must not round, normalise or reorder them, so two packings of
the same banks are byte-identical.

## Synthetic test vectors

Notation: rows are written as floats; bytes are little-endian.

1. **Position and texcoord.** Output bindings: semantic 1 at slot 0 (format 2, 3 components). One
   auxiliary binding: slot 8 (format 2, 2 components, declaration 4) with rows `(0, 1)`, `(1, 1)`,
   `(0, 0)`. Positions `(−1, −1, 0)`, `(1, −1, 0)`, `(−1, 1, 0)` (lane 3 holds anything). Raw indices
   `[0, 1, 2]`. `required = 0x0101`. Expected: `array_mask = 0x0101`, `stride = 32`, attributes
   `[(0, 0), (8, 16)]`. Vertex 1 words are `(1.0, −1.0, 0, 1.0, 1.0, 1.0, 0, 1.0)`, i.e.
   `0x3F800000, 0xBF800000, 0, 0x3F800000, 0x3F800000, 0x3F800000, 0, 0x3F800000`.
   Indices `[0, 1, 2]` (32-bit).
2. **Default lanes.** As 1, but the slot-8 row of vertex 0 has lanes 2–3 = `0x12345678`. Expected:
   bytes 24–31 of vertex 0 are still `0` and `1.0`, because only 2 components are copied.
3. **Declaration order.** Auxiliary bindings at slot 5: declaration 2 (success) and declaration 6
   (success). Expected: declaration 6's rows. Add a failed binding at slot 5 with declaration 6:
   it wins the tie, so requiring slot 5 is a *failed auxiliary (5)* error. Not requiring slot 5
   succeeds, and bit 5 is still in `array_mask`.
4. **Output overrides auxiliary.** An auxiliary binding at slot 0 and an output binding at slot 0.
   Expected: the output (position) source.
5. **Disabled component count.** An auxiliary binding at slot 3 with `components = 0`. Expected: bit
   3 clear. `required = 0x0009` is *missing array (3)*.
6. **CMP format.** Slot 2, format 6, `components = 1`, row `(0.5, −0.5, 0.25, 9.0)`. Expected lanes
   `(0.5, −0.5, 0.25, 1.0)`.
7. **Index range.** `count = 3`, indices `[0, 3, 1]`. Expected: *index (3, 3)*.
8. **Budget.** `count = 100`, `stride = 32`, 300 indices: 3200 + 1200 = 4400 bytes. `budget = 4399`
   is *budget*; `budget = 4400` succeeds.
9. **Unprepared normal.** Output binding semantic 2 at slot 1, no preparation, `required` includes
   slot 1. Expected: *unprepared primary semantic (1, 2)*.
10. **Prepared normal.** As 9 with a non-instanced preparation whose normal bank has `count` rows.
    Expected: slot 1 carries the prepared normals.
11. **Secondary direction domain.** Static source qualifies; semantic-4 row `(0.6, 0.8, 0.0, NaN)`
    is accepted. Row `(1.5, 0, 0, 0)` is *failed semantic (slot, 4)*.
12. **Conflict.** Direct and non-instanced preparations together. Expected: *conflicting
    preparation*.
13. **Group-zero colour.** A group-zero preparation with colour rows, and no other slot-12 binding.
    Expected: bit 12 set, and slot 12 rows are the prepared colours.

14. **Skinned preparation.** A group-11 skinning result with 3 vertices (positions, normals and
    tangents present) and output bindings for semantics 1 (slot 0), 2 (slot 2) and 3 (slot 6):
    effective mask `0x45`, and slots 0, 2 and 6 are the skinned positions, normals and tangents.
    The same draw with an output binding for semantic 9: *missing semantic* for that slot. With
    semantic 4: *unprepared primary semantic*.

## Open questions

1. The `CMP` rule copies three lanes from a source row of one encoded component. That relies on the
   codec having already expanded the packed value into three lanes. This is stated in the codec
   specification's terms, which are not yet public.
2. Lane order for four-component unsigned-byte arrays is memory order. An opt-in diagnostic exists
   to reverse it per slot. No retail draw has been shown to need reversal.
