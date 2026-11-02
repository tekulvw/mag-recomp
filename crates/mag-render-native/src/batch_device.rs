//! Frontend-owned ordered compute/draw driver. Raw descriptors live only through
//! synchronous native validation/encoding; generic RenderBackend owns CPU leases
//! through the final genuine terminal receipt.
use crate::NativeContext;
use mag_render::{
    backend::*,
    batch::{self, PipelineCatalog},
    resources::ResourceLease,
    sdk_compute::*,
};
use std::{
    collections::{BTreeMap, VecDeque},
    ffi::{c_char, c_void},
    marker::PhantomData,
    mem::{align_of, offset_of, size_of},
};
#[repr(C)]
#[derive(Clone, Copy)]
pub struct ShaderStage {
    pub source: *const c_void,
    pub source_length: u64,
    pub entry: *const c_char,
    pub entry_length: u32,
    pub language: u32,
}
#[repr(C)]
pub struct GraphicsDesc {
    pub struct_size: u32,
    pub abi_version: u32,
    pub vertex: ShaderStage,
    pub fragment: ShaderStage,
    pub vertex_stride: u32,
    pub reserved: u32,
}
#[repr(C)]
#[derive(Clone, Copy)]
struct Resource {
    buffer: Buffer,
    role: u32,
    reserved: u32,
}
#[repr(C)]
struct Draw {
    pipeline: Pipeline,
    vertices: Buffer,
    indices: Buffer,
    color_readback: Buffer,
    vertex_offset: u64,
    vertex_length: u64,
    index_offset: u64,
    index_length: u64,
    index_count: u32,
    width: u32,
    height: u32,
    reserved: u32,
    clear_color: [f32; 4],
    clear_depth: f32,
    reserved1: u32,
}
#[repr(C)]
struct Command {
    kind: u32,
    reserved: u32,
    dispatch: *const Frame,
    draw: *const Draw,
}
#[repr(C)]
struct Batch {
    struct_size: u32,
    abi_version: u32,
    resources: *const Resource,
    resource_count: u32,
    command_count: u32,
    commands: *const Command,
}
unsafe extern "C" {
    fn mag_native_batch_layout(out: *mut u32, words: u32) -> i32;
    fn mag_native_graphics_create(
        context: *mut c_void,
        desc: *const GraphicsDesc,
        out: *mut Pipeline,
    ) -> i32;
    fn mag_native_submit_batch(context: *mut c_void, batch: *const Batch, out: *mut Ticket) -> i32;
}
#[derive(Clone, Copy)]
/// Logical live submission bytes including optional diagnostic staging.
/// Fresh upload/generated/attachment allocations are currently per batch; warm
/// CPU cache reuse and a terminal-safe allocation pool remain next steps.
pub struct BatchDeviceLimits {
    pub max_bytes: u64,
    pub max_pipelines: usize,
    pub diagnostic_readback: bool,
}
type AllocationSet = (Vec<Buffer>, Vec<Resource>, Option<(Buffer, usize)>);
struct Definition {
    handle: Pipeline,
    compute: Option<Vec<batch::BindingRequirement>>,
    graphics: bool,
}
struct Pending {
    native: Ticket,
    rust: SubmissionTicket,
    buffers: Vec<Buffer>,
    readback: Option<(Buffer, usize)>,
}
struct Completed {
    rust: SubmissionTicket,
    readback: Buffer,
    length: usize,
    succeeded: bool,
}
pub struct BatchDevice<'owner> {
    api: SdkApi,
    generation: u64,
    limits: BatchDeviceLimits,
    pipelines: BTreeMap<u64, Definition>,
    pending: Vec<Pending>,
    completed: VecDeque<Completed>,
    ready: VecDeque<BackendEvent>,
    _owner: PhantomData<&'owner mut NativeContext>,
    // An owned driver keeps the native context alive without a self-reference.
    owned_context: Option<NativeContext>,
}
impl NativeContext {
    pub fn batch(&mut self, limits: BatchDeviceLimits) -> Result<BatchDevice<'_>, i32> {
        BatchDevice::from_api(self.api()?, limits, None)
    }
    pub(crate) fn into_batch(
        mut self,
        limits: BatchDeviceLimits,
    ) -> Result<BatchDevice<'static>, i32> {
        let api = self.api()?;
        BatchDevice::from_api(api, limits, Some(self))
    }
}
impl<'owner> BatchDevice<'owner> {
    fn from_api(
        api: SdkApi,
        limits: BatchDeviceLimits,
        owned_context: Option<NativeContext>,
    ) -> Result<Self, i32> {
        if limits.max_bytes == 0 || limits.max_bytes > 64 * 1024 * 1024 || limits.max_pipelines == 0
        {
            return Err(-1);
        }
        checked_batch_layout()?;
        // Reuse the existing checked function-table ABI validator.
        let checked = unsafe {
            SdkComputeDevice::from_api(
                api,
                NativeLimits {
                    max_entries: 1,
                    max_bytes: limits.max_bytes as usize,
                    max_pipelines: limits.max_pipelines,
                },
            )
        }?;
        drop(checked);
        let mut caps = Caps {
            struct_size: size_of::<Caps>() as u32,
            abi_version: 1,
            ..Caps::default()
        };
        let status = unsafe { api.caps.unwrap()(api.device, &mut caps) };
        if status != 0 {
            return Err(status);
        }
        let mut pending = Vec::new();
        pending.try_reserve_exact(2).map_err(|_| -5)?;
        let mut completed = VecDeque::new();
        completed.try_reserve_exact(2).map_err(|_| -5)?;
        let mut ready = VecDeque::new();
        ready.try_reserve_exact(2).map_err(|_| -5)?;
        Ok(BatchDevice {
            api,
            generation: caps.device_generation,
            limits,
            pipelines: BTreeMap::new(),
            pending,
            completed,
            ready,
            _owner: PhantomData,
            owned_context,
        })
    }
}
impl BatchDevice<'_> {
    /// # Safety
    /// Descriptor source and binding ranges must be valid for this synchronous
    /// call; native pipeline creation copies and validates the complete schema.
    pub unsafe fn compute_pipeline(&mut self, desc: &PipelineDesc) -> Result<u64, i32> {
        if self.pipelines.len() >= self.limits.max_pipelines {
            return Err(-5);
        }
        let mut handle = Pipeline::default();
        let status =
            unsafe { self.api.pipeline_create.unwrap()(self.api.device, desc, &mut handle) };
        if status != 0 {
            return Err(status);
        }
        let schema =
            unsafe { std::slice::from_raw_parts(desc.bindings, desc.binding_count as usize) };
        let requirements = schema
            .iter()
            .map(|b| batch::BindingRequirement {
                slot: b.slot,
                access: if b.access == 1 {
                    batch::Access::Read
                } else {
                    batch::Access::ReadWrite
                },
                minimum_bytes: b.minimum_bytes,
                alignment: u64::from(b.alignment_bytes),
            })
            .collect();
        self.pipelines.insert(
            handle.id,
            Definition {
                handle,
                compute: Some(requirements),
                graphics: false,
            },
        );
        Ok(handle.id)
    }
    /// # Safety
    /// All source/entry ranges must be valid for synchronous native compilation.
    pub unsafe fn graphics_pipeline(&mut self, desc: &GraphicsDesc) -> Result<u64, i32> {
        if self.pipelines.len() >= self.limits.max_pipelines {
            return Err(-5);
        }
        let mut handle = Pipeline::default();
        let status = unsafe { mag_native_graphics_create(self.api.device, desc, &mut handle) };
        if status != 0 {
            return Err(status);
        }
        self.pipelines.insert(
            handle.id,
            Definition {
                handle,
                compute: None,
                graphics: true,
            },
        );
        Ok(handle.id)
    }
    fn release(&self, buffers: &[Buffer]) {
        for buffer in buffers {
            unsafe {
                self.api.buffer_release.unwrap()(self.api.device, *buffer);
            }
        }
    }
    fn allocate(&self, bytes: u64, initial: *const c_void) -> Result<Buffer, i32> {
        let desc = BufferDesc {
            struct_size: size_of::<BufferDesc>() as u32,
            abi_version: 1,
            byte_length: bytes,
            initial_bytes: initial,
        };
        let mut out = Buffer::default();
        let status = unsafe { self.api.buffer_create.unwrap()(self.api.device, &desc, &mut out) };
        if status == 0 { Ok(out) } else { Err(status) }
    }
    /// Final diagnostic readback only. No output is CPU-readable until actual
    /// terminal polling has completed every compute and draw consumer.
    pub fn readback(&self, ticket: SubmissionTicket) -> Result<Vec<u8>, i32> {
        if !self.limits.diagnostic_readback {
            return Err(-2);
        }
        let output = self.completed.iter().find(|p| p.rust == ticket).ok_or(-4)?;
        if !output.succeeded {
            return Err(-6);
        }
        let mut bytes = Vec::new();
        bytes.try_reserve_exact(output.length).map_err(|_| -5)?;
        bytes.resize(output.length, 0);
        let status = unsafe {
            self.api.buffer_read.unwrap()(
                self.api.device,
                output.readback,
                0,
                bytes.as_mut_ptr().cast(),
                bytes.len() as u64,
            )
        };
        if status == 0 { Ok(bytes) } else { Err(status) }
    }
    fn prepare(
        &self,
        commands: &batch::BatchCommands,
        leases: &[ResourceLease],
    ) -> Result<AllocationSet, i32> {
        commands.validate_uploads(leases).map_err(|_| -1)?;
        if commands.owner_generation() != self.generation
            || leases.len()
                + commands.buffers().len()
                + usize::from(commands.draw().is_some() && self.limits.diagnostic_readback)
                > 64
            || commands.dispatches().len() + usize::from(commands.draw().is_some()) > 32
        {
            return Err(-1);
        }
        let mut bytes = leases
            .iter()
            .try_fold(0u64, |n, l| n.checked_add(l.bytes().len() as u64))
            .ok_or(-5)?;
        for buffer in commands.buffers() {
            bytes = bytes.checked_add(buffer.byte_length).ok_or(-5)?;
        }
        if let Some(draw) = commands.draw() {
            bytes = bytes
                .checked_add(u64::from(draw.target.width) * u64::from(draw.target.height) * 8)
                .ok_or(-5)?;
        }
        if self.limits.diagnostic_readback
            && let Some(draw) = commands.draw()
        {
            let row = u64::from(draw.target.width) * 4;
            let pitch = row.div_ceil(256) * 256;
            bytes = bytes
                .checked_add((row + pitch) * u64::from(draw.target.height))
                .ok_or(-5)?;
        }
        if bytes > self.limits.max_bytes {
            return Err(-5);
        }
        // Recheck actual registered pipelines before allocating any buffer.
        for dispatch in commands.dispatches() {
            if !self
                .pipelines
                .get(&dispatch.pipeline)
                .is_some_and(|p| p.compute.is_some())
            {
                return Err(-3);
            }
        }
        if commands
            .draw()
            .is_some_and(|d| !self.pipelines.get(&d.pipeline).is_some_and(|p| p.graphics))
        {
            return Err(-3);
        }
        let count = leases.len()
            + commands.buffers().len()
            + usize::from(commands.draw().is_some() && self.limits.diagnostic_readback);
        let mut buffers = Vec::new();
        buffers.try_reserve_exact(count).map_err(|_| -5)?;
        let mut resources = Vec::new();
        resources.try_reserve_exact(count).map_err(|_| -5)?;
        let result = (|| {
            for lease in leases {
                let b = self.allocate(lease.bytes().len() as u64, lease.bytes().as_ptr().cast())?;
                buffers.push(b);
                resources.push(Resource {
                    buffer: b,
                    role: 0,
                    reserved: 0,
                });
            }
            for plan in commands.buffers() {
                let b = self.allocate(plan.byte_length, std::ptr::null())?;
                buffers.push(b);
                resources.push(Resource {
                    buffer: b,
                    role: 1,
                    reserved: 0,
                });
            }
            let output =
                if let Some(draw) = commands.draw().filter(|_| self.limits.diagnostic_readback) {
                    let length = u64::from(draw.target.width) * u64::from(draw.target.height) * 4;
                    let b = self.allocate(length, std::ptr::null())?;
                    buffers.push(b);
                    resources.push(Resource {
                        buffer: b,
                        role: 2,
                        reserved: 0,
                    });
                    Some((b, length as usize))
                } else {
                    None
                };
            Ok(output)
        })();
        match result {
            Ok(output) => Ok((buffers, resources, output)),
            Err(code) => {
                self.release(&buffers);
                Err(code)
            }
        }
    }
}
impl PipelineCatalog for BatchDevice<'_> {
    fn owner_generation(&self) -> u64 {
        self.generation
    }
    fn bindings(&self, id: u64) -> Option<&[batch::BindingRequirement]> {
        self.pipelines.get(&id)?.compute.as_deref()
    }
    fn graphics(&self, id: u64) -> Option<batch::GraphicsRequirement> {
        self.pipelines
            .get(&id)?
            .graphics
            .then_some(batch::GraphicsRequirement { vertex_stride: 16 })
    }
}
impl RenderDevice for BatchDevice<'_> {
    type Commands = batch::BatchCommands;
    fn submit(
        &mut self,
        rust: SubmissionTicket,
        commands: &Self::Commands,
        leases: &[ResourceLease],
    ) -> SubmitDisposition {
        let reject = |code: i32| SubmitDisposition::DefinitelyRejected {
            code: code.unsigned_abs(),
        };
        if self.pending.len() + self.ready.len() >= 2 {
            return reject(-4);
        }
        let (buffers, resources, readback) = match self.prepare(commands, leases) {
            Ok(v) => v,
            Err(code) => return reject(code),
        };
        // All binding storage is finalized before frame pointers are taken.
        let binding_sets: Vec<Vec<Binding>> = commands
            .dispatches()
            .iter()
            .map(|dispatch| {
                dispatch
                    .bindings
                    .iter()
                    .map(|b| {
                        let (slot, index, range) = match *b {
                            batch::Binding::Read {
                                slot,
                                source,
                                range,
                            } => (
                                slot,
                                match source {
                                    batch::ReadSource::Upload(u) => u.index(),
                                    batch::ReadSource::Produced(v) => {
                                        leases.len() + v.buffer().index()
                                    }
                                },
                                range,
                            ),
                            batch::Binding::Write {
                                slot,
                                buffer,
                                range,
                            } => (slot, leases.len() + buffer.index(), range),
                        };
                        Binding {
                            slot,
                            reserved: 0,
                            buffer: buffers[index],
                            offset: range.offset,
                            byte_length: range.length,
                        }
                    })
                    .collect()
            })
            .collect();
        let frames: Vec<Frame> = commands
            .dispatches()
            .iter()
            .zip(&binding_sets)
            .map(|(d, b)| Frame {
                struct_size: size_of::<Frame>() as u32,
                abi_version: 1,
                pipeline: self.pipelines[&d.pipeline].handle,
                bindings: b.as_ptr(),
                binding_count: b.len() as u32,
                groups: d.groups,
            })
            .collect();
        let draw = commands.draw().map(|d| Draw {
            pipeline: self.pipelines[&d.pipeline].handle,
            vertices: buffers[leases.len() + d.vertices.buffer().index()],
            indices: buffers[leases.len() + d.indices.buffer().index()],
            color_readback: readback.map(|r| r.0).unwrap_or_default(),
            vertex_offset: d.vertex_range.offset,
            vertex_length: d.vertex_range.length,
            index_offset: d.index_range.offset,
            index_length: d.index_range.length,
            index_count: d.index_count,
            width: d.target.width,
            height: d.target.height,
            reserved: 0,
            clear_color: d.target.clear_color,
            clear_depth: d.target.clear_depth,
            reserved1: 0,
        });
        let mut native_commands: Vec<Command> = frames
            .iter()
            .map(|frame| Command {
                kind: 1,
                reserved: 0,
                dispatch: frame,
                draw: std::ptr::null(),
            })
            .collect();
        if let Some(draw) = &draw {
            native_commands.push(Command {
                kind: 2,
                reserved: 0,
                dispatch: std::ptr::null(),
                draw,
            });
        }
        let batch = Batch {
            struct_size: size_of::<Batch>() as u32,
            abi_version: 1,
            resources: resources.as_ptr(),
            resource_count: resources.len() as u32,
            command_count: native_commands.len() as u32,
            commands: native_commands.as_ptr(),
        };
        let mut native = Ticket::default();
        let status = unsafe { mag_native_submit_batch(self.api.device, &batch, &mut native) };
        if status < 0 {
            self.release(&buffers);
            return reject(status);
        }
        self.pending.push(Pending {
            native,
            rust,
            buffers,
            readback,
        });
        if status == 0 && native.device_generation == self.generation && native.submission_id != 0 {
            SubmitDisposition::Accepted
        } else {
            SubmitDisposition::Indeterminate { code: 0x10001 }
        }
    }
    fn poll(&mut self, capacity: usize) -> Result<Vec<BackendEvent>, DevicePollError> {
        let bad = || DevicePollError {
            code: 0x10001,
            device_lost: true,
        };
        if capacity == 0 {
            return Err(bad());
        }
        let capacity = capacity.min(64);
        if !self.ready.is_empty() {
            return Ok(self.ready.drain(..capacity.min(self.ready.len())).collect());
        }
        let mut events = vec![
            Event {
                struct_size: size_of::<Event>() as u32,
                abi_version: 1,
                ..Event::default()
            };
            capacity
        ];
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
            if event.struct_size != size_of::<Event>() as u32
                || event.abi_version != 1
                || event.no_further_gpu_access != 1
                || event.succeeded > 1
                || event.reserved != 0
            {
                fault = true;
                continue;
            }
            let Some(index) = self.pending.iter().position(|p| p.native == event.ticket) else {
                fault = true;
                continue;
            };
            let pending = self.pending.remove(index);
            for buffer in &pending.buffers {
                if pending
                    .readback
                    .is_none_or(|(r, _)| r.id != buffer.id || r.version != buffer.version)
                {
                    self.release(&[*buffer]);
                }
            }
            if let Some((readback, length)) = pending.readback {
                if self.completed.len() == 2 {
                    let old = self.completed.pop_front().unwrap();
                    self.release(&[old.readback]);
                }
                self.completed.push_back(Completed {
                    rust: pending.rust,
                    readback,
                    length,
                    succeeded: event.succeeded == 1,
                });
            }
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
        Ok(self.ready.drain(..capacity.min(self.ready.len())).collect())
    }
}
impl Drop for BatchDevice<'_> {
    fn drop(&mut self) {
        for pending in &self.pending {
            self.release(&pending.buffers);
        }
        for complete in &self.completed {
            self.release(&[complete.readback]);
        }
        for pipeline in self.pipelines.values() {
            unsafe {
                self.api.pipeline_release.unwrap()(self.api.device, pipeline.handle);
            }
        }
    }
}

const BATCH_LAYOUT: &[u32] = &[
    size_of::<crate::HostOwner>() as u32,
    align_of::<crate::HostOwner>() as u32,
    offset_of!(crate::HostOwner, lifetime) as u32,
    offset_of!(crate::HostOwner, device) as u32,
    offset_of!(crate::HostOwner, userdata) as u32,
    offset_of!(crate::HostOwner, get_device_proc) as u32,
    offset_of!(crate::HostOwner, retain) as u32,
    offset_of!(crate::HostOwner, try_begin) as u32,
    size_of::<ShaderStage>() as u32,
    align_of::<ShaderStage>() as u32,
    offset_of!(ShaderStage, source_length) as u32,
    offset_of!(ShaderStage, entry) as u32,
    offset_of!(ShaderStage, language) as u32,
    size_of::<GraphicsDesc>() as u32,
    align_of::<GraphicsDesc>() as u32,
    offset_of!(GraphicsDesc, vertex) as u32,
    offset_of!(GraphicsDesc, fragment) as u32,
    offset_of!(GraphicsDesc, vertex_stride) as u32,
    size_of::<Resource>() as u32,
    align_of::<Resource>() as u32,
    offset_of!(Resource, role) as u32,
    size_of::<Draw>() as u32,
    align_of::<Draw>() as u32,
    offset_of!(Draw, vertices) as u32,
    offset_of!(Draw, color_readback) as u32,
    offset_of!(Draw, vertex_offset) as u32,
    offset_of!(Draw, index_count) as u32,
    offset_of!(Draw, clear_color) as u32,
    offset_of!(Draw, clear_depth) as u32,
    size_of::<Command>() as u32,
    align_of::<Command>() as u32,
    offset_of!(Command, dispatch) as u32,
    offset_of!(Command, draw) as u32,
    size_of::<Batch>() as u32,
    align_of::<Batch>() as u32,
    offset_of!(Batch, resources) as u32,
    offset_of!(Batch, resource_count) as u32,
    offset_of!(Batch, commands) as u32,
];
fn checked_batch_layout() -> Result<(), i32> {
    let mut actual = vec![0; BATCH_LAYOUT.len()];
    let status = unsafe { mag_native_batch_layout(actual.as_mut_ptr(), actual.len() as u32) };
    if status != 0 {
        return Err(status);
    }
    if actual != BATCH_LAYOUT {
        return Err(-1);
    }
    Ok(())
}
#[cfg(test)]
mod abi_tests {
    #[test]
    fn independent_c_and_rust_batch_sizes_alignments_and_offsets_match() {
        super::checked_batch_layout().unwrap();
    }
}

#[path = "scene_device.rs"]
pub mod scene_device;
