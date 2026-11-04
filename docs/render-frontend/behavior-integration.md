# Integrating the clean-room behaviour crate

This document describes how `crates/mag-render-behavior` (the clean-room implementation of the
specifications in [behavior/](behavior/README.md)) plugs into the public adapter core
(`crates/mag-render-adapter-core`) and the frontend packet ABI (`crates/mag-render-abi`). It names
only public crates and public interface facts. The parts that can be built from public crates alone
live in `crates/mag-render-behavior-glue`; everything else is the adapter's integration work.

## Layers

| Layer | Crate | Owns |
| --- | --- | --- |
| Guest reads | `mag-render-adapter-core` | `GuestSnapshot`, cohort, mesh and partition sources, program sources, geometry bindings |
| Behaviour | `mag-render-behavior` | every rule of the specifications, as functions over plain inputs |
| Glue | `mag-render-behavior-glue` | adapters between the two, the plan-step assembler, `ProducerResources` and `ProgramTranslator` implementations, the shared deformation store |
| Transport | `mag-render-abi`, `mag-render` | packet records and the owned scene builder |

The behaviour crate has no dependencies apart from `mag-render-abi`'s record types. It never reads
the game or a device. The glue depends on the behaviour crate, the adapter core (default features),
`mag-render-programs` (default features: HLSL translation and source reflection) and
`mag-render-abi`.

Where the adapter core already has a migrated implementation of the same rule (for example
`material_state`, `material_pipeline`, `material_constants` or `main_flow`), the behaviour crate's
clean-room version replaces it in the frontend path. The migrated module stays only until the
replacement gates pass. One difference is intentional: the behaviour crate uses ordinary native
arithmetic within the stated tolerances, not the `material_float` SPU number model.

## Inputs to each behaviour API

| Behaviour API | Inputs from the adapter core |
| --- | --- |
| `guest::GuestMemory` (every reader in the crate) | `GuestSnapshot`, wrapped by `glue::SnapshotMemory` (a `GuestSnapshot` read maps to `GuestMemory::read`; `OutsideSnapshot` and `InvalidRange` become `SourceError`) |
| `producers::acquire`, `fog_values::acquire_fog_camera` | the snapshot and the TOC from the Main invocation |
| `setup::copy_main_setup_banks`, `setup::MainSetupRecord::decode` | the snapshot (IO-mapped main memory) and the 160-byte pass setup record |
| `setup::decode_inherited_snapshot` | the runtime register-snapshot interface's (method, value) list |
| `scene_plan::check_association`, `scene_plan::traverse` | `OwnedVisibilityCohort` header and items (`glue::cohort`), material entries, pass control word |
| `material_sources::PassSources`, `material_sources::item_sources` | the snapshot, the Main pass object address, and `CohortItem::mbi` and transform byte `+0x6F`; `MeshSource::copy` reads the same mesh record |
| `material_values::read_technique`, `ItemInputs` | material address, technique chunk and selector from `item_sources`; `TransformRecord` from `CohortItem::transform`; `mbi` from `CohortItem::mbi`; mesh constant row from the item's mesh record |
| `fragment_builtins::builtin_rows` | the snapshot, the pass address, and the material bytes, chunk and selector from `item_sources`; the result is `ItemInputs::builtin_rows` |
| `program_compilation::TechniqueSource` | `material_programs::TechniquePrograms` decoded from the chunk `item_sources` copied (vertex instructions, load slot, masks, transform word, shader control, texture-coordinate controls, relocation directory), and the microcode `item_sources` read from the asset bank |
| `geometry_streams::partition_chain`, `read_partition_streams`, `decode_partition` | the snapshot, the first partition of the item's `GeometrySource`, and the palette base it names; replaces `partition_inputs` (which lacks the large-stream rule) and `decode_gpu_attribute` (an RSX fetch decoder, not a partition codec) for partition streams |
| `vertex_inputs::VertexDraw` | `geometry_bindings::builtin_output_bindings` or `decode_output_bindings` over slot 0 (by configuration byte 4), auxiliary bindings, the primary and secondary lists and indices from `decode_partition`, and the preparation results below |
| `normals`, `skinning`, `noninstanced`, `group_zero` | banks from `PartitionGeometry::lookup`; partition fields from `PartitionRecord`; group-zero instance rows and colour pool by `group_zero::read_instance_rows` and `read_colour_pool` (an `rsx_location::RsxAddressMap` from the caller's GCM mapping) |
| `group_twelve::run_group_twelve` | the snapshot and the Main pass object address |
| `transitions` | `TransitionSources` from the setup record and pass object (`+0x114`, `+0x220`, `+0x19C`) |

`scene_plan::route` and the adapter core's `main_flow::main_item_route` must agree for every
(group, pass control). `glue::cohort` checks this, and also checks that the adapter core's
`binding_control` is the complement of the binding row's lane 2 rule in
[material-values.md](behavior/material-values.md).

## From plan steps to packet entries

`scene_plan::traverse` returns an ordered list of `PlanStep`s. `glue::packet::PacketAssembler`
turns them into owned packet entries that map one to one onto `mag_render_abi::packet` records:

| Plan step | Packet entry |
| --- | --- |
| `OpenPass` | a new pass on the current Main surface binding (`effects::open_main_pass` checks it), colour and depth attachments with load = load |
| `NoDraw { reason }` | an `Item` in the open pass with `reserved` = `ITEM_SOURCE_REPEATED`, `ITEM_SOURCE_UPDATE_ONLY` or `ITEM_SOURCE_LISTS_DISABLED`, and zero unresolved obligations |
| `Prepare { item }` | an `Item` with `ITEM_DRAW_REQUIRED`, followed by one draw per prepared partition (section 5 of the planner) |
| `Event(e)` | `effects::route_event`, then the routed producer. A producer pass becomes its own pass with one `Item` whose identity has the top bit set. Any event closes the open pass. |
| `Transition { from, to }` | `transitions::transition` on the carried pipeline state, with the emitted references cleared |
| `WaterRows` | `transitions::water_rows` written to vertex constant rows 46 and 47 |
| `ObserveUpdateOnly` | `MaterialValueState::observe_update_only` |

Attachments use the frontend's codes: load 0 clear, 1 load, 2 don't care; store 0 store, 1 discard;
an absent attachment has image `u32::MAX`. Stencil planes use `StencilAttachment` (packet v3).
Retained clear replay (`scene_plan::replay_clears`) produces passes whose attachments clear with
the replayed values.

Draws carry: the pipeline from the compiled pair (`program_compilation`), the raster state
(`raster::translate`), the viewport (`surfaces::NativeViewport`, with the shader Z transform in the
vertex uniform tail), the vertex and index buffers (`vertex_inputs`), and textures
(`scene_plan::draw_textures`), with views and samplers de-duplicated by the packet.

### Pointer-bearing records

`glue::abi::SceneBuilder` is an owned arena of programs, pipelines, buffers, images, views,
samplers, passes (with their packet v2 viewports and v3 stencil records) and items.
`SceneBuilder::add_assembled` takes the assembler's passes and items. `SceneBuilder::view` borrows
the arena and returns a `PacketView` whose `DescV3` points only into the arena and into the view's
own tables, so the synchronous copy into the native worker reads live, unchanged memory. Views and
samplers are de-duplicated by the bits of every field. Index buffers hold native-endian `u32`.

The builder is tested against the real packet validator,
`mag_render_native::validate_scene_packet_v3`, which runs exactly the copy and validation that
scene preparation runs, without a device. One test takes a clean-room plan end to end: traversal,
pass assembly, programs compiled through `RsxTranslator`, vertex inputs and uniforms.

## `ProducerResources`

`glue::resources::ImageTable` implements `producer::ProducerResources` for the lighting and fog
producers. It holds:

- images keyed by exact provenance (source address and eight descriptor words). Entries come from
  the producer texture binding list (`surfaces::plan_producer_bindings`: a depth-classified
  request is listed with its derived colour image, any other request with its imported guest
  image), validated by `effects::validate_producer_bindings`, and from images that earlier
  producers in this packet wrote. Producers bind only by provenance and decline a depth image;
- colour snapshots per surface record, for the fog producer's draw 0, which samples the surface
  it writes. Without a snapshot, a fog program that samples unit 0 declines;
- per surface record address, the `TargetInfo` built from the supplied `SurfaceBinding`, with
  initialisation flags that are updated as passes write planes.

A lookup never falls back to a nearby match: no exact provenance means no image, and the producer
declines.

## `ProgramTranslator`

`glue::programs::RsxTranslator` implements `program_compilation::ProgramTranslator` over
`mag-render-programs`:

- `translate_fragment` → `mag_render_programs::fragment_with_texcoords`, with `ShaderControls`
  mapped field for field. The sampled units come from `fragment_texture_units`, and slot =
  sampler = unit, as in [scene-plan.md](behavior/scene-plan.md);
- `translate_vertex` → `mag_render_programs::vertex`; constant reads come from
  `vertex_constant_reads`;
- `lower_vertex` / `lower_fragment` return the portable HLSL. Native lowering (`native_compile`,
  feature `native-compiler`) happens in the adapter worker that owns the device, keyed as in
  [program-compilation.md](behavior/program-compilation.md).

`glue::programs::program_usage` gives the `ProgramUsage` the producers need (sampled units, may
discard) from the fragment bytes, and `glue::programs::alpha_reference` is the shader-controls
alpha conversion.

## One deformation for Main and the depth pre-pass

Group 1 tests depth *equal* against depth written earlier in the frame. If the depth pre-pass and
Main deform a wind, ripple or skinned item with different code, coefficients or evaluation order,
positions differ in the last bit and the equal test fails on those pixels
([group-zero-deformation.md](behavior/group-zero-deformation.md#consequences-of-non-bit-exact-output)).
So:

- every deformation (group 0, 3, 4 and 11) is one function in `mag-render-behavior`, and both the
  Main path and the depth pre-pass call it. Neither keeps a private copy, a GPU variant for one
  pass only, or different wind coefficients;
- `glue::deformation::DeformationStore` computes each item's prepared banks once per frame, keyed by
  (cohort, item, partition), and hands the same buffers to both passes. A pass that cannot use the
  store must call the same function with the same inputs. The result is then bit-identical
  because the function is deterministic;
- a test asserts that the depth and Main consumers of one item receive bit-identical positions.

Groups 0 and 3 use this implementation's own wind tuning values. The recompiled original depth job
uses the game's values, so for wind items only this shared path can feed a depth pass that Main
tests for equality. Tests cover groups 0, 3, 4 and 11.

## Not covered yet

- Native shader lowering and the stored-depth Metal variant (`mag-render-programs` with
  `native-compiler`).
