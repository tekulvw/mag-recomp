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
//!
//! Floating-point outputs use ordinary native arithmetic. They are checked against the
//! tolerances each specification states, never against a particular evaluation order.

pub mod guest;
pub mod producers;
pub mod setup;
pub mod surfaces;
pub mod texture;

#[cfg(test)]
pub(crate) mod testutil;
