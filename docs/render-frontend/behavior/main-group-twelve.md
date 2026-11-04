# Main group-twelve texture and state setup

## Scope

When Main item processing first reaches group 12 and the Main setup record has lists enabled
(event order: [scene-plan.md](scene-plan.md)), fixed texture units and depth state are prepared for
the group-12-and-later items, typically refractive and water surfaces. There is no draw, clear or
target change. Applies to retail v1.0.

Writing vertex constant rows 46 and 47 at the same threshold is part of the group transition in
[scene-plan.md](scene-plan.md) and [material-setup.md](material-setup.md), not of this setup.

## Inputs

All offsets are from `PASS`, the guest address of the Main pass object for this invocation
([material-setup.md](material-setup.md)).

| Source | Content |
| --- | --- |
| `u32 PASS+0x110` | address of a 48-byte full texture descriptor for unit 11 |
| `u32 PASS+0x1BC` | address of a 32-byte compact texture descriptor for unit 12 |
| `u32 PASS+0x1C0` | address of a 32-byte compact texture descriptor for unit 13 |

Descriptor formats are in [main-producer-inputs.md](main-producer-inputs.md). These sources are
read only when lists are enabled. `PASS` must be nonzero and 4-byte aligned, and `PASS+0x1C4` must
not overflow. Each pointed-to address must be nonzero and 4-byte aligned.

The texture-unit state and fixed-function state inherited from earlier Main work are also inputs.

## Outputs

- Texture units 11, 12 and 13: new descriptors, and any previous image association cleared.
- Texture unit 14: unit disabled if its descriptor is known.
- Fixed-function state.
- Three *image requests*, each `(unit, source address, compact descriptor)`, which the caller
  resolves to images.

## Behaviour

1. Apply the full descriptor at `*(PASS+0x110)` to unit 11. Unit 11's compact descriptor becomes
   words 0–6 plus word 8, and its border colour, texture-coordinate control and vertex LOD control
   come from words 7, 9 and 10.
2. Set unit 12's compact descriptor to the eight words at `*(PASS+0x1BC)`, and unit 13's to those at
   `*(PASS+0x1C0)`.
3. If unit 14's compact descriptor is known, set its `CONTROL0` word to 0. Its other words, and its
   border and coordinate state, are unchanged. An unknown unit 14 stays unknown.
4. Forget any image previously associated with units 11, 12 and 13.
5. Set: colour write mask all channels on every target; depth test on; depth write on; depth
   comparison "less or equal"; blending off; alpha test off.
6. Produce the image requests:
   - unit 11: source address `*(PASS+0x110)`, descriptor = unit 11's compact descriptor;
   - unit 12: source address `*(PASS+0x1BC)`, descriptor = those eight words;
   - unit 13: source address `*(PASS+0x1C0)`, descriptor = those eight words.

The caller binds each request to the image whose provenance has exactly that source address and
descriptor, after checking the image's format and size against the binding. When no such image
exists, the unit stays without an image. That is not an error here. A later draw that actually
samples the unit then fails when it collects its textures.

### Which units later items need

The image requests matter only if later items sample those units. For every Main item, in item
order, whose mesh group is 12 or above and whose route is "prepare" ([scene-plan.md](scene-plan.md)),
take the set of texture units its fragment program samples and intersect it with {11, 12, 13}.
The union over all such items is the set of group-twelve images that must be importable. Items in
groups 0–11, and items not routed for preparation, contribute nothing. If lists are disabled the
set is empty. A fragment program that cannot be parsed is an error, not an empty set.

## Edge cases and errors

- A source address of zero, or a descriptor of the wrong length: error, no state change.
- The group-twelve sources are missing although lists are enabled: error.
- The group-twelve sources belong to a different pass object than the one being planned: error.

## Tolerance

No arithmetic. Every word is copied bit for bit.

## Synthetic test vectors

1. **Full to compact.** Unit 11 full words `w0..w11 = 0x10, 0x11, …, 0x1B`. Expected compact
   `(0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x18)`, border `0x17`, texture-coordinate control
   `0x19`, vertex LOD control `0x1A`. Word 11 is unused.
2. **Unit 14 disable.** Unit 14 known with `CONTROL0 = 0x80000000` and `OFFSET = 0x1000`. After:
   `CONTROL0 = 0`, `OFFSET = 0x1000`. Unit 14 unknown: still unknown.
3. **State.** Before: depth test off, blend on, colour mask alpha only. After: depth test on, depth
   write on, comparison "less or equal", blend off, alpha test off, all channels written.
4. **Requests.** `PASS+0x1BC → 0x2000`, `PASS+0x1C0 → 0x2040`. Requests: `(12, 0x2000, …)` and
   `(13, 0x2040, …)`. The unit-11 request carries the full record's address and its compact
   descriptor (word 7 of the request is full word 8).
5. **Required units.** Items: group 3 sampling unit 11; group 12 sampling units 0 and 11; group 13
   sampling units 12 and 14. Required set = {11, 12}.
6. **Unparseable program.** A group-12 prepare item with an empty fragment program. Expected: error.
7. **Null source.** `PASS+0x1C0` holds 0. Expected: error.
