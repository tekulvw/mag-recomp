# Main producer: depth downsample

## Scope

The depth-downsample producer runs at the group-15 transition (group-15 producer slot 1). It
renders a full-screen triangle into a dedicated depth target, writing depth from a fragment program
that samples the scene's depth-source texture. No colour is written.

This document relies on the framework in `main-effects.md` (surface records, selection, texture
descriptor fields, module constants `L`, `F`, `G`, `Zmin`, `Zmax`, the common producer draw
contract and the producer viewport). The depth-source descriptor is defined in `main-lighting.md`.

Applies to retail v1.0 only.

## Inputs

| Input | Source |
| --- | --- |
| Bank-zero state | `main-effects.md` |
| Downsample vertex and fragment programs | Program registries, entries named in `material-producers.md` (read only when bank zero is enabled). |
| Dedicated target record `Q` | 84-byte surface record at `R+0x2BAC`. |
| Depth-source descriptor `Dsrc` | 8 words at `R+0x3120` (bank zero enabled), as defined in `main-lighting.md`. |
| Producer texture binding for `Dsrc` | Supplied association (`main-effects.md`). |
| Surface binding for `Q` | Supplied association. |
| Current Main surface and its index | Carried planner state (`main-effects.md`). |

## Outputs

One pass with one draw, or nothing; plus state changes listed below.

## Behaviour

1. If bank zero is disabled: no-op.
2. If the downsample vertex or fragment program is missing: no-op. This check comes before any
   target change.
3. `Q` must have been captured, and its guest address must be exactly `R+0x2BAC`. Otherwise error.
4. Remember the current surface index `i0`.
5. `Q`'s binding must be consistent (`main-effects.md`). It must have a depth image whose load
   operation is *load* and whose content is initialised; if it has a stencil image, that must be
   initialised. Otherwise error: the downsample writes into existing depth content and never
   creates it.
6. The depth image's packet metadata must have the binding's depth format, be 2D, single-sampled,
   and at least as large as the binding's physical size. Otherwise error.
7. A producer texture binding must exist whose descriptor address and words equal `Dsrc`'s
   (error otherwise). Its packet image must have that binding's format and dimensions, be 2D,
   single-sampled, one layer, and not a depth format (packet formats 2, 15 and 16). Otherwise the
   producer is *unsupported*: the fragment program reads the depth source as a floating-point
   colour value, and a depth-format view would not give that.
8. Texture unit 0 becomes `Dsrc` with min/mag set to NEAREST, and the image from step 7 is
   associated with unit 0.
9. Apply the programs as in the common contract. The fragment program may sample only unit 0.
10. Pipeline writes before the draw:

    | Field | Value |
    | --- | --- |
    | Depth test | enabled |
    | Depth write | enabled |
    | Alpha test | disabled |
    | Blending | disabled |
    | Face culling | disabled |
    | Colour write mask | all channels off |
    | Depth-bias clamp | 0 |

    Depth comparison, stencil, depth bounds, polygon offset and sRGB output are *not* written:
    they are inherited and must be known.
11. Draw the full-screen triangle with the producer viewport of `Q` (sample grid from `Q`'s AA
    mode, logical size from `Q`, physical size from `Q`'s binding).
12. The pass has no colour attachment (the colour write mask is all off, so omitting it is
    exact). Depth and stencil are `Q`'s binding attachments as supplied. The packet colour format
    is "none" (0) and the depth format is the binding's. The pass is associated with `Q`.

State after the draw:

- current attributes as after a triangle draw (`main-effects.md`);
- depth test enabled and depth comparison LESS_OR_EQUAL (the comparison used *during* the draw
  is the inherited one);
- if unit 0's words are known, unit 0 `CONTROL0 = 0`, other words kept;
- the current surface becomes `select(i0, 2)` with its jittered surface viewport
  (`main-producer-inputs.md`). Its binding must be consistent (error otherwise).

### Optional omission mode

An opt-in diagnostic mode omits the downsample GPU draw while keeping the CPU-visible state
consistent. It is off by default and must stay labelled as an approximation. In that mode, after
steps 1-4:

- the vertex program's embedded constants are still written to the vertex constant bank;
- unit 0 becomes `Dsrc` with min/mag NEAREST and `CONTROL0 = 0`, and no image is associated with
  unit 0;
- current attributes are as after a triangle draw;
- depth test enabled, depth write enabled, depth comparison LESS_OR_EQUAL, alpha test, blending
  and culling disabled, colour write mask all off; every other field is unchanged;
- the current surface is restored as above;
- an omission counter increments. No pass, item or draw is added, and no depth is written.

## Edge cases and errors

- Missing `Q` record while bank zero is enabled and both programs exist: error.
- `Q` at another address: error.
- `Zmin`, `Zmax` not 0 and 1: viewport error.
- Non-finite `L`, `F` or `G`: error.
- `Q` with an unsupported AA mode: error.
- The fragment program sampling a unit other than 0, or the vertex program using inputs other
  than 0 and 8 or sampling textures: unsupported.

## Tolerance

The draw's viewport follows `main-effects.md`, "Tolerance". The depth values written are produced
on the GPU by the translated fragment program; the native depth target's precision is that of its
format.

### Consequences of non-bit-exact output

- Later passes compare depth against this target with LESS_OR_EQUAL and may use depth-bounds
  tests. If the shader Z scale or offset differs from `0.5` by more than the stated bound, written
  depth could differ by a format step and change equality ties. Compute them exactly from
  `Zmin = 0`, `Zmax = 1`.

## Synthetic test vectors

Fixture as in `main-effects.md`. `Q` at `R+0x2BAC = 0x32BAC`: width 160, height 90, depth format 2,
AA mode 0. Binding for `Q`: 160x90, depth image `d` of format 15 initialised with load *load*, no
stencil image. `Dsrc` at `0x33120`:
`[0x2000, 0x0001A522, 0x00030303, 0x80000000, 0x0000AAE4, 0x02020000, 0x00A0005A, 0]`.
Producer binding for `Dsrc`: image `c`, format 13 (R32 float), 160x90.

1. **Normal draw.** One pass: colour none, depth `d`, viewport `(0, 0, 160, 90)`, scissor
   `(0,0,160,90)`, shader Z scale 0.5 and offset 0.5. One draw with 3 indices. Unit 0 during the
   draw: `FILTER = 0x01010000`. Pass associated with `0x32BAC`, clip `(160, 90)`.
2. **After the draw.** Current attribute 0 `(-0.75, 2.5, -0.75, 1)`, attribute 8
   `(0, -0.75, 0, 1)`; unit 0 `CONTROL0 = 0`; depth comparison LESS_OR_EQUAL. With the current
   Main surface at table position role 1 index 1 before the producer, the current surface after it
   is `select(1, 2)` (`0x32D60 + 0x54 = 0x32DB4`).
3. **Inherited comparison.** Carried depth comparison GREATER before the producer: the draw's
   pipeline uses GREATER; after the producer it is LESS_OR_EQUAL.
4. **Depth-format source.** Producer binding image with format 15: unsupported.
5. **Uninitialised target.** `Q`'s depth not initialised: error.
6. **Disabled bank zero.** `R+0x329D = 0`: no pass, no state change.
7. **Missing programs.** Fragment program absent: no pass, no state change, current surface
   unchanged.
8. **Omission mode.** As 1 in omission mode: no pass; unit 0 `FILTER = 0x01010000`,
   `CONTROL0 = 0`, no image associated with unit 0; depth write enabled; colour mask all off;
   omission counter 1.

## Open questions

1. The dedicated record at `R+0x2BAC` is captured even when bank zero is disabled, because the
   depth-source descriptor used by lighting is derived from it. Whether `R+0x3140`'s descriptor
   always describes `Q` should be confirmed in `main-lighting.md`.
2. The depth-bias clamp is declared 0 for this draw while the other producers leave it inherited.
   The guest-visible reason (an explicit RSX write versus an assumption) is not established here.
