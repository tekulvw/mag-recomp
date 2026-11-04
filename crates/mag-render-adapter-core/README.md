# MAG render adapter core

The portable, publishable parts of the game render adapter. The adapter reads
the game's render sources from guest memory and prepares owned scenes for the
frontend crates. Parts arrive here as they meet the rules in
[`docs/render-frontend.md`](../../docs/render-frontend.md#game-adapter) and
[the reimplementation method](../../docs/native-graphics-reimplementation-method.md):
they depend only on public interface facts and on behaviour written in our own
words, contain no game content, and are tested with synthetic inputs only.

Current contents:

- `runtime`: the master switch for per-frame work, off by default
  (`MAG_RENDER_ADAPTER=1` or `mag_render_adapter_enable()` turns it on);
- `guest_memory`, `error`: checked reads from an immutable guest snapshot;
- `fast_hash`: a fast deterministic hasher for adapter-built table keys;
- `capture_pool`: optional persistent helper threads with a scoped `join`
  (`PS3_RENDER_CAPTURE_POOL=1`, default off);
- `original_hash_ffi`: bounded SHA-256 of a regular file, for input
  verification;
- `material_float`: the Cell SPU single-precision number model on raw bit
  patterns. Its reciprocal-estimate table is reproduced from ps3recomp
  (`runtime/spu/spu_helpers.h`) under its MIT licence; see
  [`LICENSE.ps3recomp`](../../LICENSE.ps3recomp).
- `material_constants`, `material_shader_math`: material parameter values,
  fragment constant layout and current vertex attributes, in that number model;
- `material_programs`: owned shader inputs from a selected technique resource;
- `material_state`, `material_pipeline` (with `material_pipeline::sampler`):
  primary-material alpha/cull/blend rules, per-group Main fixed-function state
  and RSX sampler state, all over explicitly known prior state;
- `mesh_sources`, `partition_inputs`, `material_capture`: owned mesh and
  partition headers and the packed-input read sets planned from them.
- `cohort`, `main_flow`: owned retained-visibility records and the per-item
  Main route chosen from the material group and pass control;
- `geometry_bindings`: source-qualified vertex bindings, custom and built-in
  output layouts and auxiliary/optional vertex arrays;
- `source`, `views`, `shadow_owner`, `configuration`, `shadow_call`: the
  Shadow corner helper inputs, its view-corner geometry, the owner's cascade
  fits, and the configuration lifetime and helper-call publication protocols;
- `ffi`: pointer-free C entry points for the configuration and Shadow call
  protocols (`mag_render_adapter_config_event`,
  `mag_render_adapter_config_snapshot`, `mag_render_adapter_shadow_call`).
  Each returns its "not ready" result and records nothing while the master
  switch is off.

With the `owned-scene` feature (which needs the pinned native compiler
libraries of `mag-render-programs`):

- `scene_programs`: transport of owned compiler output to the frontend worker;
- `scene_raster`, `scene_viewport`: effective raster and viewport state as
  frontend packet records.

`material_constants` also decodes a material's fragment builtin requests
(kinds 3, 4, 31 and 32 of `fragment-builtins.md`); `mag-render-adapter-scene`
prepares their rows.
Nothing in this crate runs unless a host calls it. The C exports are `mag_render_adapter_enabled`,
`mag_render_adapter_enable`, `mag_original_work_sha256_file` and the `ffi`
entry points above.
