//! Synchronous Depth source observation only; no submission or cutover export.
//!
//! The entry point returns its refused result at once, and records nothing,
//! while the adapter core's master switch is off.
use crate::{
    depth_sources::{self, DepthBudget, DepthIdentity, DepthInput, OwnedDepthSources},
    error::Error,
};
use std::{
    collections::VecDeque,
    ffi::c_void,
    sync::{
        Arc, Mutex, OnceLock,
        atomic::{AtomicU64, Ordering},
    },
};
#[repr(C)]
#[derive(Clone, Copy)]
pub struct DepthSourceInput {
    pub abi_version: u32,
    pub struct_bytes: u32,
    pub vm: u64,
    pub invocation: u64,
    pub owner: u64,
    pub occurrence: u64,
    pub execution_update: u64,
    pub camera: u32,
    pub bucket: u32,
    pub marker: u32,
    pub pass: u32,
    pub user: u32,
    pub job: u32,
    pub allocation: u64,
}
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct DepthSourceResult {
    pub status: u32,
    pub reserved: u32,
    pub items: u64,
    pub reads: u64,
    pub bytes: u64,
}
static REFUSALS: AtomicU64 = AtomicU64::new(0);
static REVOCATION: AtomicU64 = AtomicU64::new(0);
static VM: AtomicU64 = AtomicU64::new(0);
static SOURCES: OnceLock<Mutex<VecDeque<Arc<OwnedDepthSources>>>> = OnceLock::new();
fn sources() -> &'static Mutex<VecDeque<Arc<OwnedDepthSources>>> {
    SOURCES.get_or_init(|| Mutex::new(VecDeque::new()))
}
#[expect(
    dead_code,
    reason = "called by the cohort source FFI layer, which is migrated later"
)]
pub(crate) fn vm_changed(vm: u64) {
    let next = REVOCATION.try_update(Ordering::AcqRel, Ordering::Acquire, |n| n.checked_add(1));
    VM.store(if next.is_ok() { vm } else { 0 }, Ordering::Release);
    if let Ok(mut s) = sources().lock() {
        s.clear();
    }
}
pub fn retain_depth_sources(identity: DepthIdentity) -> Option<Arc<OwnedDepthSources>> {
    if VM.load(Ordering::Acquire) != identity.vm {
        return None;
    }
    sources()
        .lock()
        .ok()?
        .iter()
        .find(|s| s.identity == identity)
        .cloned()
}
/// # Safety
/// Caller holds the actual qualified Depth producer read lifetime for this
/// complete synchronous callback. Callback receives initialized bounded storage;
/// no pointer/callback is retained. This is observation, never a ready certificate.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mag_render_adapter_depth_sources(
    input: DepthSourceInput,
    copy: Option<crate::capture_view::SourceCopy>,
    opaque: *mut c_void,
) -> DepthSourceResult {
    let failed = DepthSourceResult {
        status: 1,
        ..Default::default()
    };
    if !mag_render_adapter_core::runtime::enabled() {
        return failed;
    }
    std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        if input.abi_version != 1
            || input.struct_bytes as usize != size_of::<DepthSourceInput>()
            || VM.load(Ordering::Acquire) != input.vm
        {
            return failed;
        }
        let revocation = REVOCATION.load(Ordering::Acquire);
        let Some(copy) = copy else {
            return failed;
        };
        let identity = DepthIdentity {
            vm: input.vm,
            invocation: input.invocation,
            owner: input.owner,
            occurrence: input.occurrence,
            execution_update: input.execution_update,
            allocation: input.allocation,
            job: input.job,
            camera: input.camera,
        };
        let mut read = |address, bytes| {
            if VM.load(Ordering::Acquire) != input.vm || REVOCATION.load(Ordering::Acquire) != revocation {
                return Err(Error::UnprovenProducerEpoch);
            }
            let count = u32::try_from(bytes).map_err(|_| Error::InvalidRange)?;
            let mut owned = Vec::new();
            owned
                .try_reserve_exact(bytes)
                .map_err(|_| Error::AllocationFailed)?;
            owned.resize(bytes, 0);
            // SAFETY: caller-owned live callback, initialized exact-size output.
            if unsafe { copy(opaque, address, count, owned.as_mut_ptr()) } != 0 {
                return Err(Error::OutsideSnapshot);
            }
            Ok(owned)
        };
        let source = depth_sources::copy_depth_sources(
            DepthInput {
                identity,
                bucket: input.bucket,
                marker: input.marker,
                pass: input.pass,
                user: input.user,
            },
            DepthBudget::default(),
            &mut read,
        );
        let source = match source {
            Ok(source) => source,
            Err(error) => {
                if REFUSALS.fetch_add(1, Ordering::Relaxed) < 8 {
                    eprintln!(
                        "[rust-depth-source] invocation={} camera={} status=refused reason={error:?}",
                        input.invocation, input.camera
                    );
                }
                return failed;
            }
        };
        let result = DepthSourceResult {
            status: 0,
            reserved: 0,
            items: source.items.len() as u64,
            reads: source.reads as u64,
            bytes: source.copied_bytes as u64,
        };
        let Ok(mut ready) = sources().lock() else {
            return failed;
        };
        if VM.load(Ordering::Acquire) != input.vm
            || REVOCATION.load(Ordering::Acquire) != revocation
            || ready
                .iter()
                .any(|s| s.identity.invocation == input.invocation)
        {
            return failed;
        }
        // Observation retention only. Consumers retain their own exact Arc; an
        // evicted lookup never changes an existing Main/Depth resource lease.
        if ready.len() == 2 {
            ready.pop_front();
        }
        ready.push_back(Arc::new(source));
        result
    }))
    .unwrap_or(failed)
}
