# MAG render adapter scene preparation

The parts of the game render adapter that sit above
[`mag-render-adapter-core`](../mag-render-adapter-core/README.md): copying the
game's render sources out of guest memory, keeping content-qualified caches of
what was copied and decoded, and decoding geometry streams for the frontend.
They follow the same rules as the core crate (see
[`docs/render-frontend.md`](../../docs/render-frontend.md#game-adapter)): only
public interface facts and behaviour written in our own words, no game content,
synthetic tests only. Where a rule has a clean-room implementation in
[`mag-render-behavior`](../mag-render-behavior), this crate calls it instead of
keeping its own copy.

Current contents:

- `shared_scene_sources`: a catalog of immutable source byte strings shared by
  the Depth and Main captures. A sampled fingerprint only nominates candidates;
  exact byte equality authorises every reuse;
- `source_acquisition`: reuse around the Main source capture and its counters.
  Resident decoded textures and the sampled content probe are on by default
  inside the adapter (`PS3_RENDER_RESIDENT_TEXTURES=0`,
  `PS3_RENDER_CONTENT_PROBE=0` or `PS3_RENDER_RESIDENT_SOURCES=0` turn them
  off) and are reused only on exact byte equality; read sharing, concurrent
  caches, in-place view validation and section profiling are opt-in;
- `capture_memo`: a per-capture memo of pure decodes keyed by every input byte;
- `material_textures`: owned RSX texture storage, layouts and image copies;
- `geometry_streams`, `geometry_attributes`, `decoded_geometry_cache`: owned
  stream decoding (positions, indices, instance rows, descriptor-driven
  attributes) and a content-qualified cache of decoded rows. Packed normals
  (codec 9) are decoded by `mag_render_behavior::normals`, within the
  tolerance of `geometry-normals.md`;
- `material_sources`, `cohort_resources`: raw material, mesh and partition
  resolution for the retained visibility cohort;
- `cohort_geometry` (with `direct` and `static_geometry`): per-item geometry
  capture for Main, including direct draws and the undeformed static path;
- `deformation`: the capture's group 0, 3/4 and palette-skinning (groups 7
  and 11) call sites. They check the capture's budget and bank populations and
  then call the single shared entry point in
  `mag_render_behavior_glue::deformation`, so Main and the depth pre-pass get
  identical positions. Results are the behaviour crate's native arithmetic
  within the tolerances of its deformation specifications;
- `depth_sources`, `depth_materials`: the depth pre-pass inputs and its
  selected-material programs and fragment builtin requests;
- `material_finish`, `material_group_twelve`: the Main pass's owned finish
  inputs and auxiliary records, and its group-twelve compact texture inputs;
- `ring_writer`, `main_terminal_writer`, `main_terminal_job`,
  `empty_pass_terminal`: the command-ring transport of a natively rendered
  pass. `ring_writer` holds what every writer shares: the progress markers of
  a writer slot, the two-bit ring generation that advances on each wrap, where
  a segment continues (slot 0 runs on in the ring, side slots return through
  their continuation), and the ordered fence, put, descriptor, epoch and
  next-writer publication. `main_terminal_writer` and `main_terminal_job` are
  Main's slot-0 transport and its CPU reference effects. `empty_pass_terminal`
  is the empty-pass writer for any side slot (1 to 15), parameterised by the
  pass's state block and reports; the Depth profile (slot 3) is defined.
  Nothing here runs unless a host calls it;
- `client_scene`: a position-only client scene prepared from the captured
  geometry, with every omission kept explicit;
- `capture_view`: optional copy-free comparison of guest spans through
  read-only views the host registers
  (`PS3_RENDER_CAPTURE_VIEW_VALIDATION=1`, default off).

The core modules these build on are re-exported under their own names, so the
whole adapter is addressable from this crate. Nothing here runs unless a host
calls it; per-frame host entry points check the core crate's default-off
master switch.

```sh
cargo fmt --manifest-path crates/mag-render-adapter-scene/Cargo.toml --check
cargo clippy --manifest-path crates/mag-render-adapter-scene/Cargo.toml --all-targets --locked -- -D warnings
cargo test --manifest-path crates/mag-render-adapter-scene/Cargo.toml --locked
```
