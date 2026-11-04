# Partition streams and attribute codecs

## Scope

How the vertex and index data of one mesh partition is located and decoded. It covers:

- the partition record and its stream descriptor table;
- the role of each stream;
- the configuration stream;
- the fixed position codecs and the descriptor-driven attribute codecs;
- vertex and index counts;
- the primary and secondary attribute lists, and how a semantic is looked up across them;
- the large-stream rule;
- the auxiliary attribute table.

The decoded banks feed [vertex-inputs.md](vertex-inputs.md) and the deformation specifications:
[group-zero-deformation.md](group-zero-deformation.md),
[noninstanced-deformation.md](noninstanced-deformation.md) and
[skinning-deformation.md](skinning-deformation.md). Applies to retail v1.0.

Out of scope:

- finding the mesh record of an item ([material-sources.md](material-sources.md));
- the packed spherical normal (codec 9), which is specified in
  [geometry-normals.md](geometry-normals.md);
- deformation;
- the GPU-side vertex fetch of auxiliary arrays, which is RSX vertex-array behaviour and is
  implemented by `decode_gpu_attribute` in `mag-render-adapter-core`.

## Inputs

### Partition chain

A mesh record (192 bytes) names its first partition: the address is
`(u32 mesh+0x50 AND 0x7FFFFFFF) + G`, modulo `2^32`. `G` is the geometry resource base, which is
pass record `u32 +0x284`. Each partition record is 544 bytes. Its `u32 +0xF0` is the address of the
next partition, and 0 ends the chain.

A chain that revisits an address is an error (cycle). A record that would extend past `2^32` is an
error. The number of records followed is bounded by an implementation-defined budget. Mesh byte
`+0xA2` records a partition count, but it is metadata only: the chain links are authoritative.

### Partition record fields used here

| Offset | Type | Meaning |
| --- | --- | --- |
| `+0x0A`, `+0x0C`, `+0x0E` | `u16` ×3 | secondary attribute data chunk sizes (bytes) |
| `+0x10` | `u32` | this partition's vertex-data size, used for auxiliary array offsets |
| `+0x14`, `+0x16` | `u16` ×2 | offsets of skinning palette segments 0 and 1 from the palette base |
| `+0x30 + 8·k` | `u32` | control word of stream slot `k`, for `k = 0..23` |
| `+0x34 + 8·k` | `u32` | guest address of stream slot `k` |
| `+0xF0` | `u32` | next partition (above) |
| `+0xF8` | `u32` | the partition's first source vertex (used by the slot-6 array; see `mag-render-adapter-core`) |
| `+0xFC` | `u32` | instance batch maximum (group 0) |
| `+0x110` | 8 bytes | auxiliary table header: byte `+0x111` is the stride, byte `+0x112` the entry count |
| `+0x118 + 8·e` | 8 bytes | auxiliary table entry `e` |

### Stream slots

A slot's control word is its byte length. A control with any of bits 16–31 set is an
*unsupported control* error for that slot. The check applies only where the length *is* the
control word: slots 0–9, 17 and 19–23 in the table below (for slots 3, 4 and 17 only the address is
replaced). The control words of slots 10–16 and 18 are never checked or used, because their lengths
are replaced by a 16-bit size or by 0. A control of 0 means the stream is empty; nothing is read
and the address is kept only as identity.

Some slots have their length or address replaced before reading:

| Slot | Role | Length | Address |
| --- | --- | --- | --- |
| 0 | custom output layout (when configuration byte 4 > 3) | control | slot address |
| 1 | index list, `u16` big-endian | control, or the large-stream rule | slot address |
| 3 | skinning palette segment 0 | control (`+0x48`) | palette base + `u16 +0x14` |
| 4 | skinning palette segment 1 | control (`+0x50`) | palette base + `u16 +0x16` |
| 5 | skinning influences, 8 bytes per vertex | control | slot address |
| 7 | vertex data: positions and primary attributes | control, or the large-stream rule | slot address |
| 10 | secondary attribute data | `u16 +0x0A` | slot address (`+0x84`) |
| 11, 12 | further secondary chunks | `u16 +0x0C`, `u16 +0x0E` | slot address |
| 13–16 | extra input stream (group 0: instance rows) | 0, except as in [group-zero-deformation.md](group-zero-deformation.md) | the extra-stream base |
| 17 | view environment record | control | the environment record address |
| 18 | unused | 0 | 0 |
| 19 | configuration, at least 16 bytes | control | slot address |
| 20 | primary attribute format (descriptor table) | control | slot address |
| 21 | secondary attribute format (descriptor table) | control | slot address |
| 2, 6, 8, 9, 22, 23 | no consumer in this specification | control | slot address |

The palette base and the extra-stream base belong to the consuming preparation. Their rules are in
[skinning-deformation.md](skinning-deformation.md) and
[group-zero-deformation.md](group-zero-deformation.md). A slot whose length is nonzero must satisfy
`address + length ≤ 2^32`, otherwise *invalid range*.

### Large-stream rule

A control of exactly `0x4000` (16 KiB) is the largest single transfer the game issues. It is not
the stream's true length. When the consumer's own counts need more than 16 KiB from such a slot,
read exactly the needed number of bytes from the slot's address. The needed length is capped at
4 MiB; above the cap the rule does not apply. Any other control, or a need of 16 KiB or less, uses
the control as the length.

The needed byte counts are:

| Slot | Needed bytes |
| --- | --- |
| 7 with position codec 0 | `12·V` |
| 7 with position codec 6 | `6·V` |
| 1 | `2·K` |

`V` and `K` come from the configuration stream (below). For slot 7 with descriptor-driven attributes
(position codec 255), the needed length is the furthest byte any declaration reads. Open question 1
says why this case differs from the reference.

### Configuration stream (slot 19)

At least 16 bytes; fewer is a *source extent* error. Fields:

| Byte | Meaning |
| --- | --- |
| 0 | skinning configuration (see [skinning-deformation.md](skinning-deformation.md)) |
| 2 | position codec: 0 = three `f32`, 6 = three rebased halves, 255 = descriptor-driven via slot 20; anything else is an *unsupported position encoding* error |
| 3 | secondary attributes: 255 = described by the slot-21 format; anything else is a built-in secondary layout, which is unsupported |
| 4 | output layout selector: 0–3 built-in, above 3 custom (from slot 0) |
| 6 | skinning influence layout; its low nibble is the direction-transform control |
| 7 | skinning matrix format |
| 8–9 | `u16` vertex count `V` |
| 10–11 | `u16` index count `K` |
| 12–15 | skinning words (see [skinning-deformation.md](skinning-deformation.md)) |

### Descriptor table (slots 20 and 21)

An 8-byte header followed by `n` 8-byte declarations:

| Header byte | Meaning |
| --- | --- |
| 0 | declaration count `n` |
| 1 | vertex stride `S` in bytes |

| Declaration byte | Meaning |
| --- | --- |
| 0 | byte offset `O` of the attribute inside a vertex |
| 1 | codec (below) |
| 2 | component count `c` |
| 3 | semantic: 1 position, 2 normal, 3 tangent, 4 secondary direction, 9 skinning weights; 0 is unclassified; others are opaque identifiers |
| 4 | element size in bytes (used by codec 2 below) |
| 5–7 | not used by the source decode |

A table shorter than `8 + 8n` bytes is a *source extent* error.

The same 8-byte entry layout is used by the custom output layout in slot 0, where byte 5 is the
output register. Decoding that layout is `decode_output_bindings`, and the built-in layouts are
`builtin_output_bindings`, both in `mag-render-adapter-core`.

## Behaviour

### Counts and indices

`V` is the number of vertex rows every bank of the partition must produce. The index list is the
first `K` big-endian `u16` of slot 1, in order, with duplicates kept. Every index must be below
`V`, otherwise *source extent*. Bytes after `2·K` are ignored.

### Positions

- **Codec 0:** `V` rows of 12 bytes, three big-endian `f32` copied bit for bit. Lane 3 is `1.0`.
- **Codec 6:** `V` rows of 6 bytes, three rebased halves (below). Lane 3 is `1.0`.
- **Codec 255:** decode the first declaration with semantic 1 in the slot-20 table over the slot-7
  data, with the descriptor codecs below. No such declaration is a *missing position attribute*
  error.

With codecs 0 and 6 there is no primary attribute list. Only positions come from slot 7.

Bytes after the decoded rows are ignored.

### Rebased half

A 16-bit field `h` with sign `s` (bit 15), `E` (bits 10–14) and `m` (bits 0–9):

- if `h AND 0x7FFF = 0`, the value is zero with sign `s`;
- otherwise it is `(−1)^s · 2^(E−15) · (1 + m/1024)` for every `E`, 0 and 31 included. There are no
  subnormals, infinities or NaNs.

This is not IEEE binary16, and it differs from RSX vertex-fetch format 3. Group-zero instance rows
use the same field ([group-zero-deformation.md](group-zero-deformation.md)).

### Descriptor codecs

For vertex `i`, the attribute starts at logical byte `L = i·S + O` of its stream. With stream
address `A` (the slot address), the attribute's bytes start at guest address
`align_down_16(A + L) + (L mod 16)`. When `A` is 16-byte aligned this is simply `A + L`; see open
question 2 for misaligned streams. Every byte read must lie within the stream as read; otherwise
the declaration fails with *source extent*.

Per lane, by codec. "Truncated" means converted to binary32 toward zero; the tolerance section
relaxes this.

| Codec | Bytes | Lane value |
| --- | --- | --- |
| 1 | 2 per lane, signed `v` | `(2v + 1) / 65535` (symmetric signed normalised, range [−1, 1]) |
| 2 | 4 per lane | big-endian `f32`, copied bit for bit |
| 3 | 2 per lane | rebased half |
| 4 | 1 per lane, unsigned `v` | `v / 255` |
| 5 | 2 per lane, signed `v` | `v`, an integer |
| 6 | 4 per vertex | packed: x = bits 0–10, y = bits 11–21, z = bits 22–31, each two's complement `s`; x and y are `(2s + 1)/2047`, z is `(2s + 1)/1023`; lane 3 is `1.0` |
| 7 | 1 per lane, unsigned `v` | `v`, an integer |
| 9 | 3 per vertex | packed spherical normal ([geometry-normals.md](geometry-normals.md)) |
| other | | the declaration fails with *unsupported encoding* (semantic 1: *unsupported position encoding*) |

Codecs 1, 4 and 6 are the partition's own source formats. They are not the RSX vertex-fetch
formats with the same numbers: RSX format 1 is `v/32767`, and RSX CMP is `x/1023`, `y/1023`,
`z/511`. Do not reuse the GPU fetch decoder for partition streams.

**Lanes decoded:**

- **Semantic 1 (positions):** lanes `0 .. min(c, 4) − 1` are decoded. Lanes 0–2 beyond that are
  `+0`, and lane 3 is `1.0`. Codecs 6 and 9 always read and decode their whole packed field (4 and
  3 bytes), even when `c < 3`. Codec 6 then zeroes those of x, y, z at or beyond `c`. Codec 9 zeroes
  lanes at or beyond `c`, except that lane 3 becomes `1.0`. Because the whole field is decoded, a
  codec-9 refusal (selector or magnitude) fails the declaration even if every lane it affects would
  be discarded, and the whole field must lie inside the stream.
- **Any other semantic:** all four lanes are decoded from consecutive bytes, whatever `c` says.
  Codec 6 gives `(x, y, z, 1)` and codec 9 its four lanes. Lanes beyond `c` therefore hold the
  following bytes of the vertex, decoded with the same codec. They have no defined meaning, but
  they are a deterministic function of the data and must be reproduced. Codec 2 has one extra
  rule for these lanes: if `(L mod 16) +` element size is at most 16, a byte that would fall past
  the end of the current 16-byte block wraps to the start of that block; otherwise bytes are read
  straight on. Lanes inside the declared element never wrap.

### Primary attribute list

With position codec 255, every declaration in the slot-20 table is decoded over slot 7, in
declaration order:

- semantic 0 fails as *unclassified semantic*;
- each declaration is recorded as a success (its `V` rows) or a failure (its error) under its
  ordinal.

A failed declaration never shortens the draw, and it never stops the others from decoding.

### Secondary attribute list

The secondary list exists only when the three chunk sizes `+0x0A`, `+0x0C`, `+0x0E` are not all
zero. It is then decoded like the primary list, from the slot-21 table over `u16 +0x0A` bytes at
slot 10's address, with `V` rows. Its own address gives the alignment phase. Errors:

- a nonzero second or third chunk size is *split input chunks*, which is unsupported;
- configuration byte 3 other than 255 is *built-in secondary*, which is unsupported;
- a read failure of the slot-21 table or of the slot-10 data, or a failure to parse the table
  header and declarations, fails the secondary list as a whole.

Nothing else fails the whole list. A declaration whose decode fails (unsupported codec, extent,
unclassified semantic, normal-codec refusal) is recorded as that declaration's failure, exactly as
in the primary list. The other declarations still decode.

### Looking up a semantic

The bank for semantic `x` is chosen as follows:

1. If the primary list exists and has any declaration with semantic `x`, use the lowest-ordinal
   one. If it failed, the lookup fails: a later successful duplicate never replaces an earlier
   failure.
2. Otherwise, if the secondary list exists, apply the same rule to it. A secondary list that failed
   as a whole fails the lookup.
3. Otherwise the semantic is missing.

Later declarations with the same semantic are kept as aliases. They are reachable by ordinal, never
by semantic lookup.

### Auxiliary attribute table

Partition byte `+0x112` gives the entry count and `+0x111` the stride. Entries are at `+0x118`.
Each entry follows the output-layout entry format, with byte 0 the array offset and byte 5 the
vertex-array slot.

The array's GPU format comes from the mesh's vertex-state record (656 bytes at `u32 mesh+0x3C`), not
from the entry. Its location and offset come from `u32 mesh+0x4C`:

- bit 31 is the location;
- bits 0–30, plus the sum of all earlier partitions' `u32 +0x10` sizes (each rounded up to 16),
  plus the entry's byte 0, give the offset.

This table is implemented by `plan_auxiliary_bindings`, and its rows by
`decode_auxiliary_attribute`, both in `mag-render-adapter-core`. Their vertex fetch is the RSX
format, not the descriptor codecs above.

## Edge cases and errors

- A stream control with bits 16–31 set: *unsupported control*, naming the slot.
- A configuration stream under 16 bytes, a missing position declaration, or an unsupported
  position codec: the partition has no geometry.
- An index of `V` or more: *source extent*.
- `V = 0`: every bank is empty and no bytes are read. `K = 0`: no indices.
- Integer overflow while computing any extent: *source extent*.

## Tolerance

| Output | Tolerance | Reason |
| --- | --- | --- |
| Codecs 0 and 2, `f32` lanes | bit-exact | data copy |
| Rebased halves (codec 3, position codec 6) | bit-exact | exact by construction |
| Codecs 5 and 7 | bit-exact | small integers are exact in binary32 |
| Codecs 1, 4, 6 | within 1 ulp of the exact rational value stated | the game's own conversion truncates slightly different constants; one binary32 ulp is far below any visible effect on positions, normals or weights |
| Codec 9 | as in [geometry-normals.md](geometry-normals.md) | |
| Defaulted lanes (`+0`, `1.0`) | bit-exact | constants |

**Consequences of non-bit-exact output.** Decoded banks feed deformation and vertex packing. If two
passes decode the same partition and compare depths for equality, they must use the same decoder
build. The rules in
[group-zero-deformation.md](group-zero-deformation.md#consequences-of-non-bit-exact-output) apply
here unchanged. A cache of decoded banks must key on the source bytes and codec parameters, never
on decoded values.

## Agreement with `mag-render-adapter-core`

- `PartitionSource` and `PartitionChain` (`mesh_sources.rs`) read the 544-byte record and follow
  `+0xF0` with cycle detection, as above. `MeshSource::main_geometry_source` computes the first
  partition from `+0x50` and the resource base, as above.
- `plan_noninstanced_inputs` (`partition_inputs.rs`) builds the 24-slot table with the same length
  and address replacements for slots 3, 4, 10–12, 13–16, 17 and 18, and the same
  unsupported-control rule. It does not yet apply the large-stream rule: a `0x4000` control reads
  16 KiB.
- `geometry_bindings.rs` implements the output layouts, the auxiliary table and the RSX vertex
  fetch. The descriptor codecs and the fixed position codecs above have no public reader yet.

## Synthetic test vectors

1. **Slot read.** Slot 7 control `0x30`, address `0x10000`: 48 bytes from `0x10000`. Control
   `0x10000`: *unsupported control* (slot 7). Control 0: empty, no read.
2. **Palette replacement.** Palette base `0x20000`, `u16 +0x14 = 0x40`, slot-3 control `0x60`:
   segment 0 is the 96 bytes at `0x20040`, whatever the slot-3 address word says.
3. **Large stream.** Position codec 0, `V = 2000`, so 24000 bytes are needed. Slot-7 control
   `0x4000`: read 24000 bytes. With control `0x3FF0`: read `0x3FF0` bytes, and the position decode
   fails with *source extent*. Control `0x4000` with `V = 1000` (12000 needed): read 16384 bytes.
   Index slot control `0x4000` with `K = 9000`: read 18000 bytes.
4. **Codec 0 position.** Row bytes `3F800000 40000000 C0400000`: `(1, 2, −3, 1)`.
5. **Rebased half.** `0x3C00` → `1.0` (`0x3F800000`). `0x8000` → `−0.0`. `0x0001` →
   `2^-15 · (1 + 1/1024)` (`0x38002000`). `0x7C00` → `65536.0` (`0x47800000`), not infinity.
6. **Codec 1.** `v = 32767` → `1.0`; `v = −32768` → `−1.0`; `v = 0` → `1/65535` (`0x37800080`);
   `v = −1` → `−1/65535`. The game's truncation gives `0x3F7FFFFF` for `v = 32767`, which is within
   tolerance.
7. **Codec 4.** `255` → `1.0`; `128` → `0.5019608` (`0x3F008081`, within 1 ulp); `0` → `+0`.
8. **Codec 6.** Word `0x002003FF` (x = 1023, y = −1024, z = 0) → `(1.0, −1.0, 1/1023, 1.0)`, with
   z ≈ `0x3A802008`.
9. **Codecs 5 and 7.** Codec 5 field `0xFFFE` → `−2.0`. Codec 7 byte `0xC8` → `200.0`.
10. **Descriptor table.** Header `02 10 …` (two declarations, stride 16). Declaration 0
    `00 02 03 01 0C 00 00 00` (float position, three components). Declaration 1
    `0C 04 04 09 04 00 00 00` (unsigned-byte weights, four components). Vertex bytes
    `3F800000 00000000 3F000000 FF 80 00 40` at a 16-aligned address. Position `(1, 0, 0.5, 1)`;
    weights `(1.0, 0.5019608, 0, 0.2509804)`.
11. **Position component count.** As vector 10 with declaration 0 byte 2 = 2: position
    `(1, 0, +0, 1)`.
12. **Primary first.** Primary list: semantic 2 fails at ordinal 1 and succeeds at ordinal 2.
    Secondary list: semantic 2 succeeds. Lookup of semantic 2 fails with the ordinal-1 error.
    Lookup of semantic 3, present only in the secondary list, returns the secondary bank.
13. **Index range.** `V = 2`, `K = 3`, indices `0, 1, 2`: *source extent*.
14. **Misaligned stream.** Slot-7 address `0x10004`, `S = 16`, `O = 0`: vertex 0 starts at guest
    address `0x10000`, before the stream, so the declaration fails with *source extent*.
15. **Unsupported codecs.** Declaration codec 8, semantic 2: that declaration fails with
    *unsupported encoding*, and the others still decode. Codec 8 on the position declaration:
    *unsupported position encoding*, and the partition has no geometry.
16. **Secondary admission.** `+0x0A = 0x40`, `+0x0C = 0x10`: *split input chunks*. With `+0x0C = 0`
    and configuration byte 3 = 0: *built-in secondary*.
17. **Control check scope.** Slot 10's control word is `0x00010000` and `u16 +0x0A = 0x40`: no
    error, and 64 bytes are read. Slot 5's control word is `0x00010000`: *unsupported control*
    (slot 5).
18. **Secondary per declaration.** Secondary table with declaration 0 codec 8 (unsupported) and
    declaration 1 codec 2: the list succeeds, with declaration 0 failed and declaration 1 decoded.
    Slot-21 table unreadable: the whole list fails.
19. **Packed position below three components.** Semantic 1, codec 6, `c = 1`, word `0x002003FF`:
    `(1.0, 0, 0, 1.0)`. The 4-byte field must fit in the stream even though only x is kept.

## Open questions

1. **Large descriptor-driven streams.** The current adapter applies the 16 KiB rule only to fixed
   position codecs and the index list. With descriptor-driven attributes, a `0x4000` control reads
   16 KiB, so a partition with more data fails with *source extent*. This specification extends the
   rule to the furthest declaration byte. That never changes a result that decoded before; it only
   turns some extent failures into decoded data. Whether retail has such partitions is unmeasured.
2. **Misaligned streams.** The guest-address rule `align_down_16(A + L) + (L mod 16)` reproduces the
   current reference for stream addresses that are not 16-byte aligned. Whether retail ever places
   a stream at such an address is unmeasured. Every observed stream is aligned, and for those the
   rule is plain `A + L`.
3. **Lanes beyond the component count** for non-position semantics are reproduced as decoded data.
   No consumer is known to read them. If none does, an implementation could default them instead;
   that needs evidence first.
4. **Slots 2, 6, 8, 9, 22 and 23** have no consumer in this specification, and their roles are not
   established.
5. **Built-in secondary layouts** (configuration byte 3 other than 255) and split secondary chunks
   are refused. Their formats are not specified.
