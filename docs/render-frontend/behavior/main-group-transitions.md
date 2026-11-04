# Main fixed-function state across mesh groups

## Scope

Main draws its items in ascending mesh-group order (groups 0..15). Entering a group changes
fixed-function render state. This specification states, for each group boundary, which pipeline
fields become which values, plus the state applied at the start of Main, per primary material, and
at a pass-boundary restore. It describes the *resulting state*, not how or in what order the game
writes it. Applies to retail v1.0.

The pipeline fields are those of [material-setup.md](material-setup.md) section 3. Every field is
either known (with a value) or unknown. Fields a rule does not mention keep their previous
knowledge, including unknown.

Out of scope: the producer side effects that also happen at some boundaries (setup banks,
group-12 textures and group-15 work; see [scene-plan.md](scene-plan.md)), viewport and scissor.

## Inputs

From the Main pass setup record (see [material-setup.md](material-setup.md)): lists enabled,
depth control, and the setup-bank references. From the Main pass object:

| Offset | Type | Meaning |
| --- | --- | --- |
| `+0x114` | `u32` | stencil use: nonzero top byte (`value >> 24 != 0`) |
| `+0x220` | `u32` | shader packer / sRGB output use: nonzero |
| `+0x19c` | 2 × 4 `u32` | two vertex-constant rows used from group 12 onward ("water constants") |
| `+0x40`, `+0x44`, `+0x48` | `u32` | three default words carried for other consumers |

The water rows are copied as raw words, row 0 from `+0x19c..+0x1ab` and row 1 from
`+0x1ac..+0x1bb`, each row as four big-endian words.

A primary material contributes its flags word (`u32 material+0x58`; bits 0, 1 and 11 are used
below) and its alpha byte (`u8 material+0x5C`), located as in
[material-sources.md](material-sources.md), and the pass has a render mode
(0..15). Those values reach this specification already decoded.

## Outputs

The updated pipeline state, or an error: `GroupOrder`, `RenderMode`, or
`ExternalSetupReference(target)`.

## Behaviour

Named values used below:

- `ALPHA_BLEND(additive)`: blend factors become source colour = source alpha, source alpha =
  source alpha, destination colour = destination alpha = (one if `additive`, else one minus
  source alpha). Blend enable is not changed by this macro.
- `MAIN_DEFAULTS`: depth test on, depth write on, depth comparison less-or-equal, blend off,
  alpha test off, all four colour masks `0b1111`.
- `BOUNDS_RANGE`: depth bounds range `[0.46, 1.0]`, carried as the binary32 words
  `0x3eeb851f`, `0x3f800000`.
- `STENCIL_MARK`: stencil comparison less-or-equal, reference 255, read mask 0, write mask 255,
  fail keep, depth-fail keep, pass replace.

### Transition from group `old` to group `new`

`old` is −1 (before the first item) or 0..15, `new` is 0..15. `new < old`, `old` outside −1..15 or
`new > 15` is a `GroupOrder` error. A group `g` is *crossed* when `old < g <= new`.

The planner must have already performed the producer side effects of every setup reference it is
about to cross. A state transition is refused with `ExternalSetupReference(target)` when lists are
enabled and any of these references is still nonzero: the three initial references when
`old == -1`; the group-13 reference when group 13 is crossed; either group-15 reference when group
15 is crossed. (The planner passes a copy of the sources with those references cleared after it
has emitted their effects.)

Then, on a copy of the state, apply in this order:

1. If `old == -1`: `MAIN_DEFAULTS`; and if lists are enabled, depth bounds on with `BOUNDS_RANGE`.
2. If the shader packer is in use and (`old == -1` or group 12 or 13 is crossed): sRGB output on.
3. If lists are enabled, for each crossed group in ascending order:

| Group | Resulting changes |
| --- | --- |
| 1 | depth bounds off; if depth control is 0, depth comparison equal |
| 3 | depth bounds on with `BOUNDS_RANGE` |
| 5 | depth bounds off; if stencil is in use: stencil on, front and back faces `STENCIL_MARK` |
| 8 | depth comparison less-or-equal; if stencil is in use: stencil off and the write mask of each *known* stencil face set to 0 (unknown faces stay unknown); colour masks `0b0111` for all four targets; `ALPHA_BLEND(false)`; depth write off; depth test on; blend on; alpha test off |
| 9 | depth test on; alpha test on, comparison greater, reference 0; cull off; `ALPHA_BLEND(false)`; blend on; depth write off; polygon offset fill on, factor −1.0 (`0xbf800000`), units −100.0 (`0xc2c80000`) |
| 10 | polygon offset fill off; cull on; colour masks `0b1111`; `ALPHA_BLEND(false)`; depth write off; depth test on; blend off |
| 11 | depth write on; blend off; alpha test off; colour masks `0b1111`; depth comparison less-or-equal |
| 12 | `MAIN_DEFAULTS`; depth test on; depth write on; blend off |
| 13 | cull on; depth write off; depth test on; blend off; `ALPHA_BLEND(false)`; colour masks `0b1111` |
| 15 | colour masks `0b0111`; `ALPHA_BLEND(false)`; depth write off; depth test on; blend on; alpha test off |

   Groups 2, 4, 6, 7 and 14 change no fixed-function state.
4. Commit the copy. On any error the state is unchanged.

When several groups are crossed at once (for example 0 → 12), all of their rows apply in ascending
order, so later rows override earlier ones field by field.

### Per primary material

`primary(flags, alpha, mode)`:

- `mode > 15`: `RenderMode` error, state unchanged.
- `mode == 14` or flags bit 1 set: no change.
- Otherwise:
  - if `alpha == 0` or `mode == 2`: alpha test off (comparison and reference unchanged);
  - else if `mode == 5`: comparison less, reference 255 (enable unchanged);
  - else: comparison greater-or-equal, reference `alpha` (enable unchanged);
  - cull enable = flags bit 0 clear;
  - if `mode == 4`: `ALPHA_BLEND(flags bit 11 set)` (bit `0x800`).

### Pass-boundary restore

Used only at a qualified pass boundary, never as a per-material reset. Sets: depth test on, depth
comparison less-or-equal, two-sided stencil off, stencil off, cull on, cull face back, blend off,
both blend equations add, `ALPHA_BLEND(false)`, alpha test off, alpha comparison greater, alpha
reference 0. Depth write, front face, depth bounds, polygon offset, colour masks, sRGB output and
texture units are untouched. It also reports a clear colour of `0xffffffff` for later clears; this
records the clear value only and is not itself a clear.

## Edge cases and errors

- A transition with `old == new` (other than −1) changes nothing.
- Unknown stencil faces are not invented at group 8; only known faces have their write mask
  cleared.
- With lists disabled, only steps 1 (defaults without bounds) and 2 apply.

## Tolerance

No arithmetic is performed. The depth-bounds and polygon-offset values are fixed binary32 words
stated above and are emitted as those exact words; this is a constant, not a computed result.

## Synthetic test vectors

Start from all-unknown state, lists enabled, depth control 0, stencil in use, shader packer in use,
references all zero.

1. `−1 → 0`: depth test on, depth write on, depth comparison less-or-equal, blend off, alpha test
   off, masks `[15, 15, 15, 15]`, depth bounds on `[0x3eeb851f, 0x3f800000]`, sRGB on; all other
   fields unknown.
2. Then `0 → 5`: crosses 1, 3, 5. Depth comparison equal (from 1), depth bounds off (5 overrides
   3's on), range still `[0x3eeb851f, 0x3f800000]`, stencil on, both faces `STENCIL_MARK`.
3. Then `5 → 8`: depth comparison less-or-equal, stencil off, both face write masks 0, masks
   `[7, 7, 7, 7]`, blend on with source alpha / one-minus-source-alpha, depth write off.
4. Then `8 → 9`: alpha test on, greater, reference 0, cull off, offset fill on, factor
   `0xbf800000`, units `0xc2c80000`.
5. From the state of vector 1, `0 → 12`: all of 1, 3, 5, 8, 9, 10, 11, 12 apply; final: offset fill
   off, cull on, masks `[15, 15, 15, 15]`, depth write on, depth test on, blend off, alpha test
   off, depth comparison less-or-equal, depth bounds off, stencil off, sRGB on.
6. `3 → 2`: `GroupOrder`. `−1 → 16`: `GroupOrder`.
7. Lists enabled with group-13 reference `0x100`, `11 → 13` without clearing it:
   `ExternalSetupReference(0x100)`, state unchanged.
8. `primary(flags 0x0801, alpha 0x40, mode 4)` on state with alpha test on: comparison
   greater-or-equal, reference `0x40`, alpha test still on, cull off, destination factors one.
   `primary(flags 0x02, …)`: no change. `primary(…, mode 16)`: `RenderMode`.

## Open questions

- The specific values (bounds 0.46..1.0, offset −1/−100, stencil reference 255, the group rows) are
  the observable render state the game sets; they are stated here as behaviour. Whether such
  game-chosen state values may appear in public specifications, or should instead be read from
  guest memory at run time, needs a ruling. No guest location holding them as data is known.
- Groups 14 and 15 have additional producer-driven behaviour not covered here.
