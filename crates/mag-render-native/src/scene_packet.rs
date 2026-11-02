//! Owned full-scene ingress sharing the production client's context and worker.
//! Source shaders are already lowered. Unknown state and unsupported resource
//! shapes are rejected as a whole; no attributes/materials are synthesized.
use super::*;
use crate::{
    batch_device::{
        ShaderStage,
        scene_device::{
            BufferLayout, SceneGraphicsV2Desc, TextureLayout as NativeTextureLayout,
            VertexAttribute,
        },
    },
    host_frame::HostFrame,
};
use mag_render::sdk_compute::BindingLayout;
pub use mag_render_abi::Attachment;
pub use mag_render_abi::packet as raw;
unsafe extern "C" {
    fn mag_native_packet_layout(out: *mut u32, words: u32) -> i32;
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
fn raster(s: &raw::RasterState) -> Result<(u32, u32, u32), i32> {
    // The existing native ABI represents these exact states only. In particular
    // its alpha preset uses ONE for source alpha, not SRC_ALPHA.
    if s.known_fields != 511
        || s.unresolved_obligations != 0
        || s.reserved != 0
        || s.cull_mode != 0
        || s.front_face > 1
        || s.color_write_mask != 15
        || s.stencil_enabled != 0
        || s.depth_bounds_enabled != 0
        || s.bias_enabled != 0
        || s.topology != 0
        || s.samples != 1
        || s.depth_enabled > 1
        || s.depth_write > 1
        || s.blend_enabled > 1
    {
        return Err(-2);
    }
    let depth = if s.depth_enabled == 0 {
        if s.depth_write != 0 {
            return Err(-2);
        }
        0
    } else {
        match s.depth_compare {
            1 => 1,
            3 => 2,
            7 => 3,
            _ => return Err(-2),
        }
    };
    let blend = if s.blend_enabled == 0 {
        0
    } else {
        if (
            s.src_rgb,
            s.dst_rgb,
            s.rgb_equation,
            s.src_alpha,
            s.dst_alpha,
            s.alpha_equation,
        ) != (6, 7, 0, 1, 7, 0)
        {
            return Err(-2);
        }
        1
    };
    Ok((depth, s.depth_write, blend))
}
#[cfg(test)]
mod tests {
    use super::*;
    fn state() -> raw::RasterState {
        unsafe { std::mem::zeroed() }
    }
    #[test]
    fn actual_alpha_factor_is_not_native_alpha_preset() {
        let mut s = state();
        s.known_fields = 511;
        s.samples = 1;
        s.color_write_mask = 15;
        s.blend_enabled = 1;
        s.src_rgb = 6;
        s.dst_rgb = 7;
        s.src_alpha = 6;
        s.dst_alpha = 7;
        assert_eq!(raster(&s), Err(-2));
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
        words(&mut k, &[self.stage, self.language]);
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
    pub desc: ImageDesc,
    pub bytes: Option<Vec<u8>>,
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
    pub images: Vec<ImagePayload>,
    pub views: Vec<raw::ImageView>,
    pub samplers: Vec<raw::Sampler>,
    pub passes: Vec<PacketPass>,
    pub items: Vec<raw::Item>,
    pub output: u32,
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
    match f {
        1 => Ok(ImageFormat::Rgba8),
        2 => Ok(ImageFormat::Depth32),
        3 => Ok(ImageFormat::Bgra8),
        _ => Err(-2),
    }
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
        // The graph currently receives already prepared graphics buffers. GPU compute
        // packet admission will use the same prefix once its explicit contract lands.
        if d.dispatch_count != 0 {
            return Err(-2);
        }
        let mut budget = Budget {
            remaining: max_bytes,
        };
        let mut programs = Vec::new();
        for p in unsafe { slice(d.programs, d.program_count as usize) }? {
            if p.stage > 1
                || p.reserved != 0
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
        for b in unsafe { slice(d.buffers, d.buffer_count as usize) }? {
            if b.reserved != 0 || b.length == 0 {
                return Err(-1);
            }
            if b.role != 0 {
                return Err(-2);
            }
            buffers.push(unsafe {
                budget.copy(
                    b.bytes.cast::<u8>(),
                    usize::try_from(b.length).map_err(|_| -5)?,
                )
            }?);
        }
        let mut images = Vec::new();
        for i in unsafe { slice(d.images, d.image_count as usize) }? {
            let f = format(i.format)?;
            if i.reserved != 0 || i.width == 0 || i.height == 0 {
                return Err(-1);
            }
            if i.dimension != 2
                || i.depth != 1
                || i.layers != 1
                || i.mip_count != 1
                || i.samples != 1
            {
                return Err(-2);
            }
            let payload = match unsafe { slice(i.subresources, i.subresource_count as usize) }? {
                [] => None,
                [s] => {
                    let row = i.width.checked_mul(4).ok_or(-5)?;
                    if f == ImageFormat::Depth32
                        || s.mip != 0
                        || s.layer != 0
                        || s.width != i.width
                        || s.height != i.height
                        || s.depth != 1
                        || s.row_bytes != row
                        || s.length != u64::from(row) * u64::from(i.height)
                    {
                        return Err(-2);
                    }
                    Some(unsafe {
                        budget.copy(
                            s.bytes.cast::<u8>(),
                            usize::try_from(s.length).map_err(|_| -5)?,
                        )
                    }?)
                }
                _ => return Err(-2),
            };
            images.push(ImagePayload {
                desc: ImageDesc {
                    width: i.width,
                    height: i.height,
                    format: f,
                },
                bytes: payload,
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
            images,
            views,
            samplers,
            passes,
            items,
            output: d.output_image,
        };
        packet.validate(max_draws, max_bytes)?;
        Ok(packet)
    }
}
fn sampler(s: &raw::Sampler) -> Result<Sampler, i32> {
    if s.known_fields != 63
        || s.min_filter != s.mag_filter
        || s.mip_filter != 0
        || s.wrap_s != 0
        || s.wrap_t != 0
        || s.wrap_r != 0
        || s.lod_min != 0.0
        || s.lod_max != 0.0
        || s.lod_bias != 0.0
        || s.max_anisotropy != 1.0
    {
        return Err(-2);
    }
    if s.comparison_enabled == 1 {
        if s.comparison != 3 || s.min_filter != 1 {
            return Err(-2);
        }
        Ok(Sampler::CompareLessEqual)
    } else if s.comparison_enabled == 0 {
        match s.min_filter {
            0 => Ok(Sampler::NearestClamp),
            1 => Ok(Sampler::LinearClamp),
            _ => Err(-2),
        }
    } else {
        Err(-2)
    }
}
impl ScenePacket {
    fn range(&self, u: raw::BufferUse) -> Result<ByteRange, i32> {
        let b = self.buffers.get(u.buffer as usize).ok_or(-1)?;
        if u.reserved != 0
            || u.length == 0
            || u.offset.checked_add(u.length).ok_or(-1)? > b.len() as u64
        {
            return Err(-1);
        }
        Ok(ByteRange {
            offset: u.offset,
            length: u.length,
        })
    }
    fn validate(&self, max_draws: usize, max_bytes: u64) -> Result<(), i32> {
        if self.items.len() as u64 != self.intended
            || self.images.get(self.output as usize).is_none()
            || self.passes.is_empty()
        {
            return Err(-1);
        }
        for p in &self.programs {
            let mut slots = BTreeSet::new();
            for b in &p.buffers {
                if b.slot >= 16
                    || !slots.insert(b.slot)
                    || b.access != 1
                    || b.minimum_bytes == 0
                    || b.alignment_bytes == 0
                    || !b.alignment_bytes.is_power_of_two()
                    || b.binding_kind > 1
                {
                    return Err(-1);
                }
            }
            slots.clear();
            for t in &p.textures {
                if t.slot >= 16 || !slots.insert(t.slot) || t.reserved != 0 {
                    return Err(-1);
                }
                if t.dimension != 2 || t.sample_kind > 3 || t.arrayed != 0 || t.multisampled != 0 {
                    return Err(-2);
                }
            }
            slots.clear();
            for s in &p.samplers {
                if s.slot >= 16 || s.comparison > 1 || !slots.insert(s.slot) {
                    return Err(-1);
                }
            }
            slots.clear();
            for loc in &p.vertex_locations {
                if *loc >= 16 || !slots.insert(*loc) {
                    return Err(-1);
                }
            }
        }
        for p in &self.pipelines {
            raster(&p.state)?;
            let vp = self.programs.get(p.vertex as usize).ok_or(-1)?;
            let fp = if p.fragment == u32::MAX {
                None
            } else {
                Some(self.programs.get(p.fragment as usize).ok_or(-1)?)
            };
            if vp.stage != 0
                || fp.is_some_and(|f| f.stage != 1)
                || p.binding_model > 1
                || p.color > 3
                || p.depth != 0 && p.depth != 2
                || p.color == 2
                || p.color != 0 && fp.is_none()
                || p.stride == 0
                || p.stride > 2048
                || !p.stride.is_multiple_of(4)
            {
                return Err(-1);
            }
            let mut slots = BTreeSet::new();
            let mut ranges = Vec::new();
            for a in &p.attributes {
                let length = match a.format {
                    1..=4 => a.format * 4,
                    5 => 4,
                    _ => return Err(-2),
                };
                if a.reserved != 0
                    || a.location >= 16
                    || !slots.insert(a.location)
                    || !a.offset.is_multiple_of(4)
                    || a.offset.checked_add(length).ok_or(-1)? > p.stride
                {
                    return Err(-1);
                }
                if ranges
                    .iter()
                    .any(|&(start, end)| a.offset < end && start < a.offset + length)
                {
                    return Err(-1);
                }
                ranges.push((a.offset, a.offset + length));
            }
            if slots != vp.vertex_locations.iter().copied().collect() {
                return Err(-1);
            }
            let mut uses = BTreeSet::new();
            for t in &p.textures {
                if t.reserved != 0
                    || t.comparison > 1
                    || !uses.insert((t.stage, t.texture_slot, t.sampler_slot))
                {
                    return Err(-1);
                }
                let program = match t.stage {
                    0 => vp,
                    1 => fp.ok_or(-1)?,
                    _ => return Err(-1),
                };
                let reflected = program
                    .textures
                    .iter()
                    .find(|r| r.slot == t.texture_slot)
                    .ok_or(-1)?;
                let reflected_sampler = program
                    .samplers
                    .iter()
                    .find(|r| r.slot == t.sampler_slot)
                    .ok_or(-1)?;
                if reflected.sample_kind != if t.format == 2 { 3 } else { 0 }
                    || reflected_sampler.comparison != t.comparison
                {
                    return Err(-2);
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
                    return Err(-1);
                }
            }
        }
        for v in &self.views {
            if v.reserved != 0 || v.image as usize >= self.images.len() {
                return Err(-1);
            }
            if v.base_mip != 0
                || v.mip_count != 1
                || v.base_layer != 0
                || v.layer_count != 1
                || v.remap != [2, 3, 4, 5]
            {
                return Err(-2);
            }
        }
        for s in &self.samplers {
            sampler(s)?;
        }
        let mut item_set = BTreeSet::new();
        for i in &self.items {
            if i.reserved != 0
                || i.pass as usize >= self.passes.len()
                || !item_set.insert((i.pass, i.source_item))
            {
                return Err(-1);
            }
            if i.unresolved_obligations != 0 && self.flags & 1 == 0 {
                return Err(-2);
            }
        }
        let mut draws = 0usize;
        let mut seen = BTreeSet::new();
        for (pass, p) in self.passes.iter().enumerate() {
            if p.source.cohort == 0 || p.source.publication == 0 || p.source.pass_occurrence == 0 {
                return Err(-1);
            }
            for a in [p.color, p.depth] {
                if a.reserved != 0
                    || a.load > 1
                    || a.store > 1
                    || a.image != u32::MAX && a.image as usize >= self.images.len()
                    || a.clear.iter().any(|x| !x.is_finite())
                {
                    return Err(-1);
                }
            }
            for d in &p.draws {
                draws = draws.checked_add(1).ok_or(-5)?;
                let pipeline = self.pipelines.get(d.pipeline as usize).ok_or(-1)?;
                if !item_set.contains(&(pass as u32, d.item))
                    || !seen.insert((pass as u32, d.item, d.partition))
                    || d.obligations != 0 && self.flags & 1 == 0
                {
                    return Err(-1);
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
                    return Err(-1);
                }
                let ib = &self.buffers[d.indices.buffer as usize]
                    [ir.offset as usize..(ir.offset + ir.length) as usize];
                if ib.as_chunks::<4>().0.iter().any(|b| {
                    u32::from_ne_bytes(*b) as u64 >= vr.length / u64::from(pipeline.stride)
                }) {
                    return Err(-1);
                }
                let mut bindings = BTreeSet::new();
                for b in &d.buffers {
                    if !bindings.insert((b.stage, b.slot)) {
                        return Err(-1);
                    }
                    let range = self.range(b.view)?;
                    let program = match b.stage {
                        0 => &self.programs[pipeline.vertex as usize],
                        1 => self.programs.get(pipeline.fragment as usize).ok_or(-1)?,
                        _ => return Err(-1),
                    };
                    let required = program
                        .buffers
                        .iter()
                        .find(|r| r.slot == b.slot)
                        .ok_or(-1)?;
                    if range.length < required.minimum_bytes
                        || !range
                            .offset
                            .is_multiple_of(u64::from(required.alignment_bytes))
                    {
                        return Err(-1);
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
                    return Err(-1);
                }
                let mut textures = BTreeSet::new();
                for t in &d.textures {
                    if t.reserved != 0
                        || t.view as usize >= self.views.len()
                        || t.sampler as usize >= self.samplers.len()
                        || !textures.insert((t.stage, t.texture_slot, t.sampler_slot))
                    {
                        return Err(-1);
                    }
                    let required = pipeline
                        .textures
                        .iter()
                        .find(|r| {
                            (r.stage, r.texture_slot, r.sampler_slot)
                                == (t.stage, t.texture_slot, t.sampler_slot)
                        })
                        .ok_or(-1)?;
                    let image = &self.images[self.views[t.view as usize].image as usize];
                    if image.desc.format != format(required.format)?
                        || (sampler(&self.samplers[t.sampler as usize])?
                            == Sampler::CompareLessEqual)
                            != (required.comparison != 0)
                    {
                        return Err(-2);
                    }
                }
            }
        }
        // Every omitted item is represented explicitly. A packet with only omissions
        // cannot erase a live target while reporting a geometry submission.
        for i in &self.items {
            if !seen
                .iter()
                .any(|&(p, n, _)| p == i.pass && n == i.source_item)
                && (i.unresolved_obligations == 0 || self.flags & 1 == 0)
            {
                return Err(-1);
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
    fn prepare(
        &self,
        device: &impl SceneCatalog,
        ids: &[u64],
        max_draws: usize,
        max_bytes: u64,
    ) -> Result<PreparedScene, i32> {
        let mut store = ResourceStore::new(ResourceLimits {
            max_resources: self.buffers.len() + self.images.len(),
            max_bytes: max_bytes as usize,
        })
        .map_err(|_| -5)?;
        let mut lease = |b: &[u8]| -> Result<ResourceLease, i32> {
            let v = store
                .apply(ResourceDelta::Create { bytes: b.to_vec() })
                .map_err(|_| -5)?;
            let l = store.lease(v).map_err(|_| -5)?;
            store.retire(v).map_err(|_| -5)?;
            Ok(l)
        };
        let mut batch = BatchBuilder::new(
            device.owner_generation(),
            BatchLimits {
                max_commands: 1,
                max_gpu_buffers: 1,
                max_gpu_bytes: max_bytes,
                max_uploads: self.buffers.len().max(1),
            },
        )
        .map_err(|_| -1)?;
        let mut uploads = Vec::new();
        for b in &self.buffers {
            uploads.push(batch.upload(lease(b)?).map_err(|_| -5)?)
        }
        let prefix = batch.prepare_scene_prefix(device).map_err(|_| -1)?;
        let mut scene = SceneBuilder::new(
            prefix,
            SceneLimits {
                max_images: self.images.len(),
                max_passes: self.passes.len(),
                max_draws,
                max_bytes,
            },
        )
        .map_err(|_| -1)?;
        let mut images = Vec::new();
        for image in &self.images {
            let i = if let Some(b) = &image.bytes {
                scene
                    .texture(image.desc, lease(b)?)
                    .map_err(|_| -1)?
                    .image()
            } else {
                scene.image(image.desc).map_err(|_| -1)?
            };
            images.push(i)
        }
        for pass in &self.passes {
            let color = if pass.color.image == u32::MAX {
                None
            } else {
                let a = pass.color;
                let i = images[a.image as usize];
                Some(ColorAttachment {
                    image: i,
                    load: if a.load == 0 {
                        Load::Clear(a.clear)
                    } else {
                        Load::Load(scene.current(i).map_err(|_| -1)?)
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
                    load: if a.load == 0 {
                        Load::Clear(a.clear[0])
                    } else {
                        Load::Load(scene.current(i).map_err(|_| -1)?)
                    },
                    store: if a.store == 0 {
                        Store::Store
                    } else {
                        Store::Discard
                    },
                })
            };
            let mut draws = Vec::new();
            for d in &pass.draws {
                let mut buffers = Vec::new();
                for b in &d.buffers {
                    buffers.push(GraphicsBuffer {
                        stage: stage(b.stage)?,
                        slot: b.slot,
                        source: ReadSource::Upload(uploads[b.view.buffer as usize]),
                        range: self.range(b.view)?,
                    })
                }
                let mut textures = Vec::new();
                for t in &d.textures {
                    let view = &self.views[t.view as usize];
                    textures.push(SampledImage {
                        stage: stage(t.stage)?,
                        texture_slot: t.texture_slot,
                        sampler_slot: t.sampler_slot,
                        image: scene.current(images[view.image as usize]).map_err(|_| -1)?,
                        sampler: sampler(&self.samplers[t.sampler as usize])?,
                    })
                }
                draws.push(SceneDraw {
                    pipeline: ids[d.pipeline as usize],
                    vertices: ReadSource::Upload(uploads[d.vertices.buffer as usize]),
                    indices: ReadSource::Upload(uploads[d.indices.buffer as usize]),
                    vertex_range: self.range(d.vertices)?,
                    index_range: self.range(d.indices)?,
                    index_count: d.count,
                    buffers,
                    textures,
                });
            }
            scene
                .render_pass(RenderPass {
                    color,
                    depth,
                    draws,
                })
                .map_err(|_| -1)?;
        }
        scene
            .output(
                scene
                    .current(images[self.output as usize])
                    .map_err(|_| -1)?,
                false,
            )
            .map_err(|_| -1)?;
        scene.prepare(device).map_err(|_| -1)
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
}
impl PipelineCache {
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
        let (depth, write, blend) = raster(&p.state)?;
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
                depth,
                write,
                blend,
                p.attributes.len() as u32,
                p.textures.len() as u32,
            ],
        );
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
        let required: BTreeSet<_> = keys.iter().cloned().collect();
        if required.len() > MAX_PIPELINES {
            return Err(-5);
        }
        let mut result = Vec::new();
        for (p, key) in packet.pipelines.iter().zip(keys) {
            if let Some((id, last)) = self.pipelines.get_mut(&key) {
                *last = epoch;
                result.push(*id);
                continue;
            }
            if self.pipelines.len() >= MAX_PIPELINES {
                let old = self
                    .pipelines
                    .iter()
                    .filter(|(k, _)| !required.contains(*k))
                    .min_by_key(|(_, (_, age))| *age)
                    .map(|(k, (id, _))| (k.clone(), *id))
                    .ok_or(-5)?;
                device.release(old.1)?;
                self.pipelines.remove(&old.0);
            }
            let id = device.compile(p, &packet.programs)?;
            self.pipelines.insert(key, (id, epoch));
            result.push(id);
        }
        Ok(result)
    }
}
trait PipelineCompiler {
    fn compile(&mut self, p: &Pipeline, programs: &[Program]) -> Result<u64, i32>;
    fn release(&mut self, id: u64) -> Result<(), i32>;
}
impl PipelineCompiler for SceneDevice<'_> {
    fn release(&mut self, id: u64) -> Result<(), i32> {
        self.release_graphics_pipeline(id)
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
        let (depth_compare, depth_write, blend) = raster(&p.state)?;
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
                depth_compare,
                depth_write,
                blend,
                buffers: buffers.as_ptr(),
                buffer_count: buffers.len() as u32,
                texture_count: p.textures.len() as u32,
                textures: p.textures.as_ptr(),
            },
            attributes: p.attributes.as_ptr(),
            attribute_count: p.attributes.len() as u32,
            binding_model: p.binding_model,
        };
        unsafe { self.graphics_pipeline_v2(&desc) }
    }
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
            if s.pending.len() + s.ready.len() >= 2 {
                return Err(-4);
            }
            packet.validate(s.max_draws, s.max_bytes)?;
            if !frame.valid() || frame.owner_lifetime != target.owner_lifetime {
                return Err(-3);
            }
            let output = packet.images.get(packet.output as usize).ok_or(-1)?;
            if output.desc.width != target.width
                || output.desc.height != target.height
                || output.desc.format != format(target.format)?
            {
                return Err(-2);
            }
            let language = match s.native_backend {
                1 => 1,
                2 => 3,
                3 => 2,
                _ => return Err(-2),
            };
            if packet.programs.iter().any(|p| p.language != language) {
                return Err(-2);
            }
            let ids = s
                .packet_cache
                .resolve(&mut *s.backend.device().0.borrow_mut(), &packet)?;
            let scene = packet.prepare(
                &*s.backend.device().0.borrow(),
                &ids,
                s.max_draws,
                s.max_bytes,
            )?;
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
            let image = unsafe { s.backend.device().0.borrow_mut().import_target(target) }?;
            if let Err(e) = unsafe { s.backend.device().0.borrow_mut().frame_context(frame) } {
                s.backend.device().0.borrow_mut().release_target(image)?;
                return Err(e);
            }
            let (commands, leases) = scene.into_submission();
            let result = s
                .backend
                .submit_diagnostic(packet.source.publication, commands, leases);
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
            stage,
            language: 1,
            source: format!("authored stage {stage}").into_bytes(),
            spirv: vec![],
            entry: b"main".to_vec(),
            buffers: vec![BindingLayout {
                slot: stage,
                access: 1,
                minimum_bytes: 16,
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
                    desc: ImageDesc {
                        width: 2,
                        height: 2,
                        format: ImageFormat::Rgba8,
                    },
                    bytes: None,
                },
                ImagePayload {
                    desc: ImageDesc {
                        width: 1,
                        height: 1,
                        format: ImageFormat::Rgba8,
                    },
                    bytes: Some(vec![127, 20, 30, 255]),
                },
            ],
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
        }
    }
    struct Catalog<'a>(&'a ScenePacket);
    impl PipelineCatalog for Catalog<'_> {
        fn owner_generation(&self) -> u64 {
            42
        }
        fn bindings(&self, _: u64) -> Option<&[BindingRequirement]> {
            None
        }
    }
    impl SceneCatalog for Catalog<'_> {
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
                depth_attachment: false,
                depth: DepthState {
                    compare: DepthCompare::Disabled,
                    write: false,
                },
                blend: Blend::Opaque,
                buffers,
                textures: vec![mag_render::scene::TextureLayout {
                    stage: Stage::Fragment,
                    texture_slot: 0,
                    sampler_slot: 0,
                    format: ImageFormat::Rgba8,
                    comparison: false,
                }],
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
    fn copy_view(packet: &ScenePacket) -> ScenePacket {
        let programs: Vec<_> = packet
            .programs
            .iter()
            .map(|p| raw::Program {
                stage: p.stage,
                reserved: 0,
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
            .map(|b| raw::Buffer {
                role: 0,
                reserved: 0,
                bytes: b.as_ptr().cast(),
                length: b.len() as u64,
            })
            .collect();
        let subs: Vec<_> = packet
            .images
            .iter()
            .map(|i| {
                i.bytes.as_ref().map(|b| raw::Subresource {
                    mip: 0,
                    layer: 0,
                    width: i.desc.width,
                    height: i.desc.height,
                    depth: 1,
                    row_bytes: i.desc.width * 4,
                    bytes: b.as_ptr().cast(),
                    length: b.len() as u64,
                })
            })
            .collect();
        let images: Vec<_> = packet
            .images
            .iter()
            .zip(&subs)
            .map(|(i, s)| raw::Image {
                format: 1,
                dimension: 2,
                width: i.desc.width,
                height: i.desc.height,
                depth: 1,
                layers: 1,
                mip_count: 1,
                samples: 1,
                subresources: s.as_ref().map_or(std::ptr::null(), |s| s),
                subresource_count: u32::from(s.is_some()),
                reserved: 0,
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
            dispatches: std::ptr::null(),
            dispatch_count: 0,
            pass_count: passes.len() as u32,
            passes: passes.as_ptr(),
            items: packet.items.as_ptr(),
            item_count: packet.items.len() as u32,
            output_image: packet.output,
            target: std::ptr::null(),
            frame: std::ptr::null(),
        };
        unsafe { ScenePacket::copy(&desc, 32, 1 << 20) }.unwrap()
    }
    #[test]
    fn synchronous_transport_owns_shader_uniform_geometry_and_texels() {
        let mut packet = fixture();
        let copy = copy_view(&packet);
        packet.programs[0].source.fill(0);
        packet.buffers[0].fill(1);
        packet.buffers[2].fill(9);
        packet.images[1].bytes.as_mut().unwrap().fill(0);
        assert_ne!(copy.programs[0].source, packet.programs[0].source);
        assert_eq!(copy.buffers[0], vec![0; 72]);
        assert_eq!(copy.buffers[2], vec![1; 16]);
        assert_eq!(copy.images[1].bytes, Some(vec![127, 20, 30, 255]));
        copy.prepare(&Catalog(&copy), &[1], 32, 1 << 20).unwrap();
    }
}
