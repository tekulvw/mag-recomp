//! Glue between `mag-render-behavior` (the clean-room behaviour crate), the public adapter core
//! and the frontend packet ABI, as described in `docs/render-frontend/behavior-integration.md`.
//!
//! | Module | Role |
//! | --- | --- |
//! | [`abi`] | the pointer-bearing packet records from an owned arena |
//! | [`memory`] | `GuestSnapshot` as a behaviour `GuestMemory` |
//! | [`cohort`] | owned cohort records as planner inputs |
//! | [`packet`] | plan steps and producer passes as packet entries |
//! | [`resources`] | `ProducerResources` over exact image provenance |
//! | [`programs`] | `ProgramTranslator` over `mag-render-programs` |
//! | [`deformation`] | one deformation per item, shared by Main and the depth pre-pass |
//!
//! Depth-equal requirement: group 1 tests depth *equal* against depth written earlier in the
//! frame. The depth pre-pass and Main must therefore consume identical deformed positions for
//! every wind, ripple or skinned item. Both passes take them from [`deformation`], which calls
//! the single behaviour-crate function for each group; no pass may keep its own copy, variant or
//! coefficients.

pub mod abi;
pub mod cohort;
pub mod deformation;
pub mod memory;
pub mod packet;
pub mod programs;
pub mod resources;

pub use memory::SnapshotMemory;
