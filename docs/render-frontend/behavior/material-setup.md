# Main setup command banks and register knowledge

## Scope

Before and during Main, the game prepares small RSX command subroutines ("setup banks") in
IO-mapped main memory and references them from the Main pass's setup record. This specification
covers:

1. copying one setup bank out of guest memory into an owned, ordered list of method writes;
2. tracking which RSX registers those writes make *known*, and which writes are render side
   effects ("obligations") that a pass planner must still perform;
3. decoding the known registers into pipeline state, texture units and vertex constants;
4. copying the full set of setup banks named by one Main pass;
5. decoding an inherited register snapshot taken just before Main.

Applies to retail v1.0. A copied or analysed bank is never evidence that the game's own
preparation of that bank can be removed; every result carries that status (see "Outputs").

Out of scope: executing obligations (clears, draws, target changes; see
[scene-plan.md](scene-plan.md) and the producer specifications), the group transitions in
[main-group-transitions.md](main-group-transitions.md), and texture decoding beyond the register
assembly given here (see [texture-unit-controls.md](texture-unit-controls.md)).

## Inputs

### Guest reads

All reads go through one caller-supplied reader that copies `n` bytes at an IO offset in main
memory (location "main"). Each request either returns exactly `n` bytes or fails. The caller
charges every request, successful or not, to its own budget.

### Command-stream format (RSX FIFO, public interface)

Words are big-endian `u32`. A header word `h` is one of:

| Condition (checked in this order) | Meaning |
| --- | --- |
| `h == 0x00020000` | return |
| `h >> 29 == 1` | jump to `h & 0x1ffffffc` |
| `h & 3 == 1` | jump to `h & ~3` |
| `h & 3 == 2` | call `h & ~3` |
| `h >> 29` is 0 or 2 and `h & 3 == 0` | method packet (below) |
| anything else | invalid header |

A method packet has `count = (h >> 18) & 0x7ff` payload words following the header,
subchannel `(h >> 13) & 31` and first method `h & 0x1ffc`. With `h >> 29 == 0` the method
increments by 4 for each payload word; with `h >> 29 == 2` every payload word writes the same
method. A packet with `count == 0` (including the all-zero word) writes nothing.

### Setup record of a Main pass

The Main pass has a 160-byte setup record (its job user block). Fields used here, as offsets in
that record:

| Offset | Type | Meaning |
| --- | --- | --- |
| `+0x48` | `u16` | enabled-group mask |
| `+0x4c` | `u8` | lists enabled (nonzero = enabled) |
| `+0x4d` | `u8` | depth control |
| `+0x50`, `+0x54`, `+0x58` | `u32` | initial setup-bank references (IO offsets; 0 = none) |
| `+0x5c` | `u32` | group-13 setup-bank reference |
| `+0x60`, `+0x64` | `u32` | group-15 setup-bank references |
| `+0x68` | `u32` | auxiliary setup-bank reference |
| `+0x6c` | `u32` | restore setup-bank reference |

The record is decoded only when exactly 160 bytes were copied; otherwise the decode fails with a
bounds error. Other pass inputs decoded alongside it are listed in
[main-group-transitions.md](main-group-transitions.md).

### Inherited snapshot

An ordered list of (method, value) pairs produced by the runtime's register-snapshot interface,
covering exactly this method list in this order:

1. `0x220`, `0x304`, `0x308`, `0x30c`, `0x310`, `0x314`, `0x318`, `0x31c`, `0x320`, `0x324`,
   `0x328`, `0x348`, `0x370`, `0x380`, `0x384`, `0x388`, `0xa68`, `0xa6c`, `0xa70`, `0xa74`,
   `0xa78`, `0xa7c`, `0x1830`, `0x1834`, `0x183c`, `0x1fec`, `0x1738`, `0x173c`;
2. `0x32c + 4i` for `i = 0..6`, then `0x34c + 4i` for `i = 0..6`;
3. for each unit `u = 0..15`: `0x1a00 + 32u + 4i` for `i = 0..7`, then `0x1840 + 4u`,
   `0xb00 + 4u`, `0x1d00 + 4u`.

## Outputs

- `OwnedSetupBank { target, methods, reads, status }`: `methods` is the ordered list of
  (IO offset of the payload word, subchannel, method, value); `reads` is the ordered list of every
  (IO offset, bytes) copied; `status` is always "prepared by the retained game code" and never
  qualifies removal of that preparation.
- `KnownRegisters`: a sparse map from method to last written value, plus a sparse map from
  (vertex-constant row 0..511, lane 0..3) to value. Absence means *unknown*, never zero.
- An ordered list of obligations: (kind, the method write that caused it).
- Decoded pipeline state, texture units and a vertex-constant bank, each with per-field knowledge.
- `RetainedMainSetupBanks`: eight independent results (three initial, group 13, two group 15,
  auxiliary, restore).
- `InheritedMainState`: pipeline state, sixteen texture units, blend constant (`0x31c`), colour
  target enable (`0x220`), vertex base offset (`0x1738`) and vertex base index (`0x173c`).

## Behaviour

### 1. Copying a bank

`copy_setup_bank(target, word_budget, branch_budget)`:

- An address is *valid* when it is nonzero, a multiple of 4 and below `0x10000000`.
  An invalid `target` fails with `Target(target)`.
- Execution starts at `target` with no pending return. It keeps a set of visited
  (address, pending return) pairs. Reaching a pair already in the set fails with
  `ControlCycle(address)`. A subroutine called twice from different places is therefore not a
  cycle.
- Each word read (header or payload) costs one unit of `word_budget`; a read that would exceed
  the remaining budget fails with `Budget` before any guest access. A read whose start is invalid,
  or whose end exceeds `0x10000000`, fails with `Target(start)`. A reader failure is returned as
  `Source(error)`; a short read as `Extent`. The header is read with its own request; a packet's
  payload is read with one request of `count` words.
- Return, jump and call each cost one unit of `branch_budget`; with none left the copy fails with
  `Budget`.
- Return with a pending return address continues there and clears it. Return with no pending
  return ends the bank successfully.
- Jump continues at its target. Call records the address after the header as the pending return
  and continues at the target; a call while a return is already pending fails with `NestedCall`.
  An invalid jump or call target fails with `Target(target)`.
- An invalid header fails with `Header(h)`. An incrementing packet whose last method
  (`first + 4(count − 1)`) exceeds `0x1ffc` fails with `MethodExtent`.
- A method packet appends one entry per payload word to `methods`, in payload order, then
  continues after the payload. Address overflow anywhere fails with `Target`.
- Any failure returns no bank: no prefix escapes.

### 2. Applying a bank to known registers

`apply(bank)` processes every method write in order, on a copy of the current knowledge:

- A nonzero subchannel fails with `UnqualifiedMethod { subchannel, method }`.
- Each method is classified:

| Methods | Classification |
| --- | --- |
| `0x1d94` | obligation: clear |
| `0x1808` | obligation: primitive end when the value is 0, otherwise primitive begin |
| `0x180c`, `0x1810`, `0x1814`, `0x1818` | obligation: draw |
| `0x200..=0x2fc`, `0x8c0`, `0x8c4`, `0xa00..=0xa5c` | obligation: attachment (target, scissor, viewport) |
| `0xb80..=0xbfc`, `0x1d60`, `0x1d64`, `0x1e9c`, `0x1ea0`, `0x1ef8`, `0x1ff0`, `0x1ff4`, `0x1ff8` | obligation: program |
| `0x50`, `0x64`, `0x68`, `0x6c`, `0x1d6c`, `0x1d70`, `0x1fc0`, `0x1fd8` | obligation: synchronisation |
| `0x1a00..=0x1bfc`, `0x1840..=0x187c`, `0xb00..=0xb3c`, `0x1d00..=0x1d3c` | obligation: texture binding |
| `0x1680..=0x17bc`, `0x1e00..=0x1e40` | obligation: vertex input |
| `0x300`, `0x31c`, `0x368`, `0x36c`, `0x378`, `0x37c`, `0xa60`, `0xa64`, `0x1d8c`, `0x1d90` | obligation: untranslated state |
| `0x304..=0x318` (step 4), `0x320`, `0x324`, `0x328`, `0x32c..=0x344`, `0x348`, `0x34c..=0x364`, `0x370`, `0x380`, `0x384`, `0x388`, `0xa68..=0xa7c` (step 4), `0x1830`, `0x1834`, `0x183c`, `0x1fec`, `0x1efc`, `0x1f00..=0x1f7c` | state only, no obligation |
| anything else | fails with `UnqualifiedMethod { subchannel: 0, method }` |

  (`0x31c` is classified as untranslated state, not state only, even though it lies in the
  `0x304..=0x318` neighbourhood.)
- Vertex constant data (`0x1f00..=0x1f7c`): the load row is the currently known value of `0x1efc`
  (including writes earlier in the same bank); if unknown, fail with `MissingConstantLoad`. With
  `k = (method − 0x1f00) / 4`, the destination is row `load + k / 4`, lane `k % 4`. A row of 512 or
  more fails with `Value { method: 0x1efc, value: load }`. The load register is not advanced by
  these writes.
- A *non-incrementing* packet that targets a vertex constant data method with more than one payload
  word fails with `RepeatedConstantData { method }`, and knowledge is unchanged. Under the rule above every word would land in the same lane,
  and real hardware may instead advance through the constant window. The two readings disagree, so
  the frontend declines instead of guessing (see Open questions). A non-incrementing constant packet
  with exactly one word is accepted. Incrementing packets are unaffected.
- Every write, whatever its class, then becomes the known value of its method.
- Every obligation is appended to the result in write order.
- After all writes, the pipeline decode (section 3) must succeed on the new knowledge. Only then is
  the new knowledge committed. On any failure the previous knowledge is unchanged and no obligation
  list is returned.

`invalidate()` makes every register and every vertex-constant lane unknown.

### 3. Decoding known registers

Pipeline fields are each unknown when their source register is unknown. A *flag* is
`value != 0`.

| Field | Source and decoding |
| --- | --- |
| sRGB output | flag `0x1fec` |
| alpha test enable | flag `0x304` |
| alpha comparison | `0x308`, comparison code |
| alpha reference | `0x30c`, raw word |
| cull enable | flag `0x183c` |
| cull face | `0x1830`: `0x404` front, `0x405` back, `0x408` front and back |
| front face | `0x1834`: `0x900` clockwise, `0x901` counter-clockwise |
| blend enable | flag `0x310` |
| blend factors | known only when both `0x314` (source) and `0x318` (destination) are known; low 16 bits are the colour factor, high 16 bits the alpha factor |
| blend equations | `0x320`: low 16 bits colour, high 16 bits alpha |
| colour masks | known only when both `0x324` and `0x370` are known (below) |
| depth test enable | flag `0xa74` |
| depth write | flag `0xa70` |
| depth comparison | `0xa6c`, comparison code |
| depth bounds enable | flag `0x380` |
| depth bounds range | `[0x384, 0x388]` raw words, known only when both are |
| polygon offset fill | flag `0xa68` |
| offset factor, units | `0xa78`, `0xa7c` raw words |
| stencil enable | flag `0x328` |
| two-sided stencil | flag `0x348` |
| front stencil | the seven words at `0x32c` (below) |
| back stencil | the seven words at `0x34c` |

Codes:

- Comparison: `0x200` never, `0x201` less, `0x202` equal, `0x203` less-or-equal, `0x204` greater,
  `0x205` not-equal, `0x206` greater-or-equal, `0x207` always.
- Blend factor: `0` zero, `1` one, `0x300` source colour, `0x301` one minus source colour, `0x302`
  source alpha, `0x303` one minus source alpha, `0x304` destination alpha, `0x305` one minus
  destination alpha, `0x306` destination colour, `0x307` one minus destination colour, `0x308`
  source-alpha saturate, `0x8001` constant colour, `0x8002` one minus constant colour, `0x8003`
  constant alpha, `0x8004` one minus constant alpha.
- Blend equation: `0x8006` add, `0x8007` min, `0x8008` max, `0x800a` subtract, `0x800b` reverse
  subtract.
- Stencil operation: `0` zero, `0x1e00` keep, `0x1e01` replace, `0x1e02` increment, `0x1e03`
  decrement, `0x150a` invert, `0x8507` increment-wrap, `0x8508` decrement-wrap.

A stencil face at base `b` is known only when all seven of `b + 0`..`b + 24` are known: write mask
(`b`), comparison (`b + 4`), reference (`b + 8`), read mask (`b + 12`), fail operation (`b + 16`),
depth-fail operation (`b + 20`), pass operation (`b + 24`).

Colour masks are four 4-bit masks (bit 0 red, 1 green, 2 blue, 3 alpha) for render targets 0..3.
Target 0 comes from `0x324`: red is bit 16, green bit 8, blue bit 0, alpha bit 24. Target `i`
(1..3) comes from nibble `n = (value(0x370) >> 4i) & 0xf`, whose bit 0 is alpha, bit 1 red, bit 2
green and bit 3 blue.

Any known register holding a value outside its code table fails the whole decode with
`Value { method, value }`, where `method` is the register's method and `value` the offending code
(the 16-bit half for split registers).

Texture unit `u` (0..15; otherwise `Value { method: 0x1a00, value: u }`) is assembled as in
[texture-unit-controls.md](texture-unit-controls.md): the compact descriptor is known only when
all eight of its registers are known; border, texture-coordinate control and vertex LOD control
are taken individually.

The vertex-constant bank contains row `r` exactly when all four lanes of `r` are known.

### 4. Copying the banks of a Main pass

When the setup record says lists are enabled, each of the eight references is copied
independently with a budget of 8192 words and 32 branches. Each result is kept separately; a zero
reference gives `Target(0)` for that slot without affecting the others. When lists are disabled
no bank is copied.

### 5. Decoding an inherited snapshot

- The method list must equal the list in "Inputs" exactly and have as many values as methods;
  otherwise `Shape`.
- For `0x304`, `0x310`, `0x328`, `0x348`, `0x380`, `0xa68`, `0xa70`, `0xa74`, `0x183c` and
  `0x1fec` only bit 0 of the value is kept.
- All snapshot registers become known; the pipeline is decoded as in section 3, except that an
  invalid code in a register whose feature is *disabled* is tolerated: that register alone becomes
  unknown and decoding is retried. The controlling enable is: `0x304` for `0x308`; `0x310` for
  `0x314`, `0x318`, `0x320`; `0xa74` for `0xa6c`; `0x183c` for `0x1830`; `0x328` for
  `0x32c..=0x344`; for `0x34c..=0x364`, `0x328` when it is 0, otherwise `0x348`. If the
  controlling enable is not exactly 0, or the register is not one of these, the decode fails with
  `State(Value { method, value })`.
- The resulting state has the decoded pipeline, all sixteen texture units, and otherwise unknown
  material values. `blend_constant`, `color_target`, `vertex_base_offset` and `vertex_base_index`
  are the raw snapshot values.
- A consumer that uses prepared direct geometry requires both vertex bases to be zero; otherwise it
  fails with `DirectVertexBases { offset, index }`.

## Edge cases and errors

- Budgets are hard limits: exhausting either budget is an error, never a truncated success.
- A bank that never returns at top level ends in `Budget` or `ControlCycle`, never in success.
- A reader failure on the first request returns `Source` after exactly one request; there is no
  retry.
- Unknown or unsupported methods refuse the whole bank. A partially applied bank must not be
  observable.
- Unknown is distinct from zero throughout. Decoders must not substitute reset values for unknown
  registers.

## Tolerance

No floating-point arithmetic is performed. Depth-bounds and polygon-offset words, alpha
references and vertex-constant lanes are carried as raw guest words and remain bit-exact copies.

## Synthetic test vectors

Memory images list (IO offset: word).

1. Ordered state and obligations.
   `0x1000: 0x00080a70, 0x1004: 0, 0x1008: 1, 0x100c: 0x00041830, 0x1010: 0x405,
   0x1014: 0x00041d94, 0x1018: 0xf1, 0x101c: 0x00041808, 0x1020: 5, 0x1024: 0x00041814,
   0x1028: 0x02000000, 0x102c: 0x00041808, 0x1030: 0, 0x1034: 0x00020000`.
   Copy at `0x1000`, budget 14 words / 1 branch: methods
   `(0x1004, 0xa70, 0)`, `(0x1008, 0xa74, 1)`, `(0x1010, 0x1830, 0x405)`, `(0x1018, 0x1d94, 0xf1)`,
   `(0x1020, 0x1808, 5)`, `(0x1028, 0x1814, 0x02000000)`, `(0x1030, 0x1808, 0)`; 13 reads.
   With 13 words: `Budget`. With 0 branches: `Budget`.
   Apply to empty knowledge: obligations clear, primitive begin, draw, primitive end; depth test
   enabled, depth write disabled, cull face back, everything else unknown.
2. Call, non-incrementing packet and return.
   `0x2000: 0x00003002, 0x2004: 0x40080a70, 0x2008: 1, 0x200c: 0, 0x2010: 0x00020000,
   0x3000: 0x00040a74, 0x3004: 1, 0x3008: 0x00020000`. Copy at `0x2000`: methods
   `(0x3004, 0xa74, 1)`, `(0x2008, 0xa70, 1)`, `(0x200c, 0xa70, 0)`; three branches used. Applied:
   depth write disabled.
3. Errors. Target `0` or `0x1002`: `Target`. `0x4000: 0x00004001`: `ControlCycle(0x4000)`.
   `0x2000: 0x00003002, 0x3000: 0x00004002`: `NestedCall`. `0x5000: 0x20006000, 0x6000:
   0x00020000`: success, no methods. `0x5000: 0x00000003`: `Header(3)`. `0x5000: 0x60000000`:
   `Header`. `0x5000: 0x00081ffc`: `MethodExtent`. `0x5000: 0x00000000` with a 1-word budget:
   `Budget`.
4. Vertex constants. Methods `0x1efc = 510`, then an incrementing 8-word packet at `0x1f00` with
   values 1..8: rows 510 = `[1, 2, 3, 4]`, 511 = `[5, 6, 7, 8]`, row 0 unknown. With `0x1efc =
   511`: `Value { method: 0x1efc, value: 511 }`, knowledge unchanged. A `0x1f00` write without any
   `0x1efc`: `MissingConstantLoad`. With `0x1efc = 4`, a non-incrementing packet at `0x1f00` with
   one word `7`: row 4 lane 0 = 7. With two words: `RepeatedConstantData { method: 0x1f00 }`,
   knowledge unchanged.
5. Colour masks. `0x324 = 0x01000100`, `0x370 = 0x00001230`: masks `[10, 9, 1, 8]`.
6. Blend. `0x314 = 0x03030302`, `0x318 = 0x00010001`, `0x320 = 0x800b8006`: colour source
   source-alpha, alpha source one-minus-source-alpha, both destinations one; colour equation add,
   alpha equation reverse subtract. `0x320 = 0x80098006`: `Value { method: 0x320, value: 0x8009 }`
   and the apply is rejected.
7. Unqualified writes. A packet with subchannel 1 for `0xa70`:
   `UnqualifiedMethod { subchannel: 1, method: 0xa70 }`. Method `0x1234`:
   `UnqualifiedMethod { subchannel: 0, method: 0x1234 }`. In both cases knowledge is unchanged.
8. Border survives compact updates. An 8-word incrementing packet at `0x1a00` (values `w0..w6`,
   border `0x11223344`), then `0x1840 = 8`, then a 7-word packet at `0x1a00`: unit 0 has the new
   compact words, border `0x11223344`. After `invalidate()`, the border is unknown.
9. Inherited snapshot. All values zero except `0x1834 = 0x901` and `0x308 = 0x999`. The zero
   codes in `0x320`, `0xa6c`, `0x1830` and both stencil comparisons are invalid but their features
   are disabled, as is alpha test, so the decode succeeds: front face counter-clockwise; alpha
   comparison, blend equations, depth comparison, cull face and both stencil faces unknown; blend
   factors zero/zero; all sixteen texture units have known all-zero compact descriptors (disabled).
   With `0x304 = 3` instead of 0: bit 0 is kept, alpha test is enabled, and the decode fails with
   `State(Value { method: 0x308, value: 0x999 })`. With `0x1834 = 0`: fails with
   `State(Value { method: 0x1834, value: 0 })`, because front face has no controlling enable.

## Open questions

- Hardware advances the vertex-constant load row after constant data writes in some
  configurations. The behaviour above does not advance it, which matches every bank form so far
  observed. A bank that writes constants with two packets and no intervening `0x1efc` write would
  expose the difference; whether retail produces one is unmeasured. Until settled, a replacement
  should refuse a second constant-data packet that is not preceded by a `0x1efc` write in the same
  bank, rather than guess. A single non-incrementing constant packet with several words raises the
  same question within one packet; it is refused (`RepeatedConstantData`) for the same reason.
- The obligation classes are an analysis aid. Whether every "state only" register is safe to
  apply out of order relative to the obligations is a planner question, not settled here.
