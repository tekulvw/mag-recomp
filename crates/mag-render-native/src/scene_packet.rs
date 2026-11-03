//! Owned full-scene ingress sharing the production client's context and worker.
//! Source shaders are already lowered. Unknown state and unsupported resource
//! shapes are rejected as a whole; no attributes/materials are synthesized.
use super::*;
#[path = "scene_packet_compute.rs"]
pub(crate) mod compute;
use crate::{
    batch_device::{
        ShaderStage,
        scene_device::{
            BufferLayout, SceneGraphicsV2Desc, SceneGraphicsV3Desc,
            TextureLayout as NativeTextureLayout, VertexAttribute,
        },
    },
    host_frame::HostFrame,
};
use mag_render::scene;
use mag_render::sdk_compute::BindingLayout;
pub use mag_render_abi::Attachment;
pub use mag_render_abi::packet as raw;
unsafe extern "C" {
    fn mag_native_packet_layout(out: *mut u32, words: u32) -> i32;
}
unsafe extern "C" {
    fn mag_native_packet_v2_layout(out: *mut u32, words: u32) -> i32;
}
fn checked_v2_layout() -> Result<(), i32> {
    let expected = raw::v2_layout();
    let mut actual = vec![0; expected.len()];
    let status = unsafe { mag_native_packet_v2_layout(actual.as_mut_ptr(), actual.len() as u32) };
    if status != 0 {
        return Err(status);
    }
    if actual != expected {
        return Err(-1);
    }
    Ok(())
}
fn checked_layout() -> Result<(), i32> {
    let expected = raw::layout();
    let mut actual = vec![0; expected.len()];
    let status = unsafe { mag_native_packet_layout(actual.as_mut_ptr(), actual.len() as u32) };
    if status != 0 {
        return Err(status);
    }
    if expected != actual {
        return Err(-1);
    }
    Ok(())
}
unsafe extern "C" {
    fn mag_native_raster_validate(state: *const raw::RasterState) -> i32;
}
fn raster(s: &raw::RasterState) -> Result<(), i32> {
    let status = unsafe { mag_native_raster_validate(s) };
    if status == 0 { Ok(()) } else { Err(status) }
}
#[cfg(test)]
mod tests {
    use super::*;
    fn state() -> raw::RasterState {
        unsafe { std::mem::zeroed() }
    }
    #[test]
    fn independent_packet_v2_layout_matches() {
        checked_v2_layout().unwrap();
    }
    #[test]
    fn actual_alpha_factor_is_preserved_by_explicit_native_state() {
        let mut s = state();
        s.known_fields = 511;
        s.samples = 1;
        s.color_write_mask = 15;
        s.blend_enabled = 1;
        s.src_rgb = 6;
        s.dst_rgb = 7;
        s.src_alpha = 6;
        s.dst_alpha = 7;
        assert_eq!(raster(&s), Ok(()));
    }
    #[test]
    fn unknown_effective_state_is_not_defaulted() {
        let mut s = state();
        s.samples = 1;
        s.color_write_mask = 15;
        assert_eq!(raster(&s), Err(-2));
    }
    #[test]
    fn independent_c_packet_layout_matches() {
        checked_layout().unwrap();
    }
}

/// Pure owned CPU transport. No native context, guest pointer, or shader compiler
/// crosses the worker channel. SPIR-V storage remains explicitly word aligned.
#[derive(Clone)]
pub struct Program {
    pub flags: u32,
    pub stage: u32,
    pub language: u32,
    pub source: Vec<u8>,
    pub spirv: Vec<u32>,
    pub entry: Vec<u8>,
    pub buffers: Vec<BindingLayout>,
    pub textures: Vec<raw::TextureSlot>,
    pub samplers: Vec<raw::SamplerSlot>,
    pub vertex_locations: Vec<u32>,
    pub threads: [u32; 3],
}
impl Program {
    fn shader(&self) -> ShaderStage {
        ShaderStage {
            source: if self.language == 3 {
                self.spirv.as_ptr().cast()
            } else {
                self.source.as_ptr().cast()
            },
            source_length: if self.language == 3 {
                self.spirv.len() as u64 * 4
            } else {
                self.source.len() as u64
            },
            entry: self.entry.as_ptr().cast(),
            entry_length: self.entry.len() as u32,
            language: self.language,
        }
    }
    fn key(&self) -> Vec<u8> {
        let mut k = Vec::new();
        words(&mut k, &[self.stage, self.language, self.flags]);
        bytes(&mut k, &self.source);
        k.extend((self.spirv.len() as u64).to_le_bytes());
        for x in &self.spirv {
            k.extend(x.to_le_bytes())
        }
        bytes(&mut k, &self.entry);
        words(
            &mut k,
            &[
                self.buffers.len() as u32,
                self.textures.len() as u32,
                self.samplers.len() as u32,
                self.vertex_locations.len() as u32,
            ],
        );
        for b in &self.buffers {
            words(
                &mut k,
                &[
                    b.slot,
                    b.access,
                    b.runtime_array_stride,
                    b.runtime_size_index,
                    b.alignment_bytes,
                    b.binding_kind,
                ],
            );
            k.extend(b.minimum_bytes.to_le_bytes())
        }
        for t in &self.textures {
            words(
                &mut k,
                &[
                    t.slot,
                    t.dimension,
                    t.sample_kind,
                    t.arrayed,
                    t.multisampled,
                    t.reserved,
                ],
            )
        }
        for s in &self.samplers {
            words(&mut k, &[s.slot, s.comparison])
        }
        words(&mut k, &self.vertex_locations);
        words(&mut k, &self.threads);
        k
    }
}
#[derive(Clone)]
pub struct Pipeline {
    pub vertex: u32,
    pub fragment: u32,
    pub stride: u32,
    pub attributes: Vec<VertexAttribute>,
    pub color: u32,
    pub depth: u32,
    pub binding_model: u32,
    pub textures: Vec<NativeTextureLayout>,
    pub state: raw::RasterState,
}
#[derive(Clone)]
pub struct ImagePayload {
    pub layout: Option<ImageLayout>,
    pub desc: ImageDesc,
    pub bytes: Option<ImageBytes>,
}
/// Owned upload bytes of one image: copied for this packet, or a resident
/// immutable lease reused across packets for the same caller content key.
#[derive(Clone)]
pub enum ImageBytes {
    Owned(Vec<u8>),
    Resident(ResourceLease),
}
impl core::ops::Deref for ImageBytes {
    type Target = [u8];
    fn deref(&self) -> &[u8] {
        match self {
            Self::Owned(v) => v,
            Self::Resident(l) => l.bytes(),
        }
    }
}
impl From<Vec<u8>> for ImageBytes {
    fn from(v: Vec<u8>) -> Self {
        Self::Owned(v)
    }
}
/// Packed upload bytes and layout of images whose caller supplied a nonzero
/// content key (`raw::Image::reserved`). The caller guarantees one key always
/// names byte-identical content for the life of the process, so a hit with the
/// same descriptor and layout is used without re-reading or copying the bytes.
/// Bounded; eviction only drops this table's lease (packets keep theirs).
mod keyed_images {
    use super::*;
    use mag_render::resources::{ResourceDelta, ResourceLimits, ResourceStore};
    use std::{
        collections::HashMap,
        sync::{Mutex, OnceLock},
    };
    pub(super) const MAX_ENTRIES: usize = 4096;
    pub(super) const MAX_BYTES: usize = 512 * 1024 * 1024;
    struct Entry {
        desc: ImageDesc,
        layout: ImageLayout,
        lease: ResourceLease,
        used: u64,
    }
    pub(super) struct Table {
        store: ResourceStore,
        entries: HashMap<u32, Entry>,
        bytes: usize,
        clock: u64,
        pub(super) hits: u64,
        pub(super) misses: u64,
    }
    impl Table {
        pub(super) fn new(max_entries: usize, max_bytes: usize) -> Option<Self> {
            Some(Self {
                // Store capacity also counts versions still leased by
                // in-flight packets; leave room for them beyond this table.
                store: ResourceStore::new(ResourceLimits {
                    max_resources: max_entries * 3,
                    max_bytes: max_bytes.saturating_mul(3),
                })
                .ok()?,
                entries: HashMap::new(),
                bytes: 0,
                clock: 0,
                hits: 0,
                misses: 0,
            })
        }
        pub(super) fn get(
            &mut self,
            key: u32,
            desc: ImageDesc,
            layout: &ImageLayout,
        ) -> Option<ResourceLease> {
            self.clock += 1;
            let clock = self.clock;
            let entry = self.entries.get_mut(&key)?;
            if entry.desc != desc || entry.layout != *layout {
                return None;
            }
            entry.used = clock;
            self.hits += 1;
            Some(entry.lease.clone())
        }
        /// Make `bytes` resident under `key`; on any capacity failure the
        /// caller keeps the ordinary owned bytes.
        pub(super) fn insert(
            &mut self,
            key: u32,
            desc: ImageDesc,
            layout: ImageLayout,
            bytes: Vec<u8>,
            max_entries: usize,
            max_bytes: usize,
        ) -> Result<ResourceLease, Vec<u8>> {
            self.misses += 1;
            if bytes.is_empty() || bytes.len() > max_bytes / 4 {
                return Err(bytes);
            }
            if let Some(old) = self.entries.remove(&key) {
                self.bytes -= old.lease.bytes().len();
            }
            while !self.entries.is_empty()
                && (self.entries.len() >= max_entries || self.bytes + bytes.len() > max_bytes)
            {
                let victim = *self
                    .entries
                    .iter()
                    .min_by_key(|(_, e)| e.used)
                    .map(|(k, _)| k)
                    .expect("non-empty");
                let old = self.entries.remove(&victim).expect("victim");
                self.bytes -= old.lease.bytes().len();
            }
            let length = bytes.len();
            let version = match self.store.apply(ResourceDelta::Create { bytes }) {
                Ok(v) => v,
                Err(_) => return Err(Vec::new()),
            };
            let lease = match self.store.lease(version) {
                Ok(l) => l,
                Err(_) => return Err(Vec::new()),
            };
            let _ = self.store.retire(version);
            self.clock += 1;
            self.entries.insert(
                key,
                Entry {
                    desc,
                    layout,
                    lease: lease.clone(),
                    used: self.clock,
                },
            );
            self.bytes += length;
            Ok(lease)
        }
    }
    pub(super) fn table() -> Option<&'static Mutex<Table>> {
        static TABLE: OnceLock<Option<Mutex<Table>>> = OnceLock::new();
        TABLE
            .get_or_init(|| Table::new(MAX_ENTRIES, MAX_BYTES).map(Mutex::new))
            .as_ref()
    }
}
#[derive(Clone)]
pub struct PacketDraw {
    pub item: u64,
    pub partition: u64,
    pub obligations: u64,
    pub pipeline: u32,
    pub count: u32,
    pub vertices: raw::BufferUse,
    pub indices: raw::BufferUse,
    pub buffers: Vec<raw::Binding>,
    pub textures: Vec<raw::TextureUse>,
}
#[derive(Clone)]
pub struct PacketPass {
    pub source: Source,
    pub color: Attachment,
    pub depth: Attachment,
    pub draws: Vec<PacketDraw>,
}
#[derive(Clone)]
pub struct ScenePacket {
    pub source: Source,
    pub intended: u64,
    pub flags: u32,
    pub programs: Vec<Program>,
    pub pipelines: Vec<Pipeline>,
    pub buffers: Vec<Vec<u8>>,
    pub produced: BTreeMap<u32, u64>,
    pub dispatches: Vec<compute::Dispatch>,
    pub images: Vec<ImagePayload>,
    pub imports: Vec<raw::ImageRequirement>,
    pub views: Vec<raw::ImageView>,
    pub view_dimensions: Vec<Dimension>,
    pub viewports: Option<Vec<Viewport>>,
    pub stencils: Option<Vec<raw::StencilAttachment>>,
    pub samplers: Vec<raw::Sampler>,
    pub passes: Vec<PacketPass>,
    pub items: Vec<raw::Item>,
    pub output: u32,
}
/// Failure-only private runtime evidence. Status values/admission remain exact;
/// neither shader bytes nor resource payloads are printed. One process emits
/// at most32 short records, including nested stage context for its first errors.
fn packet_failure(
    source: Source,
    stage: &'static str,
    index: Option<usize>,
    reason: impl std::fmt::Debug,
    status: i32,
) -> i32 {
    use std::sync::atomic::{AtomicUsize, Ordering};
    static REPORTED: AtomicUsize = AtomicUsize::new(0);
    if REPORTED
        .try_update(Ordering::Relaxed, Ordering::Relaxed, |n| {
            if n < 32 { Some(n + 1) } else { None }
        })
        .is_ok()
    {
        let reason: String = format!("{reason:?}").chars().take(512).collect();
        eprintln!(
            "[mag native packet failure] source={}/{}/{}/{} stage={stage} index={index:?} status={status} reason={reason}",
            source.engine_update, source.cohort, source.publication, source.pass_occurrence
        );
    }
    status
}
fn words(k: &mut Vec<u8>, v: &[u32]) {
    for x in v {
        k.extend(x.to_le_bytes())
    }
}
fn bytes(k: &mut Vec<u8>, v: &[u8]) {
    k.extend((v.len() as u64).to_le_bytes());
    k.extend(v)
}
fn format(f: u32) -> Result<ImageFormat, i32> {
    ImageFormat::from_native(f).map_err(|_| -2)
}
fn stage(s: u32) -> Result<Stage, i32> {
    match s {
        0 => Ok(Stage::Vertex),
        1 => Ok(Stage::Fragment),
        _ => Err(-2),
    }
}
struct Budget {
    remaining: u64,
}
impl Budget {
    fn take(&mut self, n: u64) -> Result<(), i32> {
        self.remaining = self.remaining.checked_sub(n).ok_or(-5)?;
        Ok(())
    }
    unsafe fn copy<T: Copy>(&mut self, p: *const T, n: usize) -> Result<Vec<T>, i32> {
        self.take((n as u64).checked_mul(size_of::<T>() as u64).ok_or(-5)?)?;
        let s = unsafe { slice(p, n) }?;
        let mut v = Vec::new();
        v.try_reserve_exact(n).map_err(|_| -5)?;
        v.extend_from_slice(s);
        Ok(v)
    }
}
impl ScenePacket {
    /// # Safety
    /// Every declared synchronous range is valid and aligned. All bytes are copied;
    /// the host image and frame are pinned separately by worker admission.
    pub unsafe fn copy(d: &raw::Desc, max_draws: usize, max_bytes: u64) -> Result<Self, i32> {
        unsafe { Self::copy_impl(d, max_draws, max_bytes, None, None, None) }.map_err(|status| {
            packet_failure(d.source, "copy/v1", None, "copy-or-validation", status)
        })
    }
    /// # Safety
    /// Same synchronous range contract as copy; V2 tables are complete and valid.
    pub unsafe fn copy_v2(d: &raw::DescV2, max_draws: usize, max_bytes: u64) -> Result<Self, i32> {
        unsafe { Self::copy_v2_impl(d, max_draws, max_bytes, None) }.map_err(|status| {
            packet_failure(d.base.source, "copy/v2", None, "copy-or-validation", status)
        })
    }
    /// # Safety
    /// All V3/V2 ranges remain readable through the synchronous copy. Source
    /// full-write flags require concrete producer coverage qualification.
    pub unsafe fn copy_v3(d: &raw::DescV3, max_draws: usize, max_bytes: u64) -> Result<Self, i32> {
        checked_v3_layout()?;
        if d.abi_version != 3
            || d.struct_size as usize != size_of::<raw::DescV3>()
            || d.reserved != 0
            || d.stencil_count != d.base.base.pass_count
            || d.stencil_count > 16
        {
            return Err(-1);
        }
        let stencils = unsafe { slice(d.stencils, d.stencil_count as usize) }?.to_vec();
        unsafe { Self::copy_v2_impl(&d.base, max_draws, max_bytes, Some(stencils)) }.map_err(
            |status| {
                packet_failure(
                    d.base.base.source,
                    "copy/v3",
                    None,
                    "copy-or-validation",
                    status,
                )
            },
        )
    }
    /// # Safety
    /// Same copy contract; requirement records are exact source-bound external inputs.
    pub unsafe fn copy_imported_v3(
        d: &raw::DescV3,
        requirements: &[raw::ImageRequirement],
        max_draws: usize,
        max_bytes: u64,
    ) -> Result<Self, i32> {
        if requirements.len() > 1024 {
            return Err(-5);
        }
        let metadata = (requirements.len() as u64)
            .checked_mul(size_of::<raw::ImageBinding>() as u64)
            .ok_or(-5)?;
        let mut packet =
            unsafe { Self::copy_v3(d, max_draws, max_bytes.checked_sub(metadata).ok_or(-5)?) }?;
        packet
            .imports
            .try_reserve_exact(requirements.len())
            .map_err(|_| -5)?;
        packet.imports.extend_from_slice(requirements);
        packet.validate(max_draws, max_bytes).map_err(|status| {
            packet_failure(packet.source, "copy/validate", None, "owned-packet", status)
        })?;
        Ok(packet)
    }
    unsafe fn copy_v2_impl(
        d: &raw::DescV2,
        max_draws: usize,
        max_bytes: u64,
        stencils: Option<Vec<raw::StencilAttachment>>,
    ) -> Result<Self, i32> {
        checked_v2_layout()?;
        if d.abi_version != 2
            || d.struct_size as usize != size_of::<raw::DescV2>()
            || d.reserved != 0
            || d.reserved1 != 0
            || d.viewport_count != d.base.pass_count
            || d.viewport_count > 16
            || d.view_dimension_count != d.base.view_count
            || d.view_dimension_count > 4096
        {
            return Err(-1);
        }
        let mut dimensions = Vec::new();
        for (index, dimension) in
            unsafe { slice(d.view_dimensions, d.view_dimension_count as usize) }?
                .iter()
                .enumerate()
        {
            dimensions.push(match dimension {
                2 => Dimension::D2,
                4 => Dimension::Cube,
                _ => {
                    return Err(packet_failure(
                        d.base.source,
                        "copy/view-dimension",
                        Some(index),
                        dimension,
                        -2,
                    ));
                }
            })
        }
        let viewports = unsafe { slice(d.viewports, d.viewport_count as usize) }?
            .iter()
            .map(|v| Viewport {
                x: v.x,
                y: v.y,
                width: v.width,
                height: v.height,
                min_depth: v.min_depth,
                max_depth: v.max_depth,
                scissor_x: v.scissor_x,
                scissor_y: v.scissor_y,
                scissor_width: v.scissor_width,
                scissor_height: v.scissor_height,
                known_fields: v.known_fields,
                reserved: v.reserved,
            })
            .collect();
        unsafe {
            Self::copy_impl(
                &d.base,
                max_draws,
                max_bytes,
                Some(dimensions),
                Some(viewports),
                stencils,
            )
        }
    }
    unsafe fn copy_impl(
        d: &raw::Desc,
        max_draws: usize,
        max_bytes: u64,
        dimensions: Option<Vec<Dimension>>,
        viewports: Option<Vec<Viewport>>,
        stencils: Option<Vec<raw::StencilAttachment>>,
    ) -> Result<Self, i32> {
        checked_layout()?;
        let cap = limits()?;
        if d.buffer_count as u64 + d.image_count as u64 > cap.max_resources
            || d.image_count as u64 > cap.max_images
        {
            return Err(-5);
        }
        if !header::<raw::Desc>(d.abi_version, d.struct_size)
            || d.reserved != 0
            || d.flags & !1 != 0
            || d.source.cohort == 0
            || d.source.publication == 0
            || d.source.pass_occurrence == 0
            || d.item_count as u64 != d.intended_items
            || d.item_count as usize > max_draws
            || d.program_count > 1024
            || d.pipeline_count > 1024
            || d.buffer_count > 4096
            || d.image_count > 1024
            || d.view_count > 4096
            || d.sampler_count > 4096
            || d.pass_count == 0
            || d.pass_count > 16
            || d.output_image >= d.image_count
        {
            return Err(-1);
        }
        if d.dispatch_count > 32 {
            return Err(packet_failure(
                d.source,
                "copy/dispatch",
                None,
                d.dispatch_count,
                -5,
            ));
        }
        let extra = stencils.as_ref().map_or(0, |s| {
            s.len() as u64 * size_of::<raw::StencilAttachment>() as u64
        }) + dimensions.as_ref().map_or(0, |d| d.len() as u64 * 4)
            + viewports
                .as_ref()
                .map_or(0, |v| v.len() as u64 * size_of::<Viewport>() as u64);
        let mut budget = Budget {
            remaining: max_bytes.checked_sub(extra).ok_or(-5)?,
        };
        let mut programs = Vec::new();
        for p in unsafe { slice(d.programs, d.program_count as usize) }? {
            if p.stage > 2
                || p.reserved & !raw::PROGRAM_STORED_DEPTH_AUX != 0
                || p.reserved != 0 && (p.stage != 1 || p.shader.language != 1)
                || p.reserved1 != 0
                || p.buffer_count > 16
                || p.texture_count > 16
                || p.sampler_count > 16
                || p.vertex_location_count > 16
                || !(1..=3).contains(&p.shader.language)
                || p.shader.source_length == 0
                || p.shader.entry_length == 0
                || p.shader.entry_length > 1024
            {
                return Err(-1);
            }
            let source_length = usize::try_from(p.shader.source_length).map_err(|_| -5)?;
            let (source, spirv) = if p.shader.language == 3 {
                if !source_length.is_multiple_of(4) {
                    return Err(-1);
                }
                (vec![], unsafe {
                    budget.copy(p.shader.source.cast::<u32>(), source_length / 4)
                }?)
            } else {
                (
                    unsafe { budget.copy(p.shader.source.cast::<u8>(), source_length) }?,
                    vec![],
                )
            };
            let entry = unsafe {
                budget.copy(p.shader.entry.cast::<u8>(), p.shader.entry_length as usize)
            }?;
            if entry.contains(&0) {
                return Err(-1);
            }
            programs.push(Program {
                flags: p.reserved,
                stage: p.stage,
                language: p.shader.language,
                source,
                spirv,
                entry,
                buffers: unsafe { budget.copy(p.buffers, p.buffer_count as usize) }?,
                textures: unsafe { budget.copy(p.textures, p.texture_count as usize) }?,
                samplers: unsafe { budget.copy(p.samplers, p.sampler_count as usize) }?,
                vertex_locations: unsafe {
                    budget.copy(p.vertex_locations, p.vertex_location_count as usize)
                }?,
                threads: p.threads,
            });
        }
        let mut pipelines = Vec::new();
        for p in unsafe { slice(d.pipelines, d.pipeline_count as usize) }? {
            if p.reserved != 0 || p.reserved1 != 0 || p.attribute_count > 16 || p.texture_count > 32
            {
                return Err(-1);
            }
            pipelines.push(Pipeline {
                vertex: p.vertex_program,
                fragment: p.fragment_program,
                stride: p.vertex_stride,
                attributes: unsafe { budget.copy(p.attributes, p.attribute_count as usize) }?,
                color: p.color_format,
                depth: p.depth_format,
                binding_model: p.binding_model,
                textures: unsafe { budget.copy(p.textures, p.texture_count as usize) }?,
                state: p.state,
            });
        }
        let mut buffers = Vec::new();
        let mut produced = BTreeMap::new();
        for (index, b) in unsafe { slice(d.buffers, d.buffer_count as usize) }?
            .iter()
            .enumerate()
        {
            if b.reserved != 0 || b.length == 0 {
                return Err(-1);
            }
            match b.role {
                0 => buffers.push(unsafe {
                    budget.copy(
                        b.bytes.cast::<u8>(),
                        usize::try_from(b.length).map_err(|_| -5)?,
                    )
                }?),
                1 => {
                    if !b.bytes.is_null() {
                        return Err(-1);
                    }
                    budget.take(b.length)?;
                    produced.insert(index as u32, b.length);
                    buffers.push(Vec::new());
                }
                _ => {
                    return Err(packet_failure(
                        d.source,
                        "copy/buffer-role",
                        Some(index),
                        b.role,
                        -2,
                    ));
                }
            }
        }
        let mut dispatches = Vec::new();
        for dispatch in unsafe { slice(d.dispatches, d.dispatch_count as usize) }? {
            if dispatch.reserved != 0 || dispatch.binding_count == 0 || dispatch.binding_count > 16
            {
                return Err(-1);
            }
            dispatches.push(compute::Dispatch {
                program: dispatch.program,
                groups: dispatch.groups,
                bindings: unsafe {
                    budget.copy(dispatch.bindings, dispatch.binding_count as usize)
                }?,
            });
        }
        let mut images = Vec::new();
        for (index, i) in unsafe { slice(d.images, d.image_count as usize) }?
            .iter()
            .enumerate()
        {
            let desc = ImageDesc {
                width: i.width,
                height: i.height,
                format: format(i.format).map_err(|status| {
                    packet_failure(d.source, "copy/image-format", Some(index), i.format, status)
                })?,
            };
            // `reserved` is the caller's optional content key (0 = none).
            let key = i.reserved;
            if i.width == 0
                || i.height == 0
                || i.width > 8192
                || i.height > 8192
                || i.mip_count > 14
                || i.layers > 6
            {
                return Err(-1);
            }
            let dimension = match i.dimension {
                2 => Dimension::D2,
                4 => Dimension::Cube,
                _ => {
                    return Err(packet_failure(
                        d.source,
                        "copy/image-dimension",
                        Some(index),
                        i.dimension,
                        -2,
                    ));
                }
            };
            let mut layout = ImageLayout {
                dimension,
                depth: i.depth,
                layers: i.layers,
                mip_count: i.mip_count,
                samples: i.samples,
                subresources: vec![],
            };
            if i.subresource_count > 84 {
                return Err(-1);
            }
            let source = unsafe { slice(i.subresources, i.subresource_count as usize) }?;
            let bytes = if source.is_empty() {
                layout.validate(desc, None).map_err(|_| -1)?;
                None
            } else {
                if source.len() != i.layers as usize * i.mip_count as usize {
                    return Err(-1);
                }
                // Layout first (no byte reads), so a resident key can skip them.
                let mut offset = 0u64;
                for s in source {
                    let level = desc
                        .format
                        .level_layout(s.width, s.height)
                        .map_err(|_| -1)?;
                    if s.row_bytes as u64 != level.row_bytes || s.length != level.bytes {
                        return Err(-1);
                    }
                    let aligned = offset.checked_add(15).ok_or(-5)? & !15;
                    layout.subresources.push(Subresource {
                        mip: s.mip,
                        layer: s.layer,
                        width: s.width,
                        height: s.height,
                        depth: s.depth,
                        row_bytes: s.row_bytes,
                        offset: aligned,
                        length: s.length,
                    });
                    offset = aligned.checked_add(s.length).ok_or(-5)?;
                }
                let padded = offset.checked_add(15).ok_or(-5)? & !15;
                layout.validate(desc, Some(padded)).map_err(|_| -1)?;
                // Budget is charged exactly as for an owned copy either way.
                budget.take(padded)?;
                let resident = (key != 0)
                    .then(keyed_images::table)
                    .flatten()
                    .and_then(|t| t.lock().ok()?.get(key, desc, &layout));
                let pack = || -> Result<Vec<u8>, i32> {
                    let mut packed = Vec::new();
                    packed
                        .try_reserve_exact(usize::try_from(padded).map_err(|_| -5)?)
                        .map_err(|_| -5)?;
                    for (s, placed) in source.iter().zip(&layout.subresources) {
                        packed.resize(placed.offset as usize, 0);
                        // Single copy straight from the caller's range.
                        let data = unsafe {
                            slice(
                                s.bytes.cast::<u8>(),
                                usize::try_from(s.length).map_err(|_| -5)?,
                            )
                        }?;
                        packed.extend_from_slice(data);
                    }
                    packed.resize(padded as usize, 0);
                    Ok(packed)
                };
                if let Some(lease) = resident {
                    Some(ImageBytes::Resident(lease))
                } else {
                    let packed = pack()?;
                    let table = (key != 0).then(keyed_images::table).flatten();
                    match table.and_then(|t| t.lock().ok()) {
                        Some(mut t) => match t.insert(
                            key,
                            desc,
                            layout.clone(),
                            packed,
                            keyed_images::MAX_ENTRIES,
                            keyed_images::MAX_BYTES,
                        ) {
                            Ok(lease) => Some(ImageBytes::Resident(lease)),
                            Err(back) if back.len() == padded as usize => {
                                Some(ImageBytes::Owned(back))
                            }
                            // The table's store refused the bytes it consumed:
                            // keep an ordinary owned copy instead.
                            Err(_) => Some(ImageBytes::Owned(pack()?)),
                        },
                        None => Some(ImageBytes::Owned(packed)),
                    }
                }
            };
            images.push(ImagePayload {
                desc,
                layout: Some(layout),
                bytes,
            });
        }
        let views = unsafe { budget.copy(d.views, d.view_count as usize) }?;
        let samplers = unsafe { budget.copy(d.samplers, d.sampler_count as usize) }?;
        let items = unsafe { budget.copy(d.items, d.item_count as usize) }?;
        let mut passes = Vec::new();
        let mut count = 0usize;
        for p in unsafe { slice(d.passes, d.pass_count as usize) }? {
            count = count.checked_add(p.draw_count as usize).ok_or(-5)?;
            if p.reserved != 0 || count > max_draws {
                return Err(-5);
            }
            budget.take(p.draw_count as u64 * size_of::<PacketDraw>() as u64)?;
            let mut draws = Vec::new();
            for x in unsafe { slice(p.draws, p.draw_count as usize) }? {
                if x.buffer_count > 32 || x.texture_count > 32 {
                    return Err(-1);
                }
                draws.push(PacketDraw {
                    item: x.source_item,
                    partition: x.source_partition,
                    obligations: x.unresolved_obligations,
                    pipeline: x.pipeline,
                    count: x.index_count,
                    vertices: x.vertices,
                    indices: x.indices,
                    buffers: unsafe { budget.copy(x.buffers, x.buffer_count as usize) }?,
                    textures: unsafe { budget.copy(x.textures, x.texture_count as usize) }?,
                });
            }
            passes.push(PacketPass {
                source: p.source,
                color: p.color,
                depth: p.depth,
                draws,
            });
        }
        let packet = Self {
            source: d.source,
            intended: d.intended_items,
            flags: d.flags,
            programs,
            pipelines,
            buffers,
            produced,
            dispatches,
            images,
            imports: Vec::new(),
            view_dimensions: dimensions.unwrap_or_else(|| vec![Dimension::D2; views.len()]),
            viewports,
            stencils,
            views,
            samplers,
            passes,
            items,
            output: d.output_image,
        };
        packet.validate(max_draws, max_bytes).map_err(|status| {
            packet_failure(packet.source, "copy/validate", None, "owned-packet", status)
        })?;
        Ok(packet)
    }
}
unsafe extern "C" {
    fn mag_native_sampler_state_validate(state: *const raw::Sampler) -> i32;
}
fn sampler(state: &raw::Sampler) -> Result<Sampler, i32> {
    let status = unsafe { mag_native_sampler_state_validate(state) };
    if status != 0 {
        return Err(status);
    }
    Ok(Sampler::Explicit(*state))
}
impl ScenePacket {
    fn view_desc(&self, index: usize) -> Result<ViewDesc, i32> {
        let v = self.views.get(index).ok_or_else(|| {
            packet_failure(
                self.source,
                "validate/view-reference",
                Some(index),
                self.views.len(),
                -1,
            )
        })?;
        let dimension = *self.view_dimensions.get(index).ok_or_else(|| {
            packet_failure(
                self.source,
                "validate/view-dimension-reference",
                Some(index),
                self.view_dimensions.len(),
                -1,
            )
        })?;
        let mapping = [4, 5, 0, 1, 2, 3];
        let mut remap = [0; 4];
        for (dest, source) in remap.iter_mut().zip(v.remap) {
            *dest = *mapping.get(source as usize).ok_or_else(|| {
                packet_failure(self.source, "validate/view-remap", Some(index), source, -1)
            })?;
        }
        Ok(ViewDesc {
            dimension,
            base_mip: v.base_mip,
            mip_count: v.mip_count,
            base_layer: v.base_layer,
            layer_count: v.layer_count,
            remap,
        })
    }
    fn range(&self, u: raw::BufferUse) -> Result<ByteRange, i32> {
        let b = self.buffers.get(u.buffer as usize).ok_or_else(|| {
            packet_failure(
                self.source,
                "validate/buffer-reference",
                Some(u.buffer as usize),
                (u.offset, u.length, self.buffers.len()),
                -1,
            )
        })?;
        let length = self
            .produced
            .get(&u.buffer)
            .copied()
            .unwrap_or(b.len() as u64);
        if u.reserved != 0
            || u.length == 0
            || u.offset.checked_add(u.length).ok_or_else(|| {
                packet_failure(
                    self.source,
                    "validate/buffer-range-overflow",
                    Some(u.buffer as usize),
                    (u.offset, u.length),
                    -1,
                )
            })? > length
        {
            return Err(packet_failure(
                self.source,
                "validate/buffer-range",
                Some(u.buffer as usize),
                (u.offset, u.length, u.reserved, length),
                -1,
            ));
        }
        Ok(ByteRange {
            offset: u.offset,
            length: u.length,
        })
    }
    fn validate(&self, max_draws: usize, max_bytes: u64) -> Result<(), i32> {
        macro_rules! invalid {
            ($stage:literal, $index:expr, $($reason:tt)*) => {
                packet_failure(self.source, concat!("validate/", $stage), $index, format_args!($($reason)*), -1)
            };
        }
        if self.items.len() as u64 != self.intended
            || self.images.get(self.output as usize).is_none()
            || self.passes.is_empty()
        {
            return Err(invalid!(
                "population-output",
                None,
                "items={} intended={} output={} images={} passes={}",
                self.items.len(),
                self.intended,
                self.output,
                self.images.len(),
                self.passes.len()
            ));
        }
        for (program_index, p) in self.programs.iter().enumerate() {
            if p.flags & !raw::PROGRAM_STORED_DEPTH_AUX != 0
                || p.flags != 0 && (p.stage != 1 || p.language != 1)
            {
                return Err(invalid!(
                    "program-flags",
                    Some(program_index),
                    "flags={} stage={} language={}",
                    p.flags,
                    p.stage,
                    p.language
                ));
            }
            let mut slots = BTreeSet::new();
            for b in &p.buffers {
                if b.slot >= 16
                    || !slots.insert(b.slot)
                    || !(b.access == 1 || p.stage == 2 && b.access == 2 && b.binding_kind == 0)
                    || b.minimum_bytes == 0
                    || !b.minimum_bytes.is_multiple_of(4)
                    || (p.stage != 2 && b.runtime_array_stride != 0)
                    || (b.runtime_array_stride == 0 && b.runtime_size_index != u32::MAX)
                    || (b.runtime_array_stride != 0
                        && (p.stage != 2
                            || b.runtime_size_index >= 16
                            || !b.runtime_array_stride.is_multiple_of(4)))
                    || b.alignment_bytes < 4
                    || b.alignment_bytes > 256
                    || !b.alignment_bytes.is_power_of_two()
                    || b.binding_kind > 1
                {
                    return Err(invalid!(
                        "program-buffer",
                        Some(program_index),
                        "slot={} access={} bytes={} stride={} size_index={} alignment={} kind={}",
                        b.slot,
                        b.access,
                        b.minimum_bytes,
                        b.runtime_array_stride,
                        b.runtime_size_index,
                        b.alignment_bytes,
                        b.binding_kind
                    ));
                }
            }
            slots.clear();
            for t in &p.textures {
                if t.slot >= 16 || !slots.insert(t.slot) || t.reserved != 0 {
                    return Err(invalid!(
                        "program-texture-slot",
                        Some(program_index),
                        "slot={} reserved={} prior_slots={:?}",
                        t.slot,
                        t.reserved,
                        slots
                    ));
                }
                if !matches!(t.dimension, 2 | 4)
                    || t.sample_kind > 3
                    || t.arrayed != 0
                    || t.multisampled != 0
                {
                    return Err(packet_failure(
                        self.source,
                        "validate/program-texture-shape",
                        Some(program_index),
                        (
                            t.slot,
                            t.dimension,
                            t.sample_kind,
                            t.arrayed,
                            t.multisampled,
                        ),
                        -2,
                    ));
                }
            }
            slots.clear();
            for s in &p.samplers {
                if s.slot >= 16 || s.comparison > 1 || !slots.insert(s.slot) {
                    return Err(invalid!(
                        "program-sampler-slot",
                        Some(program_index),
                        "slot={} comparison={} prior_slots={:?}",
                        s.slot,
                        s.comparison,
                        slots
                    ));
                }
            }
            slots.clear();
            for loc in &p.vertex_locations {
                if *loc >= 16 || !slots.insert(*loc) {
                    return Err(invalid!(
                        "program-vertex-locations",
                        Some(program_index),
                        "location={} slots={:?}",
                        loc,
                        slots
                    ));
                }
            }
        }
        self.validate_compute()?;
        for (pipeline_index, p) in self.pipelines.iter().enumerate() {
            raster(&p.state).map_err(|status| {
                packet_failure(
                    self.source,
                    "validate/pipeline-raster",
                    Some(pipeline_index),
                    (
                        p.state.known_fields,
                        p.state.unresolved_obligations,
                        p.state.depth_bounds_enabled,
                        p.state.bias_enabled,
                        p.state.stencil_enabled,
                    ),
                    status,
                )
            })?;
            let vp = self.programs.get(p.vertex as usize).ok_or_else(|| {
                invalid!(
                    "pipeline-vertex-program",
                    Some(pipeline_index),
                    "program={} programs={}",
                    p.vertex,
                    self.programs.len()
                )
            })?;
            let fp = if p.fragment == u32::MAX {
                None
            } else {
                Some(self.programs.get(p.fragment as usize).ok_or_else(|| {
                    invalid!(
                        "pipeline-fragment-program",
                        Some(pipeline_index),
                        "program={} programs={}",
                        p.fragment,
                        self.programs.len()
                    )
                })?)
            };
            let aux = fp.is_some_and(|f| f.flags == raw::PROGRAM_STORED_DEPTH_AUX);
            if aux != (p.state.depth_bounds_enabled != 0)
                || aux
                    && (p.binding_model != 1
                        || p.textures
                            .iter()
                            .any(|t| t.stage == 1 && t.texture_slot == 15)
                        || fp.is_some_and(|f| {
                            f.buffers.iter().any(|b| b.slot == 2)
                                || f.textures.iter().any(|t| t.slot == 15)
                        }))
            {
                return Err(packet_failure(
                    self.source,
                    "validate/stored-depth-aux",
                    Some(pipeline_index),
                    (aux, p.binding_model, p.state.depth_bounds_enabled),
                    -2,
                ));
            }
            if vp.stage != 0
                || fp.is_some_and(|f| f.stage != 1)
                || p.binding_model > 1
                || (p.color != 0 && !format(p.color)?.info().color_attachment)
                || p.depth != 0 && !format(p.depth)?.info().depth_attachment
                || p.state.stencil_enabled != 0 && !format(p.depth)?.has_stencil()
                || p.color == 2
                || p.color != 0 && fp.is_none()
                || p.stride == 0
                || p.stride > 2048
                || !p.stride.is_multiple_of(4)
            {
                return Err(invalid!(
                    "pipeline-layout",
                    Some(pipeline_index),
                    "vertex={} fragment={} vs_stage={} fs_stage={:?} binding_model={} color={} depth={} stencil={} stride={}",
                    p.vertex,
                    p.fragment,
                    vp.stage,
                    fp.map(|f| f.stage),
                    p.binding_model,
                    p.color,
                    p.depth,
                    p.state.stencil_enabled,
                    p.stride
                ));
            }
            let mut slots = BTreeSet::new();
            let mut ranges = Vec::new();
            for a in &p.attributes {
                let length = match a.format {
                    1..=4 => a.format * 4,
                    5 => 4,
                    _ => {
                        return Err(packet_failure(
                            self.source,
                            "validate/attribute-format",
                            Some(pipeline_index),
                            (a.location, a.format),
                            -2,
                        ));
                    }
                };
                if a.reserved != 0
                    || a.location >= 16
                    || !slots.insert(a.location)
                    || !a.offset.is_multiple_of(4)
                    || a.offset.checked_add(length).ok_or_else(|| {
                        invalid!(
                            "attribute-offset-overflow",
                            Some(pipeline_index),
                            "location={} offset={} bytes={}",
                            a.location,
                            a.offset,
                            length
                        )
                    })? > p.stride
                {
                    return Err(invalid!(
                        "attribute-layout",
                        Some(pipeline_index),
                        "vertex={} location={} format={} offset={} bytes={} stride={} reserved={} slots={:?}",
                        p.vertex,
                        a.location,
                        a.format,
                        a.offset,
                        length,
                        p.stride,
                        a.reserved,
                        slots
                    ));
                }
                if ranges
                    .iter()
                    .any(|&(start, end)| a.offset < end && start < a.offset + length)
                {
                    return Err(invalid!(
                        "attribute-overlap",
                        Some(pipeline_index),
                        "location={} offset={} bytes={} prior_ranges={:?}",
                        a.location,
                        a.offset,
                        length,
                        ranges
                    ));
                }
                ranges.push((a.offset, a.offset + length));
            }
            if slots != vp.vertex_locations.iter().copied().collect() {
                return Err(invalid!(
                    "attribute-reflection",
                    Some(pipeline_index),
                    "vertex={} stride={} attribute_slots={:?} reflected_slots={:?}",
                    p.vertex,
                    p.stride,
                    slots,
                    vp.vertex_locations
                ));
            }
            let mut uses = BTreeSet::new();
            for t in &p.textures {
                if t.reserved != 0
                    || t.comparison > 1
                    || !uses.insert((t.stage, t.texture_slot, t.sampler_slot))
                {
                    return Err(invalid!(
                        "pipeline-texture-layout",
                        Some(pipeline_index),
                        "stage={} texture={} sampler={} comparison={} reserved={}",
                        t.stage,
                        t.texture_slot,
                        t.sampler_slot,
                        t.comparison,
                        t.reserved
                    ));
                }
                let program = match t.stage {
                    0 => vp,
                    1 => fp.ok_or_else(|| {
                        invalid!(
                            "pipeline-texture-fragment",
                            Some(pipeline_index),
                            "fragment={}",
                            p.fragment
                        )
                    })?,
                    _ => {
                        return Err(invalid!(
                            "pipeline-texture-stage",
                            Some(pipeline_index),
                            "stage={}",
                            t.stage
                        ));
                    }
                };
                let reflected = program
                    .textures
                    .iter()
                    .find(|r| r.slot == t.texture_slot)
                    .ok_or_else(|| {
                        invalid!(
                            "pipeline-texture-reflection",
                            Some(pipeline_index),
                            "stage={} slot={} reflected_count={}",
                            t.stage,
                            t.texture_slot,
                            program.textures.len()
                        )
                    })?;
                let reflected_sampler = program
                    .samplers
                    .iter()
                    .find(|r| r.slot == t.sampler_slot)
                    .ok_or_else(|| {
                        invalid!(
                            "pipeline-sampler-reflection",
                            Some(pipeline_index),
                            "stage={} slot={} reflected_count={}",
                            t.stage,
                            t.sampler_slot,
                            program.samplers.len()
                        )
                    })?;
                if reflected.sample_kind != format(t.format)?.info().sample_kind
                    || reflected_sampler.comparison != t.comparison
                {
                    return Err(packet_failure(
                        self.source,
                        "validate/texture-reflection",
                        Some(pipeline_index),
                        (
                            t.stage,
                            t.texture_slot,
                            t.format,
                            t.comparison,
                            reflected.sample_kind,
                            reflected_sampler.comparison,
                        ),
                        -2,
                    ));
                }
                format(t.format)?;
            }
            for (stage, program) in [(0, Some(vp)), (1, fp)] {
                if let Some(program) = program
                    && (program.textures.iter().any(|t| {
                        !p.textures
                            .iter()
                            .any(|x| x.stage == stage && x.texture_slot == t.slot)
                    }) || program.samplers.iter().any(|s| {
                        !p.textures
                            .iter()
                            .any(|x| x.stage == stage && x.sampler_slot == s.slot)
                    }))
                {
                    return Err(invalid!(
                        "pipeline-missing-texture-layout",
                        Some(pipeline_index),
                        "stage={} reflected_textures={} reflected_samplers={} layouts={}",
                        stage,
                        program.textures.len(),
                        program.samplers.len(),
                        p.textures.len()
                    ));
                }
            }
        }
        if self.view_dimensions.len() != self.views.len() {
            return Err(invalid!(
                "view-dimension-count",
                None,
                "dimensions={} views={}",
                self.view_dimensions.len(),
                self.views.len()
            ));
        }
        for (index, v) in self.views.iter().enumerate() {
            if v.reserved != 0 || v.image as usize >= self.images.len() {
                return Err(invalid!(
                    "view-image",
                    Some(index),
                    "image={} images={} reserved={}",
                    v.image,
                    self.images.len(),
                    v.reserved
                ));
            }
            let image = &self.images[v.image as usize];
            let shape = image.layout.clone().unwrap_or_default();
            self.view_desc(index)?.validate(&shape).map_err(|error| {
                invalid!(
                    "view-layout",
                    Some(index),
                    "error={:?} image={} mip={}/{} layer={}/{} remap={:?}",
                    error,
                    v.image,
                    v.base_mip,
                    v.mip_count,
                    v.base_layer,
                    v.layer_count,
                    v.remap
                )
            })?;
        }
        let mut external_indices = BTreeSet::new();
        let mut source_keys = BTreeSet::new();
        for (index, r) in self.imports.iter().enumerate() {
            let image = self.images.get(r.image as usize).ok_or_else(|| {
                invalid!(
                    "import-image",
                    Some(index),
                    "image={} images={}",
                    r.image,
                    self.images.len()
                )
            })?;
            let planes = if image.desc.format.has_stencil() {
                6
            } else if image.desc.format.info().depth_attachment {
                2
            } else {
                1
            };
            if r.source_key == 0
                || r.required_initialized_planes & !planes != 0
                || !external_indices.insert(r.image)
                || !source_keys.insert(r.source_key)
                || image.bytes.is_some()
                || image.layout.as_ref().is_some_and(|l| {
                    l.dimension != Dimension::D2
                        || l.mip_count != 1
                        || l.layers != 1
                        || l.samples != 1
                        || !l.subresources.is_empty()
                })
            {
                return Err(invalid!(
                    "import-layout",
                    Some(index),
                    "image={} key={} planes={} allowed_planes={} bytes={} has_layout={}",
                    r.image,
                    r.source_key,
                    r.required_initialized_planes,
                    planes,
                    image.bytes.is_some(),
                    image.layout.is_some()
                ));
            }
        }
        for (index, image) in self.images.iter().enumerate() {
            if let Some(layout) = &image.layout {
                layout
                    .validate(image.desc, image.bytes.as_ref().map(|b| b.len() as u64))
                    .map_err(|error| {
                        invalid!(
                            "image-layout",
                            Some(index),
                            "error={:?} format={:?} width={} height={} bytes={:?}",
                            error,
                            image.desc.format,
                            image.desc.width,
                            image.desc.height,
                            image.bytes.as_ref().map(|b| b.len())
                        )
                    })?;
            }
        }
        if let Some(viewports) = &self.viewports {
            if viewports.len() != self.passes.len() {
                return Err(invalid!(
                    "viewport-count",
                    None,
                    "viewports={} passes={}",
                    viewports.len(),
                    self.passes.len()
                ));
            }
            for (index, (pass, viewport)) in self.passes.iter().zip(viewports).enumerate() {
                let image = if pass.color.image != u32::MAX {
                    pass.color.image
                } else {
                    pass.depth.image
                };
                let desc = self
                    .images
                    .get(image as usize)
                    .ok_or_else(|| {
                        invalid!(
                            "viewport-image",
                            Some(index),
                            "image={} images={}",
                            image,
                            self.images.len()
                        )
                    })?
                    .desc;
                viewport
                    .validate(desc.width, desc.height)
                    .map_err(|error| {
                        invalid!(
                            "viewport-layout",
                            Some(index),
                            "error={:?} image={} extent={}x{} viewport={:?}",
                            error,
                            image,
                            desc.width,
                            desc.height,
                            viewport
                        )
                    })?;
            }
        }
        for (index, s) in self.samplers.iter().enumerate() {
            sampler(s).map_err(|status| {
                packet_failure(
                    self.source,
                    "validate/sampler",
                    Some(index),
                    (
                        s.min_filter,
                        s.mag_filter,
                        s.mip_filter,
                        s.comparison_enabled,
                        s.comparison,
                    ),
                    status,
                )
            })?;
        }
        let mut item_set = BTreeSet::new();
        for (index, i) in self.items.iter().enumerate() {
            if i.reserved > raw::ITEM_POC_OMITTED_PRODUCER
                || i.pass as usize >= self.passes.len()
                || !item_set.insert((i.pass, i.source_item))
            {
                return Err(invalid!(
                    "item-identity",
                    Some(index),
                    "pass={} source_item={} reserved={} passes={}",
                    i.pass,
                    i.source_item,
                    i.reserved,
                    self.passes.len()
                ));
            }
            if i.reserved != raw::ITEM_DRAW_REQUIRED
                && (i.unresolved_obligations != 0
                    || (i.source_item & (1 << 63) != 0)
                        != (i.reserved == raw::ITEM_POC_OMITTED_PRODUCER))
            {
                return Err(invalid!(
                    "no-draw-kind-domain",
                    Some(index),
                    "kind={} item={} obligations={}",
                    i.reserved,
                    i.source_item,
                    i.unresolved_obligations
                ));
            }
            if i.unresolved_obligations != 0 && self.flags & 1 == 0 {
                return Err(packet_failure(
                    self.source,
                    "validate/item-obligations",
                    Some(i.pass as usize),
                    (i.source_item, i.unresolved_obligations, self.flags),
                    -2,
                ));
            }
        }
        if let Some(stencils) = &self.stencils {
            if stencils.len() != self.passes.len() {
                return Err(invalid!(
                    "stencil-count",
                    None,
                    "stencils={} passes={}",
                    stencils.len(),
                    self.passes.len()
                ));
            }
            for (index, (pass, s)) in self.passes.iter().zip(stencils).enumerate() {
                let combined = pass.depth.image != u32::MAX
                    && self
                        .images
                        .get(pass.depth.image as usize)
                        .ok_or_else(|| {
                            invalid!(
                                "stencil-depth-image",
                                Some(index),
                                "image={}",
                                pass.depth.image
                            )
                        })?
                        .desc
                        .format
                        .has_stencil();
                if s.reserved != 0
                    || s.flags & !1 != 0
                    || s.load > 2
                    || s.store > 1
                    || s.clear_value > 255
                    || (combined && (s.image != pass.depth.image))
                    || (!combined && (s.image != u32::MAX || s.flags != 0))
                {
                    return Err(invalid!(
                        "stencil-layout",
                        Some(index),
                        "image={} depth_image={} combined={} flags={} reserved={} load={} store={} clear={}",
                        s.image,
                        pass.depth.image,
                        combined,
                        s.flags,
                        s.reserved,
                        s.load,
                        s.store,
                        s.clear_value
                    ));
                }
            }
        } else if self.images.iter().any(|i| i.desc.format.has_stencil()) {
            return Err(packet_failure(
                self.source,
                "validate/missing-stencil-table",
                None,
                "stencil-image",
                -2,
            ));
        }
        let mut draws = 0usize;
        let mut seen = BTreeSet::new();
        for (pass, p) in self.passes.iter().enumerate() {
            if p.source.cohort == 0 || p.source.publication == 0 || p.source.pass_occurrence == 0 {
                return Err(invalid!(
                    "pass-source",
                    Some(pass),
                    "cohort={} publication={} occurrence={}",
                    p.source.cohort,
                    p.source.publication,
                    p.source.pass_occurrence
                ));
            }
            for a in [p.color, p.depth] {
                if a.reserved != 0
                    || a.load > if self.stencils.is_some() { 2 } else { 1 }
                    || a.store > 1
                    || a.image != u32::MAX && a.image as usize >= self.images.len()
                    || a.clear.iter().any(|x| !x.is_finite())
                {
                    return Err(invalid!(
                        "pass-attachment",
                        Some(pass),
                        "image={} images={} reserved={} load={} store={} clear={:?}",
                        a.image,
                        self.images.len(),
                        a.reserved,
                        a.load,
                        a.store,
                        a.clear
                    ));
                }
            }
            for d in &p.draws {
                draws = draws.checked_add(1).ok_or(-5)?;
                let pipeline = self.pipelines.get(d.pipeline as usize).ok_or_else(|| {
                    invalid!(
                        "draw-pipeline",
                        Some(pass),
                        "item={} partition={} pipeline={} pipelines={}",
                        d.item,
                        d.partition,
                        d.pipeline,
                        self.pipelines.len()
                    )
                })?;
                if !item_set.contains(&(pass as u32, d.item))
                    || !seen.insert((pass as u32, d.item, d.partition))
                    || d.obligations != 0 && self.flags & 1 == 0
                {
                    return Err(invalid!(
                        "draw-identity",
                        Some(pass),
                        "item={} partition={} pipeline={} obligations={} flags={}",
                        d.item,
                        d.partition,
                        d.pipeline,
                        d.obligations,
                        self.flags
                    ));
                }
                let vr = self.range(d.vertices)?;
                let ir = self.range(d.indices)?;
                if d.count == 0
                    || !d.count.is_multiple_of(3)
                    || !vr.offset.is_multiple_of(4)
                    || !vr.length.is_multiple_of(pipeline.stride as u64)
                    || !ir.offset.is_multiple_of(4)
                    || ir.length != u64::from(d.count) * 4
                {
                    return Err(invalid!(
                        "draw-ranges",
                        Some(pass),
                        "item={} partition={} count={} stride={} vertex={}/{} index={}/{}",
                        d.item,
                        d.partition,
                        d.count,
                        pipeline.stride,
                        vr.offset,
                        vr.length,
                        ir.offset,
                        ir.length
                    ));
                }
                if self.produced.contains_key(&d.indices.buffer) {
                    return Err(packet_failure(
                        self.source,
                        "validate/produced-indices",
                        None,
                        d.indices.buffer,
                        -2,
                    ));
                }
                let ib = &self.buffers[d.indices.buffer as usize]
                    [ir.offset as usize..(ir.offset + ir.length) as usize];
                if ib.as_chunks::<4>().0.iter().any(|b| {
                    u32::from_ne_bytes(*b) as u64 >= vr.length / u64::from(pipeline.stride)
                }) {
                    return Err(invalid!(
                        "draw-index-range",
                        Some(pass),
                        "item={} partition={} vertex_count={} index_count={}",
                        d.item,
                        d.partition,
                        vr.length / u64::from(pipeline.stride),
                        d.count
                    ));
                }
                let mut bindings = BTreeSet::new();
                for b in &d.buffers {
                    if !bindings.insert((b.stage, b.slot)) {
                        return Err(invalid!(
                            "draw-buffer-duplicate",
                            Some(pass),
                            "item={} partition={} stage={} slot={}",
                            d.item,
                            d.partition,
                            b.stage,
                            b.slot
                        ));
                    }
                    let range = self.range(b.view)?;
                    let program = match b.stage {
                        0 => &self.programs[pipeline.vertex as usize],
                        1 => self
                            .programs
                            .get(pipeline.fragment as usize)
                            .ok_or_else(|| {
                                invalid!(
                                    "draw-buffer-fragment",
                                    Some(pass),
                                    "item={} fragment={}",
                                    d.item,
                                    pipeline.fragment
                                )
                            })?,
                        _ => {
                            return Err(invalid!(
                                "draw-buffer-stage",
                                Some(pass),
                                "item={} stage={} slot={}",
                                d.item,
                                b.stage,
                                b.slot
                            ));
                        }
                    };
                    let required = program
                        .buffers
                        .iter()
                        .find(|r| r.slot == b.slot)
                        .ok_or_else(|| {
                            invalid!(
                                "draw-buffer-reflection",
                                Some(pass),
                                "item={} partition={} stage={} slot={} reflected_count={}",
                                d.item,
                                d.partition,
                                b.stage,
                                b.slot,
                                program.buffers.len()
                            )
                        })?;
                    if range.length < required.minimum_bytes
                        || !range
                            .offset
                            .is_multiple_of(u64::from(required.alignment_bytes))
                    {
                        return Err(invalid!(
                            "draw-buffer-layout",
                            Some(pass),
                            "item={} partition={} stage={} slot={} offset={} bytes={} required_bytes={} alignment={}",
                            d.item,
                            d.partition,
                            b.stage,
                            b.slot,
                            range.offset,
                            range.length,
                            required.minimum_bytes,
                            required.alignment_bytes
                        ));
                    }
                }
                let expected_buffers = self.programs[pipeline.vertex as usize].buffers.len()
                    + self
                        .programs
                        .get(pipeline.fragment as usize)
                        .map_or(0, |p| p.buffers.len());
                if d.buffers.len() != expected_buffers
                    || d.textures.len() != pipeline.textures.len()
                {
                    return Err(invalid!(
                        "draw-binding-counts",
                        Some(pass),
                        "item={} partition={} buffers={} required_buffers={} textures={} required_textures={}",
                        d.item,
                        d.partition,
                        d.buffers.len(),
                        expected_buffers,
                        d.textures.len(),
                        pipeline.textures.len()
                    ));
                }
                let mut textures = BTreeSet::new();
                for t in &d.textures {
                    if t.reserved != 0
                        || t.view as usize >= self.views.len()
                        || t.sampler as usize >= self.samplers.len()
                        || !textures.insert((t.stage, t.texture_slot, t.sampler_slot))
                    {
                        return Err(invalid!(
                            "draw-texture-layout",
                            Some(pass),
                            "item={} partition={} stage={} texture={} sampler_slot={} view={} views={} sampler={} samplers={} reserved={}",
                            d.item,
                            d.partition,
                            t.stage,
                            t.texture_slot,
                            t.sampler_slot,
                            t.view,
                            self.views.len(),
                            t.sampler,
                            self.samplers.len(),
                            t.reserved
                        ));
                    }
                    let required = pipeline
                        .textures
                        .iter()
                        .find(|r| {
                            (r.stage, r.texture_slot, r.sampler_slot)
                                == (t.stage, t.texture_slot, t.sampler_slot)
                        })
                        .ok_or_else(|| {
                            invalid!(
                                "draw-texture-pipeline",
                                Some(pass),
                                "item={} partition={} stage={} texture={} sampler_slot={}",
                                d.item,
                                d.partition,
                                t.stage,
                                t.texture_slot,
                                t.sampler_slot
                            )
                        })?;
                    let image = &self.images[self.views[t.view as usize].image as usize];
                    let program = if t.stage == 0 {
                        &self.programs[pipeline.vertex as usize]
                    } else {
                        self.programs
                            .get(pipeline.fragment as usize)
                            .ok_or_else(|| {
                                invalid!(
                                    "draw-texture-fragment",
                                    Some(pass),
                                    "item={} fragment={}",
                                    d.item,
                                    pipeline.fragment
                                )
                            })?
                    };
                    let reflected = program
                        .textures
                        .iter()
                        .find(|r| r.slot == t.texture_slot)
                        .ok_or_else(|| {
                            invalid!(
                                "draw-texture-program",
                                Some(pass),
                                "item={} partition={} stage={} slot={}",
                                d.item,
                                d.partition,
                                t.stage,
                                t.texture_slot
                            )
                        })?;
                    if reflected.dimension != self.view_dimensions[t.view as usize].native_id() {
                        return Err(packet_failure(
                            self.source,
                            "validate/draw-texture-dimension",
                            Some(pass),
                            (
                                d.item,
                                d.partition,
                                t.texture_slot,
                                reflected.dimension,
                                self.view_dimensions[t.view as usize].native_id(),
                            ),
                            -2,
                        ));
                    }
                    if image.desc.format != format(required.format)?
                        || sampler(&self.samplers[t.sampler as usize])?.comparison()
                            != (required.comparison != 0)
                    {
                        return Err(packet_failure(
                            self.source,
                            "validate/draw-texture-sampler",
                            Some(pass),
                            (
                                d.item,
                                d.partition,
                                t.texture_slot,
                                required.format,
                                required.comparison,
                            ),
                            -2,
                        ));
                    }
                }
            }
        }
        // Typed source-complete items and explicit POC omissions have no GPU
        // draw. Unclassified missing draws retain the original strict refusal.
        for i in &self.items {
            let drawn = seen
                .range((i.pass, i.source_item, 0)..=(i.pass, i.source_item, u64::MAX))
                .next()
                .is_some();
            if i.reserved != raw::ITEM_DRAW_REQUIRED && drawn {
                return Err(invalid!(
                    "no-draw-kind-has-draw",
                    Some(i.pass as usize),
                    "kind={} item={}",
                    i.reserved,
                    i.source_item
                ));
            }
            if i.reserved == raw::ITEM_DRAW_REQUIRED
                && !drawn
                && (i.unresolved_obligations == 0 || self.flags & 1 == 0)
            {
                return Err(invalid!(
                    "item-without-draw",
                    Some(i.pass as usize),
                    "item={} obligations={} flags={}",
                    i.source_item,
                    i.unresolved_obligations,
                    self.flags
                ));
            }
        }
        if draws > max_draws || self.intended > 0 && draws == 0 {
            return Err(-5);
        }
        let bytes = self
            .buffers
            .iter()
            .try_fold(0u64, |n, b| n.checked_add(b.len() as u64))
            .ok_or(-5)?;
        if bytes > max_bytes {
            return Err(-5);
        }
        Ok(())
    }
    fn account_source_no_draw(&self, admitted: &mut BTreeSet<(u32, u64)>, actual_draws: usize) {
        let draw_items = admitted.len();
        let mut counts = [0u64; 5];
        for item in &self.items {
            match item.reserved {
                raw::ITEM_SOURCE_REPEATED
                | raw::ITEM_SOURCE_UPDATE_ONLY
                | raw::ITEM_SOURCE_LISTS_DISABLED => {
                    counts[item.reserved as usize] += 1;
                    admitted.insert((item.pass, item.source_item));
                }
                raw::ITEM_POC_OMITTED_PRODUCER => counts[4] += 1,
                _ => {}
            }
        }
        if counts.iter().any(|&n| n != 0) {
            use std::sync::atomic::{AtomicUsize, Ordering};
            static REPORTED: AtomicUsize = AtomicUsize::new(0);
            if REPORTED
                .try_update(Ordering::Relaxed, Ordering::Relaxed, |n| {
                    if n < 8 { Some(n + 1) } else { None }
                })
                .is_ok()
            {
                eprintln!(
                    "[mag native item accounting] source={}/{}/{}/{} intended={} draw_items={draw_items} actual_draws={actual_draws} source_repeated={} source_update_only={} source_lists_disabled={} poc_omitted_producer={} admitted_logical={} omitted_logical={}",
                    self.source.engine_update,
                    self.source.cohort,
                    self.source.publication,
                    self.source.pass_occurrence,
                    self.intended,
                    counts[1],
                    counts[2],
                    counts[3],
                    counts[4],
                    admitted.len(),
                    self.intended - admitted.len() as u64
                );
            }
        }
    }
    #[cfg(test)]
    pub(crate) fn prepare(
        &self,
        device: &impl SceneCatalog,
        ids: &[u64],
        max_draws: usize,
        max_bytes: u64,
    ) -> Result<PreparedScene, i32> {
        self.prepare_with(device, ids, max_draws, max_bytes, None)
    }
    /// `resident`: the session's content-bound upload cache
    /// (`MAG_RENDER_RESIDENT_BUFFERS=1`). Byte-identical uploads then keep one
    /// immutable identity across submissions; anything it cannot keep uses
    /// the ordinary single-use snapshot below.
    pub(crate) fn prepare_with(
        &self,
        device: &impl SceneCatalog,
        ids: &[u64],
        max_draws: usize,
        max_bytes: u64,
        mut resident: Option<&mut super::scene_upload_cache::UploadCache>,
    ) -> Result<PreparedScene, i32> {
        let mut store = ResourceStore::new(ResourceLimits {
            max_resources: self.buffers.len() + self.images.len(),
            max_bytes: max_bytes as usize,
        })
        .map_err(|error| packet_failure(self.source, "scene/resource-store", None, error, -5))?;
        let mut lease = |b: &[u8], kind: &str, index: usize| -> Result<ResourceLease, i32> {
            // Only texture image uploads are kept: those are the uploads the
            // device keeps resident, and they are stable across frames. The
            // concatenated per-frame vertex/index/uniform arenas change every
            // frame and would only evict the textures.
            if kind == "image"
                && let Some(cache) = resident.as_deref_mut()
                && let Some(lease) = cache.lease(b)
            {
                return Ok(lease);
            }
            let v = store
                .apply(ResourceDelta::Create { bytes: b.to_vec() })
                .map_err(|error| {
                    packet_failure(
                        self.source,
                        "scene/resource-create",
                        Some(index),
                        (kind, b.len(), error),
                        -5,
                    )
                })?;
            let l = store.lease(v).map_err(|error| {
                packet_failure(
                    self.source,
                    "scene/resource-lease",
                    Some(index),
                    (kind, error),
                    -5,
                )
            })?;
            store.retire(v).map_err(|error| {
                packet_failure(
                    self.source,
                    "scene/resource-retire",
                    Some(index),
                    (kind, error),
                    -5,
                )
            })?;
            Ok(l)
        };
        let mut batch = BatchBuilder::new(
            device.owner_generation(),
            BatchLimits {
                max_commands: 32,
                max_gpu_buffers: self.produced.len().max(1),
                max_gpu_bytes: max_bytes,
                max_uploads: self.buffers.len().max(1),
            },
        )
        .map_err(|error| {
            packet_failure(
                self.source,
                "scene/batch-builder",
                None,
                (device.owner_generation(), max_bytes, error),
                -1,
            )
        })?;
        let mut sources = vec![None; self.buffers.len()];
        let mut allocations = BTreeMap::new();
        for (index, b) in self.buffers.iter().enumerate() {
            if let Some(&length) = self.produced.get(&(index as u32)) {
                allocations.insert(index as u32, batch.allocate(length).map_err(|_| -5)?);
            } else {
                sources[index] = Some(ReadSource::Upload(
                    batch.upload(lease(b, "buffer", index)?).map_err(|_| -5)?,
                ));
            }
        }
        self.prepare_compute_prefix(&mut batch, &mut sources, &allocations, ids)?;
        let prefix = batch
            .prepare_scene_prefix(device)
            .map_err(|error| packet_failure(self.source, "scene/upload-prefix", None, error, -1))?;
        let mut scene = SceneBuilder::new(
            prefix,
            SceneLimits {
                max_images: self.images.len(),
                max_passes: self.passes.len(),
                max_draws,
                max_bytes,
            },
        )
        .map_err(|error| {
            packet_failure(
                self.source,
                "scene/builder",
                None,
                (
                    self.images.len(),
                    self.passes.len(),
                    max_draws,
                    max_bytes,
                    error,
                ),
                -1,
            )
        })?;
        let mut images = Vec::new();
        for (index, image) in self.images.iter().enumerate() {
            let i = if let Some(requirement) =
                self.imports.iter().find(|r| r.image as usize == index)
            {
                scene
                    .require_image(
                        image.desc,
                        mag_render::scene::ExternalImage {
                            source_key: requirement.source_key,
                            required_initialized_planes: requirement.required_initialized_planes,
                        },
                    )
                    .map_err(|error| {
                        packet_failure(
                            self.source,
                            "scene/image-budget",
                            Some(index),
                            (
                                max_bytes,
                                self.buffers.iter().map(|b| b.len() as u64).sum::<u64>(),
                                self.images.len(),
                                self.images
                                    .iter()
                                    .enumerate()
                                    .take(16)
                                    .map(|(i, image)| {
                                        (i, image.desc.width, image.desc.height, image.desc.format)
                                    })
                                    .collect::<Vec<_>>(),
                            ),
                            -1,
                        );
                        packet_failure(
                            self.source,
                            "scene/require-image",
                            Some(index),
                            (
                                image.desc,
                                requirement.source_key,
                                requirement.required_initialized_planes,
                                max_bytes,
                                error,
                            ),
                            -1,
                        )
                    })?
            } else {
                match (&image.layout, &image.bytes) {
                    (Some(layout), Some(bytes)) => scene
                        .texture_with_layout(
                            image.desc,
                            layout.clone(),
                            match bytes {
                                ImageBytes::Resident(resident) => resident.clone(),
                                ImageBytes::Owned(owned) => lease(owned, "image", index)?,
                            },
                        )
                        .map_err(|error| {
                            packet_failure(
                                self.source,
                                "scene/texture-layout",
                                Some(index),
                                (image.desc, error),
                                -1,
                            )
                        })?
                        .image(),
                    (Some(layout), None) => scene
                        .image_with_layout(image.desc, layout.clone())
                        .map_err(|error| {
                            packet_failure(
                                self.source,
                                "scene/image-layout",
                                Some(index),
                                (image.desc, error),
                                -1,
                            )
                        })?,
                    (None, Some(bytes)) => scene
                        .texture(
                            image.desc,
                            match bytes {
                                ImageBytes::Resident(resident) => resident.clone(),
                                ImageBytes::Owned(owned) => lease(owned, "image", index)?,
                            },
                        )
                        .map_err(|error| {
                            packet_failure(
                                self.source,
                                "scene/texture",
                                Some(index),
                                (image.desc, error),
                                -1,
                            )
                        })?
                        .image(),
                    (None, None) => scene.image(image.desc).map_err(|error| {
                        packet_failure(
                            self.source,
                            "scene/image",
                            Some(index),
                            (image.desc, error),
                            -1,
                        )
                    })?,
                }
            };
            images.push(i)
        }
        for (pass_index, pass) in self.passes.iter().enumerate() {
            let color = if pass.color.image == u32::MAX {
                None
            } else {
                let a = pass.color;
                let i = images[a.image as usize];
                Some(ColorAttachment {
                    image: i,
                    load: if a.load == 2 {
                        Load::Discard
                    } else if a.load == 0 {
                        Load::Clear(a.clear)
                    } else {
                        Load::Load(scene.current(i).map_err(|error| {
                            packet_failure(
                                self.source,
                                "scene/color-current",
                                Some(pass_index),
                                (a.image, a.load, error),
                                -1,
                            )
                        })?)
                    },
                    store: if a.store == 0 {
                        Store::Store
                    } else {
                        Store::Discard
                    },
                })
            };
            let depth = if pass.depth.image == u32::MAX {
                None
            } else {
                let a = pass.depth;
                let i = images[a.image as usize];
                Some(DepthAttachment {
                    image: i,
                    load: if a.load == 2 {
                        Load::Discard
                    } else if a.load == 0 {
                        Load::Clear(a.clear[0])
                    } else {
                        Load::Load(scene.current(i).map_err(|error| {
                            packet_failure(
                                self.source,
                                "scene/depth-current",
                                Some(pass_index),
                                (a.image, a.load, error),
                                -1,
                            )
                        })?)
                    },
                    store: if a.store == 0 {
                        Store::Store
                    } else {
                        Store::Discard
                    },
                })
            };
            let mut draws = Vec::new();
            for (draw_index, d) in pass.draws.iter().enumerate() {
                let mut buffers = Vec::new();
                for b in &d.buffers {
                    buffers.push(GraphicsBuffer {
                        stage: stage(b.stage).map_err(|status| {
                            packet_failure(
                                self.source,
                                "scene/buffer-stage",
                                Some(pass_index),
                                (draw_index, b.slot, b.stage),
                                status,
                            )
                        })?,
                        slot: b.slot,
                        source: sources[b.view.buffer as usize].ok_or(-1)?,
                        range: self.range(b.view).map_err(|status| {
                            packet_failure(
                                self.source,
                                "scene/buffer-range",
                                Some(pass_index),
                                (
                                    draw_index,
                                    b.slot,
                                    b.view.buffer,
                                    b.view.offset,
                                    b.view.length,
                                ),
                                status,
                            )
                        })?,
                    })
                }
                let mut textures = Vec::new();
                for t in &d.textures {
                    let view = &self.views[t.view as usize];
                    textures.push(SampledImage {
                        stage: stage(t.stage).map_err(|status| {
                            packet_failure(
                                self.source,
                                "scene/texture-stage",
                                Some(pass_index),
                                (draw_index, t.texture_slot, t.stage),
                                status,
                            )
                        })?,
                        texture_slot: t.texture_slot,
                        sampler_slot: t.sampler_slot,
                        image: scene
                            .view(
                                scene
                                    .current(images[view.image as usize])
                                    .map_err(|error| {
                                        packet_failure(
                                            self.source,
                                            "scene/sampled-current",
                                            Some(pass_index),
                                            (draw_index, t.texture_slot, view.image, error),
                                            -1,
                                        )
                                    })?,
                                self.view_desc(t.view as usize).map_err(|status| {
                                    packet_failure(
                                        self.source,
                                        "scene/view-desc",
                                        Some(pass_index),
                                        (draw_index, t.view),
                                        status,
                                    )
                                })?,
                            )
                            .map_err(|error| {
                                packet_failure(
                                    self.source,
                                    "scene/view",
                                    Some(pass_index),
                                    (draw_index, t.view, view.image, error),
                                    -1,
                                )
                            })?,
                        sampler: sampler(&self.samplers[t.sampler as usize]).map_err(|status| {
                            packet_failure(
                                self.source,
                                "scene/sampler",
                                Some(pass_index),
                                (draw_index, t.sampler),
                                status,
                            )
                        })?,
                    })
                }
                draws.push(SceneDraw {
                    pipeline: ids[d.pipeline as usize],
                    vertices: sources[d.vertices.buffer as usize].ok_or(-1)?,
                    indices: sources[d.indices.buffer as usize].ok_or(-1)?,
                    vertex_range: self.range(d.vertices).map_err(|status| {
                        packet_failure(
                            self.source,
                            "scene/vertex-range",
                            Some(pass_index),
                            (draw_index, d.item, d.partition),
                            status,
                        )
                    })?,
                    index_range: self.range(d.indices).map_err(|status| {
                        packet_failure(
                            self.source,
                            "scene/index-range",
                            Some(pass_index),
                            (draw_index, d.item, d.partition),
                            status,
                        )
                    })?,
                    index_count: d.count,
                    buffers,
                    textures,
                });
            }
            let pass = RenderPass {
                color,
                depth,
                draws,
            };
            let stencil = self
                .stencils
                .as_ref()
                .and_then(|s| s.get(pass_index))
                .filter(|s| s.image != u32::MAX)
                .map(|s| scene::StencilOps {
                    image: images[s.image as usize],
                    load: match s.load {
                        0 => scene::StencilLoad::Clear(s.clear_value as u8),
                        1 => scene::StencilLoad::Load,
                        _ => scene::StencilLoad::Discard,
                    },
                    store: if s.store == 0 {
                        scene::Store::Store
                    } else {
                        scene::Store::Discard
                    },
                    source_full_write: s.flags & 1 != 0,
                });
            scene
                .render_pass_with_aspects(
                    pass,
                    self.viewports.as_ref().map(|v| v[pass_index]),
                    stencil,
                )
                .map_err(|error| {
                    packet_failure(
                        self.source,
                        "scene/render-pass",
                        Some(pass_index),
                        (
                            error,
                            [self.passes[pass_index].color, self.passes[pass_index].depth].map(
                                |a| {
                                    (
                                        a.image,
                                        self.images.get(a.image as usize).map(|i| i.desc),
                                        a.load,
                                        a.store,
                                    )
                                },
                            ),
                            self.viewports.as_ref().map(|v| v[pass_index]),
                            self.passes[pass_index].draws.len(),
                        ),
                        -1,
                    )
                })?;
            scene
                .draw_depth_bounds(
                    pass_index,
                    self.passes[pass_index]
                        .draws
                        .iter()
                        .map(|d| {
                            let r = self.pipelines[d.pipeline as usize].state;
                            (r.depth_bounds_enabled != 0).then_some(
                                mag_render::scene::DepthBounds {
                                    minimum: r.depth_bounds_min,
                                    maximum: r.depth_bounds_max,
                                },
                            )
                        })
                        .collect(),
                )
                .map_err(|error| {
                    packet_failure(
                        self.source,
                        "scene/draw-depth-bounds",
                        Some(pass_index),
                        error,
                        -1,
                    )
                })?;
        }
        scene
            .output(
                scene
                    .current(images[self.output as usize])
                    .map_err(|error| {
                        packet_failure(
                            self.source,
                            "scene/output-current",
                            Some(self.output as usize),
                            error,
                            -1,
                        )
                    })?,
                false,
            )
            .map_err(|error| {
                packet_failure(
                    self.source,
                    "scene/output",
                    Some(self.output as usize),
                    error,
                    -1,
                )
            })?;
        scene
            .prepare(device)
            .map_err(|error| packet_failure(self.source, "scene/prepare", None, error, -1))
    }
}
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct Limits {
    pub max_resources: u64,
    pub max_images: u64,
    pub max_passes: u64,
    pub max_draws: u64,
    pub max_bytes: u64,
}
unsafe extern "C" {
    fn mag_render_scene_packet_limits(out: *mut Limits) -> i32;
}
pub fn limits() -> Result<Limits, i32> {
    let mut limits = Limits::default();
    let status = unsafe { mag_render_scene_packet_limits(&mut limits) };
    if status != 0 {
        return Err(status);
    }
    use crate::batch_device::scene_device::*;
    if limits.max_resources != MAX_SCENE_RESOURCES as u64
        || limits.max_images != MAX_SCENE_IMAGES as u64
        || limits.max_draws != MAX_SCENE_DRAWS as u64
        || limits.max_bytes != MAX_SCENE_BYTES
        || limits.max_passes != 16
    {
        return Err(-1);
    }
    Ok(limits)
}
const MAX_PIPELINES: usize = 256;
const MAX_SHADER_BYTES: usize = 32 * 1024 * 1024;
#[derive(Default)]
pub(super) struct PipelineCache {
    programs: BTreeMap<Vec<u8>, u64>,
    next_program: u64,
    pipelines: BTreeMap<Vec<u8>, (u64, u64)>,
    epoch: u64,
    pins: BTreeMap<u64, usize>,
}
impl PipelineCache {
    pub(super) fn pin(&mut self, ids: &[u64]) -> Result<(), i32> {
        let ids: BTreeSet<_> = ids.iter().copied().collect();
        if ids.iter().any(|id| {
            !self.pipelines.values().any(|(p, _)| p == id) || self.pins.get(id) == Some(&usize::MAX)
        }) {
            return Err(-6);
        }
        for id in ids {
            *self.pins.entry(id).or_default() += 1;
        }
        Ok(())
    }
    pub(super) fn unpin(&mut self, ids: &[u64]) -> Result<(), i32> {
        let ids: BTreeSet<_> = ids.iter().copied().collect();
        if ids
            .iter()
            .any(|id| self.pins.get(id).is_none_or(|n| *n == 0))
        {
            return Err(-6);
        }
        for id in ids {
            let n = self.pins.get_mut(&id).unwrap();
            *n -= 1;
            if *n == 0 {
                self.pins.remove(&id);
            }
        }
        Ok(())
    }

    fn intern(&mut self, p: &Program) -> Result<u64, i32> {
        let key = p.key();
        if let Some(id) = self.programs.get(&key) {
            return Ok(*id);
        }
        if key.len() > MAX_SHADER_BYTES {
            return Err(-5);
        }
        // Program keys only authenticate exact immutable shader/reflection bytes.
        // Old pipeline keys retain non-reused IDs, so dropping lookup entries cannot
        // alias a different shader. Re-encounter can miss cache but remains correct.
        if self
            .programs
            .keys()
            .map(Vec::len)
            .sum::<usize>()
            .checked_add(key.len())
            .ok_or(-5)?
            > MAX_SHADER_BYTES
        {
            self.programs.clear();
        }
        self.next_program = self.next_program.checked_add(1).ok_or(-5)?;
        self.programs.insert(key, self.next_program);
        Ok(self.next_program)
    }
    fn pipeline_key(p: &Pipeline, vp: u64, fp: u64) -> Result<Vec<u8>, i32> {
        raster(&p.state)?;
        let mut key = Vec::new();
        key.extend(vp.to_le_bytes());
        key.extend(fp.to_le_bytes());
        words(
            &mut key,
            &[
                p.stride,
                p.color,
                p.depth,
                p.binding_model,
                p.attributes.len() as u32,
                p.textures.len() as u32,
            ],
        );
        let s = &p.state;
        words(
            &mut key,
            &[
                s.cull_mode,
                s.front_face,
                s.depth_enabled,
                s.depth_compare,
                s.depth_write,
                s.blend_enabled,
                s.src_rgb,
                s.dst_rgb,
                s.rgb_equation,
                s.src_alpha,
                s.dst_alpha,
                s.alpha_equation,
                s.color_write_mask,
            ],
        );
        words(&mut key, &[s.stencil_enabled, s.depth_bounds_enabled]);
        for f in [s.front_stencil, s.back_stencil] {
            words(
                &mut key,
                &[
                    f.compare,
                    f.fail,
                    f.depth_fail,
                    f.pass,
                    f.reference,
                    f.read_mask,
                    f.write_mask,
                    f.reserved,
                ],
            );
        }
        for f in s.blend_constant {
            key.extend(f.to_bits().to_le_bytes());
        }
        for a in &p.attributes {
            words(&mut key, &[a.location, a.format, a.offset, a.reserved])
        }
        for t in &p.textures {
            words(
                &mut key,
                &[
                    t.stage,
                    t.texture_slot,
                    t.sampler_slot,
                    t.format,
                    t.comparison,
                    t.reserved,
                ],
            )
        }
        key.insert(0, 0); // Disjoint graphics/compute cache namespaces.
        Ok(key)
    }
    fn resolve(
        &mut self,
        device: &mut impl PipelineCompiler,
        packet: &ScenePacket,
    ) -> Result<Vec<u64>, i32> {
        self.epoch = self.epoch.checked_add(1).ok_or(-5)?;
        let epoch = self.epoch;
        let mut programs = Vec::new();
        for p in &packet.programs {
            programs.push(self.intern(p)?)
        }
        let mut keys = Vec::new();
        for p in &packet.pipelines {
            keys.push(Self::pipeline_key(
                p,
                programs[p.vertex as usize],
                if p.fragment == u32::MAX {
                    0
                } else {
                    programs[p.fragment as usize]
                },
            )?)
        }
        let compute_keys: Vec<_> = packet
            .compute_programs()
            .into_iter()
            .map(|index| {
                let mut key = vec![1];
                key.extend(programs[index as usize].to_le_bytes());
                (index, key)
            })
            .collect();
        let required: BTreeSet<_> = keys
            .iter()
            .cloned()
            .chain(compute_keys.iter().map(|(_, key)| key.clone()))
            .collect();
        if required.len() > MAX_PIPELINES {
            return Err(-5);
        }
        let mut result = Vec::new();
        for (index, (p, key)) in packet.pipelines.iter().zip(keys).enumerate() {
            if let Some((id, last)) = self.pipelines.get_mut(&key) {
                *last = epoch;
                result.push(*id);
                continue;
            }
            if self.pipelines.len() >= MAX_PIPELINES {
                let old = self
                    .pipelines
                    .iter()
                    .filter(|(k, (id, _))| !required.contains(*k) && !self.pins.contains_key(id))
                    .min_by_key(|(_, (_, age))| *age)
                    .map(|(k, (id, _))| (k.clone(), *id))
                    .ok_or(-5)?;
                device.release(old.1)?;
                self.pipelines.remove(&old.0);
            }
            let id = device.compile(p, &packet.programs).map_err(|status| {
                packet_failure(
                    packet.source,
                    "cache/compile-pipeline",
                    Some(index),
                    format_args!("vertex={} fragment={} stride={} color={} depth={} attributes={} textures={} known={:x} unresolved={:x} depth_enabled={} depth_write={} stencil={} bounds={} bias={} blend={} cull={} samples={} topology={}",
                        p.vertex,p.fragment,p.stride,p.color,p.depth,p.attributes.len(),p.textures.len(),
                        p.state.known_fields,p.state.unresolved_obligations,p.state.depth_enabled,p.state.depth_write,
                        p.state.stencil_enabled,p.state.depth_bounds_enabled,p.state.bias_enabled,p.state.blend_enabled,
                        p.state.cull_mode,p.state.samples,p.state.topology),
                    status,
                )
            })?;
            self.pipelines.insert(key, (id, epoch));
            result.push(id);
        }
        for (index, key) in compute_keys {
            if let Some((id, last)) = self.pipelines.get_mut(&key) {
                *last = epoch;
                result.push(*id);
                continue;
            }
            if self.pipelines.len() >= MAX_PIPELINES {
                let old = self
                    .pipelines
                    .iter()
                    .filter(|(k, (id, _))| !required.contains(*k) && !self.pins.contains_key(id))
                    .min_by_key(|(_, (_, age))| *age)
                    .map(|(k, (id, _))| (k.clone(), *id))
                    .ok_or(-5)?;
                device.release(old.1)?;
                self.pipelines.remove(&old.0);
            }
            let id = device
                .compile_compute(&packet.programs[index as usize])
                .map_err(|status| {
                    packet_failure(
                        packet.source,
                        "cache/compile-compute",
                        Some(index as usize),
                        "owned-program",
                        status,
                    )
                })?;
            self.pipelines.insert(key, (id, epoch));
            result.push(id);
        }
        Ok(result)
    }
}
trait PipelineCompiler {
    fn compile(&mut self, p: &Pipeline, programs: &[Program]) -> Result<u64, i32>;
    fn compile_compute(&mut self, _program: &Program) -> Result<u64, i32> {
        Err(-2)
    }
    fn release(&mut self, id: u64) -> Result<(), i32>;
}
impl PipelineCompiler for SceneDevice<'_> {
    fn release(&mut self, id: u64) -> Result<(), i32> {
        self.release_scene_pipeline(id)
    }
    fn compile_compute(&mut self, program: &Program) -> Result<u64, i32> {
        let shader = program.shader();
        let desc = mag_render::sdk_compute::PipelineDesc {
            struct_size: size_of::<mag_render::sdk_compute::PipelineDesc>() as u32,
            abi_version: 1,
            source_bytes: shader.source,
            source_length: shader.source_length,
            entry_point: shader.entry,
            entry_point_length: shader.entry_length,
            binding_count: program.buffers.len() as u32,
            bindings: program.buffers.as_ptr(),
            threads_per_group: program.threads,
            shader_language: program.language,
        };
        unsafe { self.compute_pipeline(&desc) }
    }
    fn compile(&mut self, p: &Pipeline, programs: &[Program]) -> Result<u64, i32> {
        let vertex = &programs[p.vertex as usize];
        let fragment = programs.get(p.fragment as usize);
        let mut buffers = Vec::new();
        for (stage, program) in [(0, Some(vertex)), (1, fragment)] {
            if let Some(program) = program {
                for b in &program.buffers {
                    buffers.push(BufferLayout {
                        stage,
                        reserved: 0,
                        binding: *b,
                    })
                }
            }
        }
        raster(&p.state)?;
        let desc = SceneGraphicsV2Desc {
            struct_size: size_of::<SceneGraphicsV2Desc>() as u32,
            abi_version: 2,
            base: SceneGraphicsDesc {
                struct_size: size_of::<SceneGraphicsDesc>() as u32,
                abi_version: 1,
                vertex: vertex.shader(),
                fragment: fragment.map(Program::shader).unwrap_or(ShaderStage {
                    source: std::ptr::null(),
                    source_length: 0,
                    entry: std::ptr::null(),
                    entry_length: 0,
                    language: 0,
                }),
                vertex_stride: p.stride,
                color_format: p.color,
                depth_format: p.depth,
                depth_compare: 0,
                depth_write: 0,
                blend: 0,
                buffers: buffers.as_ptr(),
                buffer_count: buffers.len() as u32,
                texture_count: p.textures.len() as u32,
                textures: p.textures.as_ptr(),
            },
            attributes: p.attributes.as_ptr(),
            attribute_count: p.attributes.len() as u32,
            binding_model: p.binding_model,
        };
        let desc = SceneGraphicsV3Desc {
            struct_size: size_of::<SceneGraphicsV3Desc>() as u32,
            abi_version: 3,
            base: desc,
            raster: p.state,
        };
        unsafe { self.graphics_pipeline_aux(&desc, fragment.map_or(0, |p| p.flags)) }
    }
}
/// CPU preparation only. No target pin, queue lease, submission or original
/// completion publication occurs here. Owned leases and native PSOs remain held.
/// `prevalidated` names the exact limits a caller already validated this
/// immutable owned packet against. Only an exact match with this session's
/// limits skips the repeated actor validation; anything else validates again.
pub(crate) fn admit(
    id: u64,
    worker: u64,
    packet: ScenePacket,
    prevalidated: Option<(usize, u64)>,
) -> Result<raw::Admission, i32> {
    with_session(id, |s| {
        if s.acknowledged_failure {
            return Err(-6);
        }
        if s.admissions.len() + s.pending.len() + s.ready.len() >= 2 {
            return Err(-4);
        }
        if prevalidated != Some((s.max_draws, s.max_bytes)) {
            packet
                .validate(s.max_draws, s.max_bytes)
                .map_err(|status| {
                    packet_failure(
                        packet.source,
                        "actor/validate",
                        None,
                        "owned-packet",
                        status,
                    )
                })?;
        }
        if packet.flags != 0 {
            return Err(packet_failure(
                packet.source,
                "admit/flags",
                None,
                packet.flags,
                -2,
            ));
        }
        let language = match s.native_backend {
            1 => 1,
            2 => 3,
            3 => 2,
            _ => {
                return Err(packet_failure(
                    packet.source,
                    "admit/backend",
                    None,
                    s.native_backend,
                    -2,
                ));
            }
        };
        if let Some((index, program)) = packet
            .programs
            .iter()
            .enumerate()
            .find(|(_, p)| p.language != language)
        {
            return Err(packet_failure(
                packet.source,
                "admit/program-language",
                Some(index),
                (program.language, language),
                -2,
            ));
        }
        let ids = s
            .packet_cache
            .resolve(&mut *s.backend.device().0.borrow_mut(), &packet)
            .map_err(|status| {
                packet_failure(
                    packet.source,
                    "actor/cache-resolve",
                    None,
                    "pipeline-cache",
                    status,
                )
            })?;
        let scene = packet
            .prepare_with(
                &*s.backend.device().0.borrow(),
                &ids,
                s.max_draws,
                s.max_bytes,
                s.upload_cache.as_mut(),
            )
            .map_err(|status| {
                packet_failure(packet.source, "admit/prepare", None, "owned-scene", status)
            })?;
        if s.upload_cache.is_some() || crate::batch_device::resident_arenas_enabled() {
            let device = s.backend.device().0.borrow();
            log_resident_buffers(
                s.upload_cache.as_ref().map(|c| c.stats()),
                device.resident_buffer_stats(),
                device.arena_stats(),
            );
        }
        let generation = s.backend.device().0.borrow().owner_generation();
        let mut admitted = BTreeSet::new();
        let mut identities = Vec::new();
        for (pass, p) in packet.passes.iter().enumerate() {
            for d in &p.draws {
                admitted.insert((pass as u32, d.item));
                identities.push(DrawIdentity {
                    source_item: d.item,
                    source_partition: d.partition,
                    unresolved_obligations: d.obligations,
                });
            }
        }
        packet.account_source_no_draw(&mut admitted, identities.len());
        let receipt = Receipt {
            abi_version: 1,
            struct_size: size_of::<Receipt>() as u32,
            source: packet.source,
            flags: DIAGNOSTIC,
            intended_items: packet.intended,
            admitted_items: admitted.len() as u64,
            omitted_items: packet.intended - admitted.len() as u64,
            draws: identities.len() as u64,
            ..Receipt::default()
        };
        let output = packet.images.get(packet.output as usize).ok_or(-1)?.desc;
        let (commands, leases) = scene.into_submission();
        // Same synchronous backend admission limits, checked before a token is
        // published. This does not promise later native allocations cannot fail.
        if leases.is_empty()
            || leases.len() > crate::batch_device::scene_device::MAX_SCENE_RESOURCES
            || leases
                .iter()
                .try_fold(0u64, |n, l| n.checked_add(l.bytes().len() as u64))
                .is_none_or(|n| n > s.max_bytes)
        {
            return Err(-5);
        }
        s.packet_cache.pin(&ids)?;
        let prepared = super::scene_admission::Prepared {
            commands: std::rc::Rc::new(commands),
            leases,
            pipelines: ids.clone(),
            output,
            imports: packet.imports.clone(),
            receipt,
            identities,
        };
        match s.admissions.insert(
            (worker, generation),
            packet.source,
            packet.intended,
            receipt.draws,
            prepared,
            s.pending.len() + s.ready.len(),
        ) {
            Ok(a) => Ok(a),
            Err(e) => {
                s.packet_cache.unpin(&ids)?;
                Err(e)
            }
        }
    })
}
pub(crate) fn cancel(id: u64, admission: raw::Admission) -> Result<(), i32> {
    with_session(id, |s| {
        let ids = s
            .admissions
            .get(admission, admission.source)?
            .pipelines
            .clone();
        s.packet_cache.unpin(&ids)?;
        s.admissions.remove(admission, admission.source)?;
        Ok(())
    })
}
/// A definitely rejected commit keeps the preparation cancellable/retryable.
/// Accepted/indeterminate consumes it exactly once; only genuine backend terminal
/// releases submitted leases. This does not publish original CPU outputs.
pub(crate) fn commit(
    id: u64,
    admission: raw::Admission,
    source: Source,
    target: &HostImage,
    frame: HostFrame,
) -> (i32, Receipt) {
    commit_impl(id, admission, source, Some((target, frame)), &[])
}
pub(crate) fn commit_subgraph(
    id: u64,
    admission: raw::Admission,
    source: Source,
    bindings: &[raw::ImageBinding],
) -> (i32, Receipt) {
    commit_impl(id, admission, source, None, bindings)
}
/// Reserved-order subgraph commit. Under the owner lease (the caller waits):
/// bind and validate the imports and reserve the native queue position and
/// ticket. `acknowledge` then receives exactly the receipt the synchronous
/// commit would return (same native ordering witness). Only afterwards, with
/// the caller released, are resources allocated, encoded and committed into
/// that reserved position. A failure after acknowledgement queues a FAILED
/// terminal receipt and refuses all later admissions (fail closed); the
/// reserved position is always released. If reservation is unavailable or
/// fails, this is exactly the synchronous commit. Never implies GPU completion.
pub(crate) fn commit_subgraph_reserved(
    id: u64,
    admission: raw::Admission,
    source: Source,
    bindings: &[raw::ImageBinding],
    acknowledge: impl FnOnce((i32, Receipt)),
) {
    let mut acknowledge = Some(acknowledge);
    let mut early = None;
    let reserved = ffi(|| {
        with_session(id, |s| {
            let p = s.admissions.get(admission, source)?;
            if p.imports.is_empty() || s.acknowledged_failure {
                return Err(-1);
            }
            let requirements = p.imports.clone();
            let mut receipt = p.receipt;
            unsafe {
                s.backend
                    .device()
                    .0
                    .borrow_mut()
                    .bind_imports(bindings, &requirements)
            }?;
            let reserved = s.backend.device().0.borrow_mut().reserve_submission();
            let (generation, native) = match reserved {
                Ok(v) => v,
                Err(e) => {
                    s.backend.device().0.borrow_mut().release_imports()?;
                    return Err(e);
                }
            };
            receipt.ticket = s.backend.next_submission_id();
            receipt.native_generation = generation;
            receipt.native_ticket = native;
            receipt.status = 0;
            early = Some(receipt);
            Ok(())
        })
    });
    let Some(receipt) = early.filter(|_| reserved == 0) else {
        // Nothing reserved: identical to the synchronous commit (imports were
        // released above; commit_impl binds them again).
        let reply = commit_subgraph(id, admission, source, bindings);
        if let Some(ack) = acknowledge.take() {
            ack(reply);
        }
        return;
    };
    if let Some(ack) = acknowledge.take() {
        ack((0, receipt));
    }
    let failure = |s: &mut super::Session, status: i32| {
        let _ = s.backend.device().0.borrow_mut().abandon_reservation();
        let _ = s.backend.device().0.borrow_mut().release_imports();
        let mut failed = receipt;
        failed.flags |= TERMINAL | FAILED | UNCERTAIN;
        failed.status = if status < 0 { status } else { -6 };
        s.acknowledged_failure = true;
        s.ready.push_back(super::Pending {
            receipt: failed,
            identities: vec![],
        });
    };
    let _ = ffi(|| {
        with_session(id, |s| {
            let Ok(p) = s.admissions.get(admission, source) else {
                failure(s, -3);
                return Ok(());
            };
            let commands = p.commands.clone();
            let leases = p.leases.clone();
            let identities = p.identities.clone();
            let result = s
                .backend
                .submit_diagnostic(source.publication, commands, leases);
            let ticket = match result {
                Ok(t) => t,
                Err(BackendError::SubmissionUncertain { code, .. }) => {
                    failure(s, -(code.min(i32::MAX as u32) as i32).max(1));
                    let _ = s.admissions.remove(admission, source);
                    return Ok(());
                }
                Err(_) => {
                    failure(s, -6);
                    if let Ok(p) = s.admissions.remove(admission, source) {
                        let _ = s.packet_cache.unpin(&p.pipelines);
                    }
                    return Ok(());
                }
            };
            let native = s.backend.device().0.borrow().native_ticket(ticket);
            let p = s.admissions.remove(admission, source)?;
            let mut uncertain = s.packet_cache.unpin(&p.pipelines).is_err();
            uncertain |= s.backend.device().0.borrow_mut().release_imports().is_err();
            let mut final_receipt = receipt;
            if ticket.submission_id != receipt.ticket
                || native != Some((receipt.native_generation, receipt.native_ticket))
            {
                // The acknowledged ordering witness must be exactly this work.
                failure(s, -6);
                return Ok(());
            }
            if uncertain {
                final_receipt.flags |= UNCERTAIN;
                final_receipt.status = 1;
            }
            s.pending.insert(
                ticket.submission_id,
                Pending {
                    receipt: final_receipt,
                    identities,
                },
            );
            Ok(())
        })
    });
}
fn commit_impl(
    id: u64,
    admission: raw::Admission,
    source: Source,
    target: Option<(&HostImage, HostFrame)>,
    bindings: &[raw::ImageBinding],
) -> (i32, Receipt) {
    let mut out = Receipt {
        abi_version: 1,
        struct_size: size_of::<Receipt>() as u32,
        flags: DIAGNOSTIC,
        source,
        ..Receipt::default()
    };
    let mut effects = false;
    let status = ffi(|| {
        with_session(id, |s| {
            let p = s.admissions.get(admission, source)?;
            if let Some((target, frame)) = target {
                if !p.imports.is_empty()
                    || !frame.valid()
                    || frame.owner_lifetime != target.owner_lifetime
                    || target.width != p.output.width
                    || target.height != p.output.height
                    || format(target.format)? != p.output.format
                {
                    return Err(-3);
                }
            } else if p.imports.is_empty() {
                return Err(-1);
            }
            let requirements = p.imports.clone();
            let commands = p.commands.clone();
            let leases = p.leases.clone();
            let mut receipt = p.receipt;
            let identities = p.identities.clone();
            let image = if let Some((target, frame)) = target {
                let image = unsafe { s.backend.device().0.borrow_mut().import_target(target) }?;
                if let Err(e) = unsafe { s.backend.device().0.borrow_mut().frame_context(frame) } {
                    s.backend.device().0.borrow_mut().release_target(image)?;
                    return Err(e);
                }
                Some(image)
            } else {
                unsafe {
                    s.backend
                        .device()
                        .0
                        .borrow_mut()
                        .bind_imports(bindings, &requirements)
                }?;
                None
            };
            let result = s
                .backend
                .submit_diagnostic(source.publication, commands, leases);
            let (ticket, mut uncertain) = match result {
                Ok(t) => (t, false),
                Err(BackendError::SubmissionUncertain { ticket, .. }) => (ticket, true),
                Err(e) => {
                    if let Some(image) = image {
                        s.backend.device().0.borrow_mut().release_target(image)?
                    } else {
                        s.backend.device().0.borrow_mut().release_imports()?
                    }
                    return Err(match e {
                        BackendError::DeviceRejected { code } => {
                            -(code.min(i32::MAX as u32) as i32).max(1)
                        }
                        _ => -6,
                    });
                }
            };
            effects = true;
            // Remove the replayable preparation before any further fallible cleanup.
            let p = s.admissions.remove(admission, source)?;
            uncertain |= s.packet_cache.unpin(&p.pipelines).is_err();
            uncertain |= if let Some(image) = image {
                s.backend
                    .device()
                    .0
                    .borrow_mut()
                    .release_target(image)
                    .is_err()
            } else {
                s.backend.device().0.borrow_mut().release_imports().is_err()
            };
            let (generation, native) = s
                .backend
                .device()
                .0
                .borrow()
                .native_ticket(ticket)
                .unwrap_or((0, 0));
            receipt.ticket = ticket.submission_id;
            receipt.native_generation = generation;
            receipt.native_ticket = native;
            receipt.status = i32::from(uncertain);
            if uncertain {
                receipt.flags |= UNCERTAIN;
            }
            out = receipt;
            s.pending.insert(
                ticket.submission_id,
                Pending {
                    receipt,
                    identities,
                },
            );
            Ok(())
        })
    });
    if status != 0 {
        out.status = if effects { 1 } else { status };
        if effects {
            out.flags |= UNCERTAIN;
        }
    }
    (out.status, out)
}

/// Called only on the fixed worker's session thread. It shares the same backend,
/// generation, genuine target/frame, pending limit and terminal evidence path.
pub(crate) fn submit(
    id: u64,
    packet: ScenePacket,
    target: &HostImage,
    frame: HostFrame,
) -> (i32, Receipt) {
    let mut out = Receipt {
        abi_version: 1,
        struct_size: size_of::<Receipt>() as u32,
        flags: DIAGNOSTIC,
        source: packet.source,
        intended_items: packet.intended,
        ..Receipt::default()
    };
    let mut effects = false;
    let code = ffi(|| {
        with_session(id, |s| {
            if s.admissions.len() + s.pending.len() + s.ready.len() >= 2 {
                return Err(-4);
            }
            packet
                .validate(s.max_draws, s.max_bytes)
                .map_err(|status| {
                    packet_failure(
                        packet.source,
                        "actor/validate",
                        None,
                        "owned-packet",
                        status,
                    )
                })?;
            if !frame.valid() || frame.owner_lifetime != target.owner_lifetime {
                return Err(-3);
            }
            let output = packet.images.get(packet.output as usize).ok_or(-1)?;
            if output.desc.width != target.width
                || output.desc.height != target.height
                || output.desc.format != format(target.format)?
            {
                return Err(packet_failure(
                    packet.source,
                    "submit/output-target",
                    None,
                    (
                        output.desc.width,
                        output.desc.height,
                        target.width,
                        target.height,
                        target.format,
                    ),
                    -2,
                ));
            }
            let language = match s.native_backend {
                1 => 1,
                2 => 3,
                3 => 2,
                _ => {
                    return Err(packet_failure(
                        packet.source,
                        "submit/backend",
                        None,
                        s.native_backend,
                        -2,
                    ));
                }
            };
            if let Some((index, program)) = packet
                .programs
                .iter()
                .enumerate()
                .find(|(_, p)| p.language != language)
            {
                return Err(packet_failure(
                    packet.source,
                    "submit/program-language",
                    Some(index),
                    (program.language, language),
                    -2,
                ));
            }
            let ids = s
                .packet_cache
                .resolve(&mut *s.backend.device().0.borrow_mut(), &packet)
                .map_err(|status| {
                    packet_failure(
                        packet.source,
                        "actor/cache-resolve",
                        None,
                        "pipeline-cache",
                        status,
                    )
                })?;
            let scene = packet.prepare_with(
                &*s.backend.device().0.borrow(),
                &ids,
                s.max_draws,
                s.max_bytes,
                s.upload_cache.as_mut(),
            )?;
            if s.upload_cache.is_some() || crate::batch_device::resident_arenas_enabled() {
                let device = s.backend.device().0.borrow();
                log_resident_buffers(
                    s.upload_cache.as_ref().map(|c| c.stats()),
                    device.resident_buffer_stats(),
                    device.arena_stats(),
                );
            }
            let mut admitted = BTreeSet::new();
            let mut identities = Vec::new();
            let mut obligations = 0;
            for (pass, p) in packet.passes.iter().enumerate() {
                for d in &p.draws {
                    admitted.insert((pass as u32, d.item));
                    obligations |= d.obligations;
                    identities.push(DrawIdentity {
                        source_item: d.item,
                        source_partition: d.partition,
                        unresolved_obligations: d.obligations,
                    })
                }
            }
            for i in &packet.items {
                obligations |= i.unresolved_obligations
            }
            packet.account_source_no_draw(&mut admitted, identities.len());
            let image = unsafe { s.backend.device().0.borrow_mut().import_target(target) }?;
            if let Err(e) = unsafe { s.backend.device().0.borrow_mut().frame_context(frame) } {
                s.backend.device().0.borrow_mut().release_target(image)?;
                return Err(e);
            }
            let (commands, leases) = scene.into_submission();
            let result = s.backend.submit_diagnostic(
                packet.source.publication,
                std::rc::Rc::new(commands),
                leases,
            );
            let (ticket, uncertain) = match result {
                Ok(t) => (t, false),
                Err(BackendError::SubmissionUncertain { ticket, .. }) => (ticket, true),
                Err(e) => {
                    s.backend.device().0.borrow_mut().release_target(image)?;
                    return Err(match e {
                        BackendError::DeviceRejected { code } => {
                            -(code.min(i32::MAX as u32) as i32).max(1)
                        }
                        _ => -6,
                    });
                }
            };
            effects = true;
            let release = s.backend.device().0.borrow_mut().release_target(image);
            let (native_generation, native_ticket) = s
                .backend
                .device()
                .0
                .borrow()
                .native_ticket(ticket)
                .unwrap_or((0, 0));
            let uncertain = uncertain || release.is_err();
            out = Receipt {
                abi_version: 1,
                struct_size: size_of::<Receipt>() as u32,
                status: i32::from(uncertain),
                flags: DIAGNOSTIC | if uncertain { UNCERTAIN } else { 0 },
                source: packet.source,
                ticket: ticket.submission_id,
                native_generation,
                native_ticket,
                intended_items: packet.intended,
                admitted_items: admitted.len() as u64,
                omitted_items: packet.intended - admitted.len() as u64,
                draws: identities.len() as u64,
                unresolved_obligations: obligations,
            };
            s.pending.insert(
                ticket.submission_id,
                Pending {
                    receipt: out,
                    identities,
                },
            );
            Ok(())
        })
    });
    if code != 0 {
        out.status = if effects { 1 } else { code };
        if effects {
            out.flags |= UNCERTAIN
        }
    }
    (out.status, out)
}

#[cfg(test)]
pub(crate) mod packet_tests {
    use super::*;
    fn source() -> Source {
        Source {
            engine_update: 1,
            cohort: 2,
            publication: 3,
            pass_occurrence: 4,
        }
    }
    pub(crate) fn fixture() -> ScenePacket {
        let program = |stage| Program {
            flags: 0,
            stage,
            language: 1,
            source: format!("authored stage {stage}").into_bytes(),
            spirv: vec![],
            entry: b"main".to_vec(),
            buffers: vec![BindingLayout {
                slot: stage,
                access: 1,
                minimum_bytes: 16,
                runtime_size_index: u32::MAX,
                alignment_bytes: 16,
                binding_kind: 1,
                ..BindingLayout::default()
            }],
            textures: if stage == 1 {
                vec![raw::TextureSlot {
                    slot: 0,
                    dimension: 2,
                    sample_kind: 0,
                    arrayed: 0,
                    multisampled: 0,
                    reserved: 0,
                }]
            } else {
                vec![]
            },
            samplers: if stage == 1 {
                vec![raw::SamplerSlot {
                    slot: 0,
                    comparison: 0,
                }]
            } else {
                vec![]
            },
            vertex_locations: if stage == 0 { vec![0, 8] } else { vec![] },
            threads: [0; 3],
        };
        let mut state: raw::RasterState = unsafe { std::mem::zeroed() };
        state.known_fields = 511;
        state.color_write_mask = 15;
        state.samples = 1;
        let mut sampler: raw::Sampler = unsafe { std::mem::zeroed() };
        sampler.known_fields = 63;
        sampler.max_anisotropy = 1.0;
        let view = |buffer, length| raw::BufferUse {
            buffer,
            reserved: 0,
            offset: 0,
            length,
        };
        let draw = PacketDraw {
            item: 0,
            partition: 10,
            obligations: 0,
            pipeline: 0,
            count: 3,
            vertices: view(0, 72),
            indices: view(1, 12),
            buffers: vec![
                raw::Binding {
                    stage: 0,
                    slot: 0,
                    view: view(2, 16),
                },
                raw::Binding {
                    stage: 1,
                    slot: 1,
                    view: view(3, 16),
                },
            ],
            textures: vec![raw::TextureUse {
                stage: 1,
                texture_slot: 0,
                sampler_slot: 0,
                view: 0,
                sampler: 0,
                reserved: 0,
            }],
        };
        ScenePacket {
            source: source(),
            intended: 1,
            flags: 0,
            programs: vec![program(0), program(1)],
            pipelines: vec![Pipeline {
                vertex: 0,
                fragment: 1,
                stride: 24,
                attributes: vec![
                    VertexAttribute {
                        location: 0,
                        format: 4,
                        offset: 0,
                        reserved: 0,
                    },
                    VertexAttribute {
                        location: 8,
                        format: 2,
                        offset: 16,
                        reserved: 0,
                    },
                ],
                color: 1,
                depth: 0,
                binding_model: 1,
                textures: vec![NativeTextureLayout {
                    stage: 1,
                    texture_slot: 0,
                    sampler_slot: 0,
                    format: 1,
                    comparison: 0,
                    reserved: 0,
                }],
                state,
            }],
            produced: BTreeMap::new(),
            dispatches: Vec::new(),
            buffers: vec![
                vec![0; 72],
                [0u32, 1, 2]
                    .into_iter()
                    .flat_map(u32::to_ne_bytes)
                    .collect(),
                vec![1; 16],
                vec![2; 16],
            ],
            images: vec![
                ImagePayload {
                    layout: None,
                    desc: ImageDesc {
                        width: 2,
                        height: 2,
                        format: ImageFormat::Rgba8,
                    },
                    bytes: None,
                },
                ImagePayload {
                    layout: None,
                    desc: ImageDesc {
                        width: 1,
                        height: 1,
                        format: ImageFormat::Rgba8,
                    },
                    bytes: Some(vec![127, 20, 30, 255].into()),
                },
            ],
            view_dimensions: vec![Dimension::D2],
            viewports: None,
            stencils: None,
            views: vec![raw::ImageView {
                image: 1,
                base_mip: 0,
                mip_count: 1,
                base_layer: 0,
                layer_count: 1,
                remap: [2, 3, 4, 5],
                reserved: 0,
            }],
            samplers: vec![sampler],
            passes: vec![PacketPass {
                source: source(),
                color: Attachment {
                    image: 0,
                    load: 0,
                    store: 0,
                    reserved: 0,
                    clear: [0.; 4],
                },
                depth: Attachment {
                    image: u32::MAX,
                    load: 0,
                    store: 0,
                    reserved: 0,
                    clear: [1., 0., 0., 0.],
                },
                draws: vec![draw],
            }],
            items: vec![raw::Item {
                pass: 0,
                reserved: 0,
                source_item: 0,
                unresolved_obligations: 0,
            }],
            output: 0,
            imports: Vec::new(),
        }
    }
    pub(crate) struct Catalog<'a>(pub(crate) &'a ScenePacket);
    impl PipelineCatalog for Catalog<'_> {
        fn owner_generation(&self) -> u64 {
            42
        }
        fn bindings(&self, _: u64) -> Option<&[BindingRequirement]> {
            None
        }
    }
    impl SceneCatalog for Catalog<'_> {
        fn scene_raster(&self, id: u64) -> Option<raw::RasterState> {
            self.0
                .pipelines
                .get(id.checked_sub(1)? as usize)
                .map(|p| p.state)
        }
        fn scene_graphics(&self, id: u64) -> Option<GraphicsLayout> {
            let p = self.0.pipelines.get(id.checked_sub(1)? as usize)?;
            let mut buffers = Vec::new();
            for (stage, idx) in [(Stage::Vertex, p.vertex), (Stage::Fragment, p.fragment)] {
                if let Some(prog) = self.0.programs.get(idx as usize) {
                    for b in &prog.buffers {
                        buffers.push(GraphicsBufferLayout {
                            stage,
                            binding: BindingRequirement {
                                slot: b.slot,
                                access: Access::Read,
                                minimum_bytes: b.minimum_bytes,
                                alignment: b.alignment_bytes as u64,
                            },
                        })
                    }
                }
            }
            Some(GraphicsLayout {
                vertex_stride: p.stride,
                color_format: Some(ImageFormat::Rgba8),
                fragment_stage: true,
                depth_attachment: p.depth != 0,
                depth: DepthState {
                    compare: DepthCompare::Disabled,
                    write: p.state.depth_enabled != 0 && p.state.depth_write != 0,
                },
                blend: Blend::Opaque,
                buffers,
                textures: p
                    .textures
                    .iter()
                    .map(|t| mag_render::scene::TextureLayout {
                        stage: stage(t.stage).unwrap(),
                        texture_slot: t.texture_slot,
                        sampler_slot: t.sampler_slot,
                        format: format(t.format).unwrap(),
                        comparison: t.comparison != 0,
                    })
                    .collect(),
            })
        }
    }
    #[derive(Default)]
    struct Compiler {
        creates: u64,
        released: Vec<u64>,
    }
    impl PipelineCompiler for Compiler {
        fn compile(&mut self, _: &Pipeline, _: &[Program]) -> Result<u64, i32> {
            self.creates += 1;
            Ok(self.creates)
        }
        fn release(&mut self, id: u64) -> Result<(), i32> {
            self.released.push(id);
            Ok(())
        }
    }
    #[test]
    fn item_draw_membership_preserves_full_width_partition_and_pass_identity() {
        for partition in [0, u64::MAX] {
            let mut packet = fixture();
            packet.items[0].source_item = u64::MAX;
            packet.passes[0].draws[0].item = u64::MAX;
            packet.passes[0].draws[0].partition = partition;
            packet.validate(32, 1 << 20).unwrap();
            let mut other = packet.passes[0].clone();
            other.draws.clear();
            packet.passes.push(other);
            packet.items.push(raw::Item {
                pass: 1,
                reserved: raw::ITEM_DRAW_REQUIRED,
                source_item: u64::MAX,
                unresolved_obligations: 0,
            });
            packet.intended = 2;
            assert_eq!(packet.validate(32, 1 << 20), Err(-1));
            let draw = packet.passes[0].draws[0].clone();
            packet.passes[1].draws.push(draw);
            packet.validate(32, 1 << 20).unwrap();
        }
    }
    #[test]
    fn source_complete_no_draw_kinds_preserve_population_without_fabricating_draws() {
        let mut packet = fixture();
        for kind in [
            raw::ITEM_SOURCE_REPEATED,
            raw::ITEM_SOURCE_UPDATE_ONLY,
            raw::ITEM_SOURCE_LISTS_DISABLED,
        ] {
            packet.items.push(raw::Item {
                pass: 0,
                reserved: kind,
                source_item: u64::from(kind),
                unresolved_obligations: 0,
            });
        }
        packet.intended = 4;
        packet.validate(32, 1 << 20).unwrap();
        assert_eq!(packet.passes[0].draws.len(), 1);
        let mut admitted = BTreeSet::from([(0, 0)]);
        packet.account_source_no_draw(&mut admitted, 1);
        assert_eq!(admitted.len(), 4);
        packet.items.push(raw::Item {
            pass: 0,
            reserved: raw::ITEM_POC_OMITTED_PRODUCER,
            source_item: 1 << 63,
            unresolved_obligations: 0,
        });
        packet.intended = 5;
        packet.validate(32, 1 << 20).unwrap();
        let mut admitted = BTreeSet::from([(0, 0)]);
        packet.account_source_no_draw(&mut admitted, 1);
        assert_eq!(admitted.len(), 4);
        assert_eq!(packet.intended - admitted.len() as u64, 1);
        assert_eq!(packet.passes[0].draws.len(), 1);
    }
    #[test]
    fn no_draw_reason_rejects_unknown_conflicting_or_unqualified_items() {
        for kind in [
            raw::ITEM_SOURCE_REPEATED,
            raw::ITEM_SOURCE_UPDATE_ONLY,
            raw::ITEM_SOURCE_LISTS_DISABLED,
            raw::ITEM_POC_OMITTED_PRODUCER,
            5,
            u32::MAX,
        ] {
            let mut packet = fixture();
            packet.items[0].reserved = kind;
            if kind == raw::ITEM_POC_OMITTED_PRODUCER {
                packet.items[0].source_item = 1 << 63;
                packet.passes[0].draws[0].item = 1 << 63;
            }
            assert_eq!(packet.validate(32, 1 << 20), Err(-1));
        }
        for (kind, item) in [
            (raw::ITEM_SOURCE_REPEATED, 1),
            (raw::ITEM_SOURCE_UPDATE_ONLY, 2),
            (raw::ITEM_SOURCE_LISTS_DISABLED, 3),
            (raw::ITEM_POC_OMITTED_PRODUCER, 1 << 63),
        ] {
            let mut packet = fixture();
            packet.flags = 1;
            packet.items.push(raw::Item {
                pass: 0,
                reserved: kind,
                source_item: item,
                unresolved_obligations: 16,
            });
            packet.intended = 2;
            assert_eq!(packet.validate(32, 1 << 20), Err(-1));
        }
        for (kind, item) in [
            (raw::ITEM_SOURCE_REPEATED, 1 << 63),
            (raw::ITEM_POC_OMITTED_PRODUCER, 1),
        ] {
            let mut packet = fixture();
            packet.items.push(raw::Item {
                pass: 0,
                reserved: kind,
                source_item: item,
                unresolved_obligations: 0,
            });
            packet.intended = 2;
            assert_eq!(packet.validate(32, 1 << 20), Err(-1));
        }
        let mut packet = fixture();
        packet.items[0].reserved = raw::ITEM_SOURCE_REPEATED;
        packet.passes[0].draws.clear();
        assert_eq!(
            packet.validate(32, 1 << 20),
            Err(-5),
            "all-no-draw guard remains"
        );
    }
    #[test]
    fn pruned_vertex_interface_still_refuses_extra_pipeline_attributes() {
        let mut packet = fixture();
        packet.programs[0].vertex_locations = vec![0];
        // Position-only source reflection cannot silently accept a retained UV
        // layout. Keep the exact -1 refusal and report both actual slot sets.
        assert_eq!(packet.validate(32, 1 << 20), Err(-1));
        packet.pipelines[0].attributes.retain(|a| a.location == 0);
        packet.validate(32, 1 << 20).unwrap();
        // Unused packed padding is allowed; the source stride remains exact.
        assert_eq!(packet.pipelines[0].stride, 24);
    }
    #[test]
    fn pipeline_failure_status_is_preserved_without_caching_a_failed_native_pipeline() {
        struct Rejected;
        impl PipelineCompiler for Rejected {
            fn compile(&mut self, _: &Pipeline, _: &[Program]) -> Result<u64, i32> {
                Err(-2)
            }
            fn release(&mut self, _: u64) -> Result<(), i32> {
                panic!("no pipeline exists")
            }
        }
        let packet = fixture();
        let mut cache = PipelineCache::default();
        assert_eq!(cache.resolve(&mut Rejected, &packet), Err(-2));
        assert!(cache.pipelines.is_empty());
        let mut compiler = Compiler::default();
        assert_eq!(cache.resolve(&mut compiler, &packet).unwrap(), vec![1]);
        assert_eq!(compiler.creates, 1);
    }
    #[test]
    fn complete_owned_material_packet_reaches_real_scene_builder() {
        let packet = fixture();
        packet.validate(32, 1 << 20).unwrap();
        let prepared = packet
            .prepare(&Catalog(&packet), &[1], 32, 1 << 20)
            .unwrap();
        let (commands, leases) = prepared.into_submission();
        assert_eq!(commands.passes().len(), 1);
        assert_eq!(commands.passes()[0].draws[0].textures.len(), 1);
        assert_eq!(leases.len(), 5);
        assert!(!commands.output().unwrap().diagnostic_readback);
    }
    #[test]
    fn explicit_repeat_mip_sampler_survives_scene_preparation_and_reflection_checks() {
        let mut packet = fixture();
        packet.samplers[0].wrap_s = 1;
        packet.samplers[0].wrap_t = 2;
        packet.samplers[0].mip_filter = 2;
        packet.samplers[0].lod_max = 13.0;
        packet.validate(32, 1 << 20).unwrap();
        let scene = packet
            .prepare(&Catalog(&packet), &[1], 32, 1 << 20)
            .unwrap();
        let (commands, _) = scene.into_submission();
        let Sampler::Explicit(state) = commands.passes()[0].draws[0].textures[0].sampler else {
            panic!("state was normalized to a preset")
        };
        assert_eq!(
            (state.wrap_s, state.wrap_t, state.mip_filter, state.lod_max),
            (1, 2, 2, 13.0)
        );
        packet.samplers[0].comparison_enabled = 1;
        assert_eq!(packet.validate(32, 1 << 20), Err(-2));
    }
    #[test]
    fn pipeline_cache_ignores_uniform_values_but_not_shader_or_reflection() {
        let mut packet = fixture();
        let mut compiler = Compiler::default();
        let mut cache = PipelineCache::default();
        let first = cache.resolve(&mut compiler, &packet).unwrap();
        packet.buffers[2][0] = 77;
        assert_eq!(cache.resolve(&mut compiler, &packet).unwrap(), first);
        assert_eq!(compiler.creates, 1);
        packet.programs[0].source.push(b'x');
        assert_ne!(cache.resolve(&mut compiler, &packet).unwrap(), first);
        assert_eq!(compiler.creates, 2);
        packet.programs[0].buffers[0].minimum_bytes = 32;
        cache.resolve(&mut compiler, &packet).unwrap();
        assert_eq!(compiler.creates, 3);
    }
    #[test]
    fn explicit_blend_cull_and_compare_changes_get_distinct_pipeline_ids() {
        let mut packet = fixture();
        let mut compiler = Compiler::default();
        let mut cache = PipelineCache::default();
        packet.pipelines[0].state.blend_enabled = 1;
        packet.pipelines[0].state.src_rgb = 6;
        packet.pipelines[0].state.dst_rgb = 7;
        packet.pipelines[0].state.src_alpha = 6;
        packet.pipelines[0].state.dst_alpha = 7;
        packet.validate(32, 1 << 20).unwrap();
        let first = cache.resolve(&mut compiler, &packet).unwrap();
        packet.pipelines[0].state.src_alpha = 1;
        assert_ne!(cache.resolve(&mut compiler, &packet).unwrap(), first);
        packet.pipelines[0].state.cull_mode = 2;
        cache.resolve(&mut compiler, &packet).unwrap();
        packet.pipelines[0].state.depth_enabled = 1;
        packet.pipelines[0].state.depth_compare = 6;
        cache.resolve(&mut compiler, &packet).unwrap();
        assert_eq!(compiler.creates, 4);
        assert_eq!(cache.resolve(&mut compiler, &packet).unwrap(), vec![4]);
    }
    #[test]
    fn unsupported_or_ambiguous_raster_state_never_becomes_a_preset() {
        let mut packet = fixture();
        let mut s = packet.pipelines[0].state;
        s.depth_bounds_enabled = 1;
        packet.pipelines[0].state = s;
        assert_eq!(packet.validate(32, 1 << 20), Err(-2));
        s.depth_bounds_min = 0.8;
        s.depth_bounds_max = 0.2;
        assert_eq!(raster(&s), Err(-1));
        s.depth_bounds_min = 0.;
        s.depth_bounds_max = 1.;
        s.depth_bounds_enabled = 0;
        s.bias_enabled = 1;
        assert_eq!(raster(&s), Err(-2));
        s.bias_enabled = 0;
        s.blend_enabled = 1;
        s.src_rgb = 10;
        s.dst_rgb = 12;
        s.blend_constant = [0.1, 0.2, 0.3, 0.4];
        assert_eq!(raster(&s), Err(-2));
        s.dst_rgb = 7;
        s.color_write_mask = 16;
        assert_eq!(raster(&s), Err(-1));
        s.color_write_mask = 15;
        s.blend_constant[0] = f32::NAN;
        assert_eq!(raster(&s), Err(-1));
    }
    #[test]
    fn pipeline_lru_releases_old_public_refs_under_bounded_capacity() {
        let mut packet = fixture();
        let mut compiler = Compiler::default();
        let mut cache = PipelineCache::default();
        for i in 0..=MAX_PIPELINES {
            packet.programs[0].source = i.to_le_bytes().to_vec();
            cache.resolve(&mut compiler, &packet).unwrap();
        }
        assert_eq!(cache.pipelines.len(), MAX_PIPELINES);
        assert_eq!(compiler.released, vec![1]);
    }
    #[test]
    fn layout_ranges_and_full_population_are_not_silently_repaired() {
        let mut packet = fixture();
        packet.pipelines[0].attributes[1].offset = 12;
        assert_eq!(packet.validate(32, 1 << 20), Err(-1));
        packet = fixture();
        packet.passes[0].draws[0].indices.offset = u64::MAX;
        assert_eq!(packet.validate(32, 1 << 20), Err(-1));
        packet = fixture();
        packet.items.push(raw::Item {
            pass: 0,
            reserved: 0,
            source_item: 1,
            unresolved_obligations: 0,
        });
        packet.intended = 2;
        assert_eq!(packet.validate(32, 1 << 20), Err(-1));
        packet.flags = 1;
        packet.items[1].unresolved_obligations = 16;
        packet.validate(32, 1 << 20).unwrap();
    }
    #[test]
    fn real_scene_validation_rejects_uninitialized_or_wrong_stage_bindings() {
        let mut packet = fixture();
        packet.passes[0].color.load = 1;
        assert!(
            packet
                .prepare(&Catalog(&packet), &[1], 32, 1 << 20)
                .is_err()
        );
        packet = fixture();
        packet.passes[0].draws[0].buffers[1].stage = 0;
        assert!(
            packet
                .prepare(&Catalog(&packet), &[1], 32, 1 << 20)
                .is_err()
        );
    }
    #[test]
    fn admitted_pipeline_is_never_evicted_until_its_pin_is_cancelled() {
        let mut packet = fixture();
        let mut cache = PipelineCache::default();
        let mut compiler = Compiler::default();
        let first = cache.resolve(&mut compiler, &packet).unwrap();
        cache.pin(&first).unwrap();
        for i in 0..256u32 {
            packet.programs[0].source = i.to_le_bytes().to_vec();
            cache.resolve(&mut compiler, &packet).unwrap();
        }
        assert!(!compiler.released.contains(&first[0]));
        cache.unpin(&first).unwrap();
        packet.programs[0].source = b"aftercancel".to_vec();
        cache.resolve(&mut compiler, &packet).unwrap();
        assert!(compiler.released.contains(&first[0]));
    }
    #[test]
    fn all_pinned_pipeline_capacity_refuses_without_releasing_an_admission() {
        let mut packet = fixture();
        let mut cache = PipelineCache::default();
        let mut compiler = Compiler::default();
        for i in 0..256u32 {
            packet.programs[0].source = i.to_le_bytes().to_vec();
            let ids = cache.resolve(&mut compiler, &packet).unwrap();
            cache.pin(&ids).unwrap();
        }
        packet.programs[0].source = b"overflow".to_vec();
        assert_eq!(cache.resolve(&mut compiler, &packet), Err(-5));
        assert!(compiler.released.is_empty());
    }
    fn copy_view(packet: &ScenePacket) -> ScenePacket {
        copy_view_with_prefix(packet, &[], &[])
    }
    thread_local! {
        /// Per-image content keys the next test copy passes as raw::Image::reserved.
        static TEST_IMAGE_KEYS: std::cell::RefCell<Vec<u32>> = const { std::cell::RefCell::new(Vec::new()) };
    }
    fn copy_view_keyed(packet: &ScenePacket, keys: &[u32]) -> ScenePacket {
        TEST_IMAGE_KEYS.with(|k| *k.borrow_mut() = keys.to_vec());
        let copy = copy_view(packet);
        TEST_IMAGE_KEYS.with(|k| k.borrow_mut().clear());
        copy
    }
    pub(crate) fn copy_view_with_prefix(
        packet: &ScenePacket,
        dispatches: &[raw::Dispatch],
        produced: &[u32],
    ) -> ScenePacket {
        let programs: Vec<_> = packet
            .programs
            .iter()
            .map(|p| raw::Program {
                stage: p.stage,
                reserved: p.flags,
                shader: p.shader(),
                buffers: p.buffers.as_ptr(),
                buffer_count: p.buffers.len() as u32,
                texture_count: p.textures.len() as u32,
                textures: p.textures.as_ptr(),
                samplers: p.samplers.as_ptr(),
                sampler_count: p.samplers.len() as u32,
                vertex_location_count: p.vertex_locations.len() as u32,
                vertex_locations: p.vertex_locations.as_ptr(),
                threads: p.threads,
                reserved1: 0,
            })
            .collect();
        let pipelines: Vec<_> = packet
            .pipelines
            .iter()
            .map(|p| raw::Pipeline {
                vertex_program: p.vertex,
                fragment_program: p.fragment,
                vertex_stride: p.stride,
                attribute_count: p.attributes.len() as u32,
                attributes: p.attributes.as_ptr(),
                color_format: p.color,
                depth_format: p.depth,
                binding_model: p.binding_model,
                reserved: 0,
                textures: p.textures.as_ptr(),
                texture_count: p.textures.len() as u32,
                reserved1: 0,
                state: p.state,
            })
            .collect();
        let buffers: Vec<_> = packet
            .buffers
            .iter()
            .enumerate()
            .map(|(index, b)| raw::Buffer {
                role: u32::from(produced.contains(&(index as u32))),
                reserved: 0,
                bytes: if produced.contains(&(index as u32)) {
                    std::ptr::null()
                } else {
                    b.as_ptr().cast()
                },
                length: b.len() as u64,
            })
            .collect();
        let subs: Vec<Vec<_>> = packet
            .images
            .iter()
            .map(|image| {
                let Some(bytes) = &image.bytes else {
                    return vec![];
                };
                if let Some(layout) = &image.layout {
                    layout
                        .subresources
                        .iter()
                        .map(|s| raw::Subresource {
                            mip: s.mip,
                            layer: s.layer,
                            width: s.width,
                            height: s.height,
                            depth: s.depth,
                            row_bytes: s.row_bytes,
                            bytes: bytes[s.offset as usize..].as_ptr().cast(),
                            length: s.length,
                        })
                        .collect()
                } else {
                    let level = image
                        .desc
                        .format
                        .level_layout(image.desc.width, image.desc.height)
                        .unwrap();
                    vec![raw::Subresource {
                        mip: 0,
                        layer: 0,
                        width: image.desc.width,
                        height: image.desc.height,
                        depth: 1,
                        row_bytes: level.row_bytes as u32,
                        bytes: bytes.as_ptr().cast(),
                        length: level.bytes,
                    }]
                }
            })
            .collect();
        let images: Vec<_> = packet
            .images
            .iter()
            .zip(&subs)
            .enumerate()
            .map(|(index, (image, subs))| {
                let layout = image.layout.clone().unwrap_or_default();
                raw::Image {
                    format: image.desc.format.native_id(),
                    dimension: layout.dimension.native_id(),
                    width: image.desc.width,
                    height: image.desc.height,
                    depth: layout.depth,
                    layers: layout.layers,
                    mip_count: layout.mip_count,
                    samples: layout.samples,
                    subresources: subs.as_ptr(),
                    subresource_count: subs.len() as u32,
                    reserved: TEST_IMAGE_KEYS.with(|k| k.borrow().get(index).copied().unwrap_or(0)),
                }
            })
            .collect();
        let draws: Vec<Vec<_>> = packet
            .passes
            .iter()
            .map(|p| {
                p.draws
                    .iter()
                    .map(|d| raw::Draw {
                        source_item: d.item,
                        source_partition: d.partition,
                        unresolved_obligations: d.obligations,
                        pipeline: d.pipeline,
                        index_count: d.count,
                        vertices: d.vertices,
                        indices: d.indices,
                        buffers: d.buffers.as_ptr(),
                        buffer_count: d.buffers.len() as u32,
                        texture_count: d.textures.len() as u32,
                        textures: d.textures.as_ptr(),
                    })
                    .collect()
            })
            .collect();
        let passes: Vec<_> = packet
            .passes
            .iter()
            .zip(&draws)
            .map(|(p, d)| raw::Pass {
                source: p.source,
                color: p.color,
                depth: p.depth,
                draws: d.as_ptr(),
                draw_count: d.len() as u32,
                reserved: 0,
            })
            .collect();
        let desc = raw::Desc {
            abi_version: 1,
            struct_size: size_of::<raw::Desc>() as u32,
            flags: packet.flags,
            reserved: 0,
            source: packet.source,
            intended_items: packet.intended,
            programs: programs.as_ptr(),
            program_count: programs.len() as u32,
            pipeline_count: pipelines.len() as u32,
            pipelines: pipelines.as_ptr(),
            buffers: buffers.as_ptr(),
            buffer_count: buffers.len() as u32,
            image_count: images.len() as u32,
            images: images.as_ptr(),
            views: packet.views.as_ptr(),
            view_count: packet.views.len() as u32,
            sampler_count: packet.samplers.len() as u32,
            samplers: packet.samplers.as_ptr(),
            dispatches: dispatches.as_ptr(),
            dispatch_count: dispatches.len() as u32,
            pass_count: passes.len() as u32,
            passes: passes.as_ptr(),
            items: packet.items.as_ptr(),
            item_count: packet.items.len() as u32,
            output_image: packet.output,
            target: std::ptr::null(),
            frame: std::ptr::null(),
        };
        if let Some(viewports) = &packet.viewports {
            let viewports: Vec<_> = viewports
                .iter()
                .map(|v| raw::Viewport {
                    x: v.x,
                    y: v.y,
                    width: v.width,
                    height: v.height,
                    min_depth: v.min_depth,
                    max_depth: v.max_depth,
                    scissor_x: v.scissor_x,
                    scissor_y: v.scissor_y,
                    scissor_width: v.scissor_width,
                    scissor_height: v.scissor_height,
                    known_fields: v.known_fields,
                    reserved: v.reserved,
                })
                .collect();
            let dims: Vec<_> = packet
                .view_dimensions
                .iter()
                .map(|d| d.native_id())
                .collect();
            let desc = raw::DescV2 {
                abi_version: 2,
                struct_size: size_of::<raw::DescV2>() as u32,
                base: desc,
                viewports: viewports.as_ptr(),
                viewport_count: viewports.len() as u32,
                reserved: 0,
                view_dimensions: dims.as_ptr(),
                view_dimension_count: dims.len() as u32,
                reserved1: 0,
            };
            if let Some(stencils) = &packet.stencils {
                let v3 = raw::DescV3 {
                    abi_version: 3,
                    struct_size: size_of::<raw::DescV3>() as u32,
                    base: desc,
                    stencils: stencils.as_ptr(),
                    stencil_count: stencils.len() as u32,
                    reserved: 0,
                };
                unsafe { ScenePacket::copy_v3(&v3, 32, 1 << 20) }.unwrap()
            } else {
                unsafe { ScenePacket::copy_v2(&desc, 32, 1 << 20) }.unwrap()
            }
        } else {
            unsafe { ScenePacket::copy(&desc, 32, 1 << 20) }.unwrap()
        }
    }
    #[test]
    fn v2_copies_complete_bc_cube_mips_explicit_view_remap_and_fractional_viewport() {
        let mut packet = fixture();
        let mut layout = ImageLayout {
            dimension: Dimension::Cube,
            layers: 6,
            mip_count: 3,
            ..ImageLayout::default()
        };
        for layer in 0..6 {
            for mip in 0..3 {
                layout.subresources.push(Subresource {
                    mip,
                    layer,
                    width: 4 >> mip,
                    height: 4 >> mip,
                    depth: 1,
                    row_bytes: 8,
                    offset: u64::from(layer * 3 + mip) * 16,
                    length: 8,
                })
            }
        }
        packet.images[1] = ImagePayload {
            desc: ImageDesc {
                width: 4,
                height: 4,
                format: ImageFormat::Bc1,
            },
            layout: Some(layout),
            bytes: Some(vec![7; 288].into()),
        };
        packet.pipelines[0].textures[0].format = 6;
        packet.programs[1].textures[0].dimension = 4;
        packet.views[0].mip_count = 3;
        packet.views[0].layer_count = 6;
        packet.views[0].remap = [2, 3, 4, 1];
        packet.view_dimensions[0] = Dimension::Cube;
        packet.viewports = Some(vec![Viewport {
            x: 0.125,
            y: 0.25,
            width: 2.,
            height: 2.,
            min_depth: 0.,
            max_depth: 1.,
            scissor_x: 0,
            scissor_y: 0,
            scissor_width: 2,
            scissor_height: 2,
            known_fields: 7,
            reserved: 0,
        }]);
        let copied = copy_view(&packet);
        assert_eq!(copied.images[1].bytes.as_ref().unwrap().len(), 288);
        let prepared = copied
            .prepare(&Catalog(&copied), &[1], 32, 1 << 20)
            .unwrap();
        let (commands, _) = prepared.into_submission();
        let view = commands.passes()[0].draws[0].textures[0]
            .image
            .view()
            .unwrap();
        assert_eq!(view.remap, [0, 1, 2, 5]);
        assert_eq!(view.dimension, Dimension::Cube);
        assert_eq!(commands.viewports()[0].unwrap().x, 0.125);
        assert_eq!(
            commands.images()[1]
                .layout
                .as_ref()
                .unwrap()
                .subresources
                .len(),
            18
        );
    }
    #[test]
    fn keyed_image_bytes_are_copied_once_and_reused_for_the_same_key() {
        // Unique keys for this test: the table is process-global.
        let base = 0x5000_0000 + (std::process::id() & 0xfff) * 16;
        let packet = fixture();
        // The owned copy's packed (16-byte padded) bytes are the reference.
        let original = copy_view(&packet).images[1]
            .bytes
            .as_ref()
            .unwrap()
            .to_vec();
        let first = copy_view_keyed(&packet, &[0, base]);
        let Some(ImageBytes::Resident(a)) = &first.images[1].bytes else {
            panic!("first sight of a keyed image becomes resident");
        };
        assert_eq!(a.bytes(), original.as_slice());
        // The key promises identical content, so a hit never re-reads the
        // caller's bytes: changed bytes under the same key are not observed.
        let mut changed = fixture();
        let source_len = changed.images[1].bytes.as_ref().unwrap().len();
        changed.images[1].bytes = Some(vec![0x55; source_len].into());
        let second = copy_view_keyed(&changed, &[0, base]);
        let Some(ImageBytes::Resident(b)) = &second.images[1].bytes else {
            panic!("same key reuses the resident bytes");
        };
        assert_eq!(a.cache_key(), b.cache_key());
        assert_eq!(b.bytes(), original.as_slice());
        // A different key, or no key, copies the caller's current bytes.
        let other = copy_view_keyed(&changed, &[0, base + 1]);
        assert_eq!(
            &other.images[1].bytes.as_ref().unwrap()[..source_len],
            &vec![0x55; source_len][..]
        );
        let unkeyed = copy_view(&changed);
        assert!(matches!(
            unkeyed.images[1].bytes,
            Some(ImageBytes::Owned(_))
        ));
        // Resident leases prepare like owned bytes, with the same contents.
        second
            .prepare(&Catalog(&second), &[1], 32, 1 << 20)
            .unwrap();
        first.prepare(&Catalog(&first), &[1], 32, 1 << 20).unwrap();
    }
    #[test]
    fn keyed_table_rejects_shape_changes_and_stays_bounded() {
        let packet = copy_view(&fixture());
        let image = &packet.images[1];
        let layout = image.layout.clone().unwrap();
        let bytes = image.bytes.as_ref().unwrap().to_vec();
        let mut table = keyed_images::Table::new(2, 4 * bytes.len()).unwrap();
        for key in 1..=3 {
            assert!(
                table
                    .insert(
                        key,
                        image.desc,
                        layout.clone(),
                        bytes.clone(),
                        2,
                        4 * bytes.len()
                    )
                    .is_ok()
            );
        }
        // Two entries at most: the least recently used key was evicted.
        assert!(table.get(1, image.desc, &layout).is_none());
        assert!(table.get(3, image.desc, &layout).is_some());
        let mut other = layout.clone();
        other.mip_count += 1;
        assert!(table.get(3, image.desc, &other).is_none());
        // Oversized payloads stay owned by the caller.
        let huge = vec![0; bytes.len() * 2];
        assert_eq!(
            table
                .insert(9, image.desc, layout, huge, 2, 4 * bytes.len())
                .err()
                .map(|b| b.len()),
            Some(bytes.len() * 2)
        );
    }
    #[test]
    fn synchronous_transport_owns_shader_uniform_geometry_and_texels() {
        let mut packet = fixture();
        let copy = copy_view(&packet);
        packet.programs[0].source.fill(0);
        packet.buffers[0].fill(1);
        packet.buffers[2].fill(9);
        packet.images[1].bytes =
            Some(vec![0; packet.images[1].bytes.as_ref().unwrap().len()].into());
        assert_ne!(copy.programs[0].source, packet.programs[0].source);
        assert_eq!(copy.buffers[0], vec![0; 72]);
        assert_eq!(copy.buffers[2], vec![1; 16]);
        assert_eq!(
            &copy.images[1].bytes.as_ref().unwrap()[..4],
            &[127, 20, 30, 255]
        );
        assert!(
            copy.images[1].bytes.as_ref().unwrap()[4..]
                .iter()
                .all(|b| *b == 0)
        );
        copy.prepare(&Catalog(&copy), &[1], 32, 1 << 20).unwrap();
    }

    #[test]
    fn stencil_reference_masks_and_ops_have_distinct_cached_pipeline_ids() {
        let mut packet = fixture();
        let mut cache = PipelineCache::default();
        let mut compiler = Compiler::default();
        packet.pipelines[0].depth = 15;
        packet.pipelines[0].state.stencil_enabled = 1;
        let initial = cache.resolve(&mut compiler, &packet).unwrap();
        packet.pipelines[0].state.front_stencil.reference = 0x81;
        assert_ne!(cache.resolve(&mut compiler, &packet).unwrap(), initial);
        packet.pipelines[0].state.back_stencil.write_mask = 255;
        cache.resolve(&mut compiler, &packet).unwrap();
        packet.pipelines[0].state.back_stencil.pass = 2;
        cache.resolve(&mut compiler, &packet).unwrap();
        assert_eq!(compiler.creates, 4);
    }
    #[test]
    fn v3_copies_independent_stencil_without_inventing_depth_clear() {
        checked_v3_layout().unwrap();
        let mut packet = fixture();
        packet.pipelines[0].depth = 15;
        packet.pipelines[0].state.stencil_enabled = 1;
        let face = raw::StencilFace {
            compare: 7,
            fail: 2,
            depth_fail: 2,
            pass: 2,
            reference: 0x81,
            read_mask: 0,
            write_mask: 255,
            reserved: 0,
        };
        packet.pipelines[0].state.front_stencil = face;
        packet.pipelines[0].state.back_stencil = face;
        packet.images.push(ImagePayload {
            desc: ImageDesc {
                width: 2,
                height: 2,
                format: ImageFormat::Depth24Stencil8,
            },
            layout: None,
            bytes: None,
        });
        packet.passes[0].depth = Attachment {
            image: 2,
            load: 2,
            store: 0,
            reserved: 0,
            clear: [0.; 4],
        };
        packet.viewports = Some(vec![Viewport {
            x: 0.,
            y: 0.,
            width: 2.,
            height: 2.,
            min_depth: 0.,
            max_depth: 1.,
            scissor_x: 0,
            scissor_y: 0,
            scissor_width: 2,
            scissor_height: 2,
            known_fields: 7,
            reserved: 0,
        }]);
        packet.stencils = Some(vec![raw::StencilAttachment {
            image: 2,
            load: 0,
            store: 0,
            clear_value: 0x81,
            flags: 0,
            reserved: 0,
        }]);
        let copied = copy_view(&packet);
        let prepared = copied
            .prepare(&Catalog(&copied), &[1], 32, 1 << 20)
            .unwrap();
        let (commands, _) = prepared.into_submission();
        assert!(matches!(
            commands.passes()[0].depth.unwrap().load,
            Load::Discard
        ));
        assert_eq!(
            commands.stencils()[0].unwrap().load,
            scene::StencilLoad::Clear(0x81)
        );
        packet.stencils.as_mut().unwrap()[0].clear_value = 256;
        assert!(packet.validate(32, 1 << 20).is_err());
        packet.stencils = None;
        assert_eq!(packet.validate(32, 1 << 20), Err(-2));
    }
    #[test]
    fn imported_inputs_are_conditional_source_requirements_not_fresh_textures() {
        let mut packet = fixture();
        packet.images[1].bytes = None;
        assert!(
            packet
                .prepare(&Catalog(&packet), &[1], 32, 1 << 20)
                .is_err()
        );
        packet.imports = vec![raw::ImageRequirement {
            image: 1,
            required_initialized_planes: 1,
            source_key: 99,
        }];
        packet.validate(32, 1 << 20).unwrap();
        let prepared = packet
            .prepare(&Catalog(&packet), &[1], 32, 1 << 20)
            .unwrap();
        assert_eq!(prepared.images()[1].external.unwrap().source_key, 99);
        assert!(prepared.images()[1].upload.is_none());
        packet.imports[0].required_initialized_planes = 0;
        assert!(
            packet
                .prepare(&Catalog(&packet), &[1], 32, 1 << 20)
                .is_err()
        );
        packet.imports[0].required_initialized_planes = 2;
        assert_eq!(packet.validate(32, 1 << 20), Err(-1));
        packet.imports[0].required_initialized_planes = 1;
        packet.images[1].bytes = Some(vec![0; 4].into());
        assert_eq!(packet.validate(32, 1 << 20), Err(-1));
    }
    fn bounds_fixture() -> ScenePacket {
        let mut packet = fixture();
        packet.programs[1].flags = raw::PROGRAM_STORED_DEPTH_AUX;
        packet.images.push(ImagePayload {
            desc: ImageDesc {
                width: 2,
                height: 2,
                format: ImageFormat::Depth32,
            },
            layout: None,
            bytes: None,
        });
        packet.imports = vec![raw::ImageRequirement {
            image: 2,
            required_initialized_planes: 2,
            source_key: 101,
        }];
        packet.passes[0].depth.image = 2;
        packet.passes[0].depth.load = 1;
        let r = &mut packet.pipelines[0];
        r.depth = 2;
        r.state.depth_bounds_enabled = 1;
        r.state.depth_bounds_min = 0.2;
        r.state.depth_bounds_max = 0.7;
        packet
    }
    #[test]
    fn stored_depth_mode_requires_initialized_readonly_attachment_and_exact_aux_mode() {
        let mut packet = bounds_fixture();
        packet.validate(32, 1 << 20).unwrap();
        let prepared = packet
            .prepare(&Catalog(&packet), &[1], 32, 1 << 20)
            .unwrap();
        assert_eq!(
            prepared.depth_bounds()[0][0],
            Some(mag_render::scene::DepthBounds {
                minimum: 0.2,
                maximum: 0.7
            })
        );
        // Actual buffers/images plus depth snapshot fit 180 bytes only if the
        // hidden per-draw range allocation is incorrectly omitted.
        assert!(packet.prepare(&Catalog(&packet), &[1], 32, 180).is_err());
        packet.imports[0].required_initialized_planes = 0;
        assert!(
            packet
                .prepare(&Catalog(&packet), &[1], 32, 1 << 20)
                .is_err()
        );
        packet.imports[0].required_initialized_planes = 2;
        packet.passes[0].depth.load = 0;
        assert!(
            packet
                .prepare(&Catalog(&packet), &[1], 32, 1 << 20)
                .is_err()
        );
        packet.passes[0].depth.load = 1;
        packet.pipelines[0].state.depth_enabled = 1;
        packet.pipelines[0].state.depth_write = 1;
        assert!(
            packet
                .prepare(&Catalog(&packet), &[1], 32, 1 << 20)
                .is_err()
        );
        packet.pipelines[0].state.depth_write = 0;
        packet.programs[1].flags = 0;
        assert_eq!(packet.validate(32, 1 << 20), Err(-2));
        packet.programs[1].flags = 2;
        assert_eq!(packet.validate(32, 1 << 20), Err(-1));
        packet.programs[1].flags = 1;
        packet.programs[1].buffers[0].slot = 2;
        assert_eq!(packet.validate(32, 1 << 20), Err(-2));
    }
    #[test]
    fn dynamic_depth_bounds_reuse_pso_but_mode_and_shader_identity_do_not() {
        let mut packet = bounds_fixture();
        let mut cache = PipelineCache::default();
        let mut compiler = Compiler::default();
        let first = cache.resolve(&mut compiler, &packet).unwrap();
        packet.pipelines[0].state.depth_bounds_max = 0.9;
        assert_eq!(cache.resolve(&mut compiler, &packet).unwrap(), first);
        packet.programs[1].source.push(9);
        assert_ne!(cache.resolve(&mut compiler, &packet).unwrap(), first);
    }
}

fn checked_v3_layout() -> Result<(), i32> {
    unsafe extern "C" {
        fn mag_native_packet_v3_layout(out: *mut u32, words: u32) -> i32;
    }
    let expected = raw::v3_layout();
    let mut actual = vec![0; expected.len()];
    let status = unsafe { mag_native_packet_v3_layout(actual.as_mut_ptr(), actual.len() as u32) };
    if status != 0 {
        return Err(status);
    }
    if actual != expected {
        return Err(-1);
    }
    Ok(())
}

/// Rate-limited `[mag native resident buffers]` line; only with the switch on.
fn log_resident_buffers(
    uploads: Option<super::scene_upload_cache::UploadCacheStats>,
    buffers: crate::batch_device::scene_buffer_cache::ResidentBufferStats,
    arenas: crate::batch_device::scene_arena_pool::ArenaStats,
) {
    let uploads = uploads.unwrap_or_default();
    use std::sync::atomic::{AtomicU64, Ordering};
    static N: AtomicU64 = AtomicU64::new(0);
    let n = N.fetch_add(1, Ordering::Relaxed) + 1;
    if n > 8 && !n.is_multiple_of(120) {
        return;
    }
    eprintln!(
        "[mag native resident buffers] submission={n} upload_hits={} upload_misses={} upload_hit_bytes={} upload_evictions={} upload_entries={} upload_bytes={} gpu_hits={} gpu_misses={} gpu_hit_bytes={} gpu_retired={} gpu_entries={} gpu_bytes={} gpu_pinned_bytes={} arena_reuses={} arena_creates={} arena_written_bytes={} arena_write_refusals={} arena_retired={} arena_free={} arena_free_bytes={} arena_owned={}",
        uploads.hits,
        uploads.misses,
        uploads.hit_bytes,
        uploads.evictions,
        uploads.entries,
        uploads.resident_bytes,
        buffers.hits,
        buffers.misses,
        buffers.hit_bytes,
        buffers.retired,
        buffers.entries,
        buffers.resident_bytes,
        buffers.pinned_bytes,
        arenas.reuses,
        arenas.creates,
        arenas.written_bytes,
        arenas.write_refusals,
        arenas.retired,
        arenas.free,
        arenas.free_bytes,
        arenas.owned
    );
}
