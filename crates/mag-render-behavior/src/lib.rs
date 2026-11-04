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
//! | [`geometry_streams`] | `geometry-streams.md` |
//! | [`skinning`] | `skinning-deformation.md` |
//! | [`noninstanced`] | `noninstanced-deformation.md` (groups 3 and 4) |
//! | [`group_zero`] | `group-zero-deformation.md` |
//! | [`material_sources`] | `material-sources.md` |
//! | [`material_values`] | `material-values.md` |
//! | [`fragment_builtins`] | `fragment-builtins.md` |
//! | [`program_compilation`] | `program-compilation.md` (frontend side of the translator contract) |
//! | [`vertex_inputs`] | `vertex-inputs.md` |
//! | [`raster`] | `raster-state.md` |
//! | [`rsx_location`] | RSX location reads from `group-zero-deformation.md` |
//! | [`scene_plan`] | `scene-plan.md` (planner core) |
//! | [`transitions`] | `main-group-transitions.md` |
//! | [`effects`] | `main-effects.md` (framework and stencil-mask producer) |
//! | [`quincunx`] | `main-quincunx.md` (nearest-sample approximation, labelled) |
//! | [`downsample`] | `main-downsample.md` |
//! | [`finish`] | `main-finish.md` |
//! | [`waveform`] | helpers and wind tuning values from `group-zero-deformation.md` |
//!
//! Floating-point outputs use ordinary native arithmetic. They are checked against the
//! tolerances each specification states, never against a particular evaluation order.

// Matrix and lane arithmetic reads more clearly with explicit indices.
#![allow(clippy::needless_range_loop)]

pub mod downsample;
pub mod effects;
pub mod finish;
pub mod fog;
pub mod fog_values;
pub mod fragment_builtins;
pub mod geometry_streams;
pub mod group_twelve;
pub mod group_zero;
pub mod guest;
pub mod lighting;
pub mod material_sources;
pub mod material_values;
pub mod noninstanced;
pub mod normals;
pub mod producer;
pub mod producers;
pub mod program_compilation;
pub mod quincunx;
pub mod raster;
pub mod rsx_location;
pub mod scene_plan;
pub mod setup;
pub mod skinning;
pub mod surfaces;
pub mod texture;
pub mod transitions;
pub mod vertex_inputs;
pub mod waveform;

#[cfg(test)]
pub(crate) mod testutil;
