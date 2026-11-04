# Render frontend behaviour specifications

These documents are the specification half of the clean-room process in
[native-graphics-reimplementation-method.md](../../native-graphics-reimplementation-method.md).
Each one states, in our own words, what a native replacement must produce from guest-visible
inputs. An implementer works only from these documents and never from original code, original
documentation or captures.

## Index

### Main producers

| Specification | Behaviour | Status |
| --- | --- | --- |
| [material-producers.md](material-producers.md) | Guest data the producers read: globals, renderer and parameter objects, program registries | Complete |
| [main-producer-inputs.md](main-producer-inputs.md) | Surface selection, viewports, descriptor-to-surface association, required surfaces | Complete |
| [main-effects.md](main-effects.md) | Producer event routing, common full-screen draw contract, stencil-mask producer | Partial |
| [main-lighting.md](main-lighting.md) | Lighting producer: camera inverse, texture transforms, depth bounds, draws | Complete |
| [main-fog-values.md](main-fog-values.md) | Fog constants, program variant and depth bounds | Partial: prepared rows are game-supplied |
| [main-fog.md](main-fog.md) | Fog producer draws and state | Complete |
| [main-group-twelve.md](main-group-twelve.md) | Group-12 texture units and depth state | Complete |
| [main-quincunx.md](main-quincunx.md) | Group-15 quincunx resolve | Partial: known filtering approximation |
| [main-downsample.md](main-downsample.md) | Group-15 depth downsample | Partial |
| [main-finish.md](main-finish.md) | End-of-Main transitions and final state | Partial |

### Deformation

| Specification | Behaviour | Status |
| --- | --- | --- |
| [geometry-normals.md](geometry-normals.md) | Three-byte packed normal format | Complete |
| [skinning-deformation.md](skinning-deformation.md) | Group-11 skinning | Complete; two cases may be refused |
| [noninstanced-deformation.md](noninstanced-deformation.md) | Group-3 wind and group-4 ripple | Complete (group 3 uses this implementation's own tuning values) |
| [group-zero-deformation.md](group-zero-deformation.md) | Group-0 instance expansion and wind | Complete (own tuning values) |

### Planning and setup

| Specification | Behaviour | Status |
| --- | --- | --- |
| [texture-unit-controls.md](texture-unit-controls.md) | Texture unit decoding and native sampler translation | Complete |
| [material-setup.md](material-setup.md) | Setup command banks and register knowledge | Complete |
| [main-group-transitions.md](main-group-transitions.md) | Fixed-function state across mesh groups | Complete |
| [scene-plan.md](scene-plan.md) | Main scene planning | Complete |

### Planner dependencies

These four specifications are the next implementation batch. Implement them before
[scene-plan.md](scene-plan.md), which depends on all four. The three source specifications below
supply their guest inputs.

| Specification | Behaviour | Status |
| --- | --- | --- |
| [material-values.md](material-values.md) | Material value composition: constants, current attributes, per-item rows | Complete |
| [program-compilation.md](program-compilation.md) | Program compilation contract, fragment inline constants, uniform layouts, cache identities | Complete |
| [vertex-inputs.md](vertex-inputs.md) | Vertex-input selection, effective array mask and interleaving | Complete |
| [raster-state.md](raster-state.md) | Raster-state translation into the packet ABI | Complete |

### Guest sources

| Specification | Behaviour | Status |
| --- | --- | --- |
| [material-sources.md](material-sources.md) | Mesh record, primary material, technique chunk and selection, fragment microcode, primary flags and alpha, geometry-source dispatch | Complete |
| [geometry-streams.md](geometry-streams.md) | Partition stream roles, attribute codecs, counts and the large-stream rule | Complete |
| [fragment-builtins.md](fragment-builtins.md) | Fragment built-in rows | Complete (kind 31 uses own tuning values) |

Each document's "Open questions" section lists what is still undecided. A *Partial* status means
some behaviour depends on evidence or a ruling that is not yet available; the document says which.

## Conventions

Every specification uses the same sections:

1. **Scope**: the behaviour covered, the retail build it applies to, and what is out of scope.
2. **Inputs**: guest-memory data the behaviour reads, stated as data-format facts (offset, type,
   meaning). Offsets are relative to a named guest object, never to an original stack frame or
   SPU local store.
3. **Outputs**: scene-packet fields, shader constants, texture bindings or RSX state that result.
4. **Behaviour**: what the outputs are as a function of the inputs, including every case.
5. **Edge cases and errors**: zero, denormal, NaN and infinite inputs, out-of-range values,
   missing objects, and when the replacement must decline instead of guessing.
6. **Tolerance** (where floating-point results are produced): implementations use ordinary
   native arithmetic. They do not reproduce the original program's evaluation order or
   fused-multiply-add sequence, and no output is required to match bit-exactly. Each computed
   output has a stated maximum absolute or relative error (or ULP bound), chosen from what is
   visually and functionally acceptable. A "Consequences of non-bit-exact output" note lists any
   downstream place that previously relied on exact equality (cache keys, equality checks,
   depth-test ties) so the implementer can handle it. Values that are passed through unchanged
   from guest memory remain bit-exact copies; that is data movement, not arithmetic.
7. **Synthetic test vectors**: hand-chosen inputs and the outputs computed from the stated
   formulas. They are acceptance tests for the implementation, not samples from the game.
8. **Open questions**: anything that could not be established from data alone.

Notation:

- All guest data is big-endian. `u32`, `f32` and so on are big-endian guest fields.
- `f32` arithmetic is IEEE 754 binary32 with round-to-nearest, ties-to-even, unless a section
  says otherwise. `round32(x)` is the binary32 value nearest to the real number `x`.
- `fl(e)` means "the binary32 result of evaluating `e`" where the precision note for that output
  states the tolerance.
- Hexadecimal offsets such as `+0x40` are byte offsets inside the named guest object.
- RSX method numbers and register formats are public hardware interface facts.

Identities (function address and generated-function SHA-256) appear only where a behaviour must be
tied to the retail v1.0 build. Original code, constant tables, captured values and game bytes never
appear here.
