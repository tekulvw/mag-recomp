//! Scene preparation parts of the MAG render adapter.
//!
//! These modules sit above [`mag_render_adapter_core`]: they copy the game's
//! render sources out of guest memory, keep content-qualified caches of what
//! they copied and decoded, and decode geometry streams for the frontend.
//! Where a rule is specified in `docs/render-frontend/behavior/`, the
//! clean-room implementation in `mag-render-behavior` is called rather than a
//! private copy. Nothing here contains game content or code derived from the
//! original program; see `docs/native-graphics-reimplementation-method.md`.
//!
//! Nothing here runs unless a host calls it. Per-frame host entry points check
//! the adapter core's default-off master switch
//! ([`mag_render_adapter_core::runtime`]).
#![deny(unsafe_code)]

// The core modules this crate builds on, re-exported so the whole adapter is
// addressable from one crate under the module names its code uses.
pub use mag_render_adapter_core::{
    error, fast_hash, geometry_bindings, guest_memory, mesh_sources, partition_inputs,
};

pub mod capture_memo;
pub mod decoded_geometry_cache;
pub mod geometry_attributes;
pub mod geometry_streams;
pub mod material_textures;
pub mod shared_scene_sources;
pub mod source_acquisition;
