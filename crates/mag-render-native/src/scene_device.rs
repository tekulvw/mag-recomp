//! Explicit multipass driver using the same native registry, terminal backlog,
//! and bounded pending ownership as BatchDevice. No second device or queue.
use super::*;
use mag_render::scene;
/// Explicit whole-scene budget; over-capacity refuses before GPU effects.
pub const MAX_SCENE_DRAWS: usize = 16384;
pub const MAX_SCENE_IMAGES: usize = 1024;
pub const MAX_SCENE_RESOURCES: usize = 4096;
pub const MAX_SCENE_BYTES: u64 = 1024 * 1024 * 1024;
#[repr(C)]
#[derive(Clone, Copy)]
pub struct BufferLayout {
    pub stage: u32,
    pub reserved: u32,
    pub binding: BindingLayout,
}
#[repr(C)]
#[derive(Clone, Copy)]
pub struct TextureLayout {
    pub stage: u32,
    pub texture_slot: u32,
    pub sampler_slot: u32,
    pub format: u32,
    pub comparison: u32,
    pub reserved: u32,
}
#[repr(C)]
#[derive(Clone, Copy)]
pub struct SceneGraphicsDesc {
    pub struct_size: u32,
    pub abi_version: u32,
    pub vertex: ShaderStage,
    pub fragment: ShaderStage,
    pub vertex_stride: u32,
    pub color_format: u32,
    pub depth_format: u32,
    pub depth_compare: u32,
    pub depth_write: u32,
    pub blend: u32,
    pub buffers: *const BufferLayout,
    pub buffer_count: u32,
    pub texture_count: u32,
    pub textures: *const TextureLayout,
}
/// Prepared interleaved vertex fields, never guest layout inference.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct VertexAttribute {
    pub location: u32,
    pub format: u32,
    pub offset: u32,
    pub reserved: u32,
}
pub const BINDINGS_COMBINED: u32 = 0;
pub const BINDINGS_NAMESPACED: u32 = 1;
pub const VERTEX_FLOAT: u32 = 1;
pub const VERTEX_FLOAT2: u32 = 2;
pub const VERTEX_FLOAT3: u32 = 3;
pub const VERTEX_FLOAT4: u32 = 4;
pub const VERTEX_UNORM8X4: u32 = 5;
#[repr(C)]
pub struct SceneGraphicsV2Desc {
    pub struct_size: u32,
    pub abi_version: u32,
    pub base: SceneGraphicsDesc,
    pub attributes: *const VertexAttribute,
    pub attribute_count: u32,
    pub binding_model: u32,
}
#[repr(C)]
struct Image {
    format: u32,
    width: u32,
    height: u32,
    reserved: u32,
    upload: Buffer,
}
#[repr(C)]
struct BufferBinding {
    stage: u32,
    reserved: u32,
    binding: Binding,
}
#[repr(C)]
struct TextureBinding {
    stage: u32,
    texture_slot: u32,
    sampler_slot: u32,
    image: u32,
    sampler: u32,
    reserved: u32,
}
#[repr(C)]
struct SceneDraw {
    pipeline: Pipeline,
    vertices: Buffer,
    indices: Buffer,
    vertex_offset: u64,
    vertex_length: u64,
    index_offset: u64,
    index_length: u64,
    index_count: u32,
    buffer_count: u32,
    texture_count: u32,
    reserved: u32,
    buffers: *const BufferBinding,
    textures: *const TextureBinding,
}
#[repr(C)]
#[derive(Clone, Copy)]
struct Attachment {
    image: u32,
    load: u32,
    store: u32,
    reserved: u32,
    clear: [f32; 4],
}
#[repr(C)]
struct Pass {
    color: Attachment,
    depth: Attachment,
    draws: *const SceneDraw,
    draw_count: u32,
    reserved: u32,
}
#[repr(C)]
struct Scene {
    struct_size: u32,
    abi_version: u32,
    resources: *const Resource,
    resource_count: u32,
    image_count: u32,
    images: *const Image,
    dispatches: *const Frame,
    dispatch_count: u32,
    pass_count: u32,
    passes: *const Pass,
    output_image: u32,
    reserved: u32,
    diagnostic_readback: Buffer,
}
unsafe extern "C" {
    fn mag_native_scene_layout(out: *mut u32, words: u32) -> i32;
    fn mag_native_scene_graphics_create_v2(
        context: *mut c_void,
        desc: *const SceneGraphicsV2Desc,
        out: *mut Pipeline,
    ) -> i32;
    fn mag_native_scene_graphics_v2_layout(out: *mut u32, words: u32) -> i32;
    fn mag_native_scene_graphics_create(
        context: *mut c_void,
        desc: *const SceneGraphicsDesc,
        out: *mut Pipeline,
    ) -> i32;
    fn mag_native_submit_scene(context: *mut c_void, scene: *const Scene, out: *mut Ticket) -> i32;
    fn mag_native_submit_scene_frame(
        context: *mut c_void,
        scene: *const Scene,
        target: crate::host_image::NativeImage,
        frame: *const crate::host_frame::HostFrame,
        out: *mut Ticket,
    ) -> i32;
    fn mag_native_submit_scene_target(
        context: *mut c_void,
        scene: *const Scene,
        target: crate::host_image::NativeImage,
        out: *mut Ticket,
    ) -> i32;
}
pub struct SceneDevice<'owner> {
    frame: Option<crate::host_frame::HostFrame>,
    target: Option<crate::host_image::NativeImage>,
    inner: BatchDevice<'owner>,
    graphics: BTreeMap<u64, scene::GraphicsLayout>,
}
impl NativeContext {
    pub fn scene(&mut self, limits: BatchDeviceLimits) -> Result<SceneDevice<'_>, i32> {
        checked_scene_layout()?;
        Ok(SceneDevice {
            frame: None,
            target: None,
            inner: BatchDevice::from_api_with_byte_cap(self.api()?, limits, None, MAX_SCENE_BYTES)?,
            graphics: BTreeMap::new(),
        })
    }
}
impl SceneDevice<'static> {
    /// Own the context directly; no leaked borrow or self-referential Rust value.
    pub fn from_owned_context(
        mut context: NativeContext,
        limits: BatchDeviceLimits,
    ) -> Result<Self, i32> {
        checked_scene_layout()?;
        Ok(Self {
            frame: None,
            target: None,
            inner: BatchDevice::from_api_with_byte_cap(
                context.api()?,
                limits,
                Some(context),
                MAX_SCENE_BYTES,
            )?,
            graphics: BTreeMap::new(),
        })
    }
}
impl SceneDevice<'_> {
    /// # Safety
    /// Callbacks must be the genuine owner's non-unwinding reservation provider.
    /// Early preparation errors leave cancellation to the reservation owner.
    pub unsafe fn frame_context(&mut self, frame: crate::host_frame::HostFrame) -> Result<(), i32> {
        if !frame.valid() {
            return Err(-1);
        }
        crate::host_frame::checked_layout()?;
        self.frame = Some(frame);
        Ok(())
    }
    /// Native identity for actual accepted or indeterminate pending work only.
    pub fn native_ticket(&self, ticket: SubmissionTicket) -> Option<(u64, u64)> {
        self.inner
            .pending
            .iter()
            .find(|p| p.rust == ticket)
            .map(|p| (p.native.device_generation, p.native.submission_id))
    }
    /// # Safety
    /// The image must satisfy the genuine process-lived HostImage provider contract.
    pub unsafe fn import_target(
        &mut self,
        image: &crate::host_image::HostImage,
    ) -> Result<crate::host_image::ImageHandle, i32> {
        let context = self.inner.owned_context.as_mut().ok_or(-2)?;
        let handle = unsafe { context.import_image(image) }?;
        self.target = Some(handle.raw);
        Ok(handle)
    }
    pub fn release_target(&mut self, image: crate::host_image::ImageHandle) -> Result<(), i32> {
        self.target = None;
        self.frame = None;
        self.inner
            .owned_context
            .as_mut()
            .ok_or(-2)?
            .release_image(image)
    }
    pub(crate) fn set_output_target(
        &mut self,
        image: crate::host_image::NativeImage,
    ) -> Result<(), i32> {
        if image.device_generation != self.inner.generation || image.id == 0 {
            return Err(-3);
        }
        self.target = Some(image);
        Ok(())
    }
    /// # Safety
    /// Source, entry, and layout pointers must be valid through this synchronous
    /// native pipeline compilation. The native registry owns copied metadata.
    pub unsafe fn compute_pipeline(&mut self, desc: &PipelineDesc) -> Result<u64, i32> {
        unsafe { self.inner.compute_pipeline(desc) }
    }
    /// # Safety
    /// All source and schema pointers must be readable through this call.
    pub unsafe fn graphics_pipeline(&mut self, desc: &SceneGraphicsDesc) -> Result<u64, i32> {
        if self.inner.pipelines.len() >= self.inner.limits.max_pipelines {
            return Err(-5);
        }
        let mut out = Pipeline::default();
        let status =
            unsafe { mag_native_scene_graphics_create(self.inner.api.device, desc, &mut out) };
        if status != 0 {
            return Err(status);
        }
        unsafe { self.register_graphics(desc, out) }
    }
    /// Explicit attributes and stride for canonical prepared vertex bytes. All
    /// ranges and locations validate before platform pipeline compilation.
    /// # Safety
    /// Shader sources, binding arrays and attribute arrays are valid synchronous
    /// ranges. Location semantics match the supplied translated vertex stage.
    pub unsafe fn graphics_pipeline_v2(&mut self, desc: &SceneGraphicsV2Desc) -> Result<u64, i32> {
        checked_graphics_v2_layout()?;
        if self.inner.pipelines.len() >= self.inner.limits.max_pipelines {
            return Err(-5);
        }
        let mut out = Pipeline::default();
        let status =
            unsafe { mag_native_scene_graphics_create_v2(self.inner.api.device, desc, &mut out) };
        if status != 0 {
            return Err(status);
        }
        unsafe { self.register_graphics(&desc.base, out) }
    }
    unsafe fn register_graphics(
        &mut self,
        desc: &SceneGraphicsDesc,
        out: Pipeline,
    ) -> Result<u64, i32> {
        let buffers = if desc.buffer_count == 0 {
            &[]
        } else {
            unsafe { std::slice::from_raw_parts(desc.buffers, desc.buffer_count as usize) }
        };
        let textures = if desc.texture_count == 0 {
            &[]
        } else {
            unsafe { std::slice::from_raw_parts(desc.textures, desc.texture_count as usize) }
        };
        let schema = scene::GraphicsLayout {
            vertex_stride: desc.vertex_stride,
            color_format: match desc.color_format {
                1 => Some(scene::ImageFormat::Rgba8),
                3 => Some(scene::ImageFormat::Bgra8),
                _ => None,
            },
            fragment_stage: desc.fragment.source_length != 0,
            depth_attachment: desc.depth_format != 0,
            depth: scene::DepthState {
                compare: match desc.depth_compare {
                    0 => scene::DepthCompare::Disabled,
                    1 => scene::DepthCompare::Less,
                    2 => scene::DepthCompare::LessEqual,
                    _ => scene::DepthCompare::Always,
                },
                write: desc.depth_write != 0,
            },
            blend: if desc.blend == 0 {
                scene::Blend::Opaque
            } else {
                scene::Blend::Alpha
            },
            buffers: buffers
                .iter()
                .map(|b| scene::GraphicsBufferLayout {
                    stage: stage(b.stage),
                    binding: batch::BindingRequirement {
                        slot: b.binding.slot,
                        access: batch::Access::Read,
                        minimum_bytes: b.binding.minimum_bytes,
                        alignment: u64::from(b.binding.alignment_bytes),
                    },
                })
                .collect(),
            textures: textures
                .iter()
                .map(|t| scene::TextureLayout {
                    stage: stage(t.stage),
                    texture_slot: t.texture_slot,
                    sampler_slot: t.sampler_slot,
                    format: match t.format {
                        1 => scene::ImageFormat::Rgba8,
                        3 => scene::ImageFormat::Bgra8,
                        _ => scene::ImageFormat::Depth32,
                    },
                    comparison: t.comparison != 0,
                })
                .collect(),
        };
        self.inner.pipelines.insert(
            out.id,
            Definition {
                handle: out,
                compute: None,
                graphics: false,
            },
        );
        self.graphics.insert(out.id, schema);
        Ok(out.id)
    }
    pub fn readback(&self, ticket: SubmissionTicket) -> Result<Vec<u8>, i32> {
        self.inner.readback(ticket)
    }
    fn allocate_scene(
        &self,
        commands: &scene::SceneCommands,
        leases: &[ResourceLease],
    ) -> Result<AllocationSet, i32> {
        commands.validate_uploads(leases).map_err(|_| -1)?;
        if commands.compute().owner_generation() != self.inner.generation
            || commands.compute().draw().is_some()
            || commands.images().len() > MAX_SCENE_IMAGES
            || commands.passes().len() > 16
            || commands
                .passes()
                .iter()
                .map(|p| p.draws.len())
                .sum::<usize>()
                > MAX_SCENE_DRAWS
        {
            return Err(-1);
        }
        for pass in commands.passes() {
            for d in &pass.draws {
                if !self.graphics.contains_key(&d.pipeline) {
                    return Err(-3);
                }
            }
        }
        let diagnostic = commands.output().filter(|o| o.diagnostic_readback);
        if diagnostic.is_some() && !self.inner.limits.diagnostic_readback {
            return Err(-2);
        }
        let mut bytes = leases
            .iter()
            .try_fold(0u64, |n, l| n.checked_add(l.bytes().len() as u64))
            .ok_or(-5)?;
        for b in commands.compute().buffers() {
            bytes = bytes.checked_add(b.byte_length).ok_or(-5)?;
        }
        for im in commands.images() {
            bytes = bytes
                .checked_add(u64::from(im.desc.width) * u64::from(im.desc.height) * 4)
                .ok_or(-5)?;
        }
        let readback_len = if let Some(o) = diagnostic {
            let d = commands.images()[o.image.image().index()].desc;
            let row = u64::from(d.width) * 4;
            bytes = bytes
                .checked_add((row + row.div_ceil(256) * 256) * u64::from(d.height))
                .ok_or(-5)?;
            Some(row * u64::from(d.height))
        } else {
            None
        };
        if bytes > self.inner.limits.max_bytes
            || leases.len()
                + commands.compute().buffers().len()
                + usize::from(readback_len.is_some())
                > MAX_SCENE_RESOURCES
        {
            return Err(-5);
        }
        let compute_count = commands.texture_upload_start();
        let (mut buffers, mut resources, _) = self.inner.prepare_with_resource_cap(
            commands.compute(),
            &leases[..compute_count],
            MAX_SCENE_RESOURCES,
        )?;
        let result = (|| {
            for lease in &leases[compute_count..] {
                let b = self
                    .inner
                    .allocate(lease.bytes().len() as u64, lease.bytes().as_ptr().cast())?;
                buffers.push(b);
                resources.push(Resource {
                    buffer: b,
                    role: 0,
                    reserved: 0,
                });
            }
            if let Some(len) = readback_len {
                let b = self.inner.allocate(len, std::ptr::null())?;
                buffers.push(b);
                resources.push(Resource {
                    buffer: b,
                    role: 2,
                    reserved: 0,
                });
                Ok(Some((b, len as usize)))
            } else {
                Ok(None)
            }
        })();
        match result {
            Ok(out) => Ok((buffers, resources, out)),
            Err(code) => {
                self.inner.release(&buffers);
                Err(code)
            }
        }
    }
}
fn stage(value: u32) -> scene::Stage {
    if value == 0 {
        scene::Stage::Vertex
    } else {
        scene::Stage::Fragment
    }
}
fn source_index(source: batch::ReadSource, uploads: usize) -> usize {
    match source {
        batch::ReadSource::Upload(u) => u.index(),
        batch::ReadSource::Produced(v) => uploads + v.buffer().index(),
    }
}
fn attachment<T>(
    image: scene::Image,
    load: scene::Load<T>,
    store: scene::Store,
    clear: impl FnOnce(T) -> [f32; 4],
) -> Attachment {
    let (load, values) = match load {
        scene::Load::Clear(v) => (0, clear(v)),
        scene::Load::Load(_) => (1, [0.; 4]),
    };
    Attachment {
        image: image.index() as u32,
        load,
        store: u32::from(store == scene::Store::Discard),
        reserved: 0,
        clear: values,
    }
}
fn absent() -> Attachment {
    Attachment {
        image: u32::MAX,
        load: 0,
        store: 0,
        reserved: 0,
        clear: [0.; 4],
    }
}
impl PipelineCatalog for SceneDevice<'_> {
    fn owner_generation(&self) -> u64 {
        self.inner.generation
    }
    fn bindings(&self, id: u64) -> Option<&[batch::BindingRequirement]> {
        self.inner.bindings(id)
    }
}
impl scene::SceneCatalog for SceneDevice<'_> {
    fn scene_graphics(&self, id: u64) -> Option<scene::GraphicsLayout> {
        self.graphics.get(&id).cloned()
    }
}
impl RenderDevice for SceneDevice<'_> {
    type Commands = scene::SceneCommands;
    fn submit(
        &mut self,
        rust: SubmissionTicket,
        commands: &Self::Commands,
        leases: &[ResourceLease],
    ) -> SubmitDisposition {
        let reject = |code: i32| SubmitDisposition::DefinitelyRejected {
            code: code.unsigned_abs(),
        };
        if self.frame.is_some() && self.target.is_none() {
            return reject(-1);
        }
        if self.inner.pending.len() + self.inner.ready.len() >= 2 {
            return reject(-4);
        }
        let (buffers, resources, readback) = match self.allocate_scene(commands, leases) {
            Ok(v) => v,
            Err(c) => return reject(c),
        };
        let uploads = commands.texture_upload_start();
        let binding_sets: Vec<Vec<Binding>> = commands
            .compute()
            .dispatches()
            .iter()
            .map(|d| {
                d.bindings
                    .iter()
                    .map(|b| {
                        let (slot, index, range) = match *b {
                            batch::Binding::Read {
                                slot,
                                source,
                                range,
                            } => (slot, source_index(source, uploads), range),
                            batch::Binding::Write {
                                slot,
                                buffer,
                                range,
                            } => (slot, uploads + buffer.index(), range),
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
            .compute()
            .dispatches()
            .iter()
            .zip(&binding_sets)
            .map(|(d, b)| Frame {
                struct_size: size_of::<Frame>() as u32,
                abi_version: 1,
                pipeline: self.inner.pipelines[&d.pipeline].handle,
                bindings: b.as_ptr(),
                binding_count: b.len() as u32,
                groups: d.groups,
            })
            .collect();
        let images: Vec<Image> = commands
            .images()
            .iter()
            .map(|im| Image {
                format: match im.desc.format {
                    scene::ImageFormat::Rgba8 => 1,
                    scene::ImageFormat::Bgra8 => 3,
                    scene::ImageFormat::Depth32 => 2,
                },
                width: im.desc.width,
                height: im.desc.height,
                reserved: 0,
                upload: im
                    .upload
                    .map(|i| buffers[uploads + commands.compute().buffers().len() + i])
                    .unwrap_or_default(),
            })
            .collect();
        let draw_buffers: Vec<Vec<BufferBinding>> = commands
            .passes()
            .iter()
            .flat_map(|p| &p.draws)
            .map(|d| {
                d.buffers
                    .iter()
                    .map(|b| BufferBinding {
                        stage: b.stage as u32,
                        reserved: 0,
                        binding: Binding {
                            slot: b.slot,
                            reserved: 0,
                            buffer: buffers[source_index(b.source, uploads)],
                            offset: b.range.offset,
                            byte_length: b.range.length,
                        },
                    })
                    .collect()
            })
            .collect();
        let draw_textures: Vec<Vec<TextureBinding>> = commands
            .passes()
            .iter()
            .flat_map(|p| &p.draws)
            .map(|d| {
                d.textures
                    .iter()
                    .map(|t| TextureBinding {
                        stage: t.stage as u32,
                        texture_slot: t.texture_slot,
                        sampler_slot: t.sampler_slot,
                        image: t.image.image().index() as u32,
                        sampler: t.sampler as u32,
                        reserved: 0,
                    })
                    .collect()
            })
            .collect();
        let draws: Vec<SceneDraw> = commands
            .passes()
            .iter()
            .flat_map(|p| &p.draws)
            .zip(draw_buffers.iter().zip(&draw_textures))
            .map(|(d, (b, t))| SceneDraw {
                pipeline: self.inner.pipelines[&d.pipeline].handle,
                vertices: buffers[source_index(d.vertices, uploads)],
                indices: buffers[source_index(d.indices, uploads)],
                vertex_offset: d.vertex_range.offset,
                vertex_length: d.vertex_range.length,
                index_offset: d.index_range.offset,
                index_length: d.index_range.length,
                index_count: d.index_count,
                buffer_count: b.len() as u32,
                texture_count: t.len() as u32,
                reserved: 0,
                buffers: b.as_ptr(),
                textures: t.as_ptr(),
            })
            .collect();
        let mut offset = 0;
        let passes: Vec<Pass> = commands
            .passes()
            .iter()
            .map(|p| {
                let start = offset;
                offset += p.draws.len();
                Pass {
                    color: p
                        .color
                        .map(|a| attachment(a.image, a.load, a.store, |v| v))
                        .unwrap_or_else(absent),
                    depth: p
                        .depth
                        .map(|a| attachment(a.image, a.load, a.store, |v| [v, 0., 0., 0.]))
                        .unwrap_or_else(absent),
                    draws: draws[start..].as_ptr(),
                    draw_count: p.draws.len() as u32,
                    reserved: 0,
                }
            })
            .collect();
        let scene = Scene {
            struct_size: size_of::<Scene>() as u32,
            abi_version: 1,
            resources: resources.as_ptr(),
            resource_count: resources.len() as u32,
            image_count: images.len() as u32,
            images: images.as_ptr(),
            dispatches: frames.as_ptr(),
            dispatch_count: frames.len() as u32,
            pass_count: passes.len() as u32,
            passes: passes.as_ptr(),
            output_image: commands
                .output()
                .map(|o| o.image.image().index() as u32)
                .unwrap_or(u32::MAX),
            reserved: 0,
            diagnostic_readback: readback.map(|r| r.0).unwrap_or_default(),
        };
        let mut native = Ticket::default();
        let status = unsafe {
            if let (Some(target), Some(frame)) = (self.target, self.frame.as_ref()) {
                mag_native_submit_scene_frame(
                    self.inner.api.device,
                    &scene,
                    target,
                    frame,
                    &mut native,
                )
            } else if let Some(target) = self.target {
                mag_native_submit_scene_target(self.inner.api.device, &scene, target, &mut native)
            } else {
                mag_native_submit_scene(self.inner.api.device, &scene, &mut native)
            }
        };
        if status < 0 {
            self.inner.release(&buffers);
            return reject(status);
        }
        self.inner.pending.push(Pending {
            native,
            rust,
            buffers,
            readback,
        });
        if status == 0
            && native.device_generation == self.inner.generation
            && native.submission_id != 0
        {
            SubmitDisposition::Accepted
        } else {
            SubmitDisposition::Indeterminate { code: 0x10001 }
        }
    }
    fn poll(&mut self, capacity: usize) -> Result<Vec<BackendEvent>, DevicePollError> {
        self.inner.poll(capacity)
    }
}
const SCENE_LAYOUT: &[u32] = &[
    size_of::<BufferLayout>() as u32,
    align_of::<BufferLayout>() as u32,
    offset_of!(BufferLayout, binding) as u32,
    size_of::<TextureLayout>() as u32,
    align_of::<TextureLayout>() as u32,
    offset_of!(TextureLayout, comparison) as u32,
    size_of::<SceneGraphicsDesc>() as u32,
    align_of::<SceneGraphicsDesc>() as u32,
    offset_of!(SceneGraphicsDesc, vertex) as u32,
    offset_of!(SceneGraphicsDesc, fragment) as u32,
    offset_of!(SceneGraphicsDesc, vertex_stride) as u32,
    offset_of!(SceneGraphicsDesc, buffers) as u32,
    offset_of!(SceneGraphicsDesc, textures) as u32,
    size_of::<Image>() as u32,
    align_of::<Image>() as u32,
    offset_of!(Image, upload) as u32,
    size_of::<BufferBinding>() as u32,
    align_of::<BufferBinding>() as u32,
    offset_of!(BufferBinding, binding) as u32,
    size_of::<TextureBinding>() as u32,
    align_of::<TextureBinding>() as u32,
    offset_of!(TextureBinding, sampler) as u32,
    size_of::<SceneDraw>() as u32,
    align_of::<SceneDraw>() as u32,
    offset_of!(SceneDraw, vertices) as u32,
    offset_of!(SceneDraw, vertex_offset) as u32,
    offset_of!(SceneDraw, index_count) as u32,
    offset_of!(SceneDraw, buffers) as u32,
    offset_of!(SceneDraw, textures) as u32,
    size_of::<Attachment>() as u32,
    align_of::<Attachment>() as u32,
    offset_of!(Attachment, clear) as u32,
    size_of::<Pass>() as u32,
    align_of::<Pass>() as u32,
    offset_of!(Pass, depth) as u32,
    offset_of!(Pass, draws) as u32,
    size_of::<Scene>() as u32,
    align_of::<Scene>() as u32,
    offset_of!(Scene, resources) as u32,
    offset_of!(Scene, images) as u32,
    offset_of!(Scene, dispatches) as u32,
    offset_of!(Scene, passes) as u32,
    offset_of!(Scene, output_image) as u32,
    offset_of!(Scene, diagnostic_readback) as u32,
];
fn checked_scene_layout() -> Result<(), i32> {
    let mut out = vec![0; SCENE_LAYOUT.len()];
    let status = unsafe { mag_native_scene_layout(out.as_mut_ptr(), out.len() as u32) };
    if status != 0 {
        return Err(status);
    }
    if out != SCENE_LAYOUT {
        return Err(-1);
    }
    Ok(())
}
fn checked_graphics_v2_layout() -> Result<(), i32> {
    let expected = [
        size_of::<VertexAttribute>() as u32,
        align_of::<VertexAttribute>() as u32,
        offset_of!(VertexAttribute, offset) as u32,
        size_of::<SceneGraphicsV2Desc>() as u32,
        align_of::<SceneGraphicsV2Desc>() as u32,
        offset_of!(SceneGraphicsV2Desc, base) as u32,
        offset_of!(SceneGraphicsV2Desc, attributes) as u32,
        offset_of!(SceneGraphicsV2Desc, attribute_count) as u32,
        VERTEX_FLOAT,
        VERTEX_FLOAT2,
        VERTEX_FLOAT3,
        VERTEX_FLOAT4,
        VERTEX_UNORM8X4,
        offset_of!(SceneGraphicsV2Desc, binding_model) as u32,
        BINDINGS_COMBINED,
        BINDINGS_NAMESPACED,
    ];
    let mut actual = [0; 16];
    let status =
        unsafe { mag_native_scene_graphics_v2_layout(actual.as_mut_ptr(), actual.len() as u32) };
    if status != 0 {
        return Err(status);
    }
    if actual != expected {
        return Err(-1);
    }
    Ok(())
}
#[cfg(test)]
mod abi_tests {
    use super::*;
    unsafe extern "C" {
        fn mag_native_scene_graphics_v2_validate(desc: *const SceneGraphicsV2Desc) -> i32;
    }
    #[test]
    fn material_namespaces_allow_same_unit_for_buffer_texture_and_sampler() {
        let mut base = std::mem::MaybeUninit::uninit();
        assert_eq!(
            unsafe { crate::client::mag_render_client_diagnostic_pipeline(1, base.as_mut_ptr()) },
            0
        );
        let mut base = unsafe { base.assume_init() };
        let buffer = BufferLayout {
            stage: 1,
            reserved: 0,
            binding: BindingLayout {
                slot: 0,
                access: 1,
                minimum_bytes: 16,
                runtime_array_stride: 0,
                runtime_size_index: u32::MAX,
                alignment_bytes: 4,
                binding_kind: 1,
            },
        };
        let texture = TextureLayout {
            stage: 1,
            texture_slot: 0,
            sampler_slot: 0,
            format: 1,
            comparison: 0,
            reserved: 0,
        };
        base.buffers = &buffer;
        base.buffer_count = 1;
        base.textures = &texture;
        base.texture_count = 1;
        let attribute = VertexAttribute {
            location: 0,
            format: VERTEX_FLOAT4,
            offset: 0,
            reserved: 0,
        };
        let mut desc = SceneGraphicsV2Desc {
            struct_size: size_of::<SceneGraphicsV2Desc>() as u32,
            abi_version: 2,
            base,
            attributes: &attribute,
            attribute_count: 1,
            binding_model: BINDINGS_NAMESPACED,
        };
        assert_eq!(unsafe { mag_native_scene_graphics_v2_validate(&desc) }, 0);
        desc.binding_model = BINDINGS_COMBINED;
        assert_eq!(unsafe { mag_native_scene_graphics_v2_validate(&desc) }, -1);
        desc.binding_model = 99;
        assert_eq!(unsafe { mag_native_scene_graphics_v2_validate(&desc) }, -1);
    }
    #[test]
    fn real_common_preflight_accepts_prepared_material_attributes_and_rejects_aliases() {
        let mut base = std::mem::MaybeUninit::uninit();
        assert_eq!(
            unsafe { crate::client::mag_render_client_diagnostic_pipeline(1, base.as_mut_ptr()) },
            0
        );
        let mut base = unsafe { base.assume_init() };
        base.vertex_stride = 24;
        let mut attributes = [
            VertexAttribute {
                location: 0,
                format: VERTEX_FLOAT3,
                offset: 0,
                reserved: 0,
            },
            VertexAttribute {
                location: 3,
                format: VERTEX_FLOAT2,
                offset: 12,
                reserved: 0,
            },
            VertexAttribute {
                location: 6,
                format: VERTEX_UNORM8X4,
                offset: 20,
                reserved: 0,
            },
        ];
        let validate = |attributes: &[VertexAttribute]| {
            let desc = SceneGraphicsV2Desc {
                struct_size: size_of::<SceneGraphicsV2Desc>() as u32,
                abi_version: 2,
                base,
                attributes: attributes.as_ptr(),
                attribute_count: attributes.len() as u32,
                binding_model: 0,
            };
            unsafe { mag_native_scene_graphics_v2_validate(&desc) }
        };
        assert_eq!(validate(&attributes), 0);
        let bgra = SceneGraphicsV2Desc {
            struct_size: size_of::<SceneGraphicsV2Desc>() as u32,
            abi_version: 2,
            base: SceneGraphicsDesc {
                color_format: 3,
                ..base
            },
            attributes: attributes.as_ptr(),
            attribute_count: 3,
            binding_model: 0,
        };
        assert_eq!(unsafe { mag_native_scene_graphics_v2_validate(&bgra) }, 0);
        attributes[1].offset = 8;
        assert_eq!(validate(&attributes), -1);
        attributes[1].offset = 12;
        attributes[1].location = 0;
        assert_eq!(validate(&attributes), -1);
        attributes[1].location = 3;
        attributes[2].offset = 24;
        assert_eq!(validate(&attributes), -1);
    }

    #[test]
    fn scene_c_rust_offsets_alignments_and_sizes_match() {
        super::checked_scene_layout().unwrap();
        super::checked_graphics_v2_layout().unwrap();
    }
}
