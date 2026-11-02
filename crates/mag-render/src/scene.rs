//! Ordered scene attachments and explicit graphics dataflow. Native submission
//! consumes this validated scene without consulting original renderer state.
use crate::{batch::*, resources::ResourceLease};
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SceneError {
    InvalidLimits,
    Capacity,
    WrongScene,
    Uninitialized,
    StaleImage,
    Alias,
    InvalidPass,
    InvalidDraw,
    InvalidBindings,
    UnknownPipeline,
    Empty,
    Batch(BatchError),
}
#[derive(Clone, Copy, Debug)]
pub struct SceneLimits {
    pub max_images: usize,
    pub max_passes: usize,
    pub max_draws: usize,
    pub max_bytes: u64,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ImageFormat {
    R8,
    Rg8,
    Bc1,
    Bc2,
    Bc3,
    R16Unorm,
    Rg16Unorm,
    Rgba16Float,
    Rgba32Float,
    R32Float,
    Rg16Float,
    Rgba8,
    Bgra8,
    Depth32,
}
#[path = "scene_image.rs"]
pub mod image;
pub use image::{Dimension, ImageLayout, Subresource, ViewDesc, Viewport};
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ImageDesc {
    pub width: u32,
    pub height: u32,
    pub format: ImageFormat,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Image {
    scene: u64,
    index: usize,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ImageValue {
    image: Image,
    epoch: u64,
    view: Option<ViewDesc>,
}
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Load<T> {
    Clear(T),
    Load(ImageValue),
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Store {
    Store,
    Discard,
}
#[derive(Clone, Copy, Debug)]
pub struct ColorAttachment {
    pub image: Image,
    pub load: Load<[f32; 4]>,
    pub store: Store,
}
#[derive(Clone, Copy, Debug)]
pub struct DepthAttachment {
    pub image: Image,
    pub load: Load<f32>,
    pub store: Store,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Stage {
    Vertex,
    Fragment,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DepthCompare {
    Disabled,
    Never,
    Equal,
    Greater,
    NotEqual,
    GreaterEqual,
    Less,
    LessEqual,
    Always,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct DepthState {
    pub compare: DepthCompare,
    pub write: bool,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Blend {
    /// Exact effective factors/equations are retained by the native V3 registry.
    Explicit,
    Opaque,
    Alpha,
}
#[derive(Clone, Copy, Debug)]
pub struct GraphicsBuffer {
    pub stage: Stage,
    pub slot: u32,
    pub source: ReadSource,
    pub range: ByteRange,
}
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Sampler {
    Explicit(mag_render_abi::packet::Sampler),
    NearestClamp,
    LinearClamp,
    CompareLessEqual,
}
impl Sampler {
    pub fn comparison(self) -> bool {
        match self {
            Self::Explicit(s) => s.comparison_enabled != 0,
            Self::CompareLessEqual => true,
            _ => false,
        }
    }
}
#[derive(Clone, Copy, Debug)]
pub struct SampledImage {
    pub stage: Stage,
    pub texture_slot: u32,
    pub sampler_slot: u32,
    pub image: ImageValue,
    pub sampler: Sampler,
}
#[derive(Clone, Debug)]
pub struct GraphicsBufferLayout {
    pub stage: Stage,
    pub binding: BindingRequirement,
}
#[derive(Clone, Copy, Debug)]
pub struct TextureLayout {
    pub stage: Stage,
    pub texture_slot: u32,
    pub sampler_slot: u32,
    pub format: ImageFormat,
    pub comparison: bool,
}
#[derive(Clone, Debug)]
pub struct GraphicsLayout {
    pub vertex_stride: u32,
    pub color_format: Option<ImageFormat>,
    pub fragment_stage: bool,
    pub depth_attachment: bool,
    pub depth: DepthState,
    pub blend: Blend,
    pub buffers: Vec<GraphicsBufferLayout>,
    pub textures: Vec<TextureLayout>,
}
pub trait SceneCatalog: PipelineCatalog {
    fn scene_graphics(&self, pipeline: u64) -> Option<GraphicsLayout>;
}
#[derive(Clone, Debug)]
pub struct SceneDraw {
    pub pipeline: u64,
    pub vertices: ReadSource,
    pub indices: ReadSource,
    pub vertex_range: ByteRange,
    pub index_range: ByteRange,
    pub index_count: u32,
    pub buffers: Vec<GraphicsBuffer>,
    pub textures: Vec<SampledImage>,
}
#[derive(Clone, Debug)]
pub struct RenderPass {
    pub color: Option<ColorAttachment>,
    pub depth: Option<DepthAttachment>,
    pub draws: Vec<SceneDraw>,
}
#[derive(Clone, Copy, Debug)]
pub struct SceneOutput {
    pub image: ImageValue,
    pub diagnostic_readback: bool,
}

#[derive(Clone, Debug)]
pub struct ImagePlan {
    pub layout: Option<ImageLayout>,
    pub desc: ImageDesc,
    pub upload: Option<usize>,
}
struct ImageState {
    plan: ImagePlan,
    epoch: u64,
    initialized: bool,
}
pub struct SceneBuilder {
    base: PreparedBatch,
    limits: SceneLimits,
    images: Vec<ImageState>,
    textures: Vec<ResourceLease>,
    passes: Vec<RenderPass>,
    viewports: Vec<Option<Viewport>>,
    bytes: u64,
    draws: usize,
    output: Option<SceneOutput>,
}
pub struct PreparedScene {
    base: PreparedBatch,
    images: Vec<ImagePlan>,
    textures: Vec<ResourceLease>,
    passes: Vec<RenderPass>,
    viewports: Vec<Option<Viewport>>,
    output: Option<SceneOutput>,
}
impl Image {
    pub fn index(self) -> usize {
        self.index
    }
}
impl ImageValue {
    pub fn image(self) -> Image {
        self.image
    }
    pub fn view(self) -> Option<ViewDesc> {
        self.view
    }
    pub fn epoch(self) -> u64 {
        self.epoch
    }
}
impl SceneBuilder {
    pub fn new(base: PreparedBatch, limits: SceneLimits) -> Result<Self, SceneError> {
        if limits.max_images == 0
            || limits.max_passes == 0
            || limits.max_draws == 0
            || limits.max_bytes == 0
        {
            return Err(SceneError::InvalidLimits);
        }
        if base.draw().is_some() {
            return Err(SceneError::InvalidPass);
        }
        let bytes = base
            .buffers()
            .iter()
            .try_fold(0u64, |n, b| n.checked_add(b.byte_length))
            .and_then(|n| {
                base.uploads()
                    .iter()
                    .try_fold(n, |n, b| n.checked_add(b.bytes().len() as u64))
            })
            .ok_or(SceneError::Capacity)?;
        if bytes > limits.max_bytes {
            return Err(SceneError::Capacity);
        }
        Ok(Self {
            base,
            limits,
            images: vec![],
            textures: vec![],
            passes: vec![],
            viewports: vec![],
            bytes,
            draws: 0,
            output: None,
        })
    }
    fn state(&self, image: Image) -> Result<&ImageState, SceneError> {
        if image.scene != self.base.batch_id() {
            return Err(SceneError::WrongScene);
        }
        self.images.get(image.index).ok_or(SceneError::WrongScene)
    }
    fn valid(&self, value: ImageValue) -> Result<(), SceneError> {
        let s = self.state(value.image)?;
        if !s.initialized {
            return Err(SceneError::Uninitialized);
        }
        if s.epoch != value.epoch {
            return Err(SceneError::StaleImage);
        }
        Ok(())
    }
    fn image_bytes(desc: ImageDesc) -> Result<u64, SceneError> {
        if desc.width == 0 || desc.height == 0 || desc.width > 8192 || desc.height > 8192 {
            return Err(SceneError::InvalidPass);
        }
        Ok(desc.format.level_layout(desc.width, desc.height)?.bytes)
    }
    fn add_image(
        &mut self,
        desc: ImageDesc,
        upload: Option<ResourceLease>,
    ) -> Result<Image, SceneError> {
        self.add_image_layout(desc, None, upload)
    }
    fn add_image_layout(
        &mut self,
        desc: ImageDesc,
        layout: Option<ImageLayout>,
        upload: Option<ResourceLease>,
    ) -> Result<Image, SceneError> {
        let size = if let Some(layout) = &layout {
            layout
                .validate(desc, upload.as_ref().map(|l| l.bytes().len() as u64))?
                .0
        } else {
            let size = Self::image_bytes(desc)?;
            if let Some(lease) = &upload {
                if desc.format == ImageFormat::Depth32 || lease.bytes().len() as u64 != size {
                    return Err(SceneError::InvalidBindings);
                }
            } else if !desc.format.info().color_attachment && !desc.format.info().depth_attachment {
                return Err(SceneError::InvalidPass);
            }
            size
        };
        let upload_bytes = upload.as_ref().map_or(0, |l| l.bytes().len() as u64);
        let bytes = self
            .bytes
            .checked_add(size)
            .and_then(|n| n.checked_add(upload_bytes))
            .ok_or(SceneError::Capacity)?;
        if self.images.len() >= self.limits.max_images || bytes > self.limits.max_bytes {
            return Err(SceneError::Capacity);
        }
        self.images
            .try_reserve(1)
            .map_err(|_| SceneError::Capacity)?;
        if upload.is_some() {
            self.textures
                .try_reserve(1)
                .map_err(|_| SceneError::Capacity)?;
        }
        let image = Image {
            scene: self.base.batch_id(),
            index: self.images.len(),
        };
        let initialized = upload.is_some();
        let index = upload.map(|lease| {
            let i = self.textures.len();
            self.textures.push(lease);
            i
        });
        self.images.push(ImageState {
            plan: ImagePlan {
                layout,
                desc,
                upload: index,
            },
            epoch: u64::from(initialized),
            initialized,
        });
        self.bytes = bytes;
        Ok(image)
    }
    pub fn image(&mut self, desc: ImageDesc) -> Result<Image, SceneError> {
        self.add_image(desc, None)
    }
    pub fn texture(
        &mut self,
        desc: ImageDesc,
        lease: ResourceLease,
    ) -> Result<ImageValue, SceneError> {
        let i = self.add_image(desc, Some(lease))?;
        self.current(i)
    }
    pub fn image_with_layout(
        &mut self,
        desc: ImageDesc,
        layout: ImageLayout,
    ) -> Result<Image, SceneError> {
        self.add_image_layout(desc, Some(layout), None)
    }
    pub fn texture_with_layout(
        &mut self,
        desc: ImageDesc,
        layout: ImageLayout,
        lease: ResourceLease,
    ) -> Result<ImageValue, SceneError> {
        let image = self.add_image_layout(desc, Some(layout), Some(lease))?;
        self.current(image)
    }
    pub fn view(&self, value: ImageValue, desc: ViewDesc) -> Result<ImageValue, SceneError> {
        self.valid(value)?;
        let state = self.state(value.image)?;
        desc.validate(
            state
                .plan
                .layout
                .as_ref()
                .unwrap_or(&ImageLayout::default()),
        )?;
        Ok(ImageValue {
            view: Some(desc),
            ..value
        })
    }
    pub fn render_pass_with_viewport(
        &mut self,
        pass: RenderPass,
        viewport: Viewport,
    ) -> Result<Vec<ImageValue>, SceneError> {
        let image = pass
            .color
            .map(|a| a.image)
            .or_else(|| pass.depth.map(|a| a.image))
            .ok_or(SceneError::InvalidPass)?;
        let desc = self.state(image)?.plan.desc;
        viewport.validate(desc.width, desc.height)?;
        let result = self.render_pass(pass)?;
        *self.viewports.last_mut().ok_or(SceneError::InvalidPass)? = Some(viewport);
        Ok(result)
    }
    pub fn current(&self, image: Image) -> Result<ImageValue, SceneError> {
        let s = self.state(image)?;
        if !s.initialized {
            return Err(SceneError::Uninitialized);
        }
        Ok(ImageValue {
            image,
            epoch: s.epoch,
            view: None,
        })
    }
    fn attachment<T>(
        &self,
        image: Image,
        load: &Load<T>,
        format: ImageFormat,
    ) -> Result<ImageDesc, SceneError> {
        let s = self.state(image)?;
        if s.plan.layout.as_ref().is_some_and(|l| !l.attachment()) {
            return Err(SceneError::InvalidPass);
        }
        if s.plan.desc.format != format {
            return Err(SceneError::InvalidPass);
        }
        if let Load::Load(v) = load {
            if v.view.is_some() {
                return Err(SceneError::InvalidPass);
            }
            if v.image != image {
                return Err(SceneError::WrongScene);
            }
            self.valid(*v)?;
        }
        Ok(s.plan.desc)
    }
    pub fn render_pass(&mut self, pass: RenderPass) -> Result<Vec<ImageValue>, SceneError> {
        if pass.color.is_none() && pass.depth.is_none() {
            return Err(SceneError::InvalidPass);
        }
        let mut extent = None;
        let mut attached = vec![];
        if let Some(c) = pass.color {
            let format = self.state(c.image)?.plan.desc.format;
            if let Load::Clear(color) = c.load
                && color
                    .iter()
                    .any(|c| !c.is_finite() || !format.floating_color() && !(0.0..=1.0).contains(c))
            {
                return Err(SceneError::InvalidPass);
            }
            if !format.info().color_attachment {
                return Err(SceneError::InvalidPass);
            }
            extent = Some(self.attachment(c.image, &c.load, format)?);
            attached.push((c.image, c.store));
        }
        if let Some(d) = pass.depth {
            if let Load::Clear(depth) = d.load
                && (!depth.is_finite() || !(0.0..=1.0).contains(&depth))
            {
                return Err(SceneError::InvalidPass);
            }
            let desc = self.attachment(d.image, &d.load, ImageFormat::Depth32)?;
            if extent.is_some_and(|c| c.width != desc.width || c.height != desc.height) {
                return Err(SceneError::InvalidPass);
            }
            attached.push((d.image, d.store));
        }
        for draw in &pass.draws {
            if draw.index_count == 0
                || !draw.index_count.is_multiple_of(3)
                || !draw.index_range.offset.is_multiple_of(4)
                || draw.index_range.length < u64::from(draw.index_count) * 4
                || !draw.vertex_range.offset.is_multiple_of(4)
                || !draw.vertex_range.length.is_multiple_of(4)
            {
                return Err(SceneError::InvalidDraw);
            }
            self.base
                .scene_read(draw.vertices, draw.vertex_range)
                .map_err(SceneError::Batch)?;
            self.base
                .scene_read(draw.indices, draw.index_range)
                .map_err(SceneError::Batch)?;
            let mut buffers = std::collections::BTreeSet::new();
            let mut textures = std::collections::BTreeSet::new();
            let mut samplers = std::collections::BTreeMap::new();
            for b in &draw.buffers {
                if b.slot >= 16 || !buffers.insert((b.stage as u8, b.slot)) {
                    return Err(SceneError::InvalidBindings);
                }
                self.base
                    .scene_read(b.source, b.range)
                    .map_err(SceneError::Batch)?;
            }
            for t in &draw.textures {
                if t.texture_slot >= 16
                    || t.sampler_slot >= 16
                    || !textures.insert((t.stage as u8, t.texture_slot))
                    || samplers
                        .insert((t.stage as u8, t.sampler_slot), t.sampler)
                        .is_some_and(|old| old != t.sampler)
                {
                    return Err(SceneError::InvalidBindings);
                }
                self.valid(t.image)?;
                if attached.iter().any(|(i, _)| *i == t.image.image) {
                    return Err(SceneError::Alias);
                }
                if t.sampler.comparison()
                    && self.state(t.image.image)?.plan.desc.format != ImageFormat::Depth32
                {
                    return Err(SceneError::InvalidBindings);
                }
            }
        }
        let draws = self
            .draws
            .checked_add(pass.draws.len())
            .ok_or(SceneError::Capacity)?;
        if self.passes.len() >= self.limits.max_passes || draws > self.limits.max_draws {
            return Err(SceneError::Capacity);
        }
        self.passes
            .try_reserve(1)
            .map_err(|_| SceneError::Capacity)?;
        self.viewports
            .try_reserve(1)
            .map_err(|_| SceneError::Capacity)?;
        let mut values = Vec::new();
        values
            .try_reserve_exact(attached.len())
            .map_err(|_| SceneError::Capacity)?;
        // All validation and allocation occurs before attachment epochs change.
        for (i, store) in &attached {
            self.state(*i)?
                .epoch
                .checked_add(1)
                .ok_or(SceneError::Capacity)?;
            if *store == Store::Store {
                values.push(ImageValue {
                    image: *i,
                    epoch: self.state(*i)?.epoch + 1,
                    view: None,
                })
            }
        }
        for (i, store) in attached {
            let s = &mut self.images[i.index];
            s.epoch += 1;
            s.initialized = store == Store::Store;
        }
        self.draws = draws;
        self.passes.push(pass);
        self.viewports.push(None);
        Ok(values)
    }
    pub fn output(
        &mut self,
        value: ImageValue,
        diagnostic_readback: bool,
    ) -> Result<(), SceneError> {
        self.valid(value)?;
        let s = self.state(value.image)?;
        if value.view.is_some()
            || !s.plan.desc.format.info().color_attachment
            || s.plan.layout.as_ref().is_some_and(|l| !l.attachment())
        {
            return Err(SceneError::InvalidPass);
        }
        let extra = if diagnostic_readback {
            Self::image_bytes(s.plan.desc)?
        } else {
            0
        };
        if self
            .bytes
            .checked_add(extra)
            .is_none_or(|n| n > self.limits.max_bytes)
        {
            return Err(SceneError::Capacity);
        }
        self.output = Some(SceneOutput {
            image: value,
            diagnostic_readback,
        });
        Ok(())
    }
    pub fn prepare(self, catalog: &impl SceneCatalog) -> Result<PreparedScene, SceneError> {
        if catalog.owner_generation() != self.base.owner_generation() {
            return Err(SceneError::Batch(BatchError::WrongOwner));
        }
        if self.viewports.iter().any(Option::is_some) && self.viewports.iter().any(Option::is_none)
        {
            return Err(SceneError::InvalidPass);
        }
        if self.passes.is_empty() {
            return Err(SceneError::Empty);
        }
        if let Some(out) = self.output {
            self.valid(out.image)?;
        }
        for pass in &self.passes {
            let color_format = match pass.color {
                Some(c) => Some(self.state(c.image)?.plan.desc.format),
                None => None,
            };
            for draw in &pass.draws {
                let schema = catalog
                    .scene_graphics(draw.pipeline)
                    .ok_or(SceneError::UnknownPipeline)?;
                if schema.color_format != color_format
                    || schema.depth_attachment != pass.depth.is_some()
                    || schema.vertex_stride < 4
                    || schema.vertex_stride > 2048
                    || !schema.vertex_stride.is_multiple_of(4)
                    || !draw
                        .vertex_range
                        .length
                        .is_multiple_of(u64::from(schema.vertex_stride))
                    || schema.buffers.len() != draw.buffers.len()
                    || schema.textures.len() != draw.textures.len()
                    || (pass.color.is_none() && schema.blend != Blend::Opaque)
                    || (schema.depth.compare == DepthCompare::Disabled && schema.depth.write)
                    || (schema.depth.compare != DepthCompare::Disabled && pass.depth.is_none())
                {
                    return Err(SceneError::InvalidBindings);
                }
                if !schema.fragment_stage
                    && (draw.buffers.iter().any(|b| b.stage == Stage::Fragment)
                        || draw.textures.iter().any(|t| t.stage == Stage::Fragment))
                {
                    return Err(SceneError::InvalidBindings);
                }
                for b in &draw.buffers {
                    let r = &schema
                        .buffers
                        .iter()
                        .find(|r| r.stage == b.stage && r.binding.slot == b.slot)
                        .ok_or(SceneError::InvalidBindings)?
                        .binding;
                    if r.access != Access::Read
                        || !r.alignment.is_power_of_two()
                        || b.range.offset % r.alignment != 0
                        || b.range.length < r.minimum_bytes
                    {
                        return Err(SceneError::InvalidBindings);
                    }
                }
                for t in &draw.textures {
                    let r = schema
                        .textures
                        .iter()
                        .find(|r| {
                            r.stage == t.stage
                                && r.texture_slot == t.texture_slot
                                && r.sampler_slot == t.sampler_slot
                        })
                        .ok_or(SceneError::InvalidBindings)?;
                    if self.state(t.image.image)?.plan.desc.format != r.format
                        || r.comparison != (t.sampler.comparison())
                    {
                        return Err(SceneError::InvalidBindings);
                    }
                }
            }
        }
        Ok(PreparedScene {
            base: self.base,
            images: self.images.into_iter().map(|s| s.plan).collect(),
            textures: self.textures,
            passes: self.passes,
            viewports: self.viewports,
            output: self.output,
        })
    }
}
impl PreparedScene {
    pub fn viewports(&self) -> &[Option<Viewport>] {
        &self.viewports
    }
    pub fn compute(&self) -> &PreparedBatch {
        &self.base
    }
    pub fn passes(&self) -> &[RenderPass] {
        &self.passes
    }
    pub fn images(&self) -> &[ImagePlan] {
        &self.images
    }
    pub fn output(&self) -> Option<SceneOutput> {
        self.output
    }
    pub fn texture_uploads(&self) -> &[ResourceLease] {
        &self.textures
    }
    pub fn into_submission(self) -> (SceneCommands, Vec<ResourceLease>) {
        let (base, mut leases) = self.base.into_submission();
        let texture_start = leases.len();
        let texture_keys = self.textures.iter().map(ResourceLease::cache_key).collect();
        leases.extend(self.textures);
        (
            SceneCommands {
                base,
                images: self.images,
                passes: self.passes,
                viewports: self.viewports,
                output: self.output,
                texture_start,
                texture_keys,
            },
            leases,
        )
    }
}
pub struct SceneCommands {
    base: BatchCommands,
    images: Vec<ImagePlan>,
    passes: Vec<RenderPass>,
    viewports: Vec<Option<Viewport>>,
    output: Option<SceneOutput>,
    texture_start: usize,
    texture_keys: Vec<crate::resources::ResourceCacheKey>,
}
impl SceneCommands {
    pub fn viewports(&self) -> &[Option<Viewport>] {
        &self.viewports
    }
    pub fn compute(&self) -> &BatchCommands {
        &self.base
    }
    pub fn passes(&self) -> &[RenderPass] {
        &self.passes
    }
    pub fn images(&self) -> &[ImagePlan] {
        &self.images
    }
    pub fn output(&self) -> Option<SceneOutput> {
        self.output
    }
    pub fn texture_upload_start(&self) -> usize {
        self.texture_start
    }
    pub fn validate_uploads(&self, leases: &[ResourceLease]) -> Result<(), SceneError> {
        if leases.len() != self.texture_start + self.texture_keys.len() {
            return Err(SceneError::InvalidBindings);
        }
        self.base
            .validate_uploads(&leases[..self.texture_start])
            .map_err(SceneError::Batch)?;
        if leases[self.texture_start..]
            .iter()
            .zip(&self.texture_keys)
            .any(|(l, k)| l.cache_key() != *k)
        {
            return Err(SceneError::InvalidBindings);
        }
        Ok(())
    }
}
