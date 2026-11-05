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
    capture_pool, cohort, error, fast_hash, geometry_bindings, guest_memory, main_flow,
    material_capture, material_constants, material_pipeline, material_programs,
    material_shader_math, material_state, mesh_sources, partition_inputs,
};

pub mod capture_memo;
#[allow(unsafe_code)] // Calls the bridge's registered read-only views inside one capture call.
pub mod capture_view;
pub mod client_scene;
#[allow(unsafe_code)] // Synchronous borrowed owned-scene callback only.
pub mod client_scene_ffi;
#[allow(unsafe_code)] // Fixed-size pointer-free observation ABI only.
pub mod cohort_ffi;
pub mod cohort_geometry;
#[allow(unsafe_code)] // Synchronous copy callback only; checked owned buffers, no retained pointer.
pub mod cohort_resource_ffi;
pub mod cohort_resources;
pub mod decoded_geometry_cache;
pub mod deformation;
pub mod depth_materials;
#[allow(unsafe_code)] // Synchronous qualified source copy; no retained guest pointer.
pub mod depth_source_ffi;
pub mod depth_sources;
pub mod empty_pass_terminal;
pub mod fragment_builtin_rows;
pub mod geometry_attributes;
pub mod geometry_streams;
pub mod main_terminal_job;
pub mod main_terminal_writer;
#[cfg(feature = "owned-programs")]
pub mod material_compile;
pub mod material_finish;
pub mod material_group_twelve;
pub mod material_sources;
pub mod material_textures;
pub mod material_values;
pub mod materials;
#[allow(unsafe_code)] // Synchronous source copy; no retained guest pointer.
pub mod producer_sources;
pub mod ring_writer;
pub mod shadow_run;
pub mod shared_scene_sources;
pub mod source_acquisition;
pub mod vertex_inputs;
