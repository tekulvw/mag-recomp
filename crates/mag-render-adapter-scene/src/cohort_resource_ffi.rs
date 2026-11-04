//! Narrow synchronous owned-copy ABI. No guest pointer or callback is retained.
//! Every entry point refuses and records nothing while the adapter core's
//! master switch is off.
use crate::{
    cohort::OwnedVisibilityCohort,
    cohort_ffi::retain_latest_cohort,
    cohort_geometry::{DiagnosticGeometry, copy_diagnostic_geometry_with_snapshot},
    cohort_resources::{
        CohortResourceError, MainPassResources, MainPassSnapshot, OwnedCohortResources,
        ResourceCopyBudget, copy_cohort_resources,
    },
    error::Error,
    materials::{
        MaterialCaptureError, MaterialCopyBudget, MaterialPassInput, MaterialPassSources,
        OwnedMainMaterials, copy_main_materials_with_pass_input,
    },
};
use core::ffi::c_void;
use std::{
    panic::{AssertUnwindSafe, catch_unwind},
    sync::{Arc, Mutex, OnceLock},
};

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct CohortResourceInput {
    pub abi_version: u32,
    pub struct_bytes: u32,
    pub vm: u64,
    pub invocation: u64,
    pub render_mode: u32,
    pub technique_override_mask: u32,
    pub geometry_resource_base: u32,
    pub source_pass_address: u32,
}
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct CohortResourceResult {
    pub status: u32,
    pub reserved: u32,
    pub items: u64,
    pub reads: u64,
    pub bytes: u64,
}
pub struct OwnedMainSources {
    pub cohort: Arc<OwnedVisibilityCohort>,
    pub source_snapshot: Option<Arc<crate::shared_scene_sources::SourceSnapshot>>,
    /// Sequential resource, geometry and material capture wall durations. These
    /// include callbacks and allocation; they are neither CPU time nor FPS.
    pub capture_wall_ns: [u64; 3],
    /// Actual guest callbacks and opt-in reuse during this capture. All zero
    /// reuse counters when every `source_acquisition` switch is off.
    pub acquisition: crate::source_acquisition::AcquisitionStats,
    /// Chunked geometry ranges and serial re-runs of this capture (0, 0 when
    /// the chunked schedule is off).
    pub geometry_chunks: (u32, u32),
    pub resources: Result<OwnedCohortResources, CohortResourceError>,
    pub geometry: DiagnosticGeometry,
    pub materials: Result<OwnedMainMaterials, MaterialCaptureError>,
    /// The producer inputs and fog camera observed for this source window.
    pub producers: Option<Arc<crate::producer_sources::ProducerObservation>>,
}
pub use crate::capture_view::SourceCopy;
use crate::capture_view::{SourceWindow, ViewScope};
pub type LocatedSourceCopy = unsafe extern "C" fn(*mut c_void, u32, u32, u32, *mut u8) -> i32;
#[repr(C)]
#[derive(Clone, Copy)]
pub struct CohortResourceInputV3 {
    pub base: CohortResourceInput,
    pub located_copy: Option<LocatedSourceCopy>,
    pub located_opaque: *mut c_void,
}
#[repr(C)]
#[derive(Clone, Copy)]
pub struct CohortResourceInputV4 {
    pub base: CohortResourceInputV3,
    pub source_user_address: u32,
    pub reserved: u32,
}
#[repr(C)]
#[derive(Clone, Copy)]
pub struct CohortResourceInputV5 {
    pub base: CohortResourceInputV4,
    pub source_toc_address: u32,
    pub reserved: u32,
    pub producer_source_window: u64,
}
#[derive(Default)]
struct State {
    vm: u64,
    generation: u64,
    exhausted: bool,
    next_claim: u64,
    claim: Option<(u64, u64)>,
    claim_key: Option<MainSourceKey>,
    discarded_claim: Option<u64>,
    // Guest source invocations are monotonic. Preserve existing older leases,
    // but never copy new guest bytes for an original-continuation publication.
    discarded_through: u64,
    consumed_invocation: u64,
    ready: [Option<Arc<OwnedMainSources>>; 2],
    revocation: u64,
}
static ASSETS: OnceLock<Mutex<State>> = OnceLock::new();
fn store() -> &'static Mutex<State> {
    ASSETS.get_or_init(|| Mutex::new(State::default()))
}
// Copies of the store's (vm, generation), written under its lock after every
// change. The generation only grows and a vm change always advances it (or
// clears vm), so a read observing both equal to a snapshot means unchanged.
static MIRROR_VM: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);
static MIRROR_GENERATION: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);
fn mirror(s: &State) {
    use std::sync::atomic::Ordering;
    MIRROR_GENERATION.store(s.generation, Ordering::Release);
    MIRROR_VM.store(s.vm, Ordering::Release);
}
fn advance_copy(s: &mut State) {
    if s.exhausted {
        return;
    }
    if let Some(next) = s.generation.checked_add(1) {
        s.generation = next;
    } else {
        s.vm = 0;
        s.exhausted = true;
    }
    mirror(s);
}
fn invalidate(s: &mut State) {
    s.ready = [None, None];
    advance_copy(s);
    if let Some(next) = s.revocation.checked_add(1) {
        s.revocation = next;
    } else {
        s.exhausted = true;
        s.vm = 0;
    }
    mirror(s);
}
pub(crate) fn begin_copy(vm: u64) {
    if let Ok(mut s) = store().lock()
        && s.vm == vm
    {
        advance_copy(&mut s);
    }
}
pub(crate) fn vm_changed(vm: u64) {
    crate::decoded_geometry_cache::vm_changed();
    crate::source_acquisition::vm_changed();
    crate::depth_source_ffi::vm_changed(vm);
    crate::producer_sources::vm_changed(vm);
    if let Ok(mut s) = store().lock() {
        invalidate(&mut s);
        s.vm = if s.exhausted { 0 } else { vm };
        mirror(&s);
        s.claim = None;
        s.claim_key = None;
        s.discarded_claim = None;
        s.discarded_through = 0;
        s.consumed_invocation = 0;
    }
}
pub(crate) fn decline(vm: u64) {
    if let Ok(mut s) = store().lock()
        && s.vm == vm
    {
        invalidate(&mut s);
    }
}
pub fn take_latest_main_sources() -> Option<Arc<OwnedMainSources>> {
    let mut s = store().lock().ok()?;
    let index = s
        .ready
        .iter()
        .enumerate()
        .filter_map(|(i, x)| x.as_ref().map(|x| (i, x.cohort.header().source_invocation)))
        .max_by_key(|(_, inv)| *inv)?
        .0;
    s.ready[index].take()
}

/// Exact already-observed source association, never "latest frame" selection.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct MainSourceKey {
    pub vm: u64,
    pub invocation: u64,
    pub owner: u64,
    pub pass_occurrence: u64,
    pub execution_update: u64,
    pub camera: u32,
}
impl MainSourceKey {
    pub fn matches(self, source: &OwnedMainSources) -> bool {
        let h = source.cohort.header();
        self.vm == h.vm
            && self.invocation == h.source_invocation
            && self.owner == h.ppu_owner
            && self.pass_occurrence == h.pass_occurrence
            && self.execution_update == h.execution_update
            && self.camera == h.camera
    }
}
/// Owns immutable source bytes across a synchronous queue-commit decision.
/// Drop releases a definitely-uncommitted claim. Call consume for accepted OR
/// indeterminate work before returning; GPU completion is a separate concern.
pub struct MainSourcesClaim {
    pub source: Arc<OwnedMainSources>,
    token: u64,
    revocation: u64,
    consumed: bool,
}
impl MainSourcesClaim {
    pub fn consume(mut self) {
        self.consumed = true;
    }
}
impl Drop for MainSourcesClaim {
    fn drop(&mut self) {
        let Ok(mut s) = store().lock() else {
            return;
        };
        let h = self.source.cohort.header();
        if s.vm != h.vm || s.claim != Some((self.token, h.source_invocation)) {
            return;
        }
        s.claim = None;
        s.claim_key = None;
        let discarded = s.discarded_claim == Some(self.token);
        s.discarded_claim = None;
        if discarded {
            return;
        }
        if self.consumed {
            s.consumed_invocation = s.consumed_invocation.max(h.source_invocation);
            let consumed = s.consumed_invocation;
            for ready in &mut s.ready {
                if ready
                    .as_ref()
                    .is_some_and(|x| x.cohort.header().source_invocation <= consumed)
                {
                    *ready = None;
                }
            }
        } else if s.revocation == self.revocation
            && h.source_invocation > s.consumed_invocation
            && let Some(slot) = s.ready.iter_mut().find(|x| x.is_none())
        {
            *slot = Some(Arc::clone(&self.source));
        }
    }
}
pub fn reserve_main_sources(key: MainSourceKey) -> Option<MainSourcesClaim> {
    let mut s = store().lock().ok()?;
    if s.exhausted || s.vm != key.vm || s.claim.is_some() || key.invocation <= s.consumed_invocation
    {
        return None;
    }
    let index = s
        .ready
        .iter()
        .position(|x| x.as_ref().is_some_and(|x| key.matches(x)))?;
    let token = s.next_claim.checked_add(1)?;
    s.next_claim = token;
    s.claim = Some((token, key.invocation));
    s.claim_key = Some(key);
    s.discarded_claim = None;
    Some(MainSourcesClaim {
        source: s.ready[index].take()?,
        token,
        revocation: s.revocation,
        consumed: false,
    })
}

/// Retire only the exact publication whose original Main continuation entered.
/// Existing older ready/claimed snapshots remain usable. A monotonic capture
/// watermark prevents a later copy of this or earlier guest bytes; it does not
/// invalidate existing immutable older leases. Only an exact active claim is
/// made non-restorable, by its non-reused token rather than global revocation.
/// Returns0removed ready,2marked active claim,1unknown/stale/mismatch.
pub fn discard_original_main_source(key: MainSourceKey) -> u32 {
    let Ok(mut s) = store().lock() else {
        return 1;
    };
    if key.vm == 0
        || s.vm != key.vm
        || key.invocation == 0
        || key.owner == 0
        || key.pass_occurrence == 0
        || key.execution_update == 0
        || key.camera > 3
    {
        return 1;
    }
    if let Some(index) = s
        .ready
        .iter()
        .position(|x| x.as_ref().is_some_and(|x| key.matches(x)))
    {
        s.ready[index] = None;
        s.discarded_through = s.discarded_through.max(key.invocation);
        return 0;
    }
    if s.claim_key == Some(key)
        && let Some((token, invocation)) = s.claim
        && invocation == key.invocation
    {
        s.discarded_claim = Some(token);
        s.discarded_through = s.discarded_through.max(key.invocation);
        return 2;
    }
    1
}
/// Source-qualified original-continuation notification only. This discards
/// owned bytes; it never publishes job completion, GPU work or a native proof.
#[unsafe(no_mangle)]
pub extern "C" fn mag_render_adapter_main_original(
    vm: u64,
    invocation: u64,
    owner: u64,
    pass_occurrence: u64,
    execution_update: u64,
    camera: u32,
) -> u32 {
    if !mag_render_adapter_core::runtime::enabled() {
        return 1;
    }
    discard_original_main_source(MainSourceKey {
        vm,
        invocation,
        owner,
        pass_occurrence,
        execution_update,
        camera,
    })
}

/// Copy selected raw assets while the genuine source caller holds its read
/// window. The identifiers match an already observed cohort; they grant no new
/// readiness, asset generation, complete draw or original-removal capability.
///
/// # Safety
/// `copy` must be callable synchronously on this thread for this invocation.
/// For each request it may write at most the supplied byte count into `out` and
/// must initialize exactly that count before returning zero. `opaque` must stay
/// valid until this call returns. No callback, guest address mapping, borrowed
/// buffer or opaque pointer is retained. Production C++ validates actual VM,
/// context/owner/source lifetime and each complete guest range before copying.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mag_render_adapter_cohort_assets(
    input: CohortResourceInput,
    copy: Option<SourceCopy>,
    opaque: *mut c_void,
) -> CohortResourceResult {
    // SAFETY: the ABI2 caller supplies the same synchronous guest-copy window.
    unsafe { capture_owned(input, copy, opaque, None, std::ptr::null_mut(), None, None) }
}

/// ABI3 adds checked located texture reads without changing the ABI2 entry.
///
/// # Safety
/// `input` must be aligned and readable for the full input structure, whose
/// callbacks/opaque objects remain valid for this synchronous invocation.
/// The located callback must initialize exactly `bytes` on success, resolve the
/// explicit location without guessing, and obey the same actual source window
/// as `copy`. No callback, opaque pointer or guest mapping is retained.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mag_render_adapter_cohort_assets_v3(
    input: *const CohortResourceInputV3,
    copy: Option<SourceCopy>,
    opaque: *mut c_void,
) -> CohortResourceResult {
    let failed = CohortResourceResult {
        status: 1,
        ..CohortResourceResult::default()
    };
    if input.is_null() || !(input as usize).is_multiple_of(align_of::<CohortResourceInputV3>()) {
        return failed;
    }
    // SAFETY: caller supplies the complete readable input structure.
    let input = unsafe { *input };
    if input.base.abi_version != 3
        || input.base.struct_bytes as usize != size_of::<CohortResourceInputV3>()
        || input.located_copy.is_none()
    {
        return failed;
    }
    // Normalize only ABI metadata; all actual identifiers, ownership, queue
    // capacity, generation/revocation and consume rules stay in the common path.
    let base = CohortResourceInput {
        abi_version: 2,
        struct_bytes: size_of::<CohortResourceInput>() as u32,
        ..input.base
    };
    // SAFETY: both caller-owned read windows remain live until this call returns.
    unsafe {
        capture_owned(
            base,
            copy,
            opaque,
            input.located_copy,
            input.located_opaque,
            None,
            None,
        )
    }
}

/// ABI4 adds the actual validated Main job user source address. ABI2/3 remain
/// unchanged and explicitly lack this setup source; population never implies it.
///
/// # Safety
/// Same complete readable pointer/callback/source-window contract as ABI3.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mag_render_adapter_cohort_assets_v4(
    input: *const CohortResourceInputV4,
    copy: Option<SourceCopy>,
    opaque: *mut c_void,
) -> CohortResourceResult {
    let failed = CohortResourceResult {
        status: 1,
        ..CohortResourceResult::default()
    };
    if input.is_null() || !(input as usize).is_multiple_of(align_of::<CohortResourceInputV4>()) {
        return failed;
    }
    // SAFETY: caller provides the complete live readable source structure.
    let input = unsafe { *input };
    if input.base.base.abi_version != 4
        || input.base.base.struct_bytes as usize != size_of::<CohortResourceInputV4>()
        || input.base.located_copy.is_none()
        || input.reserved != 0
        || input.source_user_address == 0
        || input.source_user_address & 3 != 0
    {
        return failed;
    }
    let base = CohortResourceInput {
        abi_version: 2,
        struct_bytes: size_of::<CohortResourceInput>() as u32,
        ..input.base.base
    };
    // SAFETY: caller's exact source and located windows stay live synchronously.
    unsafe {
        capture_owned(
            base,
            copy,
            opaque,
            input.base.located_copy,
            input.base.located_opaque,
            Some(input.source_user_address),
            None,
        )
    }
}

/// ABI5 joins an exact pre-generator source observation. No latest lookup.
/// # Safety
/// Same complete readable input and synchronous source-window contract as ABI4.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mag_render_adapter_cohort_assets_v5(
    input: *const CohortResourceInputV5,
    copy: Option<SourceCopy>,
    opaque: *mut c_void,
) -> CohortResourceResult {
    let failed = CohortResourceResult {
        status: 1,
        ..CohortResourceResult::default()
    };
    if input.is_null() || !(input as usize).is_multiple_of(align_of::<CohortResourceInputV5>()) {
        return failed;
    }
    // SAFETY: caller supplies the complete readable structure.
    let input = unsafe { *input };
    let base = input.base;
    if base.base.base.abi_version != 5
        || base.base.base.struct_bytes as usize != size_of::<CohortResourceInputV5>()
        || base.base.located_copy.is_none()
        || base.reserved != 0
        || base.source_user_address == 0
        || !base.source_user_address.is_multiple_of(4)
        || input.reserved != 0
        || input.source_toc_address < 0x243c
        || !input.source_toc_address.is_multiple_of(4)
        || input.producer_source_window == 0
    {
        return failed;
    }
    let normalized = CohortResourceInput {
        abi_version: 2,
        struct_bytes: size_of::<CohortResourceInput>() as u32,
        ..base.base.base
    };
    // SAFETY: both read windows remain live for this complete synchronous call.
    unsafe {
        capture_owned(
            normalized,
            copy,
            opaque,
            base.base.located_copy,
            base.base.located_opaque,
            Some(base.source_user_address),
            Some((input.source_toc_address, input.producer_source_window)),
        )
    }
}

unsafe fn capture_owned(
    input: CohortResourceInput,
    copy: Option<SourceCopy>,
    opaque: *mut c_void,
    located_copy: Option<LocatedSourceCopy>,
    located_opaque: *mut c_void,
    source_user_address: Option<u32>,
    producer_source: Option<(u32, u64)>,
) -> CohortResourceResult {
    let failed = CohortResourceResult {
        status: 1,
        ..CohortResourceResult::default()
    };
    // Every asset-copy entry point arrives here: nothing is read or recorded
    // while the master switch is off.
    if !mag_render_adapter_core::runtime::enabled() {
        return failed;
    }
    catch_unwind(AssertUnwindSafe(|| {
        let generation = {
            let Ok(mut s) = store().lock() else {
                return failed;
            };
            if input.abi_version != 2
                || input.struct_bytes as usize != size_of::<CohortResourceInput>()
                || input.source_pass_address == 0
                || input.vm == 0
                || s.vm != input.vm
                || s.generation == 0
                || input.invocation <= s.consumed_invocation
                || input.invocation <= s.discarded_through
                || s.claim
                    .is_some_and(|(_, invocation)| invocation == input.invocation)
            {
                return failed;
            }
            // Bound retained immutable sources, including the active claim.
            let same = s.ready.iter().position(|x| {
                x.as_ref()
                    .is_some_and(|x| x.cohort.header().source_invocation == input.invocation)
            });
            if same.is_none()
                && s.ready.iter().flatten().count() + usize::from(s.claim.is_some()) >= 2
            {
                return CohortResourceResult {
                    status: 2,
                    ..failed
                };
            }
            if let Some(index) = same {
                s.ready[index] = None;
            }
            s.generation
        };
        let Some(copy) = copy else {
            return failed;
        };
        let Some(cohort) = retain_latest_cohort(input.vm, input.invocation) else {
            decline(input.vm);
            return failed;
        };
        let producers = if let Some((toc, window)) = producer_source {
            let h = cohort.header();
            let Some(p) = crate::producer_sources::retain(
                input.vm,
                window,
                toc,
                h.ppu_owner,
                h.execution_update,
            ) else {
                return failed;
            };
            Some(p)
        } else {
            None
        };
        let source_snapshot = crate::shared_scene_sources::enabled().then(|| {
            std::cell::RefCell::new(crate::shared_scene_sources::SourceSnapshot::default())
        });
        use crate::source_acquisition::{self as acquisition, ReadDomain};
        let acquisition_start = acquisition::thread_stats();
        let profile_start = acquisition::thread_profile();
        // Lives only for this synchronous call; never retained across captures.
        let share = std::cell::RefCell::new(acquisition::CaptureReadShare::new(
            acquisition::read_share_enabled(),
            acquisition::ShareLimits::default(),
        ));
        let pooled = acquisition::resident_textures_enabled();
        let direct = DirectReads {
            copy,
            opaque,
            located: located_copy,
            located_opaque,
            vm: input.vm,
            generation,
            pooled,
        };
        // Copy-free validation only where the copy path keeps no other record
        // of each read (read share and source snapshots stay copy-based).
        let _view_scope = (!acquisition::read_share_enabled() && source_snapshot.is_none())
            .then(|| direct.view_scope());
        let mut read_owned = |request: crate::partition_inputs::StreamRequest| {
            direct.current()?;
            u32::try_from(request.bytes).map_err(|_| Error::InvalidRange)?;
            let shared =
                share
                    .borrow_mut()
                    .shared(ReadDomain::Guest, request.address, request.bytes);
            let owned = if let Some(owned) = shared {
                owned
            } else {
                // SAFETY: this is the bridge's synchronous calling thread.
                let owned = unsafe { direct.guest_unchecked(request) }?;
                share
                    .borrow_mut()
                    .record(ReadDomain::Guest, request.address, &owned);
                owned
            };
            if let Some(snapshot) = &source_snapshot {
                // Observation only: a catalog budget refusal is explicit in the
                // optional snapshot, never a change to existing Main admission.
                let _ = snapshot.borrow_mut().observe(
                    crate::shared_scene_sources::SourceDomain::Guest,
                    request.address,
                    &owned,
                );
            }
            Ok(owned)
        };
        let budget = ResourceCopyBudget {
            bytes: 128 * 1024 * 1024,
            reads: 1048576,
            partition_headers: 16384,
        };
        let resource_capture_start = std::time::Instant::now();
        let resources = copy_cohort_resources(
            &cohort,
            MainPassResources {
                render_mode: input.render_mode as i32,
                technique_override_mask: input.technique_override_mask,
                geometry_resource_base: input.geometry_resource_base,
            },
            budget,
            &mut read_owned,
        );
        let resource_capture_ns =
            u64::try_from(resource_capture_start.elapsed().as_nanos()).unwrap_or(u64::MAX);
        let mut read_located = |request: crate::materials::textures::LocatedTextureRequest| {
            use crate::materials::textures::TextureError;
            located_copy.ok_or(TextureError::MissingSource)?;
            direct.current().map_err(TextureError::Source)?;
            u32::try_from(request.bytes).map_err(|_| TextureError::Extent)?;
            let domain = match request.location {
                crate::materials::textures::TextureLocation::Local => ReadDomain::Local,
                crate::materials::textures::TextureLocation::Main => ReadDomain::Main,
            };
            let shared = share
                .borrow_mut()
                .shared(domain, request.offset, request.bytes);
            let owned = if let Some(owned) = shared {
                owned
            } else {
                // SAFETY: this is the bridge's synchronous calling thread.
                let owned = unsafe { direct.located_unchecked(request) }?;
                share.borrow_mut().record(domain, request.offset, &owned);
                owned
            };
            if let Some(snapshot) = &source_snapshot {
                let domain = match request.location {
                    crate::materials::textures::TextureLocation::Local => {
                        crate::shared_scene_sources::SourceDomain::Local
                    }
                    crate::materials::textures::TextureLocation::Main => {
                        crate::shared_scene_sources::SourceDomain::Main
                    }
                };
                let _ = snapshot
                    .borrow_mut()
                    .observe(domain, request.offset, &owned);
            }
            Ok(owned)
        };
        // A material/format refusal cannot silently remove the remaining scene
        // population. Diagnostic geometry processes all items independently;
        // original preparation stays accounted and all omissions are retained.
        let geometry_capture_start = std::time::Instant::now();
        // The Main pass object named by this invocation is owned by this same source publication.
        // Copy it once; neither a guest EA nor an asset/frame epoch substitutes
        // for that caller-held window. Borrowed fields never escape this call.
        let Ok(pass_bytes) = read_owned(crate::partition_inputs::StreamRequest {
            address: input.source_pass_address,
            bytes: 688,
        }) else {
            return failed;
        };
        let Ok(pass_snapshot) = MainPassSnapshot::new(input.source_pass_address, &pass_bytes)
        else {
            return failed;
        };
        let geometry_budget = ResourceCopyBudget {
            bytes: budget.bytes - 688,
            reads: budget.reads - 1,
            ..budget
        };
        let material_input = || MaterialPassInput {
            parameters: MaterialPassSources {
                source_pass_address: input.source_pass_address,
                asset_bank: input.geometry_resource_base,
                render_mode: input.render_mode as i32,
                technique_override_mask: input.technique_override_mask,
            },
            snapshot: Some(pass_snapshot),
        };
        // Read sharing and source-snapshot observation record the order of
        // requests; keep those (opt-in) diagnostics on the serial schedule.
        let concurrent = (!serial_capture_phases() || helper_reads_enabled())
            && !acquisition::read_share_enabled()
            && source_snapshot.is_none();
        let phases = if concurrent {
            concurrent_phases(
                &cohort,
                resources.as_ref().ok(),
                input.geometry_resource_base,
                pass_snapshot,
                geometry_budget,
                material_input(),
                source_user_address,
                helper_reads_enabled().then_some(direct),
                &mut read_owned,
                &mut read_located,
            )
        } else {
            None
        };
        // 0 serial, 1 concurrent with source-thread round trips, 2 helper reads.
        let phases_mode: u8 = match &phases {
            Some(Ok(_)) if helper_reads_enabled() => 2,
            Some(Ok(_)) => 1,
            _ => 0,
        };
        let (geometry, geometry_capture_ns, materials, material_capture_ns, helper) = match phases {
            Some(Ok(done)) => done,
            Some(Err(())) => return failed,
            None => {
                let Ok(geometry) = copy_diagnostic_geometry_with_snapshot(
                    &cohort,
                    resources.as_ref().ok(),
                    input.geometry_resource_base,
                    pass_snapshot,
                    geometry_budget,
                    &mut read_owned,
                    &mut read_located,
                ) else {
                    return failed;
                };
                let geometry_capture_ns =
                    u64::try_from(geometry_capture_start.elapsed().as_nanos()).unwrap_or(u64::MAX);
                let material_capture_start = std::time::Instant::now();
                let materials = capture_materials(
                    &cohort,
                    resources.as_ref().ok(),
                    material_input(),
                    source_user_address,
                    &mut read_owned,
                    &mut read_located,
                );
                let material_capture_ns =
                    u64::try_from(material_capture_start.elapsed().as_nanos()).unwrap_or(u64::MAX);
                (
                    geometry,
                    geometry_capture_ns,
                    materials,
                    material_capture_ns,
                    HelperCounts::default(),
                )
            }
        };
        let acquisition_stats = acquisition::thread_stats()
            .since(acquisition_start)
            .plus(helper.stats);
        let profile_end = acquisition::thread_profile();
        let profile_end: [u64; acquisition::SECTIONS] =
            std::array::from_fn(|i| profile_end[i].wrapping_add(helper.profile[i]));
        log_acquisition(
            input.invocation,
            [
                resource_capture_ns,
                geometry_capture_ns,
                material_capture_ns,
            ],
            phases_mode,
            helper.stats.guest_callbacks,
            (helper.chunks, helper.reruns),
            acquisition_stats,
            std::array::from_fn(|i| profile_end[i].wrapping_sub(profile_start[i])),
        );
        let result = CohortResourceResult {
            status: 0,
            reserved: 0,
            items: geometry.items.len() as u64,
            reads: 1
                + resources.as_ref().map_or(0, |r| r.reads as u64)
                + geometry.source_reads as u64
                + materials.as_ref().map_or(0, |m| m.source_reads as u64)
                + producers.as_ref().map_or(0, |p| p.capture.reads as u64),
            bytes: 688
                + resources.as_ref().map_or(0, |r| r.copied_bytes as u64)
                + geometry.source_bytes as u64
                + materials.as_ref().map_or(0, |m| m.source_bytes as u64)
                + producers
                    .as_ref()
                    .map_or(0, |p| p.capture.copied_bytes as u64),
        };
        let Ok(mut s) = store().lock() else {
            return failed;
        };
        if s.vm != input.vm
            || s.generation != generation
            || input.invocation <= s.consumed_invocation
            || input.invocation <= s.discarded_through
            || s.claim
                .is_some_and(|(_, invocation)| invocation == input.invocation)
        {
            return failed;
        }
        if s.ready
            .iter()
            .flatten()
            .any(|x| x.cohort.header().source_invocation == input.invocation)
        {
            return failed;
        }
        let Some(slot) = s.ready.iter_mut().find(|x| x.is_none()) else {
            return failed;
        };
        *slot = Some(Arc::new(OwnedMainSources {
            cohort,
            source_snapshot: source_snapshot.map(|s| Arc::new(s.into_inner())),
            capture_wall_ns: [
                resource_capture_ns,
                geometry_capture_ns,
                material_capture_ns,
            ],
            acquisition: acquisition_stats,
            geometry_chunks: (helper.chunks, helper.reruns),
            resources,
            geometry,
            materials,
            producers,
        }));
        result
    }))
    .unwrap_or(failed)
}

/// Rate-limited opt-in report; silent unless a `source_acquisition` switch is
/// on, so default runs keep their existing console output unchanged.
fn log_acquisition(
    invocation: u64,
    wall_ns: [u64; 3],
    phases_mode: u8,
    helper_callbacks: u64,
    (chunks, reruns): (u32, u32),
    stats: crate::source_acquisition::AcquisitionStats,
    profile_ns: [u64; crate::source_acquisition::SECTIONS],
) {
    use std::sync::atomic::{AtomicU64, Ordering};
    static CAPTURES: AtomicU64 = AtomicU64::new(0);
    if !crate::source_acquisition::any_enabled()
        && !helper_reads_enabled()
        && !crate::source_acquisition::concurrent_caches_enabled()
        && !crate::source_acquisition::view_validation_enabled()
    {
        return;
    }
    let n = CAPTURES.fetch_add(1, Ordering::Relaxed) + 1;
    if n > 8 && !n.is_multiple_of(120) {
        return;
    }
    let (textures, texture_bytes) = crate::source_acquisition::resident_texture_occupancy();
    eprintln!(
        "[mag source acquisition] invocation={invocation} captures={n} read_share={} resident_textures={} content_probe={} wall_ns_resources_geometry_materials={wall_ns:?} phases={phases_mode} helper_callbacks={helper_callbacks} geometry_chunks={chunks} geometry_chunk_reruns={reruns} concurrent_caches={} catalog_unlocked_hits={} guest_callbacks={} guest_callback_bytes={} shared_reads={} shared_bytes={} resident_hits={} resident_hit_bytes={} resident_misses={} resident_invalidations={} pooled_buffers={} view_validation={} view_equal={} view_equal_bytes={} view_differ={} decoded_cache_busy={} large_streams={} resident_texture_entries={textures} resident_texture_bytes={texture_bytes} content_probe_stats={:?} profile={} profile_ns_callback_decodedlookup_primary_aux_group0_noninstanced_skinning_static_textures_materialintern={profile_ns:?}",
        u8::from(crate::source_acquisition::read_share_enabled()),
        u8::from(crate::source_acquisition::resident_textures_enabled()),
        u8::from(crate::source_acquisition::content_probe_enabled()),
        u8::from(crate::source_acquisition::concurrent_caches_enabled()),
        crate::shared_scene_sources::unlocked_hits(),
        stats.guest_callbacks,
        stats.guest_callback_bytes,
        stats.shared_reads,
        stats.shared_bytes,
        stats.resident_hits,
        stats.resident_hit_bytes,
        stats.resident_misses,
        stats.resident_invalidations,
        stats.pooled_buffers,
        u8::from(crate::source_acquisition::view_validation_enabled()),
        stats.view_equal,
        stats.view_equal_bytes,
        stats.view_differ,
        crate::decoded_geometry_cache::busy_decodes(),
        crate::cohort_geometry::large_streams(),
        crate::shared_scene_sources::probe_stats(),
        u8::from(crate::source_acquisition::profile_enabled()),
    );
}

/// Default-off `PS3_RENDER_CAPTURE_HELPER_READS=1`: run materials concurrently
/// with geometry, and let the material helper call the bridge copy callbacks
/// itself instead of asking the source thread for every read. The bridge must
/// open its helper read window for the same flag: it accepts a read from
/// another thread only while the source thread is inside this synchronous
/// call, which joins the helper before it returns. Reads, budgets, outputs and
/// counters are those of the serial schedule; only the calling thread differs.
pub fn helper_reads_enabled() -> bool {
    static F: OnceLock<bool> = OnceLock::new();
    *F.get_or_init(|| {
        std::env::var_os("PS3_RENDER_CAPTURE_HELPER_READS").is_some_and(|v| v == "1")
            && !std::env::var_os("PS3_RENDER_SERIAL_CAPTURE_PHASES").is_some_and(|v| v == "1")
    })
}
/// The bridge callbacks of one capture call and the store generation they are
/// valid for. Shared by the calling thread and (with helper reads) the
/// material helper; it never outlives the synchronous capture call.
#[derive(Clone, Copy)]
struct DirectReads {
    copy: SourceCopy,
    opaque: *mut c_void,
    located: Option<LocatedSourceCopy>,
    located_opaque: *mut c_void,
    vm: u64,
    generation: u64,
    pooled: bool,
}
// SAFETY: the pointers are the bridge's opaque contexts for this synchronous
// call. They are only dereferenced by the bridge, which validates the calling
// thread against its open window; the scoped helper is joined before return.
unsafe impl Send for DirectReads {}
unsafe impl Sync for DirectReads {}
/// Whether `(vm, generation)` is still the store's current source window.
fn window_current(vm: u64, generation: u64) -> bool {
    if crate::source_acquisition::concurrent_caches_enabled() {
        use std::sync::atomic::Ordering;
        return MIRROR_GENERATION.load(Ordering::Acquire) == generation
            && MIRROR_VM.load(Ordering::Acquire) == vm;
    }
    store()
        .lock()
        .is_ok_and(|state| state.vm == vm && state.generation == generation)
}
impl DirectReads {
    /// The copy-free view provider for this capture call, if enabled.
    fn view_scope(&self) -> ViewScope {
        ViewScope::install(
            self.opaque,
            self.located.map(|_| self.located_opaque),
            SourceWindow {
                vm: self.vm,
                generation: self.generation,
                current: window_current,
            },
        )
    }
    fn current(&self) -> Result<(), Error> {
        if crate::source_acquisition::concurrent_caches_enabled() {
            use std::sync::atomic::Ordering;
            // Lock-free form of the check below (see `mirror`).
            let generation = MIRROR_GENERATION.load(Ordering::Acquire);
            let vm = MIRROR_VM.load(Ordering::Acquire);
            if vm != self.vm || generation != self.generation {
                return Err(Error::UnprovenProducerEpoch);
            }
            return Ok(());
        }
        let state = store().lock().map_err(|_| Error::UnprovenProducerEpoch)?;
        if state.vm != self.vm || state.generation != self.generation {
            return Err(Error::UnprovenProducerEpoch);
        }
        Ok(())
    }
    /// One counted guest copy into fresh owned storage.
    /// # Safety
    /// Called during the capture call, on its thread or within the window.
    unsafe fn guest_unchecked(
        &self,
        request: crate::partition_inputs::StreamRequest,
    ) -> Result<Vec<u8>, Error> {
        use crate::source_acquisition as acquisition;
        let length = u32::try_from(request.bytes).map_err(|_| Error::InvalidRange)?;
        let _timer = acquisition::section(acquisition::Section::GuestCallback);
        let mut owned = Vec::new();
        owned
            .try_reserve_exact(request.bytes)
            .map_err(|_| Error::AllocationFailed)?;
        // SAFETY: required synchronous callback contract: on success it
        // initializes exactly the requested extent of this live owned
        // capacity. Only then does the length cover those bytes, so no
        // zero pre-fill is needed and no uninitialized byte is exposed.
        if unsafe { (self.copy)(self.opaque, request.address, length, owned.as_mut_ptr()) } != 0 {
            return Err(Error::OutsideSnapshot);
        }
        // SAFETY: see above; capacity >= request.bytes, all initialized.
        unsafe { owned.set_len(request.bytes) };
        acquisition::count_callback(request.bytes);
        Ok(owned)
    }
    /// One counted located (RSX local/main) copy.
    /// # Safety
    /// As `guest_unchecked`.
    unsafe fn located_unchecked(
        &self,
        request: crate::materials::textures::LocatedTextureRequest,
    ) -> Result<Vec<u8>, crate::materials::textures::TextureError> {
        use crate::{materials::textures::TextureError, source_acquisition as acquisition};
        let copy = self.located.ok_or(TextureError::MissingSource)?;
        let bytes = u32::try_from(request.bytes).map_err(|_| TextureError::Extent)?;
        let _timer = acquisition::section(acquisition::Section::GuestCallback);
        let recycled = acquisition::pooled_buffer(request.bytes, self.pooled);
        let initialized = recycled.is_some();
        let mut owned = match recycled {
            Some(buffer) => buffer,
            None => {
                let mut owned = Vec::new();
                owned
                    .try_reserve_exact(request.bytes)
                    .map_err(|_| TextureError::Allocation)?;
                owned
            }
        };
        // SAFETY: the material reader has charged its bounded source budget.
        // On success the callback initializes exactly `bytes` bytes at
        // this pointer: either a fully initialized recycled buffer of that
        // length, or live owned capacity of at least that extent.
        if unsafe {
            copy(
                self.located_opaque,
                request.location as u32,
                request.offset,
                bytes,
                owned.as_mut_ptr(),
            )
        } != 0
        {
            return Err(TextureError::Source(Error::OutsideSnapshot));
        }
        if !initialized {
            // SAFETY: the successful callback initialized the extent.
            unsafe { owned.set_len(request.bytes) };
        }
        acquisition::count_callback(request.bytes);
        Ok(owned)
    }
}
use crate::cohort_geometry::ChunkReaders;
/// Helper-thread reads with the calling thread's checks, in the same order.
impl ChunkReaders for DirectReads {
    fn guest(&self, r: crate::partition_inputs::StreamRequest) -> Result<Vec<u8>, Error> {
        self.current()?;
        u32::try_from(r.bytes).map_err(|_| Error::InvalidRange)?;
        // SAFETY: helper reads are only enabled together with the bridge's
        // window for this capture call, whose scoped helpers join before return.
        unsafe { self.guest_unchecked(r) }
    }
    fn located(
        &self,
        r: crate::materials::textures::LocatedTextureRequest,
    ) -> Result<Vec<u8>, crate::materials::textures::TextureError> {
        use crate::materials::textures::TextureError;
        self.located.ok_or(TextureError::MissingSource)?;
        self.current().map_err(TextureError::Source)?;
        u32::try_from(r.bytes).map_err(|_| TextureError::Extent)?;
        // SAFETY: as above.
        unsafe { self.located_unchecked(r) }
    }
}
/// Serial is the default: the concurrent material phase routes every helper
/// read through a source-thread round trip, which measured ~13x slower in the
/// client (materials 4.4 -> 56 ms). Opt in with
/// PS3_RENDER_CONCURRENT_CAPTURE_PHASES=1; PS3_RENDER_SERIAL_CAPTURE_PHASES=1
/// still forces serial.
fn serial_capture_phases() -> bool {
    static F: OnceLock<bool> = OnceLock::new();
    *F.get_or_init(|| {
        std::env::var_os("PS3_RENDER_SERIAL_CAPTURE_PHASES").is_some_and(|v| v == "1")
            || !std::env::var_os("PS3_RENDER_CONCURRENT_CAPTURE_PHASES").is_some_and(|v| v == "1")
    })
}
/// Thread-local acquisition counters of the material helper, added to the
/// capture thread's so the per-capture totals stay complete.
#[derive(Default)]
struct HelperCounts {
    stats: crate::source_acquisition::AcquisitionStats,
    profile: [u64; crate::source_acquisition::SECTIONS],
    /// Chunked geometry ranges, and whether the exactness guard re-ran serial.
    chunks: u32,
    reruns: u32,
}
fn capture_materials(
    cohort: &OwnedVisibilityCohort,
    resources: Option<&OwnedCohortResources>,
    input: MaterialPassInput<'_>,
    source_user_address: Option<u32>,
    read: &mut impl FnMut(crate::partition_inputs::StreamRequest) -> Result<Vec<u8>, Error>,
    located: &mut impl FnMut(
        crate::materials::textures::LocatedTextureRequest,
    ) -> Result<Vec<u8>, crate::materials::textures::TextureError>,
) -> Result<OwnedMainMaterials, MaterialCaptureError> {
    copy_main_materials_with_pass_input(
        cohort,
        resources,
        input,
        MaterialCopyBudget {
            bytes: 128 * 1024 * 1024,
            reads: 1048576,
            items: 262144,
        },
        source_user_address,
        read,
        located,
    )
}
enum PhaseRead {
    Guest(crate::partition_inputs::StreamRequest),
    Located(crate::materials::textures::LocatedTextureRequest),
}
enum PhaseReply {
    Guest(Result<Vec<u8>, Error>),
    Located(Result<Vec<u8>, crate::materials::textures::TextureError>),
}
type Phases = (
    DiagnosticGeometry,
    u64,
    Result<OwnedMainMaterials, MaterialCaptureError>,
    u64,
    HelperCounts,
);
/// Geometry on the calling (source) thread while materials run on one scoped
/// helper. The bridge callbacks are only valid on the calling thread, so the
/// helper never calls them: it sends each request here and blocks for the
/// reply, and this thread performs it with the same closures (same checks,
/// counters and budgets) between its own geometry reads and after geometry.
/// Each phase keeps its own budget, exactly as in the serial order, so all
/// outputs, refusals and per-phase counters are those of the serial capture.
/// `None` means no helper could start: run the serial schedule.
#[allow(clippy::too_many_arguments)]
fn concurrent_phases<F, L>(
    cohort: &OwnedVisibilityCohort,
    resources: Option<&OwnedCohortResources>,
    resource_base: u32,
    pass: MainPassSnapshot<'_>,
    geometry_budget: ResourceCopyBudget,
    material_input: MaterialPassInput<'_>,
    source_user_address: Option<u32>,
    direct: Option<DirectReads>,
    read_owned: &mut F,
    read_located: &mut L,
) -> Option<Result<Phases, ()>>
where
    F: FnMut(crate::partition_inputs::StreamRequest) -> Result<Vec<u8>, Error>,
    L: FnMut(
        crate::materials::textures::LocatedTextureRequest,
    ) -> Result<Vec<u8>, crate::materials::textures::TextureError>,
{
    use crate::materials::textures::TextureError;
    use std::{cell::RefCell, sync::mpsc};
    let read_owned = RefCell::new(read_owned);
    let read_located = RefCell::new(read_located);
    let (request_tx, request_rx) = mpsc::channel::<PhaseRead>();
    let (reply_tx, reply_rx) = mpsc::channel::<PhaseReply>();
    let serve = |request: PhaseRead| {
        let reply = match request {
            PhaseRead::Guest(r) => PhaseReply::Guest((read_owned.borrow_mut())(r)),
            PhaseRead::Located(r) => PhaseReply::Located((read_located.borrow_mut())(r)),
        };
        // A vanished helper is reported by its join below.
        let _ = reply_tx.send(reply);
    };
    let drain = || {
        while let Ok(request) = request_rx.try_recv() {
            serve(request);
        }
    };
    std::thread::scope(|scope| {
        let helper = std::thread::Builder::new()
            .name("mag-capture-materials".into())
            .spawn_scoped(scope, move || {
                let stats = crate::source_acquisition::thread_stats();
                let profile = crate::source_acquisition::thread_profile();
                let start = std::time::Instant::now();
                let ask = |request: PhaseRead| {
                    request_tx.send(request).ok()?;
                    reply_rx.recv().ok()
                };
                let _view_scope = direct.as_ref().map(DirectReads::view_scope);
                let materials = if let Some(direct) = direct {
                    // The bridge accepts these reads while the source thread is
                    // parked inside this synchronous call (helper read window).
                    capture_materials(
                        cohort,
                        resources,
                        material_input,
                        source_user_address,
                        &mut |r| ChunkReaders::guest(&direct, r),
                        &mut |r| ChunkReaders::located(&direct, r),
                    )
                } else {
                    capture_materials(
                        cohort,
                        resources,
                        material_input,
                        source_user_address,
                        &mut |r| match ask(PhaseRead::Guest(r)) {
                            Some(PhaseReply::Guest(result)) => result,
                            _ => Err(Error::UnprovenProducerEpoch),
                        },
                        &mut |r| match ask(PhaseRead::Located(r)) {
                            Some(PhaseReply::Located(result)) => result,
                            _ => Err(TextureError::Source(Error::UnprovenProducerEpoch)),
                        },
                    )
                };
                let ns = u64::try_from(start.elapsed().as_nanos()).unwrap_or(u64::MAX);
                let end = crate::source_acquisition::thread_profile();
                let counts = HelperCounts {
                    stats: crate::source_acquisition::thread_stats().since(stats),
                    profile: std::array::from_fn(|i| end[i].wrapping_sub(profile[i])),
                    ..HelperCounts::default()
                };
                (materials, ns, counts)
            });
        let Ok(helper) = helper else {
            return None;
        };
        let geometry_start = std::time::Instant::now();
        let chunks = crate::cohort_geometry::geometry_chunks();
        let mut chunk_outcome = crate::cohort_geometry::ChunkOutcome::default();
        let geometry = match direct.filter(|_| chunks >= 2) {
            Some(direct) => crate::cohort_geometry::copy_diagnostic_geometry_chunked(
                cohort,
                resources,
                resource_base,
                pass,
                geometry_budget,
                &direct,
                chunks,
                &mut |r| (read_owned.borrow_mut())(r),
                &mut |r| (read_located.borrow_mut())(r),
            )
            .map(|(geometry, outcome)| {
                chunk_outcome = outcome;
                geometry
            }),
            None => copy_diagnostic_geometry_with_snapshot(
                cohort,
                resources,
                resource_base,
                pass,
                geometry_budget,
                &mut |r| {
                    drain();
                    (read_owned.borrow_mut())(r)
                },
                &mut |r| {
                    drain();
                    (read_located.borrow_mut())(r)
                },
            ),
        };
        let geometry_ns = u64::try_from(geometry_start.elapsed().as_nanos()).unwrap_or(u64::MAX);
        // Serve the rest of the material reads; the channel closes when the
        // helper returns (or unwinds), which ends this loop.
        while let Ok(request) = request_rx.recv() {
            serve(request);
        }
        let Ok((materials, material_ns, mut counts)) = helper.join() else {
            return Some(Err(()));
        };
        counts.stats = counts.stats.plus(chunk_outcome.helper_stats);
        for (sum, v) in counts.profile.iter_mut().zip(chunk_outcome.helper_profile) {
            *sum = sum.wrapping_add(v);
        }
        counts.chunks = chunk_outcome.chunks;
        counts.reruns = chunk_outcome.reruns;
        let Ok(geometry) = geometry else {
            return Some(Err(()));
        };
        Some(Ok((geometry, geometry_ns, materials, material_ns, counts)))
    })
}

#[cfg(test)]
mod generation_check_bench {
    use super::*;
    /// Synthetic per-read generation check (run with --ignored --nocapture in
    /// release): the store mutex against the mirrored atomics, 1 and 4 threads.
    #[test]
    #[ignore]
    fn bench_generation_check() {
        {
            let mut s = store().lock().unwrap();
            s.vm = 7;
            advance_copy(&mut s);
        }
        let generation = store().lock().unwrap().generation;
        let per_thread = 200_000;
        for threads in [1usize, 4] {
            let time = |f: &(dyn Fn() -> bool + Sync)| {
                let start = std::time::Instant::now();
                std::thread::scope(|scope| {
                    for _ in 0..threads {
                        scope.spawn(|| {
                            for _ in 0..per_thread {
                                assert!(std::hint::black_box(f()));
                            }
                        });
                    }
                });
                start.elapsed().as_secs_f64() * 1e9 / (per_thread * threads) as f64
            };
            let locked = time(&|| {
                let s = store().lock().unwrap();
                s.vm == 7 && s.generation == generation
            });
            let mirrored = time(&|| {
                use std::sync::atomic::Ordering;
                MIRROR_GENERATION.load(Ordering::Acquire) == generation
                    && MIRROR_VM.load(Ordering::Acquire) == 7
            });
            println!("threads={threads} ns/check: store mutex {locked:.1} mirrored {mirrored:.1}");
        }
    }
}
