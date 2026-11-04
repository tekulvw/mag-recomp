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
  tolerance of `geometry-normals.md`.

The core modules these build on are re-exported under their own names, so the
whole adapter is addressable from this crate. Nothing here runs unless a host
calls it; per-frame host entry points check the core crate's default-off
master switch.

```sh
cargo fmt --manifest-path crates/mag-render-adapter-scene/Cargo.toml --check
cargo clippy --manifest-path crates/mag-render-adapter-scene/Cargo.toml --all-targets --locked -- -D warnings
cargo test --manifest-path crates/mag-render-adapter-scene/Cargo.toml --locked
```
