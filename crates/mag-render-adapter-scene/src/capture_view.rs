//! Copy-free views of guest spans inside one synchronous capture call.
//!
//! With `PS3_RENDER_CAPTURE_VIEW_VALIDATION=1` the host bridge registers two
//! read-only view functions once. While a capture call runs, a per-thread
//! provider lets the readers compare a span in place with bytes they already
//! retain, instead of copying it first. Equal bytes are reused exactly as the
//! copy path would reuse them; anything else falls back to the ordinary copy.
//! Nothing here retains a guest pointer past the call that supplied it.
use core::ffi::c_void;
use std::sync::{Arc, OnceLock};

/// The bridge's synchronous guest copy: `copy(opaque, address, bytes, out)`
/// fills `out` with `bytes` guest bytes and returns 0, or returns nonzero.
pub type SourceCopy = unsafe extern "C" fn(*mut c_void, u32, u32, *mut u8) -> i32;
/// Read-only view of a located span inside the capture window: the same
/// checks as the located copy, then a pointer to the guest bytes, or null.
/// Only valid during the synchronous capture call that supplied `opaque`.
pub type LocatedSourceView = unsafe extern "C" fn(*mut c_void, u32, u32, u32) -> *const u8;
static LOCATED_VIEW: OnceLock<LocatedSourceView> = OnceLock::new();
/// The bridge registers its located view once, and only with
/// `PS3_RENDER_CAPTURE_VIEW_VALIDATION=1`. Returns 0 when registered. This is
/// a one-time registration, not per-frame work: a view is used only inside a
/// capture call, which the master switch gates.
/// # Safety
/// `view` must follow the `LocatedSourceView` contract for the bridge's
/// located-copy contexts.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mag_render_adapter_set_located_view(
    view: Option<LocatedSourceView>,
) -> u32 {
    match view {
        Some(view) if LOCATED_VIEW.set(view).is_ok() || LOCATED_VIEW.get() == Some(&view) => 0,
        _ => 1,
    }
}
/// Read-only view of a guest span with the guest copy's checks, or null.
/// Only valid during the synchronous capture call that supplied `opaque`.
pub type GuestSourceView = unsafe extern "C" fn(*mut c_void, u32, u32) -> *const u8;
static GUEST_VIEW: OnceLock<GuestSourceView> = OnceLock::new();
/// The bridge registers its guest view once, and only with
/// `PS3_RENDER_CAPTURE_VIEW_VALIDATION=1`. Returns 0 when registered. This is
/// a one-time registration, not per-frame work: a view is used only inside a
/// capture call, which the master switch gates.
/// # Safety
/// `view` must follow the `GuestSourceView` contract for the bridge's copy
/// contexts.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mag_render_adapter_set_guest_view(view: Option<GuestSourceView>) -> u32 {
    match view {
        Some(view) if GUEST_VIEW.set(view).is_ok() || GUEST_VIEW.get() == Some(&view) => 0,
        _ => 1,
    }
}

/// The source window a capture call reads under: its VM, its store
/// generation, and the check that both are still current.
#[derive(Clone, Copy)]
pub struct SourceWindow {
    pub vm: u64,
    pub generation: u64,
    pub current: fn(u64, u64) -> bool,
}

/// Per-thread copy-free provider for the current capture call (calling thread
/// and its scoped helpers). Installed only for the call's duration.
#[derive(Clone, Copy)]
pub struct ViewProvider {
    located: Option<(LocatedSourceView, *mut c_void)>,
    guest: Option<(GuestSourceView, *mut c_void)>,
    /// `None` only for synthetic harnesses that install their own view
    /// (`scoped_guest_view`).
    window: Option<SourceWindow>,
}
// SAFETY: the opaque pointers are the bridge's contexts for one synchronous
// call; the provider is only installed on that call's thread and helpers it
// joins before returning.
unsafe impl Send for ViewProvider {}
unsafe impl Sync for ViewProvider {}
impl ViewProvider {
    fn current(&self) -> bool {
        self.window
            .as_ref()
            .is_none_or(|w| (w.current)(w.vm, w.generation))
    }
}
thread_local! {
    static VIEW_PROVIDER: std::cell::Cell<Option<ViewProvider>> = const { std::cell::Cell::new(None) };
}
/// Restores the previous provider of this thread when dropped.
pub struct ViewScope(Option<ViewProvider>);
impl ViewScope {
    /// Installs a provider for one capture call when the flag and at least
    /// one registered bridge view are present; otherwise installs nothing.
    /// `guest_opaque` is the guest copy's context and `located_opaque` the
    /// located copy's, when the call has one.
    pub fn install(
        guest_opaque: *mut c_void,
        located_opaque: Option<*mut c_void>,
        window: SourceWindow,
    ) -> Self {
        let provider = crate::source_acquisition::view_validation_enabled()
            .then(|| ViewProvider {
                located: LOCATED_VIEW.get().copied().and_then(|v| {
                    located_opaque
                        .filter(|opaque| !opaque.is_null())
                        .map(|opaque| (v, opaque))
                }),
                guest: GUEST_VIEW.get().copied().map(|v| (v, guest_opaque)),
                window: Some(window),
            })
            .filter(|p| p.located.is_some() || p.guest.is_some());
        Self::enter(provider)
    }
    /// Installs `provider` (from `current_view`) on this thread, for a helper
    /// running inside the same capture call.
    pub fn enter(provider: Option<ViewProvider>) -> Self {
        Self(VIEW_PROVIDER.with(|p| p.replace(provider)))
    }
}
impl Drop for ViewScope {
    fn drop(&mut self) {
        VIEW_PROVIDER.with(|p| p.set(self.0));
    }
}
/// This thread's provider, to hand to helpers of the same capture call.
pub fn current_view() -> Option<ViewProvider> {
    VIEW_PROVIDER.with(std::cell::Cell::get)
}
/// Synthetic harnesses only: install a guest view for this thread until the
/// guard drops. The capture installs its own provider.
/// # Safety
/// `view(opaque, address, bytes)` must return null or a pointer to `bytes`
/// readable bytes that stay unchanged while the guard lives.
pub unsafe fn scoped_guest_view(view: GuestSourceView, opaque: *mut c_void) -> ViewScope {
    ViewScope::enter(Some(ViewProvider {
        located: None,
        guest: Some((view, opaque)),
        window: None,
    }))
}
/// `Some(equal)` when the requested located span could be viewed in place,
/// under the same preconditions the located copy checks first; `None` means
/// no provider or no view, and the caller copies as usual. Nothing here is
/// charged or retained; the slice never outlives this call.
pub fn located_view_equals(
    request: crate::material_textures::LocatedTextureRequest,
    expected: &[u8],
) -> Option<bool> {
    let provider = VIEW_PROVIDER.with(std::cell::Cell::get)?;
    let (view, opaque) = provider.located?;
    if expected.len() != request.bytes || !provider.current() {
        return None;
    }
    let bytes = u32::try_from(request.bytes).ok()?;
    let equal = {
        let _timer =
            crate::source_acquisition::section(crate::source_acquisition::Section::GuestCallback);
        // SAFETY: the provider exists only inside the capture call that owns
        // `opaque`; a non-null view covers exactly `bytes` guest bytes that
        // stay mapped for that call, and the slice is dropped here.
        let pointer = unsafe { view(opaque, request.location as u32, request.offset, bytes) };
        if pointer.is_null() {
            return None;
        }
        let view = unsafe { std::slice::from_raw_parts(pointer, request.bytes) };
        view == expected
    };
    crate::source_acquisition::count_view(equal, request.bytes);
    Some(equal)
}
/// The retained catalog content byte-identical to the guest span, compared in
/// place under the same preconditions the guest copy checks first. `None`
/// (no provider, no view, or no identical retained content) means the caller
/// copies as usual. The returned content is what the copy path would intern
/// for these bytes; callers use its bytes, never the guest view, afterwards.
pub fn guest_view_retained(
    address: u32,
    bytes: usize,
) -> Option<Arc<crate::shared_scene_sources::SourceContent>> {
    let provider = VIEW_PROVIDER.with(std::cell::Cell::get)?;
    let (view, opaque) = provider.guest?;
    if bytes == 0 || !provider.current() {
        return None;
    }
    let length = u32::try_from(bytes).ok()?;
    let found = {
        let _timer =
            crate::source_acquisition::section(crate::source_acquisition::Section::GuestCallback);
        // SAFETY: as in `located_view_equals`.
        let pointer = unsafe { view(opaque, address, length) };
        if pointer.is_null() {
            return None;
        }
        let view = unsafe { std::slice::from_raw_parts(pointer, bytes) };
        crate::shared_scene_sources::find_content(view)
    };
    crate::source_acquisition::count_view(found.is_some(), bytes);
    found
}
