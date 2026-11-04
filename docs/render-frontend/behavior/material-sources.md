# Main item mesh, material, technique and microcode sources

## Scope

How the frontend finds the guest objects that describe one Main item: the mesh record, the
primary material, the selected technique chunk and the fragment microcode. It also covers which
bytes of the primary material carry its flags and alpha, and which mesh fields choose the
geometry source. Applies to retail v1.0 and to every mesh group (0–15). The rules are the same for
all groups; no group has its own material location rule.

Out of scope:

- decoding the vertex and fragment program objects inside the chunk (the published
  `material_programs` reader and [program-compilation.md](program-compilation.md));
- parameter and value composition ([material-values.md](material-values.md));
- textures;
- partition contents (the geometry stream specification);
- the item's transform slot.

## Inputs

All fields are big-endian.

### Item record (`MBI`, 32 bytes per cohort item)

| Field | Meaning |
| --- | --- |
| byte `+0x00` bits 7–4 | census group (must match the mesh group; [scene-plan.md](scene-plan.md)) |
| byte `+0x04` | technique selector byte `S` |
| bytes `+0x06..+0x08` | primary material address, bits 31–8 (bits 7–0 are zero) |
| `u32 +0x0C` | mesh index `I` |
| `u32 +0x14` | skinning palette word (geometry dispatch below) |
| byte `+0x1B` bit 0 | palette-in-place flag (geometry dispatch below) |
| `u32 +0x1C` bits 8–0 | primary material length in 16-byte units |

### Main pass object (`PASS`)

| Offset | Type | Meaning |
| --- | --- | --- |
| `+0xDC` | `i32` | render mode |
| `+0x190` | `u32` | material override mode; must be 0 for Main items |
| `+0x194` | `u32` | technique override mask |
| `+0x1FC` | `u32` | binding payload address; must equal `PASS` |
| `+0x284` | `u32` | asset-bank base. The same value is the geometry resource base. |

### Mesh record (192 bytes)

| Offset | Type | Meaning |
| --- | --- | --- |
| `+0x20` | 16 bytes | mesh constant row ([material-values.md](material-values.md)) |
| `+0x3C` | `u32` | vertex layout address |
| `+0x50` | `u32` | first partition, relative to the geometry resource base (bit 31 ignored) |
| `+0x9C` | `u32` | geometry flags; bit `0x1000` = static partitions |
| `+0xA2` | `u8` | recorded partition count (metadata, not a chain length) |
| `+0xBC` | `u16` | material group (0–15) |

### Primary material

| Offset | Type | Meaning |
| --- | --- | --- |
| `+0x38` | `u32` | technique resource base |
| `+0x44`, `+0x46` | `u16` | parameter records and values ([material-values.md](material-values.md)) |
| `+0x48` | `u16` | technique map base, a byte offset inside the material |
| `+0x4A` | `u16` | technique table base, a byte offset inside the material |
| `+0x56` | `u8` | material class ([material-values.md](material-values.md)) |
| `+0x58` | `u32` | primary flags word |
| `+0x5C` | `u8` | primary alpha byte |

Technique map record (32 bytes at `material + map_base + 32·selector`): byte `+1` is the technique
table index `t`. The same byte is the parameter-membership bit index in
[material-values.md](material-values.md).

Technique table entry (8 bytes at `material + table_base + 8·t`): `u32 +0` is the chunk offset
relative to the technique resource base, and `u32 +4` is the chunk's declared length in bytes.

## Outputs

For each item, or an error for that item:

- the mesh record (192 bytes) and its address;
- the primary material bytes and address;
- the technique `selector` and table index `t`;
- the technique chunk's address and bytes;
- the fragment microcode's address and bytes;
- the primary flags word and alpha byte;
- the geometry source.

## Behaviour

### Pass checks

Before any item is read:
- `PASS` must be nonzero and 4-byte aligned.
- `PASS+0x1FC` must equal `PASS`.
- If `PASS+0x190` is nonzero, every item fails with `UnqualifiedOverride(mode)`. Override materials
  are not specified here.

### Mesh record

`mesh address = (u32 MBI+0x0C) · 16`, computed modulo `2^32`. The mesh index counts 16-byte units
from guest address 0. Read 192 bytes there. A zero address is `NullMesh`. The mesh group is
`u16 mesh+0xBC`.

### Primary material

`material address = MBI[6]·2^24 + MBI[7]·2^16 + MBI[8]·2^8`, and
`material length = ((u32 MBI+0x1C) AND 0x1FF) · 16` bytes. Read exactly that many bytes. A zero
address or zero length is `NullResource`. Every offset below must lie inside the copied bytes;
otherwise the result is `MaterialBounds`. This rule applies to items of every mesh group.

### Technique selection

1. Initial selector:
   - mesh group 15: `S AND 7`;
   - otherwise, if the render mode (signed) is at most 15: `S >> 3`;
   - otherwise: 0.
2. `selector = initial AND NOT (u32 PASS+0x194)`.
3. Read the 32-byte map record at `material + u16(material+0x48) + 32·selector`. `t` is its byte
   `+1`.
4. Read the 8-byte table entry at `material + u16(material+0x4A) + 8·t`. `rel` is its first word and
   `len` its second.
5. `transfer = (len + 15) AND NOT 15`. If `len + 15` overflows 32 bits, the result is
   `MaterialBounds`.
6. `base = u32 material+0x38`. Zero is `NullResource`.
7. The chunk is the `transfer` bytes at `base + rel`, computed modulo `2^32`.

### Technique chunk structure

The chunk is a tree of records linked by self-relative offsets. Each link is a `u32`, measured from
the start of the record that holds it, and must be nonzero, a multiple of 4, and point to a record
that fits inside the chunk.

| Record | Reached from | Size used |
| --- | --- | --- |
| root `A` | chunk `+0` | 16 |
| `B` | `A+12` | 20 |
| vertex descriptor `VD` | `B+12` | 20 |
| vertex program object | `VD+16` | 24-byte header, then its constants and instructions |
| fragment descriptor `FD` | `B+16` | 20 |
| fragment program object `FP` | `FD+16` | 24-byte header |

No two of these regions may overlap. The regions are: the first 4 bytes, `A`, `B`, `VD`, `FD`, both
object headers, and the vertex constant registers, values and instructions. Decoding the two objects
is the published `material_programs` contract. This document uses only the fragment object's
microcode length (`u32 FP+0`, nonzero) and its storage word (`u32 FP+12`).

### Fragment microcode (asset bank)

The storage word's low two bits are a tag:

- **tag 2 (asset-bank offset):** `offset = storage AND NOT 3`. The microcode address is
  `u32 PASS+0x284 + offset`. If that sum overflows 32 bits, the result is `ParameterBounds`. Read
  `align16(length)` bytes there. The first `length` bytes are the microcode; the rest are transfer
  padding.
- **any other tag:** the item fails with `UnqualifiedFragmentReference`. Other storage kinds are
  not specified.

### Primary flags and alpha

The per-primary transition in [main-group-transitions.md](main-group-transitions.md) takes the
32-bit word `u32 material+0x58` as its flags and the byte `material+0x5C` as its alpha. That spec
calls the flags a "byte", but the whole word is meant; it uses bits 0, 1 and 11 (`0x800`). Other
specs use further bits of the same word:
- `0x10`: atlas, [material-values.md](material-values.md);
- `0x20`: skinned geometry, below;
- `0x8000`: forced binding row, [material-values.md](material-values.md).

### Geometry source

With `G = u32 PASS+0x284` (the geometry resource base) and
`first = ((u32 mesh+0x50) AND 0x7FFFFFFF) + G` (modulo `2^32`), the cases are tried in this order:

1. mesh group 0: **group-zero partitions** starting at `first`;
2. `mesh+0x9C AND 0x1000` set: **static partitions** at `first`;
3. transform byte `+0x6F ≠ 1` and primary flags bit `0x20` set: **skinned partitions** at `first`.
   The palette address is `(u32 MBI+0x14) AND NOT 15` if `MBI+0x1B` bit 0 is clear, otherwise 0
   (see [skinning-deformation.md](skinning-deformation.md));
4. otherwise: **direct draw**, instanced exactly when transform byte `+0x6F = 1`.

The recorded count `mesh+0xA2` is carried as metadata only.

## Edge cases and errors

| Condition | Result |
| --- | --- |
| `PASS` zero or misaligned, or `PASS+0x1FC ≠ PASS` | the pass fails (`InvalidPass`) |
| `PASS+0x190 ≠ 0` | every item fails (`UnqualifiedOverride`) |
| mesh address 0 | item fails (`NullMesh`) |
| material address 0 or length 0 | item fails (`NullResource`) |
| map record, table entry or field outside the material bytes | item fails (`MaterialBounds`) |
| technique resource base 0 | item fails (`NullResource`) |
| chunk link zero, misaligned, out of bounds, or overlapping regions | item fails (program error) |
| storage tag not 2 | item fails (`UnqualifiedFragmentReference`) |
| microcode address overflow | item fails (`ParameterBounds`) |
| any guest read out of range or short | item fails (read error) |

An item error does not stop other items. Addresses are formed modulo `2^32` before reading, and each
read then checks its whole range.

## Tolerance

No arithmetic on floating-point values. Every field is used or copied exactly.

## Synthetic test vectors

1. **Mesh.** `u32 MBI+0x0C = 0x00012345`: the mesh is 192 bytes at `0x00123450`. `0`: `NullMesh`.
   `0x10000001`: the address wraps to `0x00000010`.
2. **Material.** `MBI[6..8] = 12 34 50`, `u32 MBI+0x1C = 0x00000006`: 96 bytes at `0x12345000`.
   `u32 MBI+0x1C = 0x00008006` (bits above 8 set): still 96 bytes. `MBI+0x1C = 0x00000200`: length
   0, `NullResource`. The same result for a group-0, group-5 and group-11 mesh.
3. **Selector.** `S = 0x1A`. Mesh group 15 → 2. Group 5 with render mode 3 → 3; with render mode 16
   → 0; with render mode −1 → 3. Group 5, render mode 3, override mask `0x1` → `3 AND NOT 1 = 2`.
4. **Chunk.** Selector 2, `material+0x48 = 0x0100`, `material+0x4A = 0x0200`, map record at
   `+0x140` with byte `+1 = 5`, table entry at `+0x228 = (0x00004000, 0x00000123)`,
   `material+0x38 = 0x08000000`. Chunk: `0x130` bytes at `0x08004000`. With declared length
   `0xFFFFFFF8`: `MaterialBounds`. With `material+0x38 = 0`: `NullResource`.
5. **Material bounds.** As 4 with a material length of `0x140`: the map record at `+0x140` is
   outside the material bytes, so `MaterialBounds`.
6. **Microcode.** `PASS+0x284 = 0x09000000`, `FP+12 = 0x00001002`, `FP+0 = 0x44`: tag 2, offset
   `0x1000`, so read 80 bytes at `0x09001000`; the microcode is the first 68. `FP+12 = 0x00001001`:
   `UnqualifiedFragmentReference`. `PASS+0x284 = 0xFFFFF000` with offset `0x1000`:
   `ParameterBounds`.
7. **Primary state.** `material+0x58..0x5C = 00 00 08 01 40`: flags `0x00000801` (bit 0 set, so
   cull off; bit 11 set), alpha `0x40`.
8. **Geometry dispatch.** `G = 0x10000000`, `mesh+0x50 = 0x80000400`: `first = 0x10000400`.
   - Group 0 with static bit set → group-zero partitions.
   - Group 5 with `mesh+0x9C = 0x1000` → static partitions.
   - Group 11, `+0x9C = 0`, flags `0x20`, transform `+0x6F = 0`, `MBI+0x14 = 0x00ABCDE7`,
     `MBI+0x1B` bit 0 clear → skinned partitions, palette `0x00ABCDE0`. The same with transform
     `+0x6F = 1` → instanced direct draw.
9. **Override.** `PASS+0x190 = 2`: every item fails with `UnqualifiedOverride(2)`.

## Open questions

1. Override materials (`PASS+0x190 ≠ 0`) and fragment storage tags other than 2 are refused. Their
   sources are not specified.
2. The mesh index is an absolute address in 16-byte units. No mesh table is involved. Every
   observed item uses this form, but an index of 0 is only known to be invalid, not to have a
   meaning.
