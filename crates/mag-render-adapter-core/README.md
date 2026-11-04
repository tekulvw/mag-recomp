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

With the `owned-scene` feature (which needs the pinned native compiler
libraries of `mag-render-programs`):

- `scene_programs`: transport of owned compiler output to the frontend worker;
- `scene_raster`, `scene_viewport`: effective raster and viewport state as
  frontend packet records.

Fragment builtin requests are not published yet. Nothing in this crate runs
unless a host calls it. The C exports are
`mag_render_adapter_enabled`, `mag_render_adapter_enable` and
`mag_original_work_sha256_file`.
