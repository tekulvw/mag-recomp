//! Clean-room Main render behaviour.
//!
//! Every module in this crate is written from one behaviour specification under
//! `docs/render-frontend/behavior/` and from nothing else. The API takes plain input records
//! (guest data that the caller has already read, or a [`guest::GuestMemory`] reader for the
//! specifications that define their own reads) and returns plain output records. Nothing here
//! talks to the game, the runtime or a GPU.
//!
//! | Module | Specification |
//! | --- | --- |
//! | [`texture`] | `texture-unit-controls.md` |
//! | [`setup`] | `material-setup.md` |
//! | [`producers`] | `material-producers.md` |
//! | [`surfaces`] | `main-producer-inputs.md` |
//! | [`lighting`] | `main-lighting.md` |
//! | [`group_twelve`] | `main-group-twelve.md` |
//! | [`fog_values`] | `main-fog-values.md` (the specified parts) |
//! | [`fog`] | `main-fog.md` |
//! | [`producer`] | shared full-screen producer pieces |
//! | [`normals`] | `geometry-normals.md` |
//! | [`skinning`] | `skinning-deformation.md` |
//! | [`noninstanced`] | `noninstanced-deformation.md` (group 4; group 3 is held) |
//! | [`material_values`] | `material-values.md` |
//! | [`program_compilation`] | `program-compilation.md` (frontend side of the translator contract) |
//! | [`vertex_inputs`] | `vertex-inputs.md` |
//! | [`raster`] | `raster-state.md` |
//! | [`scene_plan`] | `scene-plan.md` (planner core) |
//! | [`waveform`] | helpers defined in `group-zero-deformation.md` |
//!
//! Floating-point outputs use ordinary native arithmetic. They are checked against the
//! tolerances each specification states, never against a particular evaluation order.

// Matrix and lane arithmetic reads more clearly with explicit indices.
#![allow(clippy::needless_range_loop)]

pub mod fog;
pub mod fog_values;
pub mod group_twelve;
pub mod guest;
pub mod lighting;
pub mod material_values;
pub mod noninstanced;
pub mod normals;
pub mod producer;
pub mod producers;
pub mod program_compilation;
pub mod raster;
pub mod scene_plan;
pub mod setup;
pub mod skinning;
pub mod surfaces;
pub mod texture;
pub mod vertex_inputs;
pub mod waveform;

#[cfg(test)]
pub(crate) mod testutil;
