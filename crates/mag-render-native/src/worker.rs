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
};
const MAX_WORKERS: usize = 2;
const MAX_BYTES: u64 = 64 * 1024 * 1024;
const MAX_DRAWS: u32 = crate::batch_device::scene_device::MAX_SCENE_DRAWS as u32;

struct DrawCopy {
    item: u64,
    partition: u64,
    obligations: u64,
    vertex_start: usize,
    vertex_count: usize,
    index_start: usize,
    index_count: usize,
}
struct OwnerPin(HostOwner);
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
enum Command {
    Scene(Box<OwnedScene>, SyncSender<Reply>),
    Submit(Box<OwnedInput>, SyncSender<Reply>),
    Poll(SyncSender<Reply>),
    Close(SyncSender<Reply>),
}
struct Worker {
    commands: SyncSender<Command>,
    serial: Mutex<()>,
    join: Mutex<Option<JoinHandle<()>>>,
    max_draws: u32,
    max_bytes: u64,
}
impl Worker {
    fn start<D: Driver + 'static, F: FnOnce() -> Result<D, i32> + Send + 'static>(
        factory: F,
        max_draws: u32,
        max_bytes: u64,
    ) -> Result<Self, i32> {
        let (commands, receive) = mpsc::sync_channel(1);
        let (started, ready) = mpsc::sync_channel(1);
        let join = thread::Builder::new()
            .name("mag-render-client".into())
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
                    match command {
                        Command::Scene(input, reply) => {
                            let _ = reply.send(driver.scene(*input));
                        }
                        Command::Submit(input, reply) => {
                            let _ = reply.send(driver.submit(*input));
                        }
                        Command::Poll(reply) => {
                            let _ = reply.send(driver.poll());
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
                serial: Mutex::new(()),
                join: Mutex::new(Some(join)),
                max_draws,
                max_bytes,
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
        self.commands.send(make(send)).map_err(|_| -6)?;
        reply
            .recv()
            .map_err(|_| if uncertain_on_lost_ack { 1 } else { -6 })
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
            let pin = OwnerPin(host);
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
/// Non-waiting terminal check on the same fixed native thread. Physical present
/// events remain separate SDK host observations; this API does not poll them.
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
            let (status, receipt) = get(id)?.call(Command::Poll, false)?;
            unsafe { *out = receipt };
            Ok(status)
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
            serial: Mutex::new(()),
            join: Mutex::new(Some(join)),
            max_draws: 4,
            max_bytes: 1024,
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
}
