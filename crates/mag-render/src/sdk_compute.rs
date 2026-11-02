//! Opt-in source-bound SDK adapter; no native linker references or discovery.
use std::ffi::{c_char, c_void};
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct Caps {
    pub struct_size: u32,
    pub abi_version: u32,
    pub device_generation: u64,
    pub compute_supported: u32,
    pub draws_supported: u32,
    pub max_pending: u32,
    pub max_bindings: u32,
    pub runtime_sizes_slot: u32,
    pub reserved: u32,
}
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct Buffer {
    pub device_generation: u64,
    pub id: u64,
    pub version: u64,
}
#[repr(C)]
#[derive(Clone, Copy)]
pub struct BufferDesc {
    pub struct_size: u32,
    pub abi_version: u32,
    pub byte_length: u64,
    pub initial_bytes: *const c_void,
}
pub use mag_render_abi::BindingLayout;
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct Pipeline {
    pub device_generation: u64,
    pub id: u64,
}
#[repr(C)]
#[derive(Clone, Copy)]
pub struct PipelineDesc {
    pub struct_size: u32,
    pub abi_version: u32,
    pub source_bytes: *const c_void,
    pub source_length: u64,
    pub entry_point: *const c_char,
    pub entry_point_length: u32,
    pub binding_count: u32,
    pub bindings: *const BindingLayout,
    pub threads_per_group: [u32; 3],
    pub shader_language: u32,
}
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct Binding {
    pub slot: u32,
    pub reserved: u32,
    pub buffer: Buffer,
    pub offset: u64,
    pub byte_length: u64,
}
#[repr(C)]
#[derive(Clone, Copy)]
pub struct Frame {
    pub struct_size: u32,
    pub abi_version: u32,
    pub pipeline: Pipeline,
    pub bindings: *const Binding,
    pub binding_count: u32,
    pub groups: [u32; 3],
}
#[repr(C)]
#[derive(Clone, Copy, Default, PartialEq, Eq)]
pub struct Ticket {
    pub device_generation: u64,
    pub submission_id: u64,
}
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct Event {
    pub struct_size: u32,
    pub abi_version: u32,
    pub ticket: Ticket,
    pub succeeded: u32,
    pub no_further_gpu_access: u32,
    pub native_status: u32,
    pub reserved: u32,
}
#[repr(C)]
#[derive(Clone, Copy)]
pub struct SdkApi {
    pub abi_version: u32,
    pub struct_size: u32,
    pub device: *mut c_void,
    pub layout: [u32; 52],
    pub caps: Option<unsafe extern "C" fn(*mut c_void, *mut Caps) -> i32>,
    pub buffer_create:
        Option<unsafe extern "C" fn(*mut c_void, *const BufferDesc, *mut Buffer) -> i32>,
    pub buffer_release: Option<unsafe extern "C" fn(*mut c_void, Buffer) -> i32>,
    pub buffer_read:
        Option<unsafe extern "C" fn(*mut c_void, Buffer, u64, *mut c_void, u64) -> i32>,
    pub pipeline_create:
        Option<unsafe extern "C" fn(*mut c_void, *const PipelineDesc, *mut Pipeline) -> i32>,
    pub pipeline_release: Option<unsafe extern "C" fn(*mut c_void, Pipeline) -> i32>,
    pub submit: Option<unsafe extern "C" fn(*mut c_void, *const Frame, *mut Ticket) -> i32>,
    pub poll: Option<unsafe extern "C" fn(*mut c_void, *mut Event, u32, *mut u32) -> i32>,
}
#[repr(C)]
#[derive(Default)]
pub struct ProbeReport {
    pub abi_version: u32,
    pub struct_size: u32,
    pub status: i32,
    pub submitted: u32,
    pub completed: u32,
    pub pending: u32,
    pub retained_before_terminal: u64,
    pub retained_after_terminal: u64,
    pub upload_bytes: u64,
}

use crate::{
    backend::{
        self, BackendEvent, DevicePollError, RenderDevice, SubmissionTicket, SubmitDisposition,
        TerminalStatus,
    },
    resources::{ResourceCacheKey, ResourceLease},
};
use std::{
    collections::{BTreeMap, BTreeSet, VecDeque},
    marker::PhantomData,
    mem::{align_of, offset_of, size_of},
    rc::Rc,
    thread::{self, ThreadId},
};
const BAD: u32 = 0x10001;
const ABI_LAYOUT: [u32; 52] = [
    size_of::<Caps>() as u32,
    align_of::<Caps>() as u32,
    size_of::<BufferDesc>() as u32,
    align_of::<BufferDesc>() as u32,
    size_of::<Buffer>() as u32,
    align_of::<Buffer>() as u32,
    size_of::<BindingLayout>() as u32,
    align_of::<BindingLayout>() as u32,
    size_of::<PipelineDesc>() as u32,
    align_of::<PipelineDesc>() as u32,
    size_of::<Pipeline>() as u32,
    align_of::<Pipeline>() as u32,
    size_of::<Binding>() as u32,
    align_of::<Binding>() as u32,
    size_of::<Frame>() as u32,
    align_of::<Frame>() as u32,
    size_of::<Ticket>() as u32,
    align_of::<Ticket>() as u32,
    size_of::<Event>() as u32,
    align_of::<Event>() as u32,
    offset_of!(Caps, device_generation) as u32,
    offset_of!(Caps, max_pending) as u32,
    offset_of!(BufferDesc, byte_length) as u32,
    offset_of!(BufferDesc, initial_bytes) as u32,
    offset_of!(Buffer, device_generation) as u32,
    offset_of!(Buffer, id) as u32,
    offset_of!(Buffer, version) as u32,
    offset_of!(BindingLayout, slot) as u32,
    offset_of!(BindingLayout, minimum_bytes) as u32,
    offset_of!(BindingLayout, runtime_array_stride) as u32,
    offset_of!(BindingLayout, runtime_size_index) as u32,
    offset_of!(BindingLayout, alignment_bytes) as u32,
    offset_of!(PipelineDesc, source_bytes) as u32,
    offset_of!(PipelineDesc, source_length) as u32,
    offset_of!(PipelineDesc, entry_point) as u32,
    offset_of!(PipelineDesc, bindings) as u32,
    offset_of!(PipelineDesc, threads_per_group) as u32,
    offset_of!(PipelineDesc, shader_language) as u32,
    offset_of!(Binding, buffer) as u32,
    offset_of!(Binding, offset) as u32,
    offset_of!(Binding, byte_length) as u32,
    offset_of!(Frame, pipeline) as u32,
    offset_of!(Frame, bindings) as u32,
    offset_of!(Frame, binding_count) as u32,
    offset_of!(Frame, groups) as u32,
    offset_of!(Event, ticket) as u32,
    offset_of!(Event, succeeded) as u32,
    offset_of!(Event, no_further_gpu_access) as u32,
    offset_of!(Event, native_status) as u32,
    offset_of!(BindingLayout, binding_kind) as u32,
    0, // RSX_FRONTEND_STORAGE
    1, // RSX_FRONTEND_UNIFORM
];
#[derive(Clone, Copy)]
pub struct NativeLimits {
    pub max_entries: usize,
    pub max_bytes: usize,
    pub max_pipelines: usize,
}
#[derive(Clone, Copy)]
pub struct ComputeBinding {
    pub slot: u32,
    pub lease_index: usize,
    pub offset: u64,
    pub byte_length: u64,
}
pub struct ComputeCommands {
    pub pipeline: Pipeline,
    pub bindings: Vec<ComputeBinding>,
    pub groups: [u32; 3],
}
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
struct NativeKey {
    generation: u64,
    resource: ResourceCacheKey,
}
struct PipelineDefinition {
    handle: Pipeline,
    layouts: Vec<BindingLayout>,
}
struct Cached {
    gpu_dirty: bool,
    buffer: Buffer,
    length: usize,
    last_used: u64,
}
struct Inflight {
    native: Ticket,
    rust: SubmissionTicket,
    keys: Vec<NativeKey>,
}
/// Borrowed, serialized SDK owner. !Send/!Sync; it never creates/closes a device.
/// Native cache limits include all entries, including versions used in flight.
///
/// ```compile_fail
/// use mag_render::sdk_compute::SdkComputeDevice;
/// fn require_send<T: Send>() {}
/// require_send::<SdkComputeDevice<'static>>();
/// ```
/// ```compile_fail
/// use mag_render::sdk_compute::SdkComputeDevice;
/// fn require_sync<T: Sync>() {}
/// require_sync::<SdkComputeDevice<'static>>();
/// ```
pub struct SdkComputeDevice<'owner> {
    api: SdkApi,
    caps: Caps,
    owner: ThreadId,
    limits: NativeLimits,
    cache: BTreeMap<NativeKey, Cached>,
    pipelines: Vec<PipelineDefinition>,
    inflight: Vec<Inflight>,
    ready: VecDeque<BackendEvent>,
    clock: u64,
    uploaded: u64,
    cache_hits: u64,
    _owner: PhantomData<&'owner mut c_void>,
    _thread: PhantomData<Rc<()>>,
}
impl<'owner> SdkComputeDevice<'owner> {
    /// # Safety
    /// The caller guarantees a genuine ABI table, valid function pointers and a
    /// live SDK owner for this lifetime, with no concurrent/external SDK calls.
    /// The owner must remain alive through actual terminal polling; errors do
    /// not permit teardown. Rust does not take ownership of the borrowed device.
    pub unsafe fn from_api(api: SdkApi, limits: NativeLimits) -> Result<Self, i32> {
        if api.abi_version != 1
            || api.struct_size as usize != size_of::<SdkApi>()
            || api.layout != ABI_LAYOUT
            || api.device.is_null()
            || api.caps.is_none()
            || api.buffer_create.is_none()
            || api.buffer_release.is_none()
            || api.buffer_read.is_none()
            || api.pipeline_create.is_none()
            || api.pipeline_release.is_none()
            || api.submit.is_none()
            || api.poll.is_none()
            || limits.max_entries == 0
            || limits.max_bytes == 0
            || limits.max_pipelines == 0
        {
            return Err(-1);
        }
        let mut caps = Caps {
            struct_size: size_of::<Caps>() as u32,
            abi_version: 1,
            ..Caps::default()
        };
        let status = unsafe { api.caps.unwrap()(api.device, &mut caps) };
        if status != 0 {
            return Err(status);
        }
        if caps.struct_size as usize != size_of::<Caps>()
            || caps.abi_version != 1
            || caps.reserved != 0
            || caps.device_generation == 0
            || caps.compute_supported != 1
            || caps.max_pending != 2
            || caps.max_bindings < 3
            || caps.max_bindings > 16
        {
            return Err(-2);
        }
        Ok(Self {
            api,
            caps,
            owner: thread::current().id(),
            limits,
            cache: BTreeMap::new(),
            pipelines: Vec::new(),
            inflight: Vec::new(),
            ready: VecDeque::new(),
            clock: 0,
            uploaded: 0,
            cache_hits: 0,
            _owner: PhantomData,
            _thread: PhantomData,
        })
    }
    fn same_thread(&self) -> bool {
        self.owner == thread::current().id()
    }
    fn key(&self, resource: ResourceCacheKey) -> NativeKey {
        NativeKey {
            generation: self.caps.device_generation,
            resource,
        }
    }
    pub fn create_pipeline(
        &mut self,
        source: &[u8],
        entry: &str,
        layouts: &[BindingLayout],
        threads: [u32; 3],
        language: u32,
    ) -> Result<Pipeline, i32> {
        if !self.same_thread()
            || source.is_empty()
            || source.len() > 1024 * 1024
            || entry.is_empty()
            || entry.len() > u32::MAX as usize
            || layouts.len() > 16
            || self.pipelines.len() >= self.limits.max_pipelines
        {
            return Err(-1);
        }
        let desc = PipelineDesc {
            struct_size: size_of::<PipelineDesc>() as u32,
            abi_version: 1,
            source_bytes: source.as_ptr().cast(),
            source_length: source.len() as u64,
            entry_point: entry.as_ptr().cast(),
            entry_point_length: entry.len() as u32,
            binding_count: layouts.len() as u32,
            bindings: layouts.as_ptr(),
            threads_per_group: threads,
            shader_language: language,
        };
        let mut pipeline = Pipeline::default();
        let status =
            unsafe { self.api.pipeline_create.unwrap()(self.api.device, &desc, &mut pipeline) };
        if status != 0 {
            return Err(status);
        }
        if pipeline.id == 0 || pipeline.device_generation != self.caps.device_generation {
            return Err(-3);
        }
        self.pipelines.push(PipelineDefinition {
            handle: pipeline,
            layouts: layouts.to_vec(),
        });
        Ok(pipeline)
    }
    fn prepare(&mut self, leases: &[ResourceLease]) -> Result<Vec<NativeKey>, i32> {
        let requested: BTreeSet<_> = leases.iter().map(|r| self.key(r.cache_key())).collect();
        // A ResourceCacheKey denotes immutable CPU bytes. GPU writes dirty the
        // native copy, not that CPU version. Preserve completed readback until
        // rebinding, then reseed from CPU; never reseed a buffer still in flight.
        let reseed: Vec<_> = requested
            .iter()
            .filter(|key| self.cache.get(key).is_some_and(|c| c.gpu_dirty))
            .copied()
            .collect();
        for key in &reseed {
            if self.inflight.iter().any(|t| t.keys.contains(key)) {
                return Err(-4);
            }
        }
        for key in reseed {
            let status = unsafe {
                self.api.buffer_release.unwrap()(self.api.device, self.cache[&key].buffer)
            };
            if status != 0 {
                return Err(status);
            }
            self.cache.remove(&key);
        }
        let mut missing = BTreeMap::new();
        for lease in leases {
            let key = self.key(lease.cache_key());
            if !self.cache.contains_key(&key) {
                missing.entry(key).or_insert(lease);
            }
        }
        let needed = missing
            .values()
            .try_fold(0usize, |sum, l| sum.checked_add(l.bytes().len()))
            .ok_or(-5)?;
        if missing.len() > self.limits.max_entries || needed > self.limits.max_bytes {
            return Err(-5);
        }
        loop {
            let total = self.cache.values().map(|v| v.length).sum::<usize>();
            if self
                .cache
                .len()
                .checked_add(missing.len())
                .is_some_and(|n| n <= self.limits.max_entries)
                && total
                    .checked_add(needed)
                    .is_some_and(|n| n <= self.limits.max_bytes)
            {
                break;
            }
            let candidate = self
                .cache
                .iter()
                .filter(|(key, _)| {
                    !requested.contains(key) && !self.inflight.iter().any(|t| t.keys.contains(key))
                })
                .min_by_key(|(_, v)| v.last_used)
                .map(|(key, _)| *key)
                .ok_or(-4)?;
            let buffer = self.cache[&candidate].buffer;
            let status = unsafe { self.api.buffer_release.unwrap()(self.api.device, buffer) };
            if status != 0 {
                return Err(status);
            }
            self.cache.remove(&candidate);
        }
        self.clock = self.clock.checked_add(1).ok_or(-5)?;
        for (key, lease) in missing {
            let desc = BufferDesc {
                struct_size: size_of::<BufferDesc>() as u32,
                abi_version: 1,
                byte_length: lease.bytes().len() as u64,
                initial_bytes: lease.bytes().as_ptr().cast(),
            };
            let uploaded = self
                .uploaded
                .checked_add(lease.bytes().len() as u64)
                .ok_or(-5)?;
            let mut buffer = Buffer::default();
            let status =
                unsafe { self.api.buffer_create.unwrap()(self.api.device, &desc, &mut buffer) };
            if status != 0 {
                return Err(status);
            }
            if buffer.device_generation != self.caps.device_generation
                || buffer.id == 0
                || buffer.version == 0
            {
                return Err(-3);
            }
            self.uploaded = uploaded;
            self.cache.insert(
                key,
                Cached {
                    gpu_dirty: false,
                    buffer,
                    length: lease.bytes().len(),
                    last_used: self.clock,
                },
            );
        }
        let mut keys = Vec::new();
        for lease in leases {
            let key = self.key(lease.cache_key());
            let cached = self.cache.get_mut(&key).ok_or(-3)?;
            if cached.last_used != self.clock {
                self.cache_hits += 1;
            }
            cached.last_used = self.clock;
            keys.push(key);
        }
        Ok(keys)
    }
    /// Diagnostic readback only: SDK returns BUSY until all uses are terminal
    /// and polled; normal compute submission never calls this function.
    pub fn readback(
        &self,
        key: ResourceCacheKey,
        offset: u64,
        length: usize,
    ) -> Result<Vec<u8>, i32> {
        if !self.same_thread() || length == 0 || length > self.limits.max_bytes {
            return Err(-1);
        }
        let cached = self.cache.get(&self.key(key)).ok_or(-3)?;
        if offset
            .checked_add(length as u64)
            .is_none_or(|end| end > cached.length as u64)
        {
            return Err(-1);
        }
        let mut output = Vec::new();
        output.try_reserve_exact(length).map_err(|_| -5)?;
        output.resize(length, 0);
        let status = unsafe {
            self.api.buffer_read.unwrap()(
                self.api.device,
                cached.buffer,
                offset,
                output.as_mut_ptr().cast(),
                length as u64,
            )
        };
        if status == 0 { Ok(output) } else { Err(status) }
    }
    pub fn upload_bytes(&self) -> u64 {
        self.uploaded
    }
    pub fn cached_entries(&self) -> usize {
        self.cache.len()
    }
    pub fn cached_bytes(&self) -> usize {
        self.cache.values().map(|v| v.length).sum()
    }
}
impl RenderDevice for SdkComputeDevice<'_> {
    type Commands = ComputeCommands;
    fn submit(
        &mut self,
        rust: SubmissionTicket,
        commands: &ComputeCommands,
        resources: &[ResourceLease],
    ) -> SubmitDisposition {
        let reject = |code: i32| SubmitDisposition::DefinitelyRejected {
            code: code.unsigned_abs(),
        };
        if !self.same_thread()
            || self.inflight.len() + self.ready.len() >= 2
            || commands.bindings.is_empty()
            || commands.bindings.len() > 16
            || !self.pipelines.iter().any(|p| {
                p.handle.id == commands.pipeline.id
                    && p.handle.device_generation == commands.pipeline.device_generation
            })
        {
            return reject(-1);
        }
        for b in &commands.bindings {
            let Some(lease) = resources.get(b.lease_index) else {
                return reject(-1);
            };
            if b.byte_length == 0
                || b.offset
                    .checked_add(b.byte_length)
                    .is_none_or(|end| end > lease.bytes().len() as u64)
            {
                return reject(-1);
            }
        }
        let definition = self
            .pipelines
            .iter()
            .find(|p| {
                p.handle.id == commands.pipeline.id
                    && p.handle.device_generation == commands.pipeline.device_generation
            })
            .expect("validated pipeline");
        if definition.layouts.len() != commands.bindings.len() {
            return reject(-1);
        }
        let mut slots = BTreeSet::new();
        let mut reads = BTreeSet::new();
        let mut writes = BTreeSet::new();
        for binding in &commands.bindings {
            if !slots.insert(binding.slot) {
                return reject(-1);
            }
            let Some(layout) = definition.layouts.iter().find(|l| l.slot == binding.slot) else {
                return reject(-1);
            };
            let key = self.key(resources[binding.lease_index].cache_key());
            if layout.access == 2 {
                if reads.contains(&key) || !writes.insert(key) {
                    return reject(-2);
                }
            } else if layout.access == 1 {
                if writes.contains(&key) {
                    return reject(-2);
                }
                reads.insert(key);
            } else {
                return reject(-1);
            }
        }
        let keys = match self.prepare(resources) {
            Ok(keys) => keys,
            Err(code) => return reject(code),
        };
        let bindings: Vec<_> = commands
            .bindings
            .iter()
            .map(|b| Binding {
                slot: b.slot,
                reserved: 0,
                buffer: self.cache[&keys[b.lease_index]].buffer,
                offset: b.offset,
                byte_length: b.byte_length,
            })
            .collect();
        let frame = Frame {
            struct_size: size_of::<Frame>() as u32,
            abi_version: 1,
            pipeline: commands.pipeline,
            bindings: bindings.as_ptr(),
            binding_count: bindings.len() as u32,
            groups: commands.groups,
        };
        let mut native = Ticket::default();
        let status = unsafe { self.api.submit.unwrap()(self.api.device, &frame, &mut native) };
        if status < 0 {
            return reject(status);
        }
        // Even malformed positive-status tickets represent possibly committed
        // work: preserve ownership and never downgrade to definite rejection.
        for key in writes {
            self.cache
                .get_mut(&key)
                .expect("uploaded binding")
                .gpu_dirty = true;
        }
        self.inflight.push(Inflight { native, rust, keys });
        if status == 0
            && native.device_generation == self.caps.device_generation
            && native.submission_id != 0
        {
            SubmitDisposition::Accepted
        } else {
            SubmitDisposition::Indeterminate { code: BAD }
        }
    }
    fn poll(&mut self, capacity: usize) -> Result<Vec<BackendEvent>, DevicePollError> {
        let bad = || DevicePollError {
            code: BAD,
            device_lost: true,
        };
        if !self.same_thread() || capacity == 0 {
            return Err(bad());
        }
        let capacity = capacity.min(64);
        // A valid once-only SDK terminal must survive another bad record in
        // its native batch, even if the next native poll itself fails.
        if !self.ready.is_empty() {
            let count = capacity.min(self.ready.len());
            return Ok(self.ready.drain(..count).collect());
        }
        let mut events = vec![Event::default(); capacity];
        for event in &mut events {
            event.struct_size = size_of::<Event>() as u32;
            event.abi_version = 1;
        }
        let mut count = 0;
        let status = unsafe {
            self.api.poll.unwrap()(
                self.api.device,
                events.as_mut_ptr(),
                capacity as u32,
                &mut count,
            )
        };
        if status != 0 {
            return Err(DevicePollError {
                code: status.unsigned_abs(),
                device_lost: status == -6,
            });
        }
        if count as usize > capacity {
            return Err(bad());
        }
        let mut fault = false;
        for event in events.into_iter().take(count as usize) {
            if event.struct_size as usize != size_of::<Event>()
                || event.abi_version != 1
                || event.no_further_gpu_access != 1
                || event.succeeded > 1
                || event.reserved != 0
            {
                fault = true;
                continue;
            }
            let Some(index) = self.inflight.iter().position(|p| p.native == event.ticket) else {
                fault = true;
                continue;
            };
            if self.ready.len() >= 2 {
                fault = true;
                continue;
            }
            let pending = self.inflight.remove(index);
            self.ready.push_back(BackendEvent::Terminal {
                ticket: pending.rust,
                status: if event.succeeded == 1 {
                    TerminalStatus::Succeeded
                } else {
                    TerminalStatus::Failed {
                        code: event.native_status,
                    }
                },
            });
        }
        if fault {
            return Err(bad());
        }
        let count = capacity.min(self.ready.len());
        Ok(self.ready.drain(..count).collect())
    }
}
impl Drop for SdkComputeDevice<'_> {
    fn drop(&mut self) {
        // RenderBackend never drops this owner with unresolved submissions.
        // Public releases do not cancel SDK-owned submission references.
        if self.same_thread() {
            for cached in self.cache.values() {
                unsafe {
                    self.api.buffer_release.unwrap()(self.api.device, cached.buffer);
                }
            }
            for pipeline in &self.pipelines {
                unsafe {
                    self.api.pipeline_release.unwrap()(self.api.device, pipeline.handle);
                }
            }
        }
    }
}

fn floats(values: &[f32]) -> Vec<u8> {
    values.iter().flat_map(|v| v.to_ne_bytes()).collect()
}
fn probe_resources(
    input: &[f32],
) -> Result<
    (
        crate::resources::ResourceStore,
        Vec<crate::types::ResourceVersion>,
        Vec<ResourceLease>,
    ),
    i32,
> {
    use crate::resources::{ResourceDelta, ResourceLimits, ResourceStore};
    let mut parameters = floats(&[
        2., 0., 0., 0., 0., 3., 0., 0., 0., 0., 4., 0., 10., 20., 30., 1.,
    ]);
    parameters.extend([2u32, 0, 0, 0].iter().flat_map(|v| v.to_ne_bytes()));
    let mut store = ResourceStore::new(ResourceLimits {
        max_resources: 6,
        max_bytes: 512,
    })
    .map_err(|_| -1)?;
    let versions = [
        floats(input),
        floats(&[10., 20., 30., 1., 40., 50., 60., 1., 70., 80., 90., 1.]),
        parameters,
    ]
    .into_iter()
    .map(|bytes| store.apply(ResourceDelta::Create { bytes }).map_err(|_| -1))
    .collect::<Result<Vec<_>, _>>()?;
    let leases = versions
        .iter()
        .map(|v| store.lease(*v).map_err(|_| -1))
        .collect::<Result<Vec<_>, _>>()?;
    Ok((store, versions, leases))
}
fn commands(pipeline: Pipeline) -> ComputeCommands {
    ComputeCommands {
        pipeline,
        bindings: vec![
            ComputeBinding {
                slot: 0,
                lease_index: 0,
                offset: 0,
                byte_length: 48,
            },
            ComputeBinding {
                slot: 1,
                lease_index: 1,
                offset: 0,
                byte_length: 48,
            },
            ComputeBinding {
                slot: 2,
                lease_index: 2,
                offset: 0,
                byte_length: 80,
            },
        ],
        groups: [1, 1, 1],
    }
}
fn expect_floats(bytes: Vec<u8>, expected: &[f32]) -> Result<(), i32> {
    let actual: Vec<_> = bytes
        .as_chunks::<4>()
        .0
        .iter()
        .map(|v| f32::from_ne_bytes(*v))
        .collect();
    if actual.len() != expected.len()
        || actual
            .iter()
            .zip(expected)
            .any(|(a, b)| !a.is_finite() || (a - b).abs() > 0.00001)
    {
        eprintln!("GPU output mismatch: {actual:?}, expected {expected:?}");
        Err(-1)
    } else {
        Ok(())
    }
}
fn drain_probe(
    backend: &mut backend::RenderBackend<SdkComputeDevice<'_>>,
    report: &mut ProbeReport,
) -> Result<(), i32> {
    let deadline = std::time::Instant::now() + std::time::Duration::from_secs(5);
    while backend.pending_count() != 0 {
        let receipts = backend.poll().map_err(|e| {
            eprintln!("SDK terminal poll: {e:?}");
            -1
        })?;
        if !receipts.errors.is_empty() {
            return Err(-1);
        }
        for event in receipts.events {
            if !matches!(
                event,
                BackendEvent::Terminal {
                    status: TerminalStatus::Succeeded,
                    ..
                }
            ) {
                return Err(-1);
            }
            report.completed += 1;
        }
        report.pending = backend.pending_count() as u32;
        if std::time::Instant::now() > deadline {
            return Err(-1);
        }
        if report.pending != 0 {
            std::thread::sleep(std::time::Duration::from_millis(1));
        }
    }
    Ok(())
}
unsafe fn run_probe(
    api: SdkApi,
    desc: PipelineDesc,
    release_gate: Option<unsafe extern "C" fn(*mut c_void)>,
    gate_context: *mut c_void,
    report: &mut ProbeReport,
) -> Result<(), i32> {
    use crate::{
        resources::{DirtyRange, ResourceDelta},
        types::{FrameId, FrameIdentity, WorldUpdateId},
    };
    if desc.struct_size as usize != size_of::<PipelineDesc>()
        || desc.abi_version != 1
        || desc.source_bytes.is_null()
        || desc.source_length == 0
        || desc.source_length > 1024 * 1024
        || desc.entry_point.is_null()
        || desc.entry_point_length == 0
        || desc.entry_point_length > 256
        || desc.bindings.is_null()
        || desc.binding_count != 3
        || release_gate.is_none()
    {
        return Err(-1);
    }
    // Caller guarantees these descriptor ranges. Every borrowed source slice is
    // consumed synchronously by pipeline creation before this call returns.
    let source = unsafe {
        std::slice::from_raw_parts(desc.source_bytes.cast::<u8>(), desc.source_length as usize)
    };
    let entry = unsafe {
        std::slice::from_raw_parts(
            desc.entry_point.cast::<u8>(),
            desc.entry_point_length as usize,
        )
    };
    let layouts = unsafe { std::slice::from_raw_parts(desc.bindings, desc.binding_count as usize) };
    let entry = std::str::from_utf8(entry).map_err(|_| -1)?;
    let mut device = unsafe {
        SdkComputeDevice::from_api(
            api,
            NativeLimits {
                max_entries: 6,
                max_bytes: 352,
                max_pipelines: 1,
            },
        )
    }?;
    let pipeline = device.create_pipeline(
        source,
        entry,
        layouts,
        desc.threads_per_group,
        desc.shader_language,
    )?;
    let (mut a, mut av, al) = probe_resources(&[1., 2., 3., 1., 4., 5., 6., 1., 7., 8., 9., 1.])?;
    let (mut b, bv, bl) = probe_resources(&[2., 3., 4., 1., 5., 6., 7., 1., 8., 9., 10., 1.])?;
    let output_a = al[1].cache_key();
    let output_b = bl[1].cache_key();
    let rebind_input = al[1].clone();
    let rebind_parameters = al[2].clone();
    if av != bv || output_a == output_b {
        return Err(-1);
    }
    // Populate expendable native versions, then exercise bounded eviction and
    // unchanged-key reuse before any command can be committed.
    let (_, _, dummy) = probe_resources(&[0.; 12])?;
    device.prepare(&dummy)?;
    device.prepare(&al)?;
    let warm = device.upload_bytes();
    device.prepare(&al)?;
    if device.upload_bytes() != warm {
        return Err(-1);
    }
    device.prepare(&bl)?;
    if device.cached_entries() != 6 || device.cached_bytes() != 352 {
        return Err(-1);
    }
    drop(dummy);
    let mut backend = backend::RenderBackend::new(
        device,
        backend::BackendLimits {
            max_leases_per_submission: 3,
            max_bytes_per_submission: 176,
        },
    )
    .map_err(|_| -1)?;
    let frame = |n| FrameIdentity {
        frame_id: FrameId(n),
        world_update_id: WorldUpdateId(1),
        source_publication: 1,
        camera_publication: n,
    };
    report.pending = 2;
    let first = backend
        .submit(frame(1), commands(pipeline), al)
        .map_err(|e| {
            eprintln!("SDK first submit: {e:?}");
            -1
        })?;
    report.submitted = 1;
    // A GPU-written version cannot be reseeded while its command is blocked.
    if backend.submit(
        frame(9),
        commands(pipeline),
        vec![
            rebind_input.clone(),
            bl[1].clone(),
            rebind_parameters.clone(),
        ],
    ) != Err(backend::BackendError::DeviceRejected { code: 4 })
    {
        return Err(-1);
    }

    report.pending = 2;
    let second = backend
        .submit(frame(2), commands(pipeline), bl)
        .map_err(|e| {
            eprintln!("SDK second submit: {e:?}");
            -1
        })?;
    report.submitted = 2;
    if backend.submit(frame(3), commands(pipeline), vec![])
        != Err(backend::BackendError::PendingLimit)
        || backend.try_shutdown() != Err(backend::BackendError::PendingWork)
    {
        return Err(-1);
    }
    let early = backend.poll().map_err(|_| -1)?;
    if !early.events.is_empty()
        || !early.errors.is_empty()
        || backend.pending_count() != 2
        || backend.device().readback(output_a, 0, 48) != Err(-4)
    {
        return Err(-1);
    }
    // Mutation while the old frame is actually blocked on the GPU does not
    // mutate its native buffer or its original immutable CPU lease.
    av[0] = a
        .apply(ResourceDelta::Update {
            expected: av[0],
            ranges: vec![DirtyRange {
                offset: 0,
                bytes: floats(&[99.]),
            }],
        })
        .map_err(|_| -1)?;
    for v in av {
        a.retire(v).map_err(|_| -1)?;
    }
    for v in bv {
        b.retire(v).map_err(|_| -1)?;
    }
    report.retained_before_terminal = (a.stats().resident_bytes + b.stats().resident_bytes) as u64;
    if report.retained_before_terminal != 352 {
        return Err(-1);
    }
    unsafe { release_gate.unwrap()(gate_context) };
    drain_probe(&mut backend, report)?;
    for ticket in [first, second] {
        let state = backend.state(ticket).map_err(|_| -1)?;
        if state.gpu_terminal != Some(TerminalStatus::Succeeded)
            || state.source_outputs_ready
            || state.presentations != 0
        {
            return Err(-1);
        }
    }
    expect_floats(
        backend.device().readback(output_a, 0, 48)?,
        &[12., 26., 42., 1., 18., 35., 54., 1., 70., 80., 90., 1.],
    )?;
    expect_floats(
        backend.device().readback(output_b, 0, 48)?,
        &[14., 29., 46., 1., 20., 38., 58., 1., 70., 80., 90., 1.],
    )?;
    // Rebinding an unchanged CPU lease as a source means its immutable CPU
    // bytes, not GPU output left under that upload cache key.
    let mut third_store = crate::resources::ResourceStore::new(crate::resources::ResourceLimits {
        max_resources: 1,
        max_bytes: 48,
    })
    .map_err(|_| -1)?;
    let third_version = third_store
        .apply(ResourceDelta::Create {
            bytes: floats(&[10., 20., 30., 1., 40., 50., 60., 1., 70., 80., 90., 1.]),
        })
        .map_err(|_| -1)?;
    let third_output = third_store.lease(third_version).map_err(|_| -1)?;
    let third_key = third_output.cache_key();
    report.pending = 2;
    backend
        .submit(
            frame(3),
            commands(pipeline),
            vec![rebind_input, third_output, rebind_parameters],
        )
        .map_err(|_| -1)?;
    report.submitted = 3;
    third_store.retire(third_version).map_err(|_| -1)?;
    drain_probe(&mut backend, report)?;
    expect_floats(
        backend.device().readback(third_key, 0, 48)?,
        &[30., 80., 150., 1., 90., 170., 270., 1., 70., 80., 90., 1.],
    )?;
    report.retained_after_terminal = (a.stats().resident_bytes
        + b.stats().resident_bytes
        + third_store.stats().resident_bytes) as u64;
    report.upload_bytes = backend.device().upload_bytes();
    if report.retained_after_terminal != 0 || report.completed != 3 || report.upload_bytes != 624 {
        return Err(-1);
    }
    backend.try_shutdown().map_err(|_| -1)?;
    Ok(())
}
/// # Safety
/// Caller owns valid table/descriptor/report memory and all descriptor slices,
/// and keeps the SDK owner alive/serialized on this thread through return.
/// On failure, pending>0 means teardown is forbidden without a native drain.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mag_render_sdk_compute_probe(
    api: *const SdkApi,
    desc: *const PipelineDesc,
    release_gate: Option<unsafe extern "C" fn(*mut c_void)>,
    gate_context: *mut c_void,
    report: *mut ProbeReport,
) -> i32 {
    if api.is_null() || desc.is_null() || report.is_null() {
        return -1;
    }
    let report = unsafe { &mut *report };
    if report.abi_version != 1 || report.struct_size as usize != size_of::<ProbeReport>() {
        return -1;
    }
    report.pending = 2; // Conservative on unexpected unwind before ownership known.
    let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| unsafe {
        run_probe(*api, *desc, release_gate, gate_context, report)
    }));
    let status = match result {
        Ok(Ok(())) => 0,
        Ok(Err(code)) => code,
        Err(_) => -1,
    };
    report.status = status;
    status
}

#[cfg(test)]
mod tests {
    use super::*;
    unsafe extern "C" fn caps(_: *mut c_void, out: *mut Caps) -> i32 {
        unsafe {
            *out = Caps {
                struct_size: size_of::<Caps>() as u32,
                abi_version: 1,
                device_generation: 1,
                compute_supported: 1,
                max_pending: 2,
                max_bindings: 16,
                ..Caps::default()
            };
        }
        0
    }
    unsafe extern "C" fn create(_: *mut c_void, _: *const BufferDesc, _: *mut Buffer) -> i32 {
        -1
    }
    unsafe extern "C" fn release(_: *mut c_void, _: Buffer) -> i32 {
        0
    }
    unsafe extern "C" fn read(_: *mut c_void, _: Buffer, _: u64, _: *mut c_void, _: u64) -> i32 {
        -1
    }
    unsafe extern "C" fn pipeline(_: *mut c_void, _: *const PipelineDesc, _: *mut Pipeline) -> i32 {
        -1
    }
    unsafe extern "C" fn release_pipeline(_: *mut c_void, _: Pipeline) -> i32 {
        0
    }
    unsafe extern "C" fn submit(_: *mut c_void, _: *const Frame, _: *mut Ticket) -> i32 {
        -1
    }
    unsafe extern "C" fn poll(_: *mut c_void, _: *mut Event, _: u32, _: *mut u32) -> i32 {
        -1
    }
    fn api() -> SdkApi {
        SdkApi {
            abi_version: 1,
            struct_size: size_of::<SdkApi>() as u32,
            device: std::ptr::NonNull::<u8>::dangling().as_ptr().cast(),
            layout: ABI_LAYOUT,
            caps: Some(caps),
            buffer_create: Some(create),
            buffer_release: Some(release),
            buffer_read: Some(read),
            pipeline_create: Some(pipeline),
            pipeline_release: Some(release_pipeline),
            submit: Some(submit),
            poll: Some(poll),
        }
    }
    #[test]
    fn foreign_wire_version_size_offset_and_missing_callbacks_decline() {
        let limits = NativeLimits {
            max_entries: 1,
            max_bytes: 4,
            max_pipelines: 1,
        };
        assert!(unsafe { SdkComputeDevice::from_api(api(), limits) }.is_ok());
        for mode in 0..5 {
            let mut api = api();
            match mode {
                0 => api.abi_version = 2,
                1 => api.struct_size -= 1,
                2 => api.layout[37] += 4,
                3 => api.poll = None,
                _ => api.device = std::ptr::null_mut(),
            }
            assert!(matches!(
                unsafe { SdkComputeDevice::from_api(api, limits) },
                Err(-1)
            ));
        }
    }
    static MIXED_POLLS: std::sync::atomic::AtomicUsize = std::sync::atomic::AtomicUsize::new(0);
    unsafe extern "C" fn good_buffer(
        _: *mut c_void,
        _: *const BufferDesc,
        out: *mut Buffer,
    ) -> i32 {
        unsafe {
            *out = Buffer {
                device_generation: 1,
                id: 1,
                version: 1,
            };
        }
        0
    }
    unsafe extern "C" fn good_pipeline(
        _: *mut c_void,
        _: *const PipelineDesc,
        out: *mut Pipeline,
    ) -> i32 {
        unsafe {
            *out = Pipeline {
                device_generation: 1,
                id: 1,
            };
        }
        0
    }
    unsafe extern "C" fn good_submit(_: *mut c_void, _: *const Frame, out: *mut Ticket) -> i32 {
        unsafe {
            *out = Ticket {
                device_generation: 1,
                submission_id: 1,
            };
        }
        0
    }
    unsafe extern "C" fn mixed_poll(
        _: *mut c_void,
        out: *mut Event,
        capacity: u32,
        count: *mut u32,
    ) -> i32 {
        if MIXED_POLLS.fetch_add(1, std::sync::atomic::Ordering::SeqCst) != 0 {
            return -6;
        }
        assert!(capacity >= 2);
        let valid = Event {
            struct_size: size_of::<Event>() as u32,
            abi_version: 1,
            ticket: Ticket {
                device_generation: 1,
                submission_id: 1,
            },
            succeeded: 1,
            no_further_gpu_access: 1,
            ..Event::default()
        };
        let mut bad = valid;
        bad.ticket.submission_id = 999;
        unsafe {
            *out = valid;
            *out.add(1) = bad;
            *count = 2;
        }
        0
    }
    #[test]
    fn mixed_native_batch_preserves_valid_terminal_after_fault() {
        use crate::{
            backend::{BackendError, BackendLimits, RenderBackend},
            resources::{ResourceDelta, ResourceLimits, ResourceStore},
            types::{FrameId, FrameIdentity, WorldUpdateId},
        };
        MIXED_POLLS.store(0, std::sync::atomic::Ordering::SeqCst);
        let mut api = api();
        api.buffer_create = Some(good_buffer);
        api.pipeline_create = Some(good_pipeline);
        api.submit = Some(good_submit);
        api.poll = Some(mixed_poll);
        let mut device = unsafe {
            SdkComputeDevice::from_api(
                api,
                NativeLimits {
                    max_entries: 1,
                    max_bytes: 4,
                    max_pipelines: 1,
                },
            )
        }
        .unwrap();
        let pipeline = device
            .create_pipeline(
                b"synthetic",
                "entry",
                &[BindingLayout {
                    slot: 0,
                    access: 1,
                    minimum_bytes: 4,
                    alignment_bytes: 4,
                    runtime_size_index: u32::MAX,
                    ..BindingLayout::default()
                }],
                [1, 1, 1],
                1,
            )
            .unwrap();
        let mut store = ResourceStore::new(ResourceLimits {
            max_resources: 1,
            max_bytes: 4,
        })
        .unwrap();
        let v = store
            .apply(ResourceDelta::Create {
                bytes: vec![1, 2, 3, 4],
            })
            .unwrap();
        let mut backend = RenderBackend::new(
            device,
            BackendLimits {
                max_leases_per_submission: 1,
                max_bytes_per_submission: 4,
            },
        )
        .unwrap();
        let ticket = backend
            .submit(
                FrameIdentity {
                    frame_id: FrameId(1),
                    world_update_id: WorldUpdateId(1),
                    source_publication: 1,
                    camera_publication: 1,
                },
                ComputeCommands {
                    pipeline,
                    bindings: vec![ComputeBinding {
                        slot: 0,
                        lease_index: 0,
                        offset: 0,
                        byte_length: 4,
                    }],
                    groups: [1, 1, 1],
                },
                vec![store.lease(v).unwrap()],
            )
            .unwrap();
        store.retire(v).unwrap();
        assert!(matches!(
            backend.poll(),
            Err(BackendError::DevicePoll { .. })
        ));
        assert_eq!(store.stats().resident_bytes, 4);
        let deferred = backend
            .poll()
            .expect("valid once-only terminal must survive a later malformed receipt");
        assert_eq!(
            deferred.events,
            [BackendEvent::Terminal {
                ticket,
                status: TerminalStatus::Succeeded
            }]
        );
        assert_eq!(backend.pending_count(), 0);
        assert_eq!(store.stats().resident_bytes, 0);
    }
}
