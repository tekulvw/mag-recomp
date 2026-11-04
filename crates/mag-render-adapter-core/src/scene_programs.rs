//! Transport of the actual owned compiler outputs to the existing render worker.
//! No native backend is linked here. All shader/reflection storage stays owned
//! across the synchronous packet-copy call; native PSOs belong to that worker.
use mag_render_abi::{BindingLayout, ShaderStage, packet};
use mag_render_programs::native_compile::{
    NativeProgram, Stage, StoredDepthAux, TextureDimension, TextureSampleKind,
};
use std::{ffi::c_void, sync::Arc};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Backend {
    Metal,
    Vulkan,
    D3d12,
}
impl Backend {
    /// From the host owner's MagRenderHostBackend value.
    pub fn from_host(backend: u32) -> Option<Self> {
        match backend {
            1 => Some(Self::Metal),
            2 => Some(Self::Vulkan),
            3 => Some(Self::D3d12),
            _ => None,
        }
    }
}
#[derive(Debug, PartialEq, Eq)]
pub enum ProgramPacketError {
    Extent,
    UniformLayout,
    Reflection,
}
pub struct ProgramPacket {
    program: Arc<NativeProgram>,
    buffers: Vec<BindingLayout>,
    textures: Vec<packet::TextureSlot>,
    samplers: Vec<packet::SamplerSlot>,
}
pub fn checked_aux(program: &NativeProgram) -> Result<Option<StoredDepthAux>, ProgramPacketError> {
    let Some(aux) = program.stored_depth_aux else {
        return Ok(None);
    };
    if program.stage != Stage::Fragment
        || (aux.texture_slot, aux.bounds_buffer_slot, aux.bounds_bytes) != (15, 2, 16)
        || !program.hlsl.is_empty()
        || !program.spirv.is_empty()
        || program.buffers.iter().filter(|b| b.slot == 2).count() != 1
        || !program.buffers.iter().any(|b| b.slot == 2 && b.bytes == 16)
        || program
            .texture_bindings
            .iter()
            .filter(|t| t.slot == 15)
            .count()
            != 1
        || !program.texture_bindings.iter().any(|t| {
            t.slot == 15
                && t.dimension == TextureDimension::D2
                && t.sample_kind == TextureSampleKind::Depth
                && !t.arrayed
                && !t.multisampled
        })
        || program.sampler_bindings.iter().any(|s| s.slot == 15)
    {
        return Err(ProgramPacketError::Reflection);
    }
    Ok(Some(aux))
}
impl ProgramPacket {
    pub fn new(program: Arc<NativeProgram>) -> Result<Self, ProgramPacketError> {
        if program.buffers.len() > 16
            || program.texture_bindings.len() > 16
            || program.sampler_bindings.len() > 16
            || program.vertex_locations.len() > 16
        {
            return Err(ProgramPacketError::Extent);
        }
        let aux = checked_aux(&program)?;
        let mut buffers = Vec::new();
        for b in &program.buffers {
            if aux.is_some_and(|a| b.slot == a.bounds_buffer_slot) {
                continue;
            }
            if b.bytes == 0 || b.bytes % 4 != 0 || b.slot >= 16 {
                return Err(ProgramPacketError::UniformLayout);
            }
            if buffers
                .iter()
                .any(|prior: &BindingLayout| prior.slot == b.slot)
            {
                return Err(ProgramPacketError::Reflection);
            }
            buffers.push(BindingLayout {
                slot: b.slot,
                access: 1, // READ
                minimum_bytes: u64::from(b.bytes),
                runtime_array_stride: 0,
                runtime_size_index: u32::MAX, // NO_SIZE_INDEX, not Default's zero
                alignment_bytes: 16,
                binding_kind: 1, // UNIFORM
            });
        }
        let mut textures = Vec::new();
        for t in &program.texture_bindings {
            if aux.is_some_and(|a| t.slot == a.texture_slot) {
                continue;
            }
            if t.slot >= 16
                || textures
                    .iter()
                    .any(|p: &packet::TextureSlot| p.slot == t.slot)
            {
                return Err(ProgramPacketError::Reflection);
            }
            textures.push(packet::TextureSlot {
                slot: t.slot,
                dimension: match t.dimension {
                    TextureDimension::D1 => 1,
                    TextureDimension::D2 => 2,
                    TextureDimension::D3 => 3,
                    TextureDimension::Cube => 4,
                },
                sample_kind: match t.sample_kind {
                    TextureSampleKind::Float => 0,
                    TextureSampleKind::Sint => 1,
                    TextureSampleKind::Uint => 2,
                    TextureSampleKind::Depth => 3,
                },
                arrayed: u32::from(t.arrayed),
                multisampled: u32::from(t.multisampled),
                reserved: 0,
            });
        }
        let mut samplers = Vec::new();
        for s in &program.sampler_bindings {
            if s.slot >= 16
                || samplers
                    .iter()
                    .any(|p: &packet::SamplerSlot| p.slot == s.slot)
            {
                return Err(ProgramPacketError::Reflection);
            }
            samplers.push(packet::SamplerSlot {
                slot: s.slot,
                comparison: u32::from(s.comparison),
            });
        }
        if program.vertex_locations.iter().any(|&v| v >= 16)
            || program
                .vertex_locations
                .iter()
                .enumerate()
                .any(|(i, v)| program.vertex_locations[..i].contains(v))
            || (program.stage == Stage::Fragment && !program.vertex_locations.is_empty())
        {
            return Err(ProgramPacketError::Reflection);
        }
        Ok(Self {
            program,
            buffers,
            textures,
            samplers,
        })
    }
    /// The returned pointers borrow this object. They are valid only while it
    /// lives; the native ingress must copy them synchronously before returning.
    pub fn view(&self, backend: Backend) -> Result<packet::Program, ProgramPacketError> {
        let p = &self.program;
        let (source, bytes, entry, language): (*const c_void, usize, &str, u32) = match backend {
            Backend::Metal => (p.msl.as_ptr().cast(), p.msl.len(), &p.msl_entry, 1),
            Backend::D3d12 => (p.hlsl.as_ptr().cast(), p.hlsl.len(), &p.hlsl_entry, 2),
            Backend::Vulkan => (
                p.spirv.as_ptr().cast(),
                p.spirv
                    .len()
                    .checked_mul(4)
                    .ok_or(ProgramPacketError::Extent)?,
                &p.spirv_entry,
                3,
            ),
        };
        if bytes == 0 || entry.is_empty() || entry.as_bytes().contains(&0) {
            return Err(ProgramPacketError::Extent);
        }
        Ok(packet::Program {
            stage: if p.stage == Stage::Vertex { 0 } else { 1 },
            reserved: if checked_aux(p)?.is_some() {
                packet::PROGRAM_STORED_DEPTH_AUX
            } else {
                0
            },
            shader: ShaderStage {
                source,
                source_length: bytes as u64,
                entry: entry.as_ptr().cast(),
                entry_length: u32::try_from(entry.len()).map_err(|_| ProgramPacketError::Extent)?,
                language,
            },
            buffers: self.buffers.as_ptr(),
            buffer_count: self.buffers.len() as u32,
            texture_count: self.textures.len() as u32,
            textures: self.textures.as_ptr(),
            samplers: self.samplers.as_ptr(),
            sampler_count: self.samplers.len() as u32,
            vertex_location_count: p.vertex_locations.len() as u32,
            vertex_locations: p.vertex_locations.as_ptr(),
            threads: [0; 3],
            reserved1: 0,
        })
    }
    pub fn uniform_layouts(&self) -> &[BindingLayout] {
        &self.buffers
    }
    pub fn texture_layouts(&self) -> &[packet::TextureSlot] {
        &self.textures
    }
    pub fn sampler_layouts(&self) -> &[packet::SamplerSlot] {
        &self.samplers
    }
}
