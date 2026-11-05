//! The static library a host links when it uses both the native frontend and
//! the game render adapter.
//!
//! Two Rust static libraries in one executable would each carry their own copy
//! of the standard library and clash at link time, so this crate is the single
//! archive for both. It adds no code: every C entry point comes from
//! `mag-render-native` (and the `mag-render` core it re-exports) or from
//! `mag-render-adapter-core` and `mag-render-adapter-scene`, unchanged. The adapter's master switch
//! (`MAG_RENDER_ADAPTER=1` or `mag_render_adapter_enable()`) still gates all of
//! the adapter's per-frame work.

pub use mag_render_adapter_core as adapter;
pub use mag_render_adapter_scene as scene;
pub use mag_render_native as native;

#[cfg(test)]
mod tests {
    #[test]
    fn adapter_switch_is_off_until_enabled() {
        // The test process does not set MAG_RENDER_ADAPTER.
        if std::env::var_os(super::adapter::runtime::ENVIRONMENT).is_none() {
            assert!(!super::adapter::runtime::enabled());
        }
    }
}
