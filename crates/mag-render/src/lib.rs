//! Portable contracts only: this crate does not render or certify live removal.
pub mod accounting;
pub mod ffi;
pub mod types;

pub mod resources;

pub mod backend;

#[cfg(feature = "sdk-compute")]
pub mod sdk_compute;
