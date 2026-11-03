//! Thread-affine production ingress for owned diagnostic scene geometry.
//! No game addresses, inferred transforms, fabricated frame identities or probes.
use crate::{
    HostOwner, NativeContext,
    batch_device::{
        BatchDeviceLimits,
        scene_device::{SceneDevice, SceneGraphicsDesc},
    },
    host_image::HostImage,
};
use mag_render::{backend::*, batch::*, resources::*, scene::*};
use std::{
    cell::{Cell, RefCell},
    collections::{BTreeMap, BTreeSet, VecDeque},
    mem::{align_of, offset_of, size_of},
    panic::{AssertUnwindSafe, catch_unwind},
    sync::atomic::{AtomicU64, Ordering},
};
const MAX_DRAWS: usize = crate::batch_device::scene_device::MAX_SCENE_DRAWS;
const MAX_BYTES: u64 = 64 * 1024 * 1024;
const DIAGNOSTIC: u32 = 1;
const UNCERTAIN: u32 = 2;
const TERMINAL: u32 = 4;
const FAILED: u32 = 8;
pub use mag_render_abi::Source;
#[repr(C)]
pub struct Draw {
    pub abi_version: u32,
    pub struct_size: u32,
    pub source_item: u64,
    pub source_partition: u64,
    pub positions: *const [f32; 4],
    pub indices: *const u16,
    pub vertex_count: u32,
    pub index_count: u32,
    /// 1 = explicit homogeneous clip positions. No implicit matrix conversion.
    pub position_space: u32,
    pub reserved: u32,
    pub unresolved_obligations: u64,
}
#[repr(C)]
pub struct Input {
    pub abi_version: u32,
    pub struct_size: u32,
    pub source: Source,
    pub intended_items: u64,
    pub draws: *const Draw,
    pub draw_count: u32,
    pub reserved: u32,
    pub target: *const HostImage,
    pub frame: *const crate::host_frame::HostFrame,
    pub clear_color: [f32; 4],
    pub clear_depth: f32,
    pub reserved1: u32,
}
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct Receipt {
    pub abi_version: u32,
    pub struct_size: u32,
    pub status: i32,
    pub flags: u32,
    pub source: Source,
    pub ticket: u64,
    pub native_generation: u64,
    pub native_ticket: u64,
    pub intended_items: u64,
    pub admitted_items: u64,
    pub omitted_items: u64,
    pub draws: u64,
    pub unresolved_obligations: u64,
}
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct DrawIdentity {
    pub source_item: u64,
    pub source_partition: u64,
    pub unresolved_obligations: u64,
}
struct PackedDraw {
    identity: DrawIdentity,
    vertex: ByteRange,
    index: ByteRange,
    count: u32,
}
struct Packed {
    vertices: Vec<u8>,
    indices: Vec<u8>,
    draws: Vec<PackedDraw>,
    admitted: u64,
    obligations: u64,
}
unsafe extern "C" {
    fn mag_native_client_layout(out: *mut u32, words: u32) -> i32;
}
fn checked_layout() -> Result<(), i32> {
    let expected = [
        size_of::<Source>() as u32,
        align_of::<Source>() as u32,
        size_of::<Draw>() as u32,
        align_of::<Draw>() as u32,
        offset_of!(Draw, positions) as u32,
        offset_of!(Draw, unresolved_obligations) as u32,
        size_of::<Input>() as u32,
        align_of::<Input>() as u32,
        offset_of!(Input, target) as u32,
        offset_of!(Input, frame) as u32,
        offset_of!(Input, clear_color) as u32,
        size_of::<Receipt>() as u32,
        align_of::<Receipt>() as u32,
        offset_of!(Receipt, ticket) as u32,
        offset_of!(Receipt, unresolved_obligations) as u32,
        size_of::<DrawIdentity>() as u32,
        align_of::<DrawIdentity>() as u32,
        MAX_DRAWS as u32,
        MAX_DRAWS as u32,
    ];
    let mut actual = [0; 19];
    let status = unsafe { mag_native_client_layout(actual.as_mut_ptr(), actual.len() as u32) };
    if status != 0 {
        return Err(status);
    }
    if actual != expected {
        return Err(-1);
    }
    Ok(())
}
fn header<T>(abi: u32, bytes: u32) -> bool {
    abi == 1 && bytes as usize == size_of::<T>()
}
unsafe fn slice<'a, T>(pointer: *const T, count: usize) -> Result<&'a [T], i32> {
    if count == 0 {
        return Ok(&[]);
    }
    if pointer.is_null()
        || !(pointer as usize).is_multiple_of(align_of::<T>())
        || count > isize::MAX as usize / size_of::<T>()
    {
        return Err(-1);
    }
    Ok(unsafe { std::slice::from_raw_parts(pointer, count) })
}
unsafe fn pack(input: &Input, max_draws: usize, max_bytes: u64) -> Result<Packed, i32> {
    if !header::<Input>(input.abi_version, input.struct_size)
        || input.reserved != 0
        || input.reserved1 != 0
        || input.intended_items == 0
        || input.draw_count == 0
        || input.draw_count as usize > max_draws
        || input.source.publication == 0
        || input.source.cohort == 0
        || input.source.pass_occurrence == 0
        || input.clear_color.iter().any(|x| !x.is_finite())
        || !input.clear_depth.is_finite()
        || !(0.0..=1.0).contains(&input.clear_depth)
    {
        return Err(-1);
    }
    let draws = unsafe { slice(input.draws, input.draw_count as usize) }?;
    // Check all declared extents and aggregate budgets before reading vertex/index bytes.
    let mut vb = 0usize;
    let mut ib = 0usize;
    for d in draws {
        if !header::<Draw>(d.abi_version, d.struct_size)
            || d.reserved != 0
            || d.position_space != 1
            || d.source_item >= input.intended_items
            || d.vertex_count == 0
            || d.index_count == 0
            || !d.index_count.is_multiple_of(3)
        {
            return Err(-1);
        }
        vb = vb
            .checked_add((d.vertex_count as usize).checked_mul(16).ok_or(-5)?)
            .ok_or(-5)?;
        ib = ib
            .checked_add((d.index_count as usize).checked_mul(4).ok_or(-5)?)
            .ok_or(-5)?;
    }
    let padded = ib.checked_add(3).ok_or(-5)? & !3;
    if vb.checked_add(padded).ok_or(-5)? as u64 > max_bytes {
        return Err(-5);
    }
    let mut out = Packed {
        vertices: Vec::new(),
        indices: Vec::new(),
        draws: Vec::new(),
        admitted: 0,
        obligations: 0,
    };
    out.vertices.try_reserve_exact(vb).map_err(|_| -5)?;
    out.indices.try_reserve_exact(padded).map_err(|_| -5)?;
    out.draws.try_reserve_exact(draws.len()).map_err(|_| -5)?;
    let mut items = BTreeSet::new();
    for d in draws {
        let vertices = unsafe { slice(d.positions, d.vertex_count as usize) }?;
        let indices = unsafe { slice(d.indices, d.index_count as usize) }?;
        if vertices.iter().flatten().any(|n| !n.is_finite())
            || indices.iter().any(|i| u32::from(*i) >= d.vertex_count)
        {
            return Err(-1);
        }
        let vertex = ByteRange {
            offset: out.vertices.len() as u64,
            length: vertices.len() as u64 * 16,
        };
        let index = ByteRange {
            offset: out.indices.len() as u64,
            length: indices.len() as u64 * 4,
        };
        for v in vertices {
            for n in v {
                out.vertices.extend(n.to_ne_bytes())
            }
        }
        for i in indices {
            // The game decoder supplies u16; the native scene ABI consumes u32.
            out.indices.extend(u32::from(*i).to_ne_bytes())
        }
        out.draws.push(PackedDraw {
            identity: DrawIdentity {
                source_item: d.source_item,
                source_partition: d.source_partition,
                unresolved_obligations: d.unresolved_obligations,
            },
            vertex,
            index,
            count: d.index_count,
        });
        items.insert(d.source_item);
        out.obligations |= d.unresolved_obligations;
    }
    out.indices.resize(padded, 0);
    out.admitted = items.len() as u64;
    Ok(out)
}
struct Driver(RefCell<SceneDevice<'static>>);
impl RenderDevice for Driver {
    type Commands = std::rc::Rc<SceneCommands>;
    fn submit(
        &mut self,
        t: SubmissionTicket,
        c: &std::rc::Rc<SceneCommands>,
        l: &[ResourceLease],
    ) -> SubmitDisposition {
        self.0.get_mut().submit(t, c, l)
    }
    fn poll(&mut self, n: usize) -> Result<Vec<BackendEvent>, DevicePollError> {
        self.0.get_mut().poll(n)
    }
}
struct Pending {
    receipt: Receipt,
    identities: Vec<DrawIdentity>,
}
struct Session {
    native_backend: u32,
    packet_cache: scene_packet::PipelineCache,
    /// Some only with `MAG_RENDER_RESIDENT_BUFFERS=1`.
    upload_cache: Option<scene_upload_cache::UploadCache>,
    admissions: scene_admission::Transactions<scene_admission::Prepared>,
    backend: RenderBackend<Driver>,
    pipeline: Option<u64>,
    color_format: Option<ImageFormat>,
    max_draws: usize,
    max_bytes: u64,
    pending: BTreeMap<u64, Pending>,
    ready: VecDeque<Pending>,
    /// A reserved submission already acknowledged to the caller failed while
    /// encoding: its failed terminal receipt was queued; nothing more is admitted.
    acknowledged_failure: bool,
}
/// GPU work admitted or submitted and not yet terminal, per session.
const MAX_IN_FLIGHT: usize = 2;
/// Terminal receipts held for the host's next poll. They hold no GPU work,
/// so they no longer count against MAX_IN_FLIGHT, but stay bounded.
const MAX_READY: usize = 8;
static BACKPRESSURE_WAITS: AtomicU64 = AtomicU64::new(0);
static BACKPRESSURE_WAIT_NS: AtomicU64 = AtomicU64::new(0);
static BACKPRESSURE_MAX_NS: AtomicU64 = AtomicU64::new(0);
static BACKPRESSURE_TIMEOUTS: AtomicU64 = AtomicU64::new(0);
/// Bounded wait for in-flight capacity: `MAG_RENDER_MAIN_BACKPRESSURE_MS`
/// (default 100; 0 restores the immediate BUSY refusal).
fn backpressure_budget() -> std::time::Duration {
    static BUDGET: std::sync::OnceLock<u64> = std::sync::OnceLock::new();
    std::time::Duration::from_millis(*BUDGET.get_or_init(|| {
        std::env::var("MAG_RENDER_MAIN_BACKPRESSURE_MS")
            .ok()
            .and_then(|v| v.parse().ok())
            .unwrap_or(100)
            .min(10_000)
    }))
}
impl Session {
    /// Turn every terminal backend event into a ready receipt for the host's
    /// poll (in completion order). Never pops a receipt.
    fn collect_terminals(&mut self) -> Result<(), i32> {
        let poll = self.backend.poll().map_err(|_| -6)?;
        for event in poll.events {
            if let BackendEvent::Terminal { ticket, status } = event {
                let mut p = self.pending.remove(&ticket.submission_id).ok_or(-6)?;
                p.receipt.flags |= TERMINAL;
                if let TerminalStatus::Failed { code } = status {
                    p.receipt.flags |= FAILED;
                    p.receipt.status = -(code.min(i32::MAX as u32) as i32).max(1)
                }
                self.ready.push_back(p);
            }
        }
        if !poll.errors.is_empty() {
            return Err(-6);
        }
        Ok(())
    }
    fn in_flight(&self) -> usize {
        self.admissions.len() + self.pending.len()
    }
    /// Capacity for one more admission or submission. When the in-flight limit
    /// is reached, wait (bounded) for earlier GPU work to reach its terminal
    /// instead of refusing: the CPU may legitimately run ahead of the GPU.
    /// See [`wait_for_capacity`] for the exact refusal rules.
    pub(crate) fn admission_capacity(&mut self) -> Result<(), i32> {
        let budget = backpressure_budget();
        let mut first = true;
        let outcome = wait_for_capacity(budget, || {
            if !first {
                self.collect_terminals()?;
            }
            first = false;
            Ok((self.in_flight(), self.ready.len()))
        });
        let (result, waited) = match outcome {
            Ok(None) => return Ok(()),
            Ok(Some(waited)) => (Ok(()), Some(waited)),
            Err((code, waited)) => (Err(code), waited),
        };
        let Some(waited) = waited else {
            return result;
        };
        let waits = BACKPRESSURE_WAITS.fetch_add(1, Ordering::Relaxed) + 1;
        let total = BACKPRESSURE_WAIT_NS.fetch_add(waited, Ordering::Relaxed) + waited;
        let max = BACKPRESSURE_MAX_NS
            .fetch_max(waited, Ordering::Relaxed)
            .max(waited);
        let timeouts = BACKPRESSURE_TIMEOUTS
            .fetch_add(u64::from(result.is_err()), Ordering::Relaxed)
            + u64::from(result.is_err());
        if result.is_err() || waits <= 4 || waits.is_multiple_of(120) {
            eprintln!(
                "[mag native backpressure] waits={waits} wait_ms={:.3} total_ms={:.3} max_ms={:.3} timeouts={timeouts} in_flight={} ready={} status={}",
                waited as f64 / 1e6,
                total as f64 / 1e6,
                max as f64 / 1e6,
                self.in_flight(),
                self.ready.len(),
                result.err().unwrap_or(0)
            );
        }
        result
    }
}
/// Bounded backpressure over `observe`, which reports (in flight, ready
/// receipts); every call after the first collects terminal GPU events before
/// reporting.
/// - `Ok(None)`: capacity at once, or after one collection (no wait).
/// - `Ok(Some(ns))`: capacity after waiting `ns`.
/// - `Err((-4, ..))`: still full after `budget`, or at once when the only
///   obstacle is host-held receipts (waiting cannot drain them) or the budget
///   is zero. `Some(ns)` when a wait was attempted.
/// - Any `observe` error (a failed poll) is returned unchanged: fail closed.
fn wait_for_capacity(
    budget: std::time::Duration,
    mut observe: impl FnMut() -> Result<(usize, usize), i32>,
) -> Result<Option<u64>, (i32, Option<u64>)> {
    let fits = |(in_flight, ready): (usize, usize)| in_flight < MAX_IN_FLIGHT && ready < MAX_READY;
    if fits(observe().map_err(|e| (e, None))?) {
        return Ok(None);
    }
    let state = observe().map_err(|e| (e, None))?;
    if fits(state) {
        return Ok(None);
    }
    if state.1 >= MAX_READY || budget.is_zero() {
        return Err((-4, None));
    }
    let start = std::time::Instant::now();
    let elapsed = || start.elapsed().as_nanos().min(u64::MAX as u128) as u64;
    loop {
        std::thread::sleep(std::time::Duration::from_micros(100));
        let state = observe().map_err(|e| (e, Some(elapsed())))?;
        if fits(state) {
            return Ok(Some(elapsed()));
        }
        if start.elapsed() >= budget || state.1 >= MAX_READY {
            return Err((-4, Some(elapsed())));
        }
    }
}
thread_local! {static SESSIONS:RefCell<BTreeMap<u64,Session>>=const {RefCell::new(BTreeMap::new())};}
static NEXT_SESSION: AtomicU64 = AtomicU64::new(1);
fn ffi(f: impl FnOnce() -> Result<(), i32>) -> i32 {
    match catch_unwind(AssertUnwindSafe(f)) {
        Ok(Ok(())) => 0,
        Ok(Err(e)) => e,
        Err(_) => -6,
    }
}
fn with_session<T>(id: u64, f: impl FnOnce(&mut Session) -> Result<T, i32>) -> Result<T, i32> {
    SESSIONS.with(|s| {
        let mut s = s.try_borrow_mut().map_err(|_| -4)?;
        f(s.get_mut(&id).ok_or(-3)?)
    })
}
mod shaders {
    include!(concat!(env!("OUT_DIR"), "/client_shaders.rs"));
}
fn diagnostic_stage(backend: u32, vertex: bool) -> Result<crate::batch_device::ShaderStage, i32> {
    use shaders::*;
    let (source, length, entry, language) = match (backend, vertex) {
        (1, true) => (
            VERTEX_MSL.as_ptr().cast(),
            VERTEX_MSL.len(),
            VERTEX_MSL_ENTRY,
            1,
        ),
        (1, false) => (
            FRAGMENT_MSL.as_ptr().cast(),
            FRAGMENT_MSL.len(),
            FRAGMENT_MSL_ENTRY,
            1,
        ),
        (3, true) => (
            VERTEX_HLSL.as_ptr().cast(),
            VERTEX_HLSL.len(),
            VERTEX_HLSL_ENTRY,
            2,
        ),
        (3, false) => (
            FRAGMENT_HLSL.as_ptr().cast(),
            FRAGMENT_HLSL.len(),
            FRAGMENT_HLSL_ENTRY,
            2,
        ),
        (2, true) => (
            VERTEX_SPIRV.as_ptr().cast(),
            std::mem::size_of_val(VERTEX_SPIRV),
            VERTEX_SPIRV_ENTRY,
            3,
        ),
        (2, false) => (
            FRAGMENT_SPIRV.as_ptr().cast(),
            std::mem::size_of_val(FRAGMENT_SPIRV),
            FRAGMENT_SPIRV_ENTRY,
            3,
        ),
        _ => return Err(-2),
    };
    Ok(crate::batch_device::ShaderStage {
        source,
        source_length: length as u64,
        entry: entry.as_ptr().cast(),
        entry_length: entry.len() as u32,
        language,
    })
}
/// Returns process-lived source/entry descriptors compiled at build time from
/// authored WGSL. This is explicitly unlit diagnostic shading, not game material
/// parity. Platform pipeline compilation still occurs on the actual host device.
/// # Safety
/// `out` points to writable SceneGraphicsDesc storage for this synchronous call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mag_render_client_diagnostic_pipeline(
    backend: u32,
    out: *mut SceneGraphicsDesc,
) -> i32 {
    if out.is_null() {
        return -1;
    }
    ffi(|| {
        let vertex = diagnostic_stage(backend, true)?;
        let fragment = diagnostic_stage(backend, false)?;
        unsafe {
            *out = SceneGraphicsDesc {
                struct_size: size_of::<SceneGraphicsDesc>() as u32,
                abi_version: 1,
                vertex,
                fragment,
                vertex_stride: 16,
                color_format: 1,
                depth_format: 2,
                depth_compare: 2,
                depth_write: 1,
                blend: 0,
                buffers: std::ptr::null(),
                buffer_count: 0,
                texture_count: 0,
                textures: std::ptr::null(),
            }
        };
        Ok(())
    })
}
/// # Safety
/// Pointers describe valid synchronous ranges. Host callbacks must be genuine,
/// process-lived and non-unwinding. Shader sources/entries obey graphics ABI.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mag_render_client_create(
    owner: *const HostOwner,
    graphics: *const SceneGraphicsDesc,
    max_draws: u32,
    max_bytes: u64,
    out: *mut u64,
) -> i32 {
    if owner.is_null() || graphics.is_null() || out.is_null() {
        return -1;
    }
    unsafe { *out = 0 };
    ffi(|| {
        checked_layout()?;
        if max_draws == 0
            || max_draws as usize > MAX_DRAWS
            || max_bytes == 0
            || max_bytes > MAX_BYTES
        {
            return Err(-1);
        }
        let d = unsafe { &*graphics };
        // Initial ingress has explicit clip positions, one pipeline and no hidden
        // material bindings. Full material ingress is a separate extension.
        if d.vertex_stride != 16
            || !matches!(d.color_format, 1 | 3)
            || d.depth_format != 2
            || d.buffer_count != 0
            || d.texture_count != 0
        {
            return Err(-2);
        }
        SESSIONS.with(|s| {
            if s.borrow().len() >= 2 {
                Err(-4)
            } else {
                Ok(())
            }
        })?;
        let context = unsafe { NativeContext::from_host(&*owner) }?;
        let id = create_session(
            context,
            unsafe { (*owner).backend },
            Some(d),
            max_draws,
            max_bytes,
        )?;
        unsafe { *out = id };
        Ok(())
    })
}
pub(crate) fn create_scene_session(
    context: NativeContext,
    native_backend: u32,
    max_draws: u32,
    max_bytes: u64,
) -> Result<u64, i32> {
    if max_draws == 0
        || max_draws as usize > MAX_DRAWS
        || max_bytes == 0
        || max_bytes > crate::batch_device::scene_device::MAX_SCENE_BYTES
    {
        return Err(-1);
    }
    SESSIONS.with(|s| {
        if s.borrow().len() >= 2 {
            Err(-4)
        } else {
            Ok(())
        }
    })?;
    create_session(context, native_backend, None, max_draws, max_bytes)
}
fn create_session(
    context: NativeContext,
    native_backend: u32,
    graphics: Option<&SceneGraphicsDesc>,
    max_draws: u32,
    max_bytes: u64,
) -> Result<u64, i32> {
    let mut device = SceneDevice::from_owned_context(
        context,
        BatchDeviceLimits {
            max_bytes,
            max_pipelines: 257,
            diagnostic_readback: false,
        },
    )?;
    let pipeline = if let Some(d) = graphics {
        Some(unsafe { device.graphics_pipeline(d) }?)
    } else {
        None
    };
    let backend = RenderBackend::new(
        Driver(RefCell::new(device)),
        BackendLimits {
            max_leases_per_submission: crate::batch_device::scene_device::MAX_SCENE_RESOURCES,
            max_bytes_per_submission: max_bytes as usize,
        },
    )
    .map_err(|_| -5)?;
    let id = NEXT_SESSION
        .try_update(Ordering::Relaxed, Ordering::Relaxed, |v| v.checked_add(1))
        .map_err(|_| -5)?;
    // A cache that cannot be created only leaves the session on the
    // ordinary per-submission uploads.
    let upload_cache = crate::batch_device::resident_buffers_enabled()
        .then(|| {
            scene_upload_cache::UploadCache::new(
                crate::batch_device::scene_device::MAX_SCENE_RESOURCES,
                512 * 1024 * 1024,
            )
            .ok()
        })
        .flatten();
    SESSIONS.with(|s| {
        s.borrow_mut().insert(
            id,
            Session {
                native_backend,
                packet_cache: scene_packet::PipelineCache::default(),
                upload_cache,
                admissions: scene_admission::Transactions::default(),
                backend,
                pipeline,
                color_format: graphics.and_then(|d| ImageFormat::from_native(d.color_format).ok()),
                max_draws: max_draws as usize,
                max_bytes,
                pending: BTreeMap::new(),
                ready: VecDeque::new(),
                acknowledged_failure: false,
            },
        )
    });
    Ok(id)
}
struct SceneSetup {
    pipeline: u64,
    color_format: ImageFormat,
    max_draws: usize,
    max_bytes: u64,
    width: u32,
    height: u32,
    clear: [f32; 4],
    depth_clear: f32,
}
fn prepare(
    device: &impl SceneCatalog,
    setup: SceneSetup,
    packed: &mut Packed,
) -> Result<PreparedScene, i32> {
    let SceneSetup {
        pipeline,
        color_format,
        max_draws,
        max_bytes,
        width,
        height,
        clear,
        depth_clear,
    } = setup;
    let mut store = ResourceStore::new(ResourceLimits {
        max_resources: 2,
        max_bytes: max_bytes as usize,
    })
    .map_err(|_| -5)?;
    let mut leases = Vec::new();
    for bytes in [
        std::mem::take(&mut packed.vertices),
        std::mem::take(&mut packed.indices),
    ] {
        let v = store
            .apply(ResourceDelta::Create { bytes })
            .map_err(|_| -5)?;
        leases.push(store.lease(v).map_err(|_| -5)?);
        store.retire(v).map_err(|_| -5)?;
    }
    let mut batch = BatchBuilder::new(
        device.owner_generation(),
        BatchLimits {
            max_commands: 1,
            max_gpu_buffers: 1,
            max_gpu_bytes: max_bytes,
            max_uploads: 2,
        },
    )
    .map_err(|_| -1)?;
    let vertices = batch.upload(leases[0].clone()).map_err(|_| -5)?;
    let indices = batch.upload(leases[1].clone()).map_err(|_| -5)?;
    let base = batch.prepare_scene_prefix(device).map_err(|_| -1)?;
    let mut scene = SceneBuilder::new(
        base,
        SceneLimits {
            max_images: 2,
            max_passes: 1,
            max_draws,
            max_bytes,
        },
    )
    .map_err(|_| -1)?;
    let color = scene
        .image(ImageDesc {
            width,
            height,
            format: color_format,
        })
        .map_err(|_| -5)?;
    let depth = scene
        .image(ImageDesc {
            width,
            height,
            format: ImageFormat::Depth32,
        })
        .map_err(|_| -5)?;
    let draws = packed
        .draws
        .iter()
        .map(|d| SceneDraw {
            pipeline,
            vertices: ReadSource::Upload(vertices),
            indices: ReadSource::Upload(indices),
            vertex_range: d.vertex,
            index_range: d.index,
            index_count: d.count,
            buffers: vec![],
            textures: vec![],
        })
        .collect();
    let outputs = scene
        .render_pass(RenderPass {
            color: Some(ColorAttachment {
                image: color,
                load: Load::Clear(clear),
                store: Store::Store,
            }),
            depth: Some(DepthAttachment {
                image: depth,
                load: Load::Clear(depth_clear),
                store: Store::Store,
            }),
            draws,
        })
        .map_err(|_| -1)?;
    scene.output(outputs[0], false).map_err(|_| -1)?;
    scene.prepare(device).map_err(|_| -1)
}
/// # Safety
/// All descriptor and geometry ranges are valid for this synchronous call.
/// Target metadata must come from its actual owner; coordinates must already
/// be source-qualified clip positions. Never call under the owner's queue lock.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mag_render_client_submit(
    id: u64,
    input: *const Input,
    out: *mut Receipt,
) -> i32 {
    if input.is_null() || out.is_null() {
        return -1;
    }
    unsafe {
        *out = Receipt {
            abi_version: 1,
            struct_size: size_of::<Receipt>() as u32,
            flags: DIAGNOSTIC,
            ..Receipt::default()
        }
    };
    let effects_possible = Cell::new(false);
    let status = ffi(|| {
        with_session(id, |s| {
            s.admission_capacity()?;
            let pipeline = s.pipeline.ok_or(-2)?;
            let color_format = s.color_format.ok_or(-2)?;
            let input = unsafe { &*input };
            let mut packed = unsafe { pack(input, s.max_draws, s.max_bytes) }?;
            if input.target.is_null() || input.frame.is_null() {
                return Err(-1);
            }
            let frame = unsafe { *input.frame };
            if !frame.valid() {
                return Err(-1);
            }
            let target = unsafe { &*input.target };
            if frame.owner_lifetime != target.owner_lifetime {
                return Err(-3);
            }
            if target.format != color_format.native_id() {
                return Err(-2);
            }
            let identities: Vec<_> = packed.draws.iter().map(|d| d.identity).collect();
            let scene = prepare(
                &*s.backend.device().0.borrow(),
                SceneSetup {
                    pipeline,
                    color_format,
                    max_draws: s.max_draws,
                    max_bytes: s.max_bytes,
                    width: target.width,
                    height: target.height,
                    clear: input.clear_color,
                    depth_clear: input.clear_depth,
                },
                &mut packed,
            )?;
            let image = unsafe { s.backend.device().0.borrow_mut().import_target(target) }?;
            if let Err(code) = unsafe { s.backend.device().0.borrow_mut().frame_context(frame) } {
                s.backend.device().0.borrow_mut().release_target(image)?;
                return Err(code);
            }
            let (commands, leases) = scene.into_submission();
            let result = s.backend.submit_diagnostic(
                input.source.publication,
                std::rc::Rc::new(commands),
                leases,
            );
            let (ticket, uncertain) = match result {
                Ok(t) => (t, false),
                Err(BackendError::SubmissionUncertain { ticket, .. }) => (ticket, true),
                Err(BackendError::DeviceRejected { code }) => {
                    s.backend.device().0.borrow_mut().release_target(image)?;
                    return Err(-(code.min(i32::MAX as u32) as i32).max(1));
                }
                Err(_) => {
                    s.backend.device().0.borrow_mut().release_target(image)?;
                    return Err(-6);
                }
            };
            effects_possible.set(true);
            unsafe {
                (*out).ticket = ticket.submission_id;
                (*out).source = input.source;
            }
            let released = s.backend.device().0.borrow_mut().release_target(image);
            let (native_generation, native_ticket) = s
                .backend
                .device()
                .0
                .borrow()
                .native_ticket(ticket)
                .unwrap_or((0, 0));
            let receipt = Receipt {
                abi_version: 1,
                struct_size: size_of::<Receipt>() as u32,
                status: if uncertain { 1 } else { 0 },
                flags: DIAGNOSTIC | if uncertain { UNCERTAIN } else { 0 },
                source: input.source,
                ticket: ticket.submission_id,
                native_generation,
                native_ticket,
                intended_items: input.intended_items,
                admitted_items: packed.admitted,
                omitted_items: input.intended_items - packed.admitted,
                draws: packed.draws.len() as u64,
                unresolved_obligations: packed.obligations,
            };
            s.pending.insert(
                ticket.submission_id,
                Pending {
                    receipt,
                    identities,
                },
            );
            unsafe { *out = receipt };
            // A failure after real submission cannot become a definitely-rejected result.
            if uncertain || released.is_err() {
                unsafe {
                    (*out).flags |= UNCERTAIN;
                    (*out).status = 1
                };
                return Ok(());
            }
            Ok(())
        })
    });
    if status != 0 && effects_possible.get() {
        unsafe {
            (*out).flags |= UNCERTAIN;
            (*out).status = 1;
        }
        return 1;
    }
    if status != 0 {
        unsafe { (*out).status = status }
    } else if unsafe { (*out).status } > 0 {
        return 1;
    }
    status
}
/// Poll emits at most one genuine terminal receipt. status -4 means no receipt,
/// not completion. Presentation is independently observed by the host frame API.
/// # Safety
/// `out` points to writable Receipt storage for this synchronous call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mag_render_client_poll(id: u64, out: *mut Receipt) -> i32 {
    if out.is_null() {
        return -1;
    }
    ffi(|| {
        with_session(id, |s| {
            if let Some(p) = s.ready.pop_front() {
                unsafe { *out = p.receipt };
                return Ok(());
            }
            s.collect_terminals()?;
            let p = s.ready.pop_front().ok_or(-4)?;
            unsafe { *out = p.receipt };
            Ok(())
        })
    })
}
/// Inspect the exact ordered source draw identities while its ticket is pending
/// or its terminal receipt is queued. Capacity shortage performs no partial copy.
/// # Safety
/// `out` has `capacity` writable elements and `count` is writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mag_render_client_draw_identities(
    id: u64,
    ticket: u64,
    out: *mut DrawIdentity,
    capacity: u32,
    count: *mut u32,
) -> i32 {
    if count.is_null() {
        return -1;
    }
    ffi(|| {
        with_session(id, |s| {
            let p = s
                .pending
                .get(&ticket)
                .or_else(|| s.ready.iter().find(|p| p.receipt.ticket == ticket))
                .ok_or(-3)?;
            unsafe { *count = p.identities.len() as u32 };
            if (capacity as usize) < p.identities.len() {
                return Err(-5);
            }
            if out.is_null() {
                return Err(-1);
            }
            unsafe {
                std::ptr::copy_nonoverlapping(p.identities.as_ptr(), out, p.identities.len())
            };
            Ok(())
        })
    })
}
#[unsafe(no_mangle)]
pub extern "C" fn mag_render_client_close(id: u64) -> i32 {
    ffi(|| {
        SESSIONS.with(|sessions| {
            let mut all = sessions.try_borrow_mut().map_err(|_| -4)?;
            let s = all.get_mut(&id).ok_or(-3)?;
            if s.admissions.len() != 0 || !s.pending.is_empty() || !s.ready.is_empty() {
                return Err(-4);
            }
            s.backend.try_shutdown().map_err(|_| -4)?;
            all.remove(&id);
            Ok(())
        })
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn capacity_backpressure_waits_bounded_and_fails_closed() {
        use std::time::Duration;
        let budget = Duration::from_millis(50);
        // Free capacity: no collection, no wait.
        let mut calls = 0;
        assert_eq!(
            wait_for_capacity(budget, || {
                calls += 1;
                Ok((1, 7))
            }),
            Ok(None)
        );
        assert_eq!(calls, 1);
        // Host-held receipts no longer count as GPU work in flight.
        assert_eq!(
            wait_for_capacity(budget, || Ok((0, MAX_READY - 1))),
            Ok(None)
        );
        // One collection frees a slot: still no wait.
        let mut states = [(2, 0), (1, 1)].into_iter();
        assert_eq!(
            wait_for_capacity(budget, || Ok(states.next().unwrap())),
            Ok(None)
        );
        // GPU work completes during the wait: admitted after a counted wait.
        let mut polls = 0;
        let waited = wait_for_capacity(budget, || {
            polls += 1;
            Ok(if polls < 5 { (2, 0) } else { (1, 1) })
        });
        assert!(matches!(waited, Ok(Some(ns)) if ns > 0), "{waited:?}");
        assert_eq!(polls, 5);
        // Never completes: BUSY after the budget, never unbounded.
        let start = std::time::Instant::now();
        let stuck = wait_for_capacity(budget, || Ok((2, 0)));
        assert!(
            matches!(stuck, Err((-4, Some(ns))) if ns >= 50_000_000),
            "{stuck:?}"
        );
        assert!(start.elapsed() < Duration::from_secs(2));
        // Full host-held receipts or a zero budget: BUSY at once, no wait.
        assert_eq!(
            wait_for_capacity(budget, || Ok((0, MAX_READY))),
            Err((-4, None))
        );
        assert_eq!(
            wait_for_capacity(Duration::ZERO, || Ok((2, 0))),
            Err((-4, None))
        );
        // A failed poll stays fatal, during the first collection or the wait.
        let mut n = 0;
        assert_eq!(
            wait_for_capacity(budget, || {
                n += 1;
                if n == 1 { Ok((2, 0)) } else { Err(-6) }
            }),
            Err((-6, None))
        );
        let mut n = 0;
        let failed = wait_for_capacity(budget, || {
            n += 1;
            if n < 3 { Ok((2, 0)) } else { Err(-6) }
        });
        assert!(matches!(failed, Err((-6, Some(_)))), "{failed:?}");
    }
    fn draw(vertices: &[[f32; 4]], indices: &[u16], item: u64, part: u64) -> Draw {
        Draw {
            abi_version: 1,
            struct_size: size_of::<Draw>() as u32,
            source_item: item,
            source_partition: part,
            positions: vertices.as_ptr(),
            indices: indices.as_ptr(),
            vertex_count: vertices.len() as u32,
            index_count: indices.len() as u32,
            position_space: 1,
            reserved: 0,
            unresolved_obligations: 7,
        }
    }
    fn input(draws: &[Draw]) -> Input {
        Input {
            abi_version: 1,
            struct_size: size_of::<Input>() as u32,
            source: Source {
                engine_update: 0,
                cohort: 3,
                publication: 5,
                pass_occurrence: 2,
            },
            intended_items: 9,
            draws: draws.as_ptr(),
            draw_count: draws.len() as u32,
            reserved: 0,
            target: std::ptr::null(),
            frame: std::ptr::null(),
            clear_color: [0.; 4],
            clear_depth: 1.,
            reserved1: 0,
        }
    }
    #[test]
    fn copies_actual_geometry_preserves_order_duplicates_and_population_gaps() {
        let mut positions = [[1., 2., 3., 1.]; 3];
        let indices = [0, 2, 1];
        let draws = [
            draw(&positions, &indices, 6, 9),
            draw(&positions, &indices, 6, 9),
            draw(&positions, &indices, 2, 11),
        ];
        let packed = unsafe { pack(&input(&draws), MAX_DRAWS, MAX_BYTES) }.unwrap();
        positions[0] = [9.; 4];
        assert_eq!(positions[0][0], 9.);
        assert_eq!(packed.admitted, 2);
        assert_eq!(packed.draws.len(), 3);
        assert_eq!(
            packed
                .draws
                .iter()
                .map(|d| (d.identity.source_item, d.identity.source_partition))
                .collect::<Vec<_>>(),
            vec![(6, 9), (6, 9), (2, 11)]
        );
        assert_eq!(&packed.vertices[..4], &1f32.to_ne_bytes());
        assert_eq!(packed.draws[1].index.offset, 12);
        assert_eq!(packed.indices.len(), 36);
    }
    #[test]
    fn rejects_unknown_space_indices_nonfinite_and_aggregate_budget_before_reads() {
        let positions = [[1., 2., 3., 1.]; 3];
        let indices = [0, 1, 3];
        let mut draws = [draw(&positions, &indices, 1, 2)];
        assert!(matches!(
            unsafe { pack(&input(&draws), MAX_DRAWS, MAX_BYTES) },
            Err(-1)
        ));
        draws[0].position_space = 0;
        assert!(matches!(
            unsafe { pack(&input(&draws), MAX_DRAWS, MAX_BYTES) },
            Err(-1)
        ));
        draws[0].position_space = 1;
        draws[0].vertex_count = u32::MAX;
        draws[0].positions = std::ptr::null();
        assert!(matches!(
            unsafe { pack(&input(&draws), MAX_DRAWS, MAX_BYTES) },
            Err(-5)
        ));
        let positions = [[f32::NAN, 0., 0., 1.]; 3];
        let indices = [0, 1, 2];
        let draws = [draw(&positions, &indices, 0, 0)];
        assert!(matches!(
            unsafe { pack(&input(&draws), MAX_DRAWS, MAX_BYTES) },
            Err(-1)
        ));
    }
    #[test]
    fn more_than_fixture_draw_count_is_kept_without_truncation() {
        let positions = [[0., 0., 0., 1.]; 3];
        let indices = [0, 1, 2];
        let draws: Vec<_> = (0..1024)
            .map(|i| draw(&positions, &indices, 0, i))
            .collect();
        let packed = unsafe { pack(&input(&draws), MAX_DRAWS, MAX_BYTES) }.unwrap();
        assert_eq!(packed.draws.len(), 1024);
        assert_eq!(packed.draws[1023].identity.source_partition, 1023);
        assert!(matches!(
            unsafe { pack(&input(&draws), 64, MAX_BYTES) },
            Err(-1)
        ));
    }
    #[test]
    fn actual_c_header_layout_and_native_capacity_match_rust() {
        checked_layout().unwrap();
    }
    struct Catalog;
    impl PipelineCatalog for Catalog {
        fn owner_generation(&self) -> u64 {
            7
        }
        fn bindings(&self, _: u64) -> Option<&[BindingRequirement]> {
            None
        }
    }
    impl SceneCatalog for Catalog {
        fn scene_graphics(&self, pipeline: u64) -> Option<GraphicsLayout> {
            (pipeline == 1).then_some(GraphicsLayout {
                vertex_stride: 16,
                color_format: Some(ImageFormat::Rgba8),
                fragment_stage: true,
                depth_attachment: true,
                depth: DepthState {
                    compare: DepthCompare::LessEqual,
                    write: true,
                },
                blend: Blend::Opaque,
                buffers: vec![],
                textures: vec![],
            })
        }
    }
    #[test]
    fn production_scene_preparation_keeps_actual_bgra_target_format() {
        struct BgraCatalog;
        impl PipelineCatalog for BgraCatalog {
            fn owner_generation(&self) -> u64 {
                7
            }
            fn bindings(&self, _: u64) -> Option<&[BindingRequirement]> {
                None
            }
        }
        impl SceneCatalog for BgraCatalog {
            fn scene_graphics(&self, id: u64) -> Option<GraphicsLayout> {
                Catalog.scene_graphics(id).map(|mut layout| {
                    layout.color_format = Some(ImageFormat::Bgra8);
                    layout
                })
            }
        }
        let positions = [[0., 0., 0., 1.]; 3];
        let indices = [0, 1, 2];
        let draws = [draw(&positions, &indices, 0, 0)];
        let mut packed = unsafe { pack(&input(&draws), MAX_DRAWS, MAX_BYTES) }.unwrap();
        let scene = prepare(
            &BgraCatalog,
            SceneSetup {
                pipeline: 1,
                color_format: ImageFormat::Bgra8,
                max_draws: MAX_DRAWS,
                max_bytes: MAX_BYTES,
                width: 2560,
                height: 720,
                clear: [0.; 4],
                depth_clear: 1.,
            },
            &mut packed,
        )
        .unwrap();
        assert_eq!(scene.images()[0].desc.format, ImageFormat::Bgra8);
        assert_eq!(scene.images()[0].desc.width, 2560);
        assert!(!scene.output().unwrap().diagnostic_readback);
    }
    #[test]
    fn packed_geometry_passes_actual_common_scene_validation() {
        let positions = [[0., 0., 0., 1.]; 3];
        let indices = [0, 1, 2];
        let draws = [
            draw(&positions, &indices, 0, 0),
            draw(&positions, &indices, 1, 0),
        ];
        let mut packed = unsafe { pack(&input(&draws), MAX_DRAWS, MAX_BYTES) }.unwrap();
        let scene = prepare(
            &Catalog,
            SceneSetup {
                pipeline: 1,
                color_format: ImageFormat::Rgba8,
                max_draws: MAX_DRAWS,
                max_bytes: MAX_BYTES,
                width: 1280,
                height: 720,
                clear: [0.; 4],
                depth_clear: 1.,
            },
            &mut packed,
        )
        .unwrap();
        let (commands, leases) = scene.into_submission();
        commands.validate_uploads(&leases).unwrap();
        assert_eq!(commands.passes()[0].draws.len(), 2);
        assert_eq!(leases[1].bytes().len(), 24);
        assert_eq!(&leases[1].bytes()[4..8], &1u32.to_ne_bytes());
    }
    #[test]
    fn embedded_shader_provider_uses_translated_entries_and_aligned_spirv() {
        for backend in [1, 2, 3] {
            for vertex in [false, true] {
                let stage = diagnostic_stage(backend, vertex).unwrap();
                assert!(stage.source_length > 0 && stage.entry_length > 0);
                if backend == 2 {
                    assert_eq!(stage.source as usize % 4, 0);
                    assert_eq!(unsafe { *(stage.source.cast::<u32>()) }, 0x07230203);
                }
            }
        }
        assert!(matches!(diagnostic_stage(99, true), Err(-2)));
    }
    #[test]
    fn unknown_or_foreign_session_cannot_be_closed() {
        assert_eq!(mag_render_client_close(u64::MAX), -3)
    }
}

#[path = "scene_packet.rs"]
pub mod scene_packet;

#[path = "scene_admission.rs"]
pub(crate) mod scene_admission;

#[path = "scene_upload_cache.rs"]
pub(crate) mod scene_upload_cache;
