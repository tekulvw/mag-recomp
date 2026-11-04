//! Portable parts of the MAG render adapter.
//!
//! The adapter reads the game's render sources from guest memory and prepares
//! owned scenes for the frontend. This crate holds the parts that depend only
//! on public interface facts (RSX method layout, guest data-structure layouts
//! needed to read and write guest memory) and on behaviour specified in our own
//! words. It contains no game content and no code derived from the original
//! program; see `docs/native-graphics-reimplementation-method.md`.
//!
//! Nothing here runs unless a host calls it. Per-frame entry points also check
//! the default-off master switch in [`runtime`].
#![deny(unsafe_code)]

#[allow(unsafe_code)] // Lifetime erasure of scoped jobs; join waits for every job.
pub mod capture_pool;
pub mod cohort;
pub mod configuration;
pub mod error;
pub mod fast_hash;
#[allow(unsafe_code)] // Fixed-size, pointer-free C exports; no unsafe memory operations.
pub mod ffi;
pub mod geometry_bindings;
pub mod guest_memory;
pub mod main_flow;
pub mod material_capture;
pub mod material_constants;
pub mod material_float;
pub mod material_pipeline;
pub mod material_programs;
pub mod material_shader_math;
pub mod material_state;
pub mod mesh_sources;
#[allow(unsafe_code)] // Bounded caller-owned path/output; no retained pointer.
pub mod original_hash_ffi;
pub mod partition_inputs;
#[allow(unsafe_code)] // Unmangled C exports of the switch only.
pub mod runtime;
pub mod shadow_call;
pub mod shadow_owner;
pub mod source;
pub mod views;

#[cfg(feature = "owned-scene")]
pub mod scene_programs;
#[cfg(feature = "owned-scene")]
pub mod scene_raster;
#[cfg(feature = "owned-scene")]
pub mod scene_viewport;
