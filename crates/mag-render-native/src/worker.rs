//! Fixed-thread production client ingress. NativeContext itself remains !Send.
//! Only copied geometry and genuinely pinned host-provider metadata cross the
//! channel; replies acknowledge CPU queue admission, never GPU completion.
use crate::{HostOwner, client, host_frame::HostFrame, host_image::HostImage};
use std::{
    collections::BTreeMap,
    mem::{align_of, size_of},
    panic::{AssertUnwindSafe, catch_unwind},
    sync::{
        Arc, Mutex, OnceLock,
        atomic::{AtomicU64, AtomicUsize, Ordering},
        mpsc::{self, SyncSender},
    },
    thread::{self, JoinHandle},
    time::Instant,
};
const MAX_WORKERS: usize = 2;
const MAX_BYTES: u64 = 64 * 1024 * 1024;
const MAX_DRAWS: u32 = crate::batch_device::scene_device::MAX_SCENE_DRAWS as u32;
fn env_flag(name: &str) -> bool {
    std::env::var(name).is_ok_and(|v| v == "1")
}
/// Default-off per-occurrence FIFO-thread timing lines for Main transport.
/// Diagnostics only: durations are host wall time, never GPU completion.
fn fifo_timing() -> bool {
    static VALUE: OnceLock<bool> = OnceLock::new();
    *VALUE.get_or_init(|| env_flag("MAG_RENDER_FIFO_TIMING"))
}
/// Default-off: an owned packet that the ingress copy already validated against
/// these exact worker limits is not validated a second time by the actor.
fn single_validation() -> bool {
    static VALUE: OnceLock<bool> = OnceLock::new();
    *VALUE.get_or_init(|| env_flag("MAG_RENDER_WORKER_SINGLE_VALIDATION"))
}
/// Default-on (MAG_RENDER_MAIN_ASYNC_COMMIT=0 disables): an imported subgraph commit
/// acknowledges once its native queue position and ticket are reserved and its
/// imports validated under the owner lease; allocation, encoding and the GPU
/// commit then finish on this actor after the caller is released.
/// Read once per process; each actor captures it when it starts (tests pin
/// it per actor with `Worker::start_with`).
fn reserved_commit() -> bool {
    static VALUE: OnceLock<bool> = OnceLock::new();
    *VALUE.get_or_init(|| std::env::var("MAG_RENDER_MAIN_ASYNC_COMMIT").map_or(true, |v| v != "0"))
}
/// Default-off: `mag_render_client_worker_poll` never waits behind other actor
/// work. While the actor is still executing an earlier command (for example the
/// encode and GPU commit that follow a reserved commit's acknowledgement) the
/// poll returns POLL_BUSY without polling. The default poll queues behind that
/// work, which blocks a caller that polls from its FIFO/presentation thread.
fn poll_nowait_enabled() -> bool {
    static VALUE: OnceLock<bool> = OnceLock::new();
    *VALUE.get_or_init(|| env_flag("MAG_RENDER_WORKER_POLL_NOWAIT"))
}
/// Positive, like the existing indeterminate status 1: no receipt and no error.
/// Only the nowait poll returns it, and only when nothing was polled.
const POLL_BUSY: i32 = 2;
/// Per-second poll counters, printed with MAG_RENDER_FIFO_TIMING=1 or the nowait
/// poll. `wait` is host wall time inside the poll call, so the default mode shows
/// how long a poll queued behind actor work.
struct PollStats {
    calls: AtomicU64,
    receipts: AtomicU64,
    empty: AtomicU64,
    busy: AtomicU64,
    errors: AtomicU64,
    wait_ns: AtomicU64,
    wait_max_ns: AtomicU64,
    waits_over_1ms: AtomicU64,
    window: Mutex<Option<Instant>>,
}
static POLL_STATS: PollStats = PollStats {
    calls: AtomicU64::new(0),
    receipts: AtomicU64::new(0),
    empty: AtomicU64::new(0),
    busy: AtomicU64::new(0),
    errors: AtomicU64::new(0),
    wait_ns: AtomicU64::new(0),
    wait_max_ns: AtomicU64::new(0),
    waits_over_1ms: AtomicU64::new(0),
    window: Mutex::new(None),
};
impl PollStats {
    fn record(&self, nowait: bool, polled: &Result<Option<Reply>, i32>, wait: u64) {
        if !nowait && !fifo_timing() {
            return;
        }
        self.calls.fetch_add(1, Ordering::Relaxed);
        match polled {
            Ok(Some((0, _))) => &self.receipts,
            Ok(Some(_)) => &self.empty,
            Ok(None) => &self.busy,
            Err(_) => &self.errors,
        }
        .fetch_add(1, Ordering::Relaxed);
        self.wait_ns.fetch_add(wait, Ordering::Relaxed);
        self.wait_max_ns.fetch_max(wait, Ordering::Relaxed);
        if wait > 1_000_000 {
            self.waits_over_1ms.fetch_add(1, Ordering::Relaxed);
        }
        let Ok(mut window) = self.window.try_lock() else {
            return;
        };
        let now = Instant::now();
        let start = *window.get_or_insert(now);
        if now.duration_since(start).as_secs() < 1 {
            return;
        }
        *window = Some(now);
        let take = |c: &AtomicU64| c.swap(0, Ordering::Relaxed);
        eprintln!(
            "[mag native worker poll] nowait={} calls={} receipts={} empty={} busy_skips={} errors={} wait_ms_total={:.3} wait_ms_max={:.3} waits_over_1ms={}",
            u32::from(nowait),
            take(&self.calls),
            take(&self.receipts),
            take(&self.empty),
            take(&self.busy),
            take(&self.errors),
            take(&self.wait_ns) as f64 / 1e6,
            take(&self.wait_max_ns) as f64 / 1e6,
            take(&self.waits_over_1ms),
        );
    }
}
fn elapsed_ns(start: Instant) -> u64 {
    u64::try_from(start.elapsed().as_nanos()).unwrap_or(u64::MAX)
}
/// Host wall durations of the last actor admission/commit, written by the fixed
/// actor before it replies and read by the caller after that reply arrives.
#[derive(Default)]
struct ActorTiming {
    admit_ns: AtomicU64,
    commit_ns: AtomicU64,
}
impl ActorTiming {
    fn reset(&self) {
        self.admit_ns.store(0, Ordering::Relaxed);
        self.commit_ns.store(0, Ordering::Relaxed);
    }
    fn read(&self) -> (u64, u64) {
        (
            self.admit_ns.load(Ordering::Relaxed),
            self.commit_ns.load(Ordering::Relaxed),
        )
    }
}
#[derive(Default)]
struct CallerTiming {
    copy_ns: u64,
    pin_ns: u64,
    wait_ns: u64,
}
fn report_timing(
    op: &str,
    source: client::Source,
    status: i32,
    caller: &CallerTiming,
    actor: (u64, u64),
    single_validation: bool,
) {
    eprintln!(
        "[mag native worker timing] op={op} engine_update={} cohort={} publication={} pass_occurrence={} status={status} copy_ns={} pin_ns={} send_wait_ns={} actor_admit_ns={} actor_commit_ns={} single_validation={}",
        source.engine_update,
        source.cohort,
        source.publication,
        source.pass_occurrence,
        caller.copy_ns,
        caller.pin_ns,
        caller.wait_ns,
        actor.0,
        actor.1,
        u32::from(single_validation)
    );
}
fn scene_refusal(
    stage: &str,
    status: i32,
    backend: u32,
    lifetime: u64,
    source: client::Source,
) -> i32 {
    static FAILURES: AtomicUsize = AtomicUsize::new(0);
    if FAILURES.fetch_add(1, Ordering::Relaxed) < 8 {
        eprintln!(
            "[mag native worker refusal] stage={stage} status={status} backend={backend} owner_lifetime={lifetime} engine_update={} cohort={} publication={} occurrence={}",
            source.engine_update, source.cohort, source.publication, source.pass_occurrence
        );
    }
    status
}

struct DrawCopy {
    item: u64,
    partition: u64,
    obligations: u64,
    vertex_start: usize,
    vertex_count: usize,
    index_start: usize,
    index_count: usize,
}
struct OwnerPin(HostOwner, Option<(u64, u64)>);
// SAFETY: The unsafe public constructor requires genuine process-lived owner
// callbacks, callable across host threads and using only the owner's gate.
// retain succeeds before transfer, pinning all device/queue/proc pointers.
// No NativeContext or thread-affine session is transferred.
unsafe impl Send for OwnerPin {}
impl Drop for OwnerPin {
    fn drop(&mut self) {
        unsafe { (self.0.release.unwrap())(self.0.userdata, self.0.lifetime) };
    }
}
struct TargetPin {
    image: HostImage,
    frame: HostFrame,
}
// SAFETY: same provider contract as OwnerPin. Image retain pins this exact
// allocation before transfer. Frame is an already retained actual reservation;
// caller promises not to cancel it until the synchronous commit ACK returns.
// Worker NEVER invokes frame.cancel outside the native queue lease.
unsafe impl Send for TargetPin {}
impl Drop for TargetPin {
    fn drop(&mut self) {
        unsafe {
            (self.image.release.unwrap())(
                self.image.userdata,
                self.image.owner_lifetime,
                self.image.resource_id,
                self.image.resource_generation,
            )
        };
    }
}
struct ImportPins(Vec<mag_render_abi::packet::ImageBinding>);
// SAFETY: only genuine process-lived provider callbacks cross threads. Each
// image is pinned before sending; no NativeContext/thread-affine value crosses.
unsafe impl Send for ImportPins {}
impl Drop for ImportPins {
    fn drop(&mut self) {
        for b in &self.0 {
            unsafe {
                (b.host.release.unwrap())(
                    b.host.userdata,
                    b.host.owner_lifetime,
                    b.host.resource_id,
                    b.host.resource_generation,
                )
            }
        }
    }
}
impl ImportPins {
    unsafe fn copy(
        bindings: *const mag_render_abi::packet::ImageBinding,
        count: u32,
    ) -> Result<Self, i32> {
        if count == 0 || count > 1024 {
            return Err(-1);
        }
        let bindings = unsafe { checked_slice(bindings, count as usize) }?;
        let mut seen = std::collections::BTreeSet::new();
        for b in bindings {
            let h = &b.host;
            if b.reserved != 0
                || b.source_key == 0
                || !seen.insert(b.image)
                || h.abi_version != 1
                || h.struct_size as usize != size_of::<HostImage>()
                || h.reserved != 0
                || h.reserved1 != 0
                || h.owner_lifetime == 0
                || h.resource_id == 0
                || h.resource_generation == 0
                || h.width == 0
                || h.height == 0
                || h.image.is_null()
                || h.retain.is_none()
                || h.release.is_none()
                || h.validate.is_none()
            {
                return Err(-1);
            }
        }
        let mut result = Self(Vec::new());
        result.0.try_reserve_exact(bindings.len()).map_err(|_| -5)?;
        for b in bindings {
            let h = &b.host;
            let status = unsafe {
                h.retain.unwrap()(
                    h.userdata,
                    h.owner_lifetime,
                    h.resource_id,
                    h.resource_generation,
                )
            };
            if status != 0 {
                return Err(if status < 0 { status } else { -6 });
            }
            result.0.push(*b);
        }
        Ok(result)
    }
}
struct OwnedScene {
    packet: client::scene_packet::ScenePacket,
    target: TargetPin,
}
impl TargetPin {
    unsafe fn copy(image: *const HostImage, frame: *const HostFrame) -> Result<Self, i32> {
        let image = *unsafe { checked_ref(image) }?;
        let frame = *unsafe { checked_ref(frame) }?;
        if image.abi_version != 1
            || image.struct_size as usize != size_of::<HostImage>()
            || image.reserved != 0
            || image.reserved1 != 0
            || image.owner_lifetime == 0
            || image.resource_id == 0
            || image.resource_generation == 0
            || image.width == 0
            || image.height == 0
            || image.retain.is_none()
            || image.release.is_none()
            || image.validate.is_none()
            || !frame.valid()
            || frame.owner_lifetime != image.owner_lifetime
        {
            return Err(-1);
        }
        let status = unsafe {
            (image.retain.unwrap())(
                image.userdata,
                image.owner_lifetime,
                image.resource_id,
                image.resource_generation,
            )
        };
        if status != 0 {
            return Err(status);
        }
        Ok(Self { image, frame })
    }
}
struct OwnedInput {
    source: client::Source,
    intended: u64,
    draws: Vec<DrawCopy>,
    positions: Vec<[f32; 4]>,
    indices: Vec<u16>,
    target: TargetPin,
    clear: [f32; 4],
    depth: f32,
}
unsafe fn checked_ref<'a, T>(pointer: *const T) -> Result<&'a T, i32> {
    if pointer.is_null() || !(pointer as usize).is_multiple_of(align_of::<T>()) {
        return Err(-1);
    }
    Ok(unsafe { &*pointer })
}
unsafe fn checked_slice<'a, T>(pointer: *const T, count: usize) -> Result<&'a [T], i32> {
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
impl OwnedInput {
    unsafe fn copy(input: &client::Input, max_draws: u32, max_bytes: u64) -> Result<Self, i32> {
        if input.abi_version != 1
            || input.struct_size as usize != size_of::<client::Input>()
            || input.reserved != 0
            || input.reserved1 != 0
            || input.intended_items == 0
            || input.draw_count == 0
            || input.draw_count > max_draws
            || input.source.cohort == 0
            || input.source.publication == 0
            || input.source.pass_occurrence == 0
            || input.clear_color.iter().any(|n| !n.is_finite())
            || !input.clear_depth.is_finite()
            || !(0.0..=1.0).contains(&input.clear_depth)
        {
            return Err(-1);
        }
        let draws = unsafe { checked_slice(input.draws, input.draw_count as usize) }?;
        let mut vertices = 0usize;
        let mut indices = 0usize;
        for d in draws {
            if d.abi_version != 1
                || d.struct_size as usize != size_of::<client::Draw>()
                || d.reserved != 0
                || d.position_space != 1
                || d.source_item >= input.intended_items
                || d.vertex_count == 0
                || d.index_count == 0
                || !d.index_count.is_multiple_of(3)
            {
                return Err(-1);
            }
            vertices = vertices.checked_add(d.vertex_count as usize).ok_or(-5)?;
            indices = indices.checked_add(d.index_count as usize).ok_or(-5)?;
        }
        // Account the actual GPU uint32 index packing, not just this uint16 copy.
        if vertices
            .checked_mul(16)
            .and_then(|v| indices.checked_mul(4).and_then(|i| v.checked_add(i)))
            .ok_or(-5)? as u64
            > max_bytes
        {
            return Err(-5);
        }
        let image = *unsafe { checked_ref(input.target) }?;
        let frame = *unsafe { checked_ref(input.frame) }?;
        if image.abi_version != 1
            || image.struct_size as usize != size_of::<HostImage>()
            || image.reserved != 0
            || image.reserved1 != 0
            || image.owner_lifetime == 0
            || image.resource_id == 0
            || image.resource_generation == 0
            || image.width == 0
            || image.height == 0
            || image.retain.is_none()
            || image.release.is_none()
            || image.validate.is_none()
            || !frame.valid()
            || frame.owner_lifetime != image.owner_lifetime
        {
            return Err(-1);
        }
        let mut positions = Vec::new();
        positions.try_reserve_exact(vertices).map_err(|_| -5)?;
        let mut index_bytes = Vec::new();
        index_bytes.try_reserve_exact(indices).map_err(|_| -5)?;
        let mut records = Vec::new();
        records.try_reserve_exact(draws.len()).map_err(|_| -5)?;
        for d in draws {
            let vs = unsafe { checked_slice(d.positions, d.vertex_count as usize) }?;
            let is = unsafe { checked_slice(d.indices, d.index_count as usize) }?;
            if vs.iter().flatten().any(|n| !n.is_finite())
                || is.iter().any(|i| u32::from(*i) >= d.vertex_count)
            {
                return Err(-1);
            }
            records.push(DrawCopy {
                item: d.source_item,
                partition: d.source_partition,
                obligations: d.unresolved_obligations,
                vertex_start: positions.len(),
                vertex_count: vs.len(),
                index_start: index_bytes.len(),
                index_count: is.len(),
            });
            positions.extend_from_slice(vs);
            index_bytes.extend_from_slice(is);
        }
        let status = unsafe {
            (image.retain.unwrap())(
                image.userdata,
                image.owner_lifetime,
                image.resource_id,
                image.resource_generation,
            )
        };
        if status != 0 {
            return Err(status);
        }
        Ok(Self {
            source: input.source,
            intended: input.intended_items,
            draws: records,
            positions,
            indices: index_bytes,
            target: TargetPin { image, frame },
            clear: input.clear_color,
            depth: input.clear_depth,
        })
    }
    fn submit(&self, session: u64) -> (i32, client::Receipt) {
        let draws: Vec<_> = self
            .draws
            .iter()
            .map(|d| client::Draw {
                abi_version: 1,
                struct_size: size_of::<client::Draw>() as u32,
                source_item: d.item,
                source_partition: d.partition,
                positions: self.positions[d.vertex_start..].as_ptr(),
                indices: self.indices[d.index_start..].as_ptr(),
                vertex_count: d.vertex_count as u32,
                index_count: d.index_count as u32,
                position_space: 1,
                reserved: 0,
                unresolved_obligations: d.obligations,
            })
            .collect();
        let input = client::Input {
            abi_version: 1,
            struct_size: size_of::<client::Input>() as u32,
            source: self.source,
            intended_items: self.intended,
            draws: draws.as_ptr(),
            draw_count: draws.len() as u32,
            reserved: 0,
            target: &self.target.image,
            frame: &self.target.frame,
            clear_color: self.clear,
            clear_depth: self.depth,
            reserved1: 0,
        };
        let mut receipt = client::Receipt::default();
        let status = unsafe { client::mag_render_client_submit(session, &input, &mut receipt) };
        (status, receipt)
    }
}
trait Driver {
    fn admit(
        &mut self,
        _worker: u64,
        _packet: client::scene_packet::ScenePacket,
        _prevalidated: Option<(usize, u64)>,
    ) -> Result<mag_render_abi::packet::Admission, i32> {
        Err(-2)
    }
    fn commit(
        &mut self,
        _admission: mag_render_abi::packet::Admission,
        _source: client::Source,
        _target: TargetPin,
    ) -> Reply {
        (-2, client::Receipt::default())
    }
    fn commit_subgraph(
        &mut self,
        _admission: mag_render_abi::packet::Admission,
        _source: client::Source,
        _bindings: ImportPins,
    ) -> Reply {
        (-2, client::Receipt::default())
    }
    fn cancel(&mut self, _admission: mag_render_abi::packet::Admission) -> Result<(), i32> {
        Err(-2)
    }
    /// Commit whose acknowledgement may be sent before it completes on this
    /// actor (see `reserved_commit`). Exactly one acknowledgement is sent.
    fn commit_subgraph_reserved(
        &mut self,
        admission: mag_render_abi::packet::Admission,
        source: client::Source,
        bindings: ImportPins,
        acknowledge: &mut dyn FnMut(Reply),
    ) {
        acknowledge(self.commit_subgraph(admission, source, bindings));
    }

    fn scene(&mut self, _: OwnedScene) -> (i32, client::Receipt) {
        (-2, client::Receipt::default())
    }
    fn submit(&mut self, input: OwnedInput) -> (i32, client::Receipt);
    fn poll(&mut self) -> (i32, client::Receipt);
    fn close(&mut self) -> i32;
}
struct ClientDriver {
    session: u64,
}
impl Driver for ClientDriver {
    fn admit(
        &mut self,
        worker: u64,
        packet: client::scene_packet::ScenePacket,
        prevalidated: Option<(usize, u64)>,
    ) -> Result<mag_render_abi::packet::Admission, i32> {
        client::scene_packet::admit(self.session, worker, packet, prevalidated)
    }
    fn commit(
        &mut self,
        admission: mag_render_abi::packet::Admission,
        source: client::Source,
        target: TargetPin,
    ) -> Reply {
        client::scene_packet::commit(self.session, admission, source, &target.image, target.frame)
    }
    fn commit_subgraph(
        &mut self,
        admission: mag_render_abi::packet::Admission,
        source: client::Source,
        bindings: ImportPins,
    ) -> Reply {
        client::scene_packet::commit_subgraph(self.session, admission, source, &bindings.0)
    }
    fn cancel(&mut self, admission: mag_render_abi::packet::Admission) -> Result<(), i32> {
        client::scene_packet::cancel(self.session, admission)
    }
    fn commit_subgraph_reserved(
        &mut self,
        admission: mag_render_abi::packet::Admission,
        source: client::Source,
        bindings: ImportPins,
        acknowledge: &mut dyn FnMut(Reply),
    ) {
        client::scene_packet::commit_subgraph_reserved(
            self.session,
            admission,
            source,
            &bindings.0,
            acknowledge,
        );
    }

    fn scene(&mut self, input: OwnedScene) -> (i32, client::Receipt) {
        client::scene_packet::submit(
            self.session,
            input.packet,
            &input.target.image,
            input.target.frame,
        )
    }
    fn submit(&mut self, input: OwnedInput) -> (i32, client::Receipt) {
        input.submit(self.session)
    }
    fn poll(&mut self) -> (i32, client::Receipt) {
        let mut r = client::Receipt::default();
        let s = unsafe { client::mag_render_client_poll(self.session, &mut r) };
        (s, r)
    }
    fn close(&mut self) -> i32 {
        client::mag_render_client_close(self.session)
    }
}
type Reply = (i32, client::Receipt);
struct AdmitInput {
    worker: u64,
    packet: Box<client::scene_packet::ScenePacket>,
    prevalidated: Option<(usize, u64)>,
}
enum Command {
    Admit(
        AdmitInput,
        SyncSender<Result<mag_render_abi::packet::Admission, i32>>,
    ),
    /// Admission immediately followed by its subgraph commit on the actor, in
    /// the same order the two separate commands would run. A definite commit
    /// rejection cancels the uncommitted preparation before replying.
    AdmitSubgraph(AdmitInput, client::Source, ImportPins, SyncSender<Reply>),
    Commit(
        mag_render_abi::packet::Admission,
        client::Source,
        TargetPin,
        SyncSender<Reply>,
    ),
    Subgraph(
        mag_render_abi::packet::Admission,
        client::Source,
        ImportPins,
        SyncSender<Reply>,
    ),
    Cancel(mag_render_abi::packet::Admission, SyncSender<Reply>),
    Scene(Box<OwnedScene>, SyncSender<Reply>),
    Submit(Box<OwnedInput>, SyncSender<Reply>),
    Poll(SyncSender<Reply>),
    Close(SyncSender<Reply>),
}
struct Worker {
    commands: SyncSender<Command>,
    timing: Arc<ActorTiming>,
    /// Commands sent and not yet finished by the actor, including work it
    /// continues after acknowledging (a reserved commit's encode and GPU commit).
    outstanding: Arc<AtomicUsize>,
    serial: Mutex<()>,
    join: Mutex<Option<JoinHandle<()>>>,
    max_draws: u32,
    max_bytes: u64,
    /// Scene workers: terminal receipts the session already published. The
    /// host poll takes them directly, without the actor or the serial lock.
    outbox: Option<client::Outbox>,
}
impl Worker {
    fn start<D: Driver + 'static, F: FnOnce() -> Result<D, i32> + Send + 'static>(
        factory: F,
        max_draws: u32,
        max_bytes: u64,
    ) -> Result<Self, i32> {
        Self::start_with(factory, max_draws, max_bytes, reserved_commit())
    }
    /// `reserved`: whether imported subgraph commits use the reserved
    /// (acknowledge-early) path for this actor's whole life.
    fn start_with<D: Driver + 'static, F: FnOnce() -> Result<D, i32> + Send + 'static>(
        factory: F,
        max_draws: u32,
        max_bytes: u64,
        reserved: bool,
    ) -> Result<Self, i32> {
        let (commands, receive) = mpsc::sync_channel(1);
        let (started, ready) = mpsc::sync_channel(1);
        let timing = Arc::new(ActorTiming::default());
        let actor_timing = Arc::clone(&timing);
        let outstanding = Arc::new(AtomicUsize::new(0));
        let actor_outstanding = Arc::clone(&outstanding);
        let join = thread::Builder::new()
            .name("mag-render-client".into())
            // Platform pipelines compile on this thread.
            .stack_size(crate::thread_stack::with_overhead(
                crate::thread_stack::PIPELINE_COMPILER_STACK_BYTES,
            ))
            .spawn(move || {
                let mut driver = match factory() {
                    Ok(d) => d,
                    Err(e) => {
                        let _ = started.send(Err(e));
                        return;
                    }
                };
                if started.send(Ok(())).is_err() {
                    let _ = driver.close();
                    return;
                }
                while let Ok(command) = receive.recv() {
                    // Finished when this iteration ends, whichever way it ends.
                    let finished = Finished(&actor_outstanding);
                    match command {
                        Command::Admit(input, reply) => {
                            let start = Instant::now();
                            let result =
                                driver.admit(input.worker, *input.packet, input.prevalidated);
                            actor_timing
                                .admit_ns
                                .store(elapsed_ns(start), Ordering::Relaxed);
                            let token = result.as_ref().ok().copied();
                            if reply.send(result).is_err()
                                && let Some(token) = token
                            {
                                let _ = driver.cancel(token);
                            }
                        }
                        Command::AdmitSubgraph(input, source, bindings, reply) => {
                            let start = Instant::now();
                            let admitted =
                                driver.admit(input.worker, *input.packet, input.prevalidated);
                            actor_timing
                                .admit_ns
                                .store(elapsed_ns(start), Ordering::Relaxed);
                            let result = match admitted {
                                Err(status) => (status, client::Receipt::default()),
                                Ok(token) if reserved => {
                                    let start = Instant::now();
                                    let mut status = 0;
                                    driver.commit_subgraph_reserved(
                                        token,
                                        source,
                                        bindings,
                                        &mut |r: Reply| {
                                            status = r.0;
                                            actor_timing
                                                .commit_ns
                                                .store(elapsed_ns(start), Ordering::Relaxed);
                                            let _ = reply.send(r);
                                        },
                                    );
                                    if status < 0 {
                                        let _ = driver.cancel(token);
                                    }
                                    continue;
                                }
                                Ok(token) => {
                                    let start = Instant::now();
                                    let result = driver.commit_subgraph(token, source, bindings);
                                    if result.0 < 0 {
                                        let _ = driver.cancel(token);
                                    }
                                    actor_timing
                                        .commit_ns
                                        .store(elapsed_ns(start), Ordering::Relaxed);
                                    result
                                }
                            };
                            let _ = reply.send(result);
                        }
                        Command::Commit(admission, source, target, reply) => {
                            let start = Instant::now();
                            let result = driver.commit(admission, source, target);
                            actor_timing
                                .commit_ns
                                .store(elapsed_ns(start), Ordering::Relaxed);
                            let _ = reply.send(result);
                        }
                        Command::Subgraph(admission, source, bindings, reply) if reserved => {
                            let start = Instant::now();
                            driver.commit_subgraph_reserved(
                                admission,
                                source,
                                bindings,
                                &mut |r: Reply| {
                                    // Time until acknowledgement: what the caller waits for.
                                    actor_timing
                                        .commit_ns
                                        .store(elapsed_ns(start), Ordering::Relaxed);
                                    let _ = reply.send(r);
                                },
                            );
                        }
                        Command::Subgraph(admission, source, bindings, reply) => {
                            let start = Instant::now();
                            let result = driver.commit_subgraph(admission, source, bindings);
                            actor_timing
                                .commit_ns
                                .store(elapsed_ns(start), Ordering::Relaxed);
                            let _ = reply.send(result);
                        }
                        Command::Cancel(admission, reply) => {
                            let status = driver.cancel(admission).map_or_else(|e| e, |()| 0);
                            let _ = reply.send((status, client::Receipt::default()));
                        }

                        Command::Scene(input, reply) => {
                            let _ = reply.send(driver.scene(*input));
                        }
                        Command::Submit(input, reply) => {
                            let _ = reply.send(driver.submit(*input));
                        }
                        Command::Poll(reply) => {
                            let polled = driver.poll();
                            // Nothing continues after a poll: idle before the reply,
                            // so the caller's next nowait poll is not refused.
                            drop(finished);
                            let _ = reply.send(polled);
                        }
                        Command::Close(reply) => {
                            let status = driver.close();
                            let _ = reply.send((status, client::Receipt::default()));
                            if status == 0 {
                                return;
                            }
                        }
                    }
                }
                // Normally only successful Close ends the actor. A lost host still
                // cannot manufacture completion: close refuses pending GPU work.
                let _ = driver.close();
            })
            .map_err(|_| -5)?;
        match ready.recv() {
            Ok(Ok(())) => Ok(Self {
                commands,
                timing,
                outstanding,
                serial: Mutex::new(()),
                join: Mutex::new(Some(join)),
                max_draws,
                max_bytes,
                outbox: None,
            }),
            other => {
                let _ = join.join();
                Err(other.ok().and_then(Result::err).unwrap_or(-6))
            }
        }
    }
    fn call(
        &self,
        make: impl FnOnce(SyncSender<Reply>) -> Command,
        uncertain_on_lost_ack: bool,
    ) -> Result<Reply, i32> {
        let _serial = self.serial.try_lock().map_err(|_| -4)?;
        self.call_serial(make, uncertain_on_lost_ack)
    }
    fn call_serial(
        &self,
        make: impl FnOnce(SyncSender<Reply>) -> Command,
        uncertain_on_lost_ack: bool,
    ) -> Result<Reply, i32> {
        let (send, reply) = mpsc::sync_channel(1);
        self.enqueue(make(send))?;
        reply
            .recv()
            .map_err(|_| if uncertain_on_lost_ack { 1 } else { -6 })
    }
    /// The only way a command reaches the actor: counted until the actor has
    /// finished it, so an idle actor can be told apart from a busy one.
    fn enqueue(&self, command: Command) -> Result<(), i32> {
        self.outstanding.fetch_add(1, Ordering::AcqRel);
        self.commands.send(command).map_err(|_| {
            release_outstanding(&self.outstanding);
            -6
        })
    }
    /// A terminal check that never waits behind other actor work. Ok(None) means
    /// the actor is still executing an earlier command (for example the encode
    /// that follows a reserved commit's acknowledgement), so nothing was polled;
    /// the caller retries on a later poll. Otherwise identical to a Poll call.
    /// The host's terminal poll. A receipt the session already published is
    /// taken directly (never behind actor work, never refused by a busy
    /// actor); otherwise a Poll command, which with `nowait` is skipped
    /// (Ok(None)) while the actor is busy.
    fn host_poll(&self, nowait: bool) -> Result<Option<Reply>, i32> {
        if let Some(receipt) = self.take_published()? {
            return Ok(Some((0, receipt)));
        }
        if nowait {
            self.poll_nowait()
        } else {
            self.call(Command::Poll, false).map(Some)
        }
    }
    /// One already published receipt, if any; touches only the outbox.
    fn take_published(&self) -> Result<Option<client::Receipt>, i32> {
        match &self.outbox {
            Some(outbox) => Ok(outbox.lock().map_err(|_| -6)?.pop_front()),
            None => Ok(None),
        }
    }
    fn poll_nowait(&self) -> Result<Option<Reply>, i32> {
        let _serial = self.serial.try_lock().map_err(|_| -4)?;
        // With the serial lock held no other caller can enqueue, so an idle
        // count here means the Poll below is the only command on the actor.
        if self.outstanding.load(Ordering::Acquire) != 0 {
            return Ok(None);
        }
        self.call_serial(Command::Poll, false).map(Some)
    }
}
fn release_outstanding(count: &AtomicUsize) {
    // Saturating: a command that bypassed enqueue can never wrap the count.
    let _ = count.try_update(Ordering::AcqRel, Ordering::Acquire, |v| v.checked_sub(1));
}
struct Finished<'a>(&'a AtomicUsize);
impl Drop for Finished<'_> {
    fn drop(&mut self) {
        release_outstanding(self.0);
    }
}
static WORKERS: OnceLock<Mutex<BTreeMap<u64, Arc<Worker>>>> = OnceLock::new();
static NEXT: AtomicU64 = AtomicU64::new(1);
static COUNT: AtomicUsize = AtomicUsize::new(0);
struct CountReservation;
impl Drop for CountReservation {
    fn drop(&mut self) {
        COUNT.fetch_sub(1, Ordering::AcqRel);
    }
}
fn all() -> &'static Mutex<BTreeMap<u64, Arc<Worker>>> {
    WORKERS.get_or_init(|| Mutex::new(BTreeMap::new()))
}
fn get(id: u64) -> Result<Arc<Worker>, i32> {
    all().lock().map_err(|_| -6)?.get(&id).cloned().ok_or(-3)
}
fn ffi(work: impl FnOnce() -> Result<i32, i32>, panic_status: i32) -> i32 {
    catch_unwind(AssertUnwindSafe(work)).map_or(panic_status, |r| r.unwrap_or_else(|s| s))
}

/// Start a fixed worker using build-generated diagnostic stages on the genuine
/// existing owner. Provider callbacks are process-lived, non-unwinding and
/// thread-independent; retain/release/try_begin/end must NOT acquire the FIFO
/// consumer lease or wait for GPU completion. NativeContext stays on the worker.
/// # Safety
/// owner/out point to valid synchronous storage; the above host contract holds.
/// # Safety
/// Same genuine provider and synchronous storage contract as create_for_format.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mag_render_client_worker_create(
    owner: *const HostOwner,
    max_draws: u32,
    max_bytes: u64,
    out: *mut u64,
) -> i32 {
    unsafe { mag_render_client_worker_create_for_format(owner, 1, max_draws, max_bytes, out) }
}
/// Create with the exact imported host target format (RGBA8=1 or BGRA8=3).
/// No source-format inference or channel conversion is performed.
/// # Safety
/// Genuine provider callback and synchronous storage contract described above.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mag_render_client_worker_create_for_format(
    owner: *const HostOwner,
    color_format: u32,
    max_draws: u32,
    max_bytes: u64,
    out: *mut u64,
) -> i32 {
    if out.is_null() {
        return -1;
    }
    unsafe { *out = 0 };
    ffi(
        || {
            if !matches!(color_format, 1 | 3) {
                return Err(-2);
            }
            let host = *unsafe { checked_ref(owner) }?;
            if host.abi_version != 1
                || host.struct_size as usize != size_of::<HostOwner>()
                || host.reserved != 0
                || host.reserved1 != 0
                || host.lifetime == 0
                || host.device.is_null()
                || host.queue.is_null()
                || host.retain.is_none()
                || host.release.is_none()
                || host.try_begin.is_none()
                || host.end.is_none()
                || max_draws == 0
                || max_draws > MAX_DRAWS
                || max_bytes == 0
                || max_bytes > MAX_BYTES
            {
                return Err(-1);
            }
            let status = unsafe { (host.retain.unwrap())(host.userdata, host.lifetime) };
            if status != 0 {
                return Err(status);
            }
            let pin = OwnerPin(host, None);
            COUNT
                .try_update(Ordering::AcqRel, Ordering::Acquire, |n| {
                    (n < MAX_WORKERS).then_some(n + 1)
                })
                .map_err(|_| -4)?;
            let reservation = CountReservation;
            let result = (|| {
                let id = NEXT
                    .try_update(Ordering::AcqRel, Ordering::Acquire, |n| n.checked_add(1))
                    .map_err(|_| -5)?;
                let worker = Worker::start(
                    move || {
                        let host = &pin.0;
                        let mut pipeline = std::mem::MaybeUninit::uninit();
                        let status = unsafe {
                            client::mag_render_client_diagnostic_pipeline(
                                host.backend,
                                pipeline.as_mut_ptr(),
                            )
                        };
                        if status != 0 {
                            return Err(status);
                        }
                        let mut pipeline = unsafe { pipeline.assume_init() };
                        pipeline.color_format = color_format;
                        let mut session = 0;
                        let status = unsafe {
                            client::mag_render_client_create(
                                host,
                                &pipeline,
                                max_draws,
                                max_bytes,
                                &mut session,
                            )
                        };
                        drop(pin);
                        if status != 0 {
                            return Err(status);
                        }
                        Ok(ClientDriver { session })
                    },
                    max_draws,
                    max_bytes,
                )?;
                all().lock().map_err(|_| -6)?.insert(id, Arc::new(worker));
                unsafe { *out = id };
                Ok(0)
            })();
            if result.is_ok() {
                std::mem::forget(reservation);
            }
            result
        },
        -6,
    )
}
/// Create a material-scene actor without compiling a diagnostic pipeline or
/// assuming a target format. The context is still constructed on its actor.
/// # Safety
/// Genuine process-lived owner callbacks obey the existing thread-independent,
/// nonblocking host contract; out points to writable storage.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mag_render_client_worker_create_scene(
    owner: *const HostOwner,
    max_draws: u32,
    max_bytes: u64,
    out: *mut u64,
) -> i32 {
    if out.is_null() {
        return -1;
    }
    unsafe { *out = 0 };
    ffi(
        || {
            let host = *unsafe { checked_ref(owner) }?;
            start_scene(host, None, max_draws, max_bytes, out)
        },
        -6,
    )
}
/// # Safety
/// Same owner contract; enabled features are actual generation-qualified device
/// creation facts, never inferred from physical device support.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mag_render_client_worker_create_scene_v2(
    owner: *const crate::HostOwnerV2,
    max_draws: u32,
    max_bytes: u64,
    out: *mut u64,
) -> i32 {
    if out.is_null() {
        return -1;
    }
    unsafe { *out = 0 };
    ffi(
        || {
            let host = *unsafe { checked_ref(owner) }?;
            if host.abi_version != 2
                || host.struct_size as usize != size_of::<crate::HostOwnerV2>()
                || host.known_features & !7 != 0
                || host.enabled_features & !host.known_features != 0
            {
                return Err(-1);
            }
            start_scene(
                host.base,
                Some((host.known_features, host.enabled_features)),
                max_draws,
                max_bytes,
                out,
            )
        },
        -6,
    )
}
fn start_scene(
    host: HostOwner,
    features: Option<(u64, u64)>,
    max_draws: u32,
    max_bytes: u64,
    out: *mut u64,
) -> Result<i32, i32> {
    if host.abi_version != 1
        || host.struct_size as usize != size_of::<HostOwner>()
        || host.reserved != 0
        || host.reserved1 != 0
        || host.lifetime == 0
        || host.device.is_null()
        || host.queue.is_null()
        || host.retain.is_none()
        || host.release.is_none()
        || host.try_begin.is_none()
        || host.end.is_none()
        || max_draws == 0
        || max_draws > MAX_DRAWS
        || max_bytes == 0
        || max_bytes > crate::batch_device::scene_device::MAX_SCENE_BYTES
    {
        return Err(-1);
    }
    let status = unsafe { host.retain.unwrap()(host.userdata, host.lifetime) };
    if status != 0 {
        return Err(status);
    }
    let pin = OwnerPin(host, features);
    COUNT
        .try_update(Ordering::AcqRel, Ordering::Acquire, |n| {
            (n < MAX_WORKERS).then_some(n + 1)
        })
        .map_err(|_| -4)?;
    let reservation = CountReservation;
    let id = NEXT
        .try_update(Ordering::AcqRel, Ordering::Acquire, |n| n.checked_add(1))
        .map_err(|_| -5)?;
    let outbox = client::Outbox::default();
    let session_outbox = Arc::clone(&outbox);
    let mut worker = Worker::start(
        move || {
            let context = if let Some((known_features, enabled_features)) = pin.1 {
                let owner = crate::HostOwnerV2 {
                    abi_version: 2,
                    struct_size: size_of::<crate::HostOwnerV2>() as u32,
                    base: pin.0,
                    known_features,
                    enabled_features,
                };
                unsafe { crate::NativeContext::from_host_v2(&owner) }
            } else {
                unsafe { crate::NativeContext::from_host(&pin.0) }
            }
            .map_err(|status| {
                scene_refusal(
                    "create-native-context",
                    status,
                    pin.0.backend,
                    pin.0.lifetime,
                    client::Source::default(),
                )
            })?;
            let session =
                client::create_scene_session(context, pin.0.backend, max_draws, max_bytes)
                    .map_err(|status| {
                        scene_refusal(
                            "create-scene-session",
                            status,
                            pin.0.backend,
                            pin.0.lifetime,
                            client::Source::default(),
                        )
                    })?;
            drop(pin);
            client::set_session_outbox(session, session_outbox)?;
            Ok(ClientDriver { session })
        },
        max_draws,
        max_bytes,
    )?;
    worker.outbox = Some(outbox);
    all().lock().map_err(|_| -6)?.insert(id, Arc::new(worker));
    unsafe { *out = id };
    std::mem::forget(reservation);
    Ok(0)
}
/// Copy all source geometry before enqueueing, then await CPU queue-commit ACK.
/// This never waits for a GPU fence. Negative status is definitely uncommitted;
/// caller retains frame cancellation responsibility on its consumer/lifecycle
/// thread. Positive status is indeterminate and MUST NOT be replayed/cancelled.
/// # Safety
/// Synchronous input arrays are valid/aligned; target/frame are genuine retained
/// provider metadata, not cancelled concurrently, and obey the host contract above.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mag_render_client_worker_submit(
    id: u64,
    input: *const client::Input,
    out: *mut client::Receipt,
) -> i32 {
    if out.is_null() {
        return -1;
    }
    unsafe {
        *out = client::Receipt {
            abi_version: 1,
            struct_size: size_of::<client::Receipt>() as u32,
            flags: 1,
            ..client::Receipt::default()
        }
    };
    let result = ffi(
        || {
            let worker = get(id)?;
            // Admit before allocating/copying: concurrent producers cannot build an
            // unbounded collection of rejected full-scene messages.
            let _serial = worker.serial.try_lock().map_err(|_| -4)?;
            let input = unsafe { checked_ref(input) }?;
            let owned = unsafe { OwnedInput::copy(input, worker.max_draws, worker.max_bytes) }?;
            unsafe {
                (*out).source = input.source;
                (*out).intended_items = input.intended_items;
            }
            let (status, receipt) =
                worker.call_serial(|reply| Command::Submit(Box::new(owned), reply), true)?;
            unsafe { *out = receipt };
            Ok(status)
        },
        1,
    );
    unsafe {
        (*out).status = result;
        if result > 0 {
            (*out).flags |= 3;
        }
    }
    result
}
/// Copy a complete material/attribute scene into the existing fixed worker.
/// The acknowledgement reports CPU queue admission, never GPU completion.
/// # Safety
/// All packet ranges are readable for this call; target/frame callbacks are
/// genuine, process-lived and thread-independent under the existing worker
/// contract. The caller must not cancel the frame until this ACK returns.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mag_render_client_worker_submit_scene(
    id: u64,
    input: *const client::scene_packet::raw::Desc,
    out: *mut client::Receipt,
) -> i32 {
    if out.is_null() {
        return -1;
    }
    unsafe {
        *out = client::Receipt {
            abi_version: 1,
            struct_size: size_of::<client::Receipt>() as u32,
            flags: 1,
            ..client::Receipt::default()
        }
    };
    let status = ffi(
        || {
            let worker = get(id)?;
            let _serial = worker.serial.try_lock().map_err(|_| -4)?;
            let input = unsafe { checked_ref(input) }?;
            let packet = unsafe {
                client::scene_packet::ScenePacket::copy(
                    input,
                    worker.max_draws as usize,
                    worker.max_bytes,
                )
            }?;
            let target = unsafe { TargetPin::copy(input.target, input.frame) }?;
            unsafe {
                (*out).source = packet.source;
                (*out).intended_items = packet.intended;
            }
            let owned = OwnedScene { packet, target };
            let (status, receipt) =
                worker.call_serial(|reply| Command::Scene(Box::new(owned), reply), true)?;
            unsafe {
                *out = receipt;
            }
            Ok(status)
        },
        1,
    );
    unsafe {
        (*out).status = status;
        if status > 0 {
            (*out).flags |= 3;
        }
    }
    status
}
/// Submit complete owned source view/image/viewport state through the same actor.
/// # Safety
/// All V2/base ranges are valid synchronously and target/frame obey the retained
/// genuine-provider contract of worker_submit_scene. No GPU wait is performed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mag_render_client_worker_submit_scene_v2(
    id: u64,
    input: *const client::scene_packet::raw::DescV2,
    out: *mut client::Receipt,
) -> i32 {
    if out.is_null() {
        return -1;
    }
    unsafe {
        *out = client::Receipt {
            abi_version: 1,
            struct_size: size_of::<client::Receipt>() as u32,
            flags: 1,
            ..client::Receipt::default()
        }
    };
    let status = ffi(
        || {
            let worker = get(id)?;
            let _serial = worker.serial.try_lock().map_err(|_| -4)?;
            let input = unsafe { checked_ref(input) }?;
            let packet = unsafe {
                client::scene_packet::ScenePacket::copy_v2(
                    input,
                    worker.max_draws as usize,
                    worker.max_bytes,
                )
            }?;
            let target = unsafe { TargetPin::copy(input.base.target, input.base.frame) }?;
            unsafe {
                (*out).source = packet.source;
                (*out).intended_items = packet.intended;
            }
            let (status, receipt) = worker.call_serial(
                |reply| Command::Scene(Box::new(OwnedScene { packet, target }), reply),
                true,
            )?;
            unsafe {
                *out = receipt;
            }
            Ok(status)
        },
        1,
    );
    unsafe {
        (*out).status = status;
        if status > 0 {
            (*out).flags |= 3;
        }
    }
    status
}
/// # Safety
/// V3 packet, nested tables and receipt storage are valid synchronous ranges.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mag_render_client_worker_submit_scene_v3(
    id: u64,
    input: *const client::scene_packet::raw::DescV3,
    out: *mut client::Receipt,
) -> i32 {
    if out.is_null() {
        return -1;
    }
    unsafe {
        *out = client::Receipt {
            abi_version: 1,
            struct_size: size_of::<client::Receipt>() as u32,
            flags: 1,
            ..client::Receipt::default()
        }
    };
    let status = ffi(
        || {
            let worker = get(id)?;
            let _serial = worker.serial.try_lock().map_err(|_| -4)?;
            let input = unsafe { checked_ref(input) }?;
            let packet = unsafe {
                client::scene_packet::ScenePacket::copy_v3(
                    input,
                    worker.max_draws as usize,
                    worker.max_bytes,
                )
            }?;
            let target = unsafe { TargetPin::copy(input.base.base.target, input.base.base.frame) }?;
            unsafe {
                (*out).source = packet.source;
                (*out).intended_items = packet.intended;
            }
            let (status, receipt) = worker.call_serial(
                |reply| Command::Scene(Box::new(OwnedScene { packet, target }), reply),
                true,
            )?;
            unsafe {
                *out = receipt;
            }
            Ok(status)
        },
        1,
    );
    unsafe {
        (*out).status = status;
        if status > 0 {
            (*out).flags |= 3;
        }
    }
    status
}
/// Prepare an owned, fully resolved scene on the fixed native thread. This
/// obtains no target/frame and commits no GPU work or original CPU outputs.
/// # Safety
/// Complete ABI3 nested ranges and writable admission storage are valid for the
/// synchronous call. Both embedded target/frame pointers must be null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mag_render_client_worker_prepare_scene_v3(
    id: u64,
    input: *const client::scene_packet::raw::DescV3,
    out: *mut mag_render_abi::packet::Admission,
) -> i32 {
    unsafe {
        mag_render_client_worker_prepare_scene_imported_v3(id, input, std::ptr::null(), 0, out)
    }
}
/// Prepare exact source-key external requirements without observing any current
/// image, queue or display frame. Plane initialization is conditional until commit.
/// # Safety
/// All nested input and requirement ranges are readable through synchronous copy.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mag_render_client_worker_prepare_scene_imported_v3(
    id: u64,
    input: *const client::scene_packet::raw::DescV3,
    requirements: *const mag_render_abi::packet::ImageRequirement,
    count: u32,
    out: *mut mag_render_abi::packet::Admission,
) -> i32 {
    if out.is_null() {
        return -1;
    }
    unsafe {
        *out = mag_render_abi::packet::Admission::default();
    }
    ffi(
        || {
            client::scene_admission::checked_layout()?;
            client::scene_admission::checked_import_layout()?;
            let worker = get(id)?;
            let _serial = worker.serial.try_lock().map_err(|_| -4)?;
            let input = unsafe { checked_ref(input) }?;
            if !input.base.base.target.is_null() || !input.base.base.frame.is_null() {
                return Err(-1);
            }
            if count > 1024 {
                return Err(-1);
            }
            let requirements = unsafe { checked_slice(requirements, count as usize) }?;
            let timed = fifo_timing();
            let mut timing = CallerTiming::default();
            let copy_start = Instant::now();
            let admit = unsafe { copy_admit_input(&worker, id, input, requirements) }?;
            timing.copy_ns = elapsed_ns(copy_start);
            let single = admit.prevalidated.is_some();
            if timed {
                worker.timing.reset();
            }
            let wait_start = Instant::now();
            let (send, reply) = mpsc::sync_channel(1);
            worker.enqueue(Command::Admit(admit, send))?;
            let admitted = reply.recv().map_err(|_| -6)?;
            timing.wait_ns = elapsed_ns(wait_start);
            if timed {
                report_timing(
                    "prepare",
                    input.base.base.source,
                    admitted.as_ref().map_or_else(|&e| e, |_| 0),
                    &timing,
                    worker.timing.read(),
                    single,
                );
            }
            let admission = admitted.map_err(|status| {
                scene_refusal("prepare-actor-admit", status, 0, 0, input.base.base.source)
            })?;
            unsafe {
                *out = admission;
            }
            Ok(0)
        },
        -6,
    )
}
/// Copy (and validate) the imported packet on the calling thread. The returned
/// owned input moves into the actor; no further copy of the packet is made.
/// # Safety
/// Same synchronous range contract as `prepare_scene_imported_v3`.
unsafe fn copy_admit_input(
    worker: &Worker,
    id: u64,
    input: &client::scene_packet::raw::DescV3,
    requirements: &[mag_render_abi::packet::ImageRequirement],
) -> Result<AdmitInput, i32> {
    let limits = (worker.max_draws as usize, worker.max_bytes);
    let packet = unsafe {
        client::scene_packet::ScenePacket::copy_imported_v3(input, requirements, limits.0, limits.1)
    }
    .inspect_err(|&status| {
        if status < 0 {
            scene_refusal(
                "prepare-copy-imported-v3",
                status,
                0,
                0,
                input.base.base.source,
            );
        }
    })?;
    Ok(AdmitInput {
        worker: id,
        packet: Box::new(packet),
        // copy_imported_v3 validated this exact packet against these limits.
        prevalidated: single_validation().then_some(limits),
    })
}
/// Default-off alternative to prepare_imported_v3 followed by commit_subgraph:
/// one copy, one pin step and ONE ordered actor command, so the caller does not
/// wait for a separate admission acknowledgement before requesting the commit.
/// Status meaning is unchanged: negative is definitely uncommitted (a rejected
/// commit's preparation is cancelled on the actor), zero is accepted and
/// positive is indeterminate. No GPU completion or publication is implied.
/// # Safety
/// All nested input, requirement and binding ranges are valid for the
/// synchronous call; provider metadata obeys the retained callback contract.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mag_render_client_worker_submit_scene_subgraph_imported_v3(
    id: u64,
    input: *const client::scene_packet::raw::DescV3,
    requirements: *const mag_render_abi::packet::ImageRequirement,
    count: u32,
    bindings: *const mag_render_abi::packet::ImageBinding,
    binding_count: u32,
    out: *mut client::Receipt,
) -> i32 {
    if out.is_null() {
        return -1;
    }
    unsafe {
        *out = client::Receipt {
            abi_version: 1,
            struct_size: size_of::<client::Receipt>() as u32,
            flags: 1,
            ..client::Receipt::default()
        }
    };
    let status = ffi(
        || {
            client::scene_admission::checked_layout()?;
            client::scene_admission::checked_import_layout()?;
            let worker = get(id)?;
            let _serial = worker.serial.try_lock().map_err(|_| -4)?;
            let input = unsafe { checked_ref(input) }?;
            if !input.base.base.target.is_null() || !input.base.base.frame.is_null() {
                return Err(-1);
            }
            if count > 1024 {
                return Err(-1);
            }
            let source = input.base.base.source;
            unsafe { (*out).source = source };
            let requirements = unsafe { checked_slice(requirements, count as usize) }?;
            let timed = fifo_timing();
            let mut timing = CallerTiming::default();
            let copy_start = Instant::now();
            let admit = unsafe { copy_admit_input(&worker, id, input, requirements) }?;
            timing.copy_ns = elapsed_ns(copy_start);
            let single = admit.prevalidated.is_some();
            let pin_start = Instant::now();
            let pins = unsafe { ImportPins::copy(bindings, binding_count) }?;
            timing.pin_ns = elapsed_ns(pin_start);
            if timed {
                worker.timing.reset();
            }
            let wait_start = Instant::now();
            let result = worker.call_serial(
                |reply| Command::AdmitSubgraph(admit, source, pins, reply),
                true,
            );
            timing.wait_ns = elapsed_ns(wait_start);
            if timed {
                report_timing(
                    "fused-subgraph",
                    source,
                    result.as_ref().map_or_else(|&e| e, |r| r.0),
                    &timing,
                    worker.timing.read(),
                    single,
                );
            }
            let (status, receipt) = result?;
            unsafe { *out = receipt };
            Ok(status)
        },
        1,
    );
    unsafe {
        (*out).status = status;
        if status > 0 {
            (*out).flags |= 3;
        }
    }
    status
}
/// Commit an exact preparation/source on a genuine source-qualified target and
/// frame. Accepted/indeterminate consumes the token, negative leaves it intact.
/// # Safety
/// POD pointers and writable receipt are valid. Provider metadata obeys the
/// existing process-lived, cross-thread retained callback contract. No GPU wait.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mag_render_client_worker_commit_scene(
    id: u64,
    admission: *const mag_render_abi::packet::Admission,
    source: *const client::Source,
    target: *const HostImage,
    frame: *const HostFrame,
    out: *mut client::Receipt,
) -> i32 {
    if out.is_null() {
        return -1;
    }
    unsafe {
        *out = client::Receipt {
            abi_version: 1,
            struct_size: size_of::<client::Receipt>() as u32,
            flags: 1,
            ..client::Receipt::default()
        };
    }
    let status = ffi(
        || {
            client::scene_admission::checked_layout()?;
            let worker = get(id)?;
            let _serial = worker.serial.try_lock().map_err(|_| -4)?;
            let admission = *unsafe { checked_ref(admission) }?;
            let source = *unsafe { checked_ref(source) }?;
            if admission.worker != id
                || !client::scene_admission::same_source(admission.source, source)
            {
                return Err(-3);
            }
            let target = unsafe { TargetPin::copy(target, frame) }?;
            let (status, receipt) = worker.call_serial(
                |reply| Command::Commit(admission, source, target, reply),
                true,
            )?;
            unsafe {
                *out = receipt;
            }
            Ok(status)
        },
        1,
    );
    unsafe {
        (*out).status = status;
        if status > 0 {
            (*out).flags |= 3;
        }
    }
    status
}
/// Cancel only an uncommitted preparation. Native GPU completion is not implied.
/// # Safety
/// admission points to a valid synchronous POD range.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mag_render_client_worker_cancel_scene(
    id: u64,
    admission: *const mag_render_abi::packet::Admission,
) -> i32 {
    ffi(
        || {
            client::scene_admission::checked_layout()?;
            let worker = get(id)?;
            let admission = *unsafe { checked_ref(admission) }?;
            if admission.worker != id {
                return Err(-3);
            }
            let (status, _) = worker.call(|reply| Command::Cancel(admission, reply), false)?;
            Ok(status)
        },
        -6,
    )
}
/// Terminal check on the same fixed native thread; it does not wait for the GPU.
/// By default it queues behind any command the actor is still executing. With
/// MAG_RENDER_WORKER_POLL_NOWAIT=1 it returns 2 (busy, nothing polled) instead.
/// Physical present events remain separate SDK host observations; this API does
/// not poll them.
/// # Safety
/// out points to writable Receipt storage.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mag_render_client_worker_poll(id: u64, out: *mut client::Receipt) -> i32 {
    if out.is_null() {
        return -1;
    }
    unsafe { *out = client::Receipt::default() };
    ffi(
        || {
            let worker = get(id)?;
            let nowait = poll_nowait_enabled();
            let start = Instant::now();
            let polled = worker.host_poll(nowait);
            POLL_STATS.record(nowait, &polled, elapsed_ns(start));
            let (status, receipt) = match polled? {
                Some(reply) => reply,
                // Busy actor: nothing polled; the caller's next poll retries.
                None => return Ok(POLL_BUSY),
            };
            unsafe { *out = receipt };
            Ok(status)
        },
        -6,
    )
}
/// Takes one receipt the scene session already published (see Outbox): no
/// actor command and no serial lock, so it never refuses or delays another
/// caller of this worker. -4 when none is published (or no outbox).
/// # Safety
/// out points to writable Receipt storage.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mag_render_client_worker_take_receipt(
    id: u64,
    out: *mut client::Receipt,
) -> i32 {
    if out.is_null() {
        return -1;
    }
    unsafe { *out = client::Receipt::default() };
    ffi(
        || {
            let receipt = get(id)?.take_published()?.ok_or(-4)?;
            unsafe { *out = receipt };
            Ok(0)
        },
        -6,
    )
}
/// BUSY preserves worker/session/pending leases. No GPU wait, frame cancellation
/// or SDK shutdown retry is hidden here. Only successful close retires the actor.
#[unsafe(no_mangle)]
pub extern "C" fn mag_render_client_worker_close(id: u64) -> i32 {
    ffi(
        || {
            let worker = get(id)?;
            let (status, _) = worker.call(Command::Close, false)?;
            if status != 0 {
                return Ok(status);
            }
            all().lock().map_err(|_| -6)?.remove(&id);
            COUNT.fetch_sub(1, Ordering::AcqRel);
            if let Some(join) = worker.join.lock().map_err(|_| -6)?.take() {
                join.join().map_err(|_| -6)?;
            }
            Ok(0)
        },
        -6,
    )
}

/// Commit a prepared Main subgraph on explicitly retained existing surfaces.
/// No final target, display frame, presentation or original CPU completion is implied.
/// # Safety
/// POD ranges and genuine provider metadata remain valid through the synchronous
/// retain/copy. Callbacks are process-lived, non-unwinding and thread-safe.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mag_render_client_worker_commit_scene_subgraph(
    id: u64,
    admission: *const mag_render_abi::packet::Admission,
    source: *const client::Source,
    bindings: *const mag_render_abi::packet::ImageBinding,
    count: u32,
    out: *mut client::Receipt,
) -> i32 {
    if out.is_null() {
        return -1;
    }
    unsafe {
        *out = client::Receipt {
            abi_version: 1,
            struct_size: size_of::<client::Receipt>() as u32,
            flags: 1,
            ..client::Receipt::default()
        }
    };
    let status = ffi(
        || {
            client::scene_admission::checked_import_layout()?;
            let worker = get(id)?;
            let _serial = worker.serial.try_lock().map_err(|_| -4)?;
            let admission = *unsafe { checked_ref(admission) }?;
            let source = *unsafe { checked_ref(source) }?;
            if admission.worker != id
                || !client::scene_admission::same_source(admission.source, source)
            {
                return Err(-3);
            }
            let timed = fifo_timing();
            let mut timing = CallerTiming::default();
            let pin_start = Instant::now();
            let bindings = unsafe { ImportPins::copy(bindings, count) }?;
            timing.pin_ns = elapsed_ns(pin_start);
            if timed {
                worker.timing.reset();
            }
            let wait_start = Instant::now();
            let result = worker.call_serial(
                |reply| Command::Subgraph(admission, source, bindings, reply),
                true,
            );
            timing.wait_ns = elapsed_ns(wait_start);
            if timed {
                report_timing(
                    "commit-subgraph",
                    source,
                    result.as_ref().map_or_else(|&e| e, |r| r.0),
                    &timing,
                    worker.timing.read(),
                    false,
                );
            }
            let (status, receipt) = result?;
            unsafe { *out = receipt };
            Ok(status)
        },
        1,
    );
    unsafe {
        (*out).status = status;
        if status > 0 {
            (*out).flags |= 3
        }
    };
    status
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::{
        ffi::c_void, marker::PhantomData, rc::Rc, sync::atomic::AtomicI32, thread::ThreadId,
    };
    unsafe extern "C" fn retain(user: *mut c_void, _: u64, _: u64, _: u64) -> i32 {
        unsafe { &*user.cast::<AtomicI32>() }.fetch_add(1, Ordering::SeqCst);
        0
    }
    unsafe extern "C" fn release(user: *mut c_void, _: u64, _: u64, _: u64) {
        unsafe { &*user.cast::<AtomicI32>() }.fetch_sub(1, Ordering::SeqCst);
    }
    unsafe extern "C" fn validate(_: *mut c_void, _: u64, _: u64, _: u64) -> i32 {
        0
    }
    unsafe extern "C" fn preflight(_: *mut c_void, _: u64, _: u64, _: u64, _: u64) -> i32 {
        panic!("CPU ownership check must not encode native work")
    }
    unsafe extern "C" fn committed(_: *mut c_void, _: u64, _: u64, _: u64, _: u64, _: u32) -> i32 {
        panic!("CPU ownership check must not publish native work")
    }
    unsafe extern "C" fn cancel(_: *mut c_void, _: u64, _: u64) -> i32 {
        panic!("worker must leave early cancellation to consumer")
    }
    fn with_input<T>(
        test: impl FnOnce(&mut client::Input, &mut [[f32; 4]; 3], &AtomicI32) -> T,
    ) -> T {
        let pins = AtomicI32::new(0);
        let target = HostImage {
            abi_version: 1,
            struct_size: size_of::<HostImage>() as u32,
            backend: 1,
            format: 1,
            width: 8,
            height: 8,
            reserved: 0,
            reserved1: 0,
            owner_lifetime: 7,
            resource_id: 2,
            resource_generation: 3,
            image: std::ptr::null_mut(),
            userdata: (&pins as *const AtomicI32).cast_mut().cast(),
            retain: Some(retain),
            release: Some(release),
            validate: Some(validate),
        };
        let frame = HostFrame {
            abi_version: 1,
            struct_size: size_of::<HostFrame>() as u32,
            owner_lifetime: 7,
            reservation: 11,
            userdata: std::ptr::null_mut(),
            preflight: Some(preflight),
            committed: Some(committed),
            cancel: Some(cancel),
        };
        let mut vertices = [[1., 2., 3., 1.], [4., 5., 6., 1.], [7., 8., 9., 1.]];
        let indices = [0, 2, 1];
        let draw = client::Draw {
            abi_version: 1,
            struct_size: size_of::<client::Draw>() as u32,
            source_item: 2,
            source_partition: 17,
            positions: vertices.as_ptr(),
            indices: indices.as_ptr(),
            vertex_count: 3,
            index_count: 3,
            position_space: 1,
            reserved: 0,
            unresolved_obligations: 0x800,
        };
        let mut input = client::Input {
            abi_version: 1,
            struct_size: size_of::<client::Input>() as u32,
            source: client::Source {
                engine_update: 4,
                cohort: 5,
                publication: 6,
                pass_occurrence: 7,
            },
            intended_items: 9,
            draws: &draw,
            draw_count: 1,
            reserved: 0,
            target: &target,
            frame: &frame,
            clear_color: [0.; 4],
            clear_depth: 1.,
            reserved1: 0,
        };
        let result = test(&mut input, &mut vertices, &pins);
        assert_eq!(pins.load(Ordering::SeqCst), 0);
        result
    }
    #[test]
    fn scene_only_owner_v2_rejects_unproven_enabled_features_before_pinning() {
        let mut host: crate::HostOwnerV2 = unsafe { std::mem::zeroed() };
        host.abi_version = 2;
        host.struct_size = size_of::<crate::HostOwnerV2>() as u32;
        host.enabled_features = 1;
        let mut id = 99;
        assert_eq!(
            unsafe { mag_render_client_worker_create_scene_v2(&host, 32, 1024, &mut id) },
            -1
        );
        assert_eq!(id, 0);
    }
    #[test]
    fn copied_scene_survives_source_mutation_and_releases_exact_target_pin() {
        with_input(|input, vertices, pins| {
            let owned = unsafe { OwnedInput::copy(input, 4, 1024) }.unwrap();
            assert_eq!(pins.load(Ordering::SeqCst), 1);
            vertices[0] = [99.; 4];
            assert_eq!(owned.positions[0], [1., 2., 3., 1.]);
            assert_eq!(owned.indices, [0, 2, 1]);
            assert_eq!(owned.intended, 9);
            assert_eq!(owned.draws[0].item, 2);
            assert_eq!(owned.draws[0].partition, 17);
            assert_eq!(owned.draws[0].obligations, 0x800);
            drop(owned);
        });
    }
    #[test]
    fn whole_scene_budget_and_bad_indices_refuse_before_pinning() {
        with_input(|input, _, pins| {
            assert!(matches!(unsafe { OwnedInput::copy(input, 4, 59) }, Err(-5))); //48vertex+12nativeindex bytes
            let original = unsafe { &*input.draws };
            let invalid = [0, 3, 1];
            let draw = client::Draw {
                indices: invalid.as_ptr(),
                ..unsafe { std::ptr::read(original) }
            };
            input.draws = &draw;
            assert!(matches!(
                unsafe { OwnedInput::copy(input, 4, 1024) },
                Err(-1)
            ));
            assert_eq!(pins.load(Ordering::SeqCst), 0);
        });
    }
    struct AffineDriver {
        thread: ThreadId,
        events: Arc<Mutex<Vec<(&'static str, ThreadId)>>>,
        pending: bool,
        _not_send: PhantomData<Rc<()>>,
    }
    impl AffineDriver {
        fn record(&self, name: &'static str) {
            assert_eq!(thread::current().id(), self.thread);
            self.events.lock().unwrap().push((name, self.thread));
        }
    }
    impl Driver for AffineDriver {
        fn scene(&mut self, input: OwnedScene) -> Reply {
            self.record("scene");
            assert_eq!(input.packet.programs[0].source, b"authored stage 0");
            assert_eq!(input.packet.passes[0].draws[0].partition, 10);
            assert_eq!(
                input.packet.images[1].bytes.as_deref(),
                Some(&[127, 20, 30, 255][..])
            );
            self.pending = true;
            (
                0,
                client::Receipt {
                    ticket: 29,
                    ..client::Receipt::default()
                },
            )
        }
        fn submit(&mut self, input: OwnedInput) -> Reply {
            self.record("submit");
            assert_eq!(input.positions[0], [1., 2., 3., 1.]);
            self.pending = true;
            (
                1,
                client::Receipt {
                    ticket: 19,
                    ..client::Receipt::default()
                },
            )
        }
        fn poll(&mut self) -> Reply {
            self.record("poll");
            self.pending = false;
            (
                0,
                client::Receipt {
                    ticket: 19,
                    ..client::Receipt::default()
                },
            )
        }
        fn close(&mut self) -> i32 {
            self.record("close");
            if self.pending { -4 } else { 0 }
        }
    }
    impl Drop for AffineDriver {
        fn drop(&mut self) {
            self.record("drop");
        }
    }
    #[test]
    fn migrating_callers_share_one_fixed_thread_and_busy_close_preserves_it() {
        let events = Arc::new(Mutex::new(Vec::new()));
        let observed = events.clone();
        let caller = thread::current().id();
        let worker = Arc::new(
            Worker::start(
                move || {
                    let thread = thread::current().id();
                    observed.lock().unwrap().push(("create", thread));
                    Ok(AffineDriver {
                        thread,
                        events: observed,
                        pending: false,
                        _not_send: PhantomData,
                    })
                },
                4,
                1024,
            )
            .unwrap(),
        );
        with_input(|input, _, _| {
            let owned = unsafe { OwnedInput::copy(input, 4, 1024) }.unwrap();
            assert_eq!(
                worker
                    .call(|reply| Command::Submit(Box::new(owned), reply), true)
                    .unwrap()
                    .0,
                1
            );
            assert_eq!(worker.call(Command::Close, false).unwrap().0, -4);
            let other = worker.clone();
            thread::spawn(move || {
                assert_eq!(other.call(Command::Poll, false).unwrap().1.ticket, 19);
            })
            .join()
            .unwrap();
            assert_eq!(worker.call(Command::Close, false).unwrap().0, 0);
            worker.join.lock().unwrap().take().unwrap().join().unwrap();
        });
        let events = events.lock().unwrap();
        assert_eq!(
            events.iter().map(|e| e.0).collect::<Vec<_>>(),
            ["create", "submit", "close", "poll", "close", "drop"]
        );
        let fixed = events[0].1;
        assert_ne!(fixed, caller);
        assert!(events.iter().all(|e| e.1 == fixed));
    }
    #[test]
    fn full_material_packet_uses_same_affine_actor_and_target_pin() {
        let events = Arc::new(Mutex::new(Vec::new()));
        let observed = events.clone();
        let caller = thread::current().id();
        let worker = Worker::start(
            move || {
                Ok(AffineDriver {
                    thread: thread::current().id(),
                    events: observed,
                    pending: false,
                    _not_send: PhantomData,
                })
            },
            32,
            1 << 20,
        )
        .unwrap();
        with_input(|input, _, pins| {
            let target = unsafe { TargetPin::copy(input.target, input.frame) }.unwrap();
            assert_eq!(pins.load(Ordering::SeqCst), 1);
            let owned = OwnedScene {
                packet: client::scene_packet::packet_tests::fixture(),
                target,
            };
            assert_eq!(
                worker
                    .call(|r| Command::Scene(Box::new(owned), r), true)
                    .unwrap()
                    .1
                    .ticket,
                29
            );
            assert_eq!(pins.load(Ordering::SeqCst), 0);
            assert_eq!(worker.call(Command::Close, false).unwrap().0, -4);
            worker.call(Command::Poll, false).unwrap();
            assert_eq!(worker.call(Command::Close, false).unwrap().0, 0);
            worker.join.lock().unwrap().take().unwrap().join().unwrap();
        });
        let events = events.lock().unwrap();
        assert_eq!(
            events.iter().map(|e| e.0).collect::<Vec<_>>(),
            ["scene", "close", "poll", "close", "drop"]
        );
        assert!(events.iter().all(|e| e.1 != caller && e.1 == events[0].1));
    }
    #[test]
    fn concurrent_operation_is_busy_before_work_is_enqueued() {
        let worker = Worker::start(
            || {
                Ok(AffineDriver {
                    thread: thread::current().id(),
                    events: Arc::new(Mutex::new(Vec::new())),
                    pending: false,
                    _not_send: PhantomData,
                })
            },
            4,
            1024,
        )
        .unwrap();
        let guard = worker.serial.lock().unwrap();
        assert!(matches!(worker.call(Command::Poll, false), Err(-4)));
        drop(guard);
        assert_eq!(worker.call(Command::Close, false).unwrap().0, 0);
        worker.join.lock().unwrap().take().unwrap().join().unwrap();
    }
    #[test]
    fn lost_submit_acknowledgment_is_indeterminate_never_rejected() {
        let (commands, receive) = mpsc::sync_channel(1);
        let join = thread::spawn(move || {
            if let Command::Submit(_, reply) = receive.recv().unwrap() {
                drop(reply);
            } else {
                panic!("unexpected operation")
            }
        });
        let worker = Worker {
            commands,
            timing: Arc::default(),
            outstanding: Arc::default(),
            serial: Mutex::new(()),
            join: Mutex::new(Some(join)),
            max_draws: 4,
            max_bytes: 1024,
            outbox: None,
        };
        with_input(|input, _, _| {
            let owned = unsafe { OwnedInput::copy(input, 4, 1024) }.unwrap();
            assert!(matches!(
                worker.call(|reply| Command::Submit(Box::new(owned), reply), true),
                Err(1)
            ));
            worker.join.lock().unwrap().take().unwrap().join().unwrap();
        });
    }
    #[test]
    fn unsupported_target_format_refuses_before_borrowing_owner() {
        let mut id = 99;
        assert_eq!(
            unsafe {
                mag_render_client_worker_create_for_format(std::ptr::null(), 99, 4, 1024, &mut id)
            },
            -2
        );
        assert_eq!(id, 0);
    }
    #[test]
    fn unknown_worker_is_not_a_thread_local_session_alias() {
        assert_eq!(mag_render_client_worker_close(u64::MAX), -3);
    }
    struct PreparedDriver {
        prepared: client::scene_admission::Transactions<client::scene_packet::ScenePacket>,
        pending: Vec<client::scene_packet::ScenePacket>,
        status: Arc<AtomicI32>,
        events: Arc<Mutex<Vec<&'static str>>>,
        thread: ThreadId,
    }
    impl Driver for PreparedDriver {
        fn admit(
            &mut self,
            worker: u64,
            packet: client::scene_packet::ScenePacket,
            _prevalidated: Option<(usize, u64)>,
        ) -> Result<mag_render_abi::packet::Admission, i32> {
            assert_eq!(self.thread, thread::current().id());
            self.events.lock().unwrap().push("admit");
            self.prepared.insert(
                (worker, 7),
                packet.source,
                packet.intended,
                packet.passes[0].draws.len() as u64,
                packet,
                self.pending.len(),
            )
        }
        fn commit(
            &mut self,
            a: mag_render_abi::packet::Admission,
            source: client::Source,
            _target: TargetPin,
        ) -> Reply {
            assert_eq!(self.thread, thread::current().id());
            if self.prepared.get(a, source).is_err() {
                return (-3, client::Receipt::default());
            }
            let status = self.status.load(Ordering::SeqCst);
            self.events.lock().unwrap().push("commit");
            if status < 0 {
                return (status, client::Receipt::default());
            }
            self.pending.push(self.prepared.remove(a, source).unwrap());
            (
                status,
                client::Receipt {
                    ticket: 99,
                    ..client::Receipt::default()
                },
            )
        }
        fn commit_subgraph(
            &mut self,
            a: mag_render_abi::packet::Admission,
            source: client::Source,
            bindings: ImportPins,
        ) -> Reply {
            assert_eq!(self.thread, thread::current().id());
            if self.prepared.get(a, source).is_err() {
                return (-3, client::Receipt::default());
            }
            assert_eq!(bindings.0.len(), 1);
            assert_eq!(bindings.0[0].source_key, 99);
            let status = self.status.load(Ordering::SeqCst);
            self.events.lock().unwrap().push("subgraph");
            if status < 0 {
                return (status, client::Receipt::default());
            }
            self.pending.push(self.prepared.remove(a, source).unwrap());
            (
                status,
                client::Receipt {
                    source,
                    ..client::Receipt::default()
                },
            )
        }
        fn cancel(&mut self, a: mag_render_abi::packet::Admission) -> Result<(), i32> {
            assert_eq!(self.thread, thread::current().id());
            self.prepared.remove(a, a.source)?;
            self.events.lock().unwrap().push("cancel");
            Ok(())
        }
        fn submit(&mut self, _: OwnedInput) -> Reply {
            (-2, client::Receipt::default())
        }
        fn poll(&mut self) -> Reply {
            self.pending.clear();
            self.events.lock().unwrap().push("terminal");
            (
                0,
                client::Receipt {
                    ticket: 99,
                    ..client::Receipt::default()
                },
            )
        }
        fn close(&mut self) -> i32 {
            if self.prepared.len() + self.pending.len() != 0 {
                -4
            } else {
                0
            }
        }
    }
    fn prepared_worker(status: Arc<AtomicI32>, events: Arc<Mutex<Vec<&'static str>>>) -> Worker {
        prepared_worker_with(status, events, reserved_commit())
    }
    fn prepared_worker_with(
        status: Arc<AtomicI32>,
        events: Arc<Mutex<Vec<&'static str>>>,
        reserved: bool,
    ) -> Worker {
        Worker::start_with(
            move || {
                Ok(PreparedDriver {
                    prepared: Default::default(),
                    pending: vec![],
                    status,
                    events,
                    thread: thread::current().id(),
                })
            },
            32,
            1 << 20,
            reserved,
        )
        .unwrap()
    }
    fn fixture_admit() -> AdmitInput {
        AdmitInput {
            worker: 23,
            packet: Box::new(client::scene_packet::packet_tests::fixture()),
            prevalidated: None,
        }
    }
    fn prepare_on(worker: &Worker) -> Result<mag_render_abi::packet::Admission, i32> {
        let (tx, rx) = mpsc::sync_channel(1);
        worker.enqueue(Command::Admit(fixture_admit(), tx)).unwrap();
        rx.recv().unwrap()
    }
    #[test]
    fn fixed_actor_prepares_without_target_rejects_replay_and_preserves_definite_rejection() {
        let status = Arc::new(AtomicI32::new(-4));
        let events = Arc::new(Mutex::new(vec![]));
        let worker = prepared_worker(status.clone(), events.clone());
        let first = prepare_on(&worker).unwrap();
        let second = prepare_on(&worker).unwrap();
        assert_eq!(prepare_on(&worker).err(), Some(-4));
        assert_eq!(worker.call(Command::Close, false).unwrap().0, -4);
        with_input(|input, _, pins| {
            let mut wrong = first.source;
            wrong.publication += 1;
            let target = unsafe { TargetPin::copy(input.target, input.frame) }.unwrap();
            assert_eq!(
                worker
                    .call(|r| Command::Commit(first, wrong, target, r), true)
                    .unwrap()
                    .0,
                -3
            );
            assert_eq!(pins.load(Ordering::SeqCst), 0);
            let target = unsafe { TargetPin::copy(input.target, input.frame) }.unwrap();
            assert_eq!(
                worker
                    .call(|r| Command::Commit(first, first.source, target, r), true)
                    .unwrap()
                    .0,
                -4
            );
            assert_eq!(
                worker.call(|r| Command::Cancel(first, r), false).unwrap().0,
                0
            );
            assert_eq!(
                worker.call(|r| Command::Cancel(first, r), false).unwrap().0,
                -3
            );
            status.store(1, Ordering::SeqCst);
            let target = unsafe { TargetPin::copy(input.target, input.frame) }.unwrap();
            assert_eq!(
                worker
                    .call(|r| Command::Commit(second, second.source, target, r), true)
                    .unwrap()
                    .0,
                1
            );
            assert_eq!(
                worker
                    .call(|r| Command::Cancel(second, r), false)
                    .unwrap()
                    .0,
                -3
            );
            assert_eq!(worker.call(Command::Close, false).unwrap().0, -4);
            worker.call(Command::Poll, false).unwrap();
            assert_eq!(pins.load(Ordering::SeqCst), 0);
        });
        assert_eq!(worker.call(Command::Close, false).unwrap().0, 0);
        worker.join.lock().unwrap().take().unwrap().join().unwrap();
        assert_eq!(
            events
                .lock()
                .unwrap()
                .iter()
                .filter(|e| **e == "cancel")
                .count(),
            1
        );
    }
    unsafe extern "C" fn retain_except_stale(
        user: *mut c_void,
        life: u64,
        id: u64,
        generation: u64,
    ) -> i32 {
        if id == 99 {
            return -3;
        }
        unsafe { retain(user, life, id, generation) }
    }
    #[test]
    fn imported_batch_partial_retain_failure_releases_prior_pins() {
        with_input(|input, _, pins| {
            let mut host = unsafe { *input.target };
            host.image = std::ptr::dangling_mut::<u8>().cast();
            host.retain = Some(retain_except_stale);
            let a = mag_render_abi::packet::ImageBinding {
                image: 0,
                reserved: 0,
                source_key: 99,
                host,
                validate_planes: None,
            };
            let mut b = a;
            b.image = 1;
            b.source_key = 100;
            b.host.resource_id = 99;
            assert!(matches!(
                unsafe { ImportPins::copy([a, b].as_ptr(), 2) },
                Err(-3)
            ));
            assert_eq!(pins.load(Ordering::SeqCst), 0);
            b.host.resource_id = 4;
            let owned = unsafe { ImportPins::copy([a, b].as_ptr(), 2) }.unwrap();
            assert_eq!(pins.load(Ordering::SeqCst), 2);
            drop(owned);
            assert_eq!(pins.load(Ordering::SeqCst), 0);
            b.image = 0;
            assert!(matches!(
                unsafe { ImportPins::copy([a, b].as_ptr(), 2) },
                Err(-1)
            ));
            assert_eq!(pins.load(Ordering::SeqCst), 0);
        });
    }
    #[test]
    fn imported_subgraph_uses_fixed_actor_without_frame_and_preserves_rejected_token() {
        let status = Arc::new(AtomicI32::new(-4));
        let events = Arc::new(Mutex::new(vec![]));
        let worker = prepared_worker(status.clone(), events.clone());
        let token = prepare_on(&worker).unwrap();
        with_input(|input, _, pins| {
            let mut host = unsafe { *input.target };
            host.image = std::ptr::dangling_mut::<u8>().cast();
            let binding = mag_render_abi::packet::ImageBinding {
                image: 0,
                reserved: 0,
                source_key: 99,
                host,
                validate_planes: None,
            };
            let own = || unsafe { ImportPins::copy(&binding, 1) }.unwrap();
            assert_eq!(
                worker
                    .call(|r| Command::Subgraph(token, token.source, own(), r), true)
                    .unwrap()
                    .0,
                -4
            );
            assert_eq!(pins.load(Ordering::SeqCst), 0);
            assert_eq!(worker.call(Command::Close, false).unwrap().0, -4);
            let mut wrong = token.source;
            wrong.publication += 1;
            assert_eq!(
                worker
                    .call(|r| Command::Subgraph(token, wrong, own(), r), true)
                    .unwrap()
                    .0,
                -3
            );
            status.store(1, Ordering::SeqCst);
            assert_eq!(
                worker
                    .call(|r| Command::Subgraph(token, token.source, own(), r), true)
                    .unwrap()
                    .0,
                1
            );
            assert_eq!(
                worker.call(|r| Command::Cancel(token, r), false).unwrap().0,
                -3
            );
            assert_eq!(pins.load(Ordering::SeqCst), 0);
            assert_eq!(worker.call(Command::Close, false).unwrap().0, -4);
            worker.call(Command::Poll, false).unwrap();
        });
        assert_eq!(worker.call(Command::Close, false).unwrap().0, 0);
        worker.join.lock().unwrap().take().unwrap().join().unwrap();
        assert_eq!(
            *events.lock().unwrap(),
            ["admit", "subgraph", "subgraph", "terminal"]
        );
    }
    #[test]
    fn fused_admit_subgraph_keeps_order_cancels_definite_rejection_and_reports_busy() {
        // Both commit modes, each pinned on its own actor (no process state).
        for reserved in [false, true] {
            fused_admit_subgraph_case(reserved);
        }
    }
    fn fused_admit_subgraph_case(reserved: bool) {
        let status = Arc::new(AtomicI32::new(-4));
        let events = Arc::new(Mutex::new(vec![]));
        let worker = prepared_worker_with(status.clone(), events.clone(), reserved);
        with_input(|input, _, pins| {
            let mut host = unsafe { *input.target };
            host.image = std::ptr::dangling_mut::<u8>().cast();
            let binding = mag_render_abi::packet::ImageBinding {
                image: 0,
                reserved: 0,
                source_key: 99,
                host,
                validate_planes: None,
            };
            let own = || unsafe { ImportPins::copy(&binding, 1) }.unwrap();
            let source = client::scene_packet::packet_tests::fixture().source;
            let fused = |pins: ImportPins| {
                worker
                    .call(
                        |r| Command::AdmitSubgraph(fixture_admit(), source, pins, r),
                        true,
                    )
                    .unwrap()
                    .0
            };
            // A definite commit rejection leaves nothing admitted or pinned.
            assert_eq!(fused(own()), -4);
            // The reserved path replies at acknowledgement and cancels the
            // rejected admission right after; wait for the actor to finish.
            wait_idle(&worker);
            assert_eq!(pins.load(Ordering::SeqCst), 0);
            assert_eq!(*events.lock().unwrap(), ["admit", "subgraph", "cancel"]);
            assert_eq!(worker.call(Command::Close, false).unwrap().0, 0);
        });
        // Close succeeded above: the actor retired. Use a fresh one.
        worker.join.lock().unwrap().take().unwrap().join().unwrap();
        let events = Arc::new(Mutex::new(vec![]));
        let worker = prepared_worker_with(status.clone(), events.clone(), reserved);
        status.store(0, Ordering::SeqCst);
        with_input(|input, _, pins| {
            let mut host = unsafe { *input.target };
            host.image = std::ptr::dangling_mut::<u8>().cast();
            let binding = mag_render_abi::packet::ImageBinding {
                image: 0,
                reserved: 0,
                source_key: 99,
                host,
                validate_planes: None,
            };
            let own = || unsafe { ImportPins::copy(&binding, 1) }.unwrap();
            let source = client::scene_packet::packet_tests::fixture().source;
            let fused = |pins: ImportPins| {
                worker
                    .call(
                        |r| Command::AdmitSubgraph(fixture_admit(), source, pins, r),
                        true,
                    )
                    .unwrap()
                    .0
            };
            assert_eq!(fused(own()), 0);
            // Accepted work stays pending until its terminal: close is BUSY.
            assert_eq!(worker.call(Command::Close, false).unwrap().0, -4);
            // Fill the remaining admission slot; the next fused admission is a
            // definite BUSY refusal that never reaches commit.
            let held = prepare_on(&worker).unwrap();
            assert_eq!(fused(own()), -4);
            wait_idle(&worker);
            assert_eq!(pins.load(Ordering::SeqCst), 0);
            assert_eq!(
                worker.call(|r| Command::Cancel(held, r), false).unwrap().0,
                0
            );
            worker.call(Command::Poll, false).unwrap();
        });
        assert_eq!(worker.call(Command::Close, false).unwrap().0, 0);
        worker.join.lock().unwrap().take().unwrap().join().unwrap();
        assert_eq!(
            *events.lock().unwrap(),
            ["admit", "subgraph", "admit", "admit", "cancel", "terminal"]
        );
    }
    #[test]
    fn actor_records_admit_and_commit_durations_before_replying() {
        let status = Arc::new(AtomicI32::new(-4));
        let events = Arc::new(Mutex::new(vec![]));
        let worker = prepared_worker(status, events);
        worker.timing.admit_ns.store(u64::MAX, Ordering::Relaxed);
        worker.timing.reset();
        assert_eq!(worker.timing.read(), (0, 0));
        let token = prepare_on(&worker).unwrap();
        assert_eq!(worker.timing.read().1, 0);
        assert_eq!(
            worker.call(|r| Command::Cancel(token, r), false).unwrap().0,
            0
        );
        assert_eq!(worker.call(Command::Close, false).unwrap().0, 0);
        worker.join.lock().unwrap().take().unwrap().join().unwrap();
    }
    /// Acknowledges first, then keeps "encoding" until released.
    struct ReservingDriver {
        release: mpsc::Receiver<()>,
        events: Arc<Mutex<Vec<&'static str>>>,
    }
    impl Driver for ReservingDriver {
        fn commit_subgraph_reserved(
            &mut self,
            a: mag_render_abi::packet::Admission,
            source: client::Source,
            _bindings: ImportPins,
            acknowledge: &mut dyn FnMut(Reply),
        ) {
            assert_eq!(a.source.publication, source.publication);
            self.events.lock().unwrap().push("reserved");
            acknowledge((
                0,
                client::Receipt {
                    native_generation: 3,
                    native_ticket: 4,
                    ..client::Receipt::default()
                },
            ));
            self.release.recv().unwrap();
            self.events.lock().unwrap().push("encoded");
        }
        fn submit(&mut self, _: OwnedInput) -> Reply {
            (-2, client::Receipt::default())
        }
        fn poll(&mut self) -> Reply {
            self.events.lock().unwrap().push("poll");
            (-4, client::Receipt::default())
        }
        fn close(&mut self) -> i32 {
            0
        }
    }
    #[test]
    fn reserved_commit_acknowledges_before_the_actor_finishes_and_keeps_order() {
        let (release, wait) = mpsc::channel();
        let events = Arc::new(Mutex::new(vec![]));
        let recorded = events.clone();
        let worker = Worker::start_with(
            move || {
                Ok(ReservingDriver {
                    release: wait,
                    events: recorded,
                })
            },
            32,
            1 << 20,
            true,
        )
        .unwrap();
        with_input(|input, _, pins| {
            let mut host = unsafe { *input.target };
            host.image = std::ptr::dangling_mut::<u8>().cast();
            let binding = mag_render_abi::packet::ImageBinding {
                image: 0,
                reserved: 0,
                source_key: 99,
                host,
                validate_planes: None,
            };
            let pinned = unsafe { ImportPins::copy(&binding, 1) }.unwrap();
            let admission = mag_render_abi::packet::Admission {
                source: client::scene_packet::packet_tests::fixture().source,
                ..Default::default()
            };
            let (status, receipt) = worker
                .call(
                    |r| Command::Subgraph(admission, admission.source, pinned, r),
                    true,
                )
                .unwrap();
            // The caller is released while the actor is still encoding.
            assert_eq!((status, receipt.native_ticket), (0, 4));
            assert_eq!(*events.lock().unwrap(), ["reserved"]);
            // The next command waits behind the remaining encode, in order.
            let (send, reply) = mpsc::sync_channel(1);
            worker.enqueue(Command::Poll(send)).unwrap();
            release.send(()).unwrap();
            assert_eq!(reply.recv().unwrap().0, -4);
            assert_eq!(*events.lock().unwrap(), ["reserved", "encoded", "poll"]);
            assert_eq!(pins.load(Ordering::SeqCst), 0);
        });
        assert_eq!(worker.call(Command::Close, false).unwrap().0, 0);
        worker.join.lock().unwrap().take().unwrap().join().unwrap();
    }
    /// A session stand-in: terminal receipts go to the shared outbox when the
    /// "encode" after a reserved acknowledgement finishes, and admission is
    /// refused while two receipts are still held for the host (cap of two).
    struct OutboxDriver {
        release: mpsc::Receiver<()>,
        outbox: client::Outbox,
        next: u64,
    }
    impl Driver for OutboxDriver {
        fn admit(
            &mut self,
            _: u64,
            packet: client::scene_packet::ScenePacket,
            _: Option<(usize, u64)>,
        ) -> Result<mag_render_abi::packet::Admission, i32> {
            if self.outbox.lock().unwrap().len() >= 2 {
                return Err(-4);
            }
            Ok(mag_render_abi::packet::Admission {
                source: packet.source,
                ..Default::default()
            })
        }
        fn cancel(&mut self, _: mag_render_abi::packet::Admission) -> Result<(), i32> {
            Ok(())
        }
        fn commit_subgraph_reserved(
            &mut self,
            _: mag_render_abi::packet::Admission,
            _: client::Source,
            _: ImportPins,
            acknowledge: &mut dyn FnMut(Reply),
        ) {
            self.next += 1;
            acknowledge((0, client::Receipt::default()));
            self.release.recv().unwrap();
            self.outbox.lock().unwrap().push_back(client::Receipt {
                ticket: self.next,
                ..client::Receipt::default()
            });
        }
        fn submit(&mut self, _: OwnedInput) -> Reply {
            (-2, client::Receipt::default())
        }
        fn poll(&mut self) -> Reply {
            match self.outbox.lock().unwrap().pop_front() {
                Some(r) => (0, r),
                None => (-4, client::Receipt::default()),
            }
        }
        fn close(&mut self) -> i32 {
            0
        }
    }
    #[test]
    fn nowait_host_poll_drains_published_receipts_while_the_actor_is_busy() {
        let (release, wait) = mpsc::channel();
        let outbox = client::Outbox::default();
        let actor_outbox = outbox.clone();
        let mut worker = Worker::start_with(
            move || {
                Ok(OutboxDriver {
                    release: wait,
                    outbox: actor_outbox,
                    next: 0,
                })
            },
            32,
            1 << 20,
            true,
        )
        .unwrap();
        worker.outbox = Some(outbox.clone());
        with_input(|input, _, _| {
            let mut host = unsafe { *input.target };
            host.image = std::ptr::dangling_mut::<u8>().cast();
            let binding = mag_render_abi::packet::ImageBinding {
                image: 0,
                reserved: 0,
                source_key: 99,
                host,
                validate_planes: None,
            };
            let source = client::scene_packet::packet_tests::fixture().source;
            let commit = || {
                let pins = unsafe { ImportPins::copy(&binding, 1) }.unwrap();
                worker
                    .call(
                        |r| Command::AdmitSubgraph(fixture_admit(), source, pins, r),
                        true,
                    )
                    .unwrap()
                    .0
            };
            // Frame 1: acknowledged, then terminal and published.
            assert_eq!(commit(), 0);
            release.send(()).unwrap();
            wait_idle(&worker);
            // Frame 2: acknowledged; the actor is still encoding it.
            assert_eq!(commit(), 0);
            assert_ne!(worker.outstanding.load(Ordering::SeqCst), 0);
            // The busy actor does not hide frame 1's published receipt.
            assert!(matches!(worker.host_poll(true), Ok(Some((0, r))) if r.ticket == 1));
            // Nothing else published yet: a busy skip, as before.
            assert!(matches!(worker.host_poll(true), Ok(None)));
            release.send(()).unwrap();
            wait_idle(&worker);
            // Frame 3 fills the host-held receipts (2 and 3): refused until
            // the host drains one, then admitted again.
            assert_eq!(commit(), 0);
            release.send(()).unwrap();
            wait_idle(&worker);
            assert_eq!(prepare_on(&worker).err(), Some(-4));
            {
                // Another caller is mid-call on this worker (as the FIFO is
                // during a Main admission): published receipts are still taken.
                let _caller = worker.serial.lock().unwrap();
                assert!(matches!(worker.take_published(), Ok(Some(r)) if r.ticket == 2));
                assert!(matches!(worker.poll_nowait(), Err(-4)));
            }
            let admitted = prepare_on(&worker).unwrap();
            assert_eq!(
                worker
                    .call(|r| Command::Cancel(admitted, r), false)
                    .unwrap()
                    .0,
                0
            );
            assert!(matches!(worker.host_poll(true), Ok(Some((0, r))) if r.ticket == 3));
        });
        assert_eq!(worker.call(Command::Close, false).unwrap().0, 0);
        worker.join.lock().unwrap().take().unwrap().join().unwrap();
    }
    /// The actor replies before it finishes the iteration, so the count reaches
    /// zero a moment after the reply. Bounded: a leaked count fails the test.
    fn wait_idle(worker: &Worker) {
        let start = Instant::now();
        while worker.outstanding.load(Ordering::SeqCst) != 0 {
            assert!(start.elapsed().as_secs() < 5, "actor never became idle");
            thread::yield_now();
        }
    }
    #[test]
    fn nowait_poll_skips_a_busy_actor_and_polls_once_it_is_idle() {
        let (release, wait) = mpsc::channel();
        let events = Arc::new(Mutex::new(vec![]));
        let recorded = events.clone();
        let worker = Worker::start_with(
            move || {
                Ok(ReservingDriver {
                    release: wait,
                    events: recorded,
                })
            },
            32,
            1 << 20,
            true,
        )
        .unwrap();
        // Idle actor: identical to a Poll call, and a poll leaves it idle
        // before replying, so back-to-back nowait polls both run.
        assert!(matches!(worker.poll_nowait(), Ok(Some((-4, _)))));
        assert!(matches!(worker.poll_nowait(), Ok(Some((-4, _)))));
        assert_eq!(*events.lock().unwrap(), ["poll", "poll"]);
        with_input(|input, _, pins| {
            let mut host = unsafe { *input.target };
            host.image = std::ptr::dangling_mut::<u8>().cast();
            let binding = mag_render_abi::packet::ImageBinding {
                image: 0,
                reserved: 0,
                source_key: 99,
                host,
                validate_planes: None,
            };
            let pinned = unsafe { ImportPins::copy(&binding, 1) }.unwrap();
            let admission = mag_render_abi::packet::Admission {
                source: client::scene_packet::packet_tests::fixture().source,
                ..Default::default()
            };
            let (status, _) = worker
                .call(
                    |r| Command::Subgraph(admission, admission.source, pinned, r),
                    true,
                )
                .unwrap();
            assert_eq!(status, 0);
            // The actor is still encoding: the nowait poll returns at once and
            // enqueues nothing (a blocking poll would wait for `release`).
            for _ in 0..3 {
                assert!(matches!(worker.poll_nowait(), Ok(None)));
            }
            assert_eq!(*events.lock().unwrap(), ["poll", "poll", "reserved"]);
            // A concurrent caller still sees the serial BUSY status.
            {
                let guard = worker.serial.lock().unwrap();
                assert!(matches!(worker.poll_nowait(), Err(-4)));
                drop(guard);
            }
            release.send(()).unwrap();
            // An ordinary poll queues behind the encode, then the actor is idle.
            assert_eq!(worker.call(Command::Poll, false).unwrap().0, -4);
            wait_idle(&worker);
            assert!(matches!(worker.poll_nowait(), Ok(Some((-4, _)))));
            assert_eq!(
                *events.lock().unwrap(),
                ["poll", "poll", "reserved", "encoded", "poll", "poll"]
            );
            assert_eq!(pins.load(Ordering::SeqCst), 0);
        });
        assert_eq!(worker.call(Command::Close, false).unwrap().0, 0);
        worker.join.lock().unwrap().take().unwrap().join().unwrap();
    }
    #[test]
    fn outstanding_count_returns_to_idle_after_lost_replies_and_refusals() {
        let events = Arc::new(Mutex::new(vec![]));
        let worker = prepared_worker(Arc::new(AtomicI32::new(0)), events.clone());
        let (tx, rx) = mpsc::sync_channel(1);
        drop(rx);
        worker.enqueue(Command::Admit(fixture_admit(), tx)).unwrap();
        worker.call(Command::Poll, false).unwrap();
        wait_idle(&worker);
        assert!(matches!(worker.poll_nowait(), Ok(Some(_))));
        wait_idle(&worker);
        assert_eq!(worker.call(Command::Close, false).unwrap().0, 0);
        worker.join.lock().unwrap().take().unwrap().join().unwrap();
        // Saturating release: a stray decrement can never wrap the count.
        let count = AtomicUsize::new(0);
        release_outstanding(&count);
        assert_eq!(count.load(Ordering::SeqCst), 0);
    }
    #[test]
    fn lost_prepare_reply_cancels_only_uncommitted_state_on_actor() {
        let events = Arc::new(Mutex::new(vec![]));
        let worker = prepared_worker(Arc::new(AtomicI32::new(0)), events.clone());
        let (tx, rx) = mpsc::sync_channel(1);
        drop(rx);
        worker.enqueue(Command::Admit(fixture_admit(), tx)).unwrap();
        assert_eq!(worker.call(Command::Close, false).unwrap().0, 0);
        worker.join.lock().unwrap().take().unwrap().join().unwrap();
        assert_eq!(*events.lock().unwrap(), ["admit", "cancel"]);
    }
}
