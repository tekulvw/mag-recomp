//! GPU group-zero deformation: the packet's compute prefix evaluates the per-vertex wind
//! deformation of `group-zero-deformation.md`, and the draw reads the produced vertices.
//!
//! The CPU still runs every check of `behaviour::group_zero::validate_group_zero` and computes
//! the per-instance placements, which is proportional to the instance count only. The kernel
//! (`shaders/render_frontend/group_zero.wgsl`, translated at build time with the pinned Naga)
//! evaluates every (instance, vertex) in native binary32 and writes two rows per vertex: the
//! deformed position `(x, y, z, 1)` and the slot-12 colour row. The packet validator does not
//! accept produced index buffers, so the expanded 32-bit indices stay a CPU product.
//!
//! The path is off by default ([`OPTION`]). [`prepare`] refuses visibly whenever it cannot promise
//! the CPU result within the specification's tolerance: a refusal the CPU path would also make,
//! a non-finite or possibly overflowing input, an unsupported vertex layout or a grid beyond the
//! dispatch limits. The caller then uses the CPU path.
//!
//! Depth-equal requirement: a CPU and a GPU evaluation differ within the tolerance, so they must
//! not be mixed for one item across passes that compare depth for equality
//! (`group-zero-deformation.md`, "Consequences of non-bit-exact output"). The choice is made once
//! per item in [`crate::deformation::DeformationStore`], and every pass of the packet binds the
//! same produced buffer.

use crate::abi::{
    BindingLayoutRecord, LANGUAGE_HLSL, LANGUAGE_MSL, LANGUAGE_SPIRV, OwnedBuffer, OwnedDispatch,
    OwnedProgram, SceneBuilder,
};
use mag_render_behavior::group_zero::{
    GroupZeroError, GroupZeroInputs, ValidatedGroupZero, colour_row, validate_group_zero,
};
use mag_render_behavior::vertex_inputs::{AuxiliarySet, GpuLayoutPlan, VertexBinding};
use mag_render_behavior::waveform::{KA1, KB1, KC};

/// The kernel translations and binding layout, embedded by the build script.
pub mod kernel {
    include!(concat!(env!("OUT_DIR"), "/group_zero_kernel.rs"));
}

/// The opt-in switch: the GPU path is used only when this environment variable is `1`.
pub const OPTION: &str = "MAG_RENDER_GPU_GROUP_ZERO";

/// Whether [`OPTION`] enables the GPU path in this process.
pub fn enabled_by_environment() -> bool {
    std::env::var_os(OPTION).is_some_and(|value| value == "1")
}

/// Rows the kernel writes per expanded vertex, and the resulting vertex stride.
pub const ROWS_PER_VERTEX: u32 = 2;
pub const STRIDE: u32 = 16 * ROWS_PER_VERTEX;
/// Byte sizes of one record of each kernel input.
pub const PARAMETER_BYTES: usize = 64 + 1024;
pub const INSTANCE_BYTES: usize = 48;
pub const SOURCE_VERTEX_BYTES: usize = 32;
/// The largest workgroup count per grid dimension the path uses.
pub const MAX_GROUPS: u32 = 65535;
/// Inputs and results must stay below this magnitude, far from the binary32 range.
const MAGNITUDE_LIMIT: f64 = 1.0e30;

/// Why the GPU path declined; the caller uses the CPU path instead.
#[derive(Clone, Debug, PartialEq)]
pub enum GpuRefusal {
    /// The behaviour crate refuses the item; the CPU path reports the same error.
    Deformation(GroupZeroError),
    /// An input or derived term that the CPU path would carry into a non-finite result.
    NonFinite(&'static str),
    /// A result could exceed the binary32 range, which only the CPU path checks exactly.
    Magnitude,
    /// No vertices, or a grid beyond the dispatch limits.
    Grid,
    /// The work exceeds the caller's byte budget.
    Budget,
    /// The draw's vertex layout is not one the kernel's output rows serve.
    Layout(&'static str),
    /// No kernel translation for this shader language.
    Language(u32),
}

/// One item's GPU deformation work: the kernel's input buffers, the grid, and the CPU-expanded
/// indices.
#[derive(Clone, Debug, PartialEq)]
pub struct GpuGroupZeroWork {
    pub parameters: Vec<u8>,
    pub instances: Vec<u8>,
    pub vertices: Vec<u8>,
    pub pool: Vec<u8>,
    pub groups: [u32; 3],
    /// Expanded vertices: instances × source vertices.
    pub vertex_count: usize,
    /// Expanded 32-bit indices, instance-major.
    pub indices: Vec<u32>,
}

/// The packet entries [`GpuGroupZeroWork::append`] added for one item.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ProducedVertices {
    /// `(buffer, offset, length)` of the produced vertex rows.
    pub vertices: (u32, u64, u64),
    /// `(buffer, offset, length)` of the native-endian 32-bit indices.
    pub indices: (u32, u64, u64),
    pub index_count: u32,
    pub stride: u32,
    pub dispatch: u32,
}

/// The compute program record for `language` (`LANGUAGE_MSL`, `LANGUAGE_HLSL` or
/// `LANGUAGE_SPIRV`).
pub fn kernel_program(language: u32) -> Result<OwnedProgram, GpuRefusal> {
    let (source, entry) = match language {
        LANGUAGE_MSL => (kernel::MSL.as_bytes().to_vec(), kernel::MSL_ENTRY),
        LANGUAGE_HLSL => (kernel::HLSL.as_bytes().to_vec(), kernel::HLSL_ENTRY),
        LANGUAGE_SPIRV => (
            kernel::SPIRV.iter().flat_map(|w| w.to_le_bytes()).collect(),
            kernel::SPIRV_ENTRY,
        ),
        other => return Err(GpuRefusal::Language(other)),
    };
    Ok(OwnedProgram {
        stage: 2,
        flags: 0,
        language,
        source,
        entry: entry.as_bytes().to_vec(),
        buffers: kernel::BINDINGS
            .iter()
            .map(
                |&(slot, access, minimum_bytes, stride, size_index, alignment, kind)| {
                    BindingLayoutRecord {
                        slot,
                        access,
                        minimum_bytes,
                        runtime_array_stride: stride,
                        runtime_size_index: size_index,
                        alignment_bytes: alignment,
                        binding_kind: kind,
                    }
                },
            )
            .collect(),
        textures: Vec::new(),
        samplers: Vec::new(),
        vertex_locations: Vec::new(),
        threads: kernel::THREADS,
    })
}

fn word(bits: u32) -> f64 {
    f32::from_bits(bits) as f64
}

fn narrow(value: f64, what: &'static str) -> Result<f32, GpuRefusal> {
    if value.is_finite() && value.abs() <= MAGNITUDE_LIMIT {
        Ok(value as f32)
    } else {
        Err(GpuRefusal::NonFinite(what))
    }
}

fn push(bytes: &mut Vec<u8>, values: &[f32]) {
    for v in values {
        bytes.extend_from_slice(&v.to_le_bytes());
    }
}

fn push_words(bytes: &mut Vec<u8>, words: &[u32]) {
    for w in words {
        bytes.extend_from_slice(&w.to_le_bytes());
    }
}

/// Prepares the GPU work for one item, or declines.
pub fn prepare(
    inputs: &GroupZeroInputs<'_>,
    max_vertices: u64,
) -> Result<GpuGroupZeroWork, GpuRefusal> {
    let validated = validate_group_zero(inputs, max_vertices).map_err(GpuRefusal::Deformation)?;
    let vertices = inputs.positions.len();
    let instances = validated.placements.len();
    let total = vertices * instances;
    if total == 0 || total as u64 * ROWS_PER_VERTEX as u64 > u32::MAX as u64 {
        return Err(GpuRefusal::Grid);
    }
    let workgroup = kernel::THREADS[0];
    let workgroups = (total as u64).div_ceil(workgroup as u64);
    let columns = workgroups.min(MAX_GROUPS as u64);
    let rows = workgroups.div_ceil(columns);
    if rows > MAX_GROUPS as u64 {
        return Err(GpuRefusal::Grid);
    }

    // Per-draw terms.
    let frequency = narrow(validated.frequency, "frequency")?;
    let amplitudes = [
        narrow(validated.amplitudes[0], "amplitude")?,
        narrow(validated.amplitudes[1], "amplitude")?,
    ];
    let phase = narrow(validated.phase, "phase")?;

    // Source vertices, and the bounds that keep every result in range.
    let mut source = Vec::with_capacity(vertices * SOURCE_VERTEX_BYTES);
    let (mut extent, mut sway) = (0f64, 0f64);
    for v in 0..vertices {
        let (p, n, w) = (inputs.positions[v], inputs.normals[v], inputs.weights[v]);
        for &bits in p[..3].iter().chain(&n[..3]).chain(&w[3..]) {
            if !f32::from_bits(bits).is_finite() {
                return Err(GpuRefusal::NonFinite("source vertex"));
            }
        }
        extent = extent.max((0..3).map(|l| word(p[l]).abs()).sum());
        sway = sway.max((1.0 - word(w[3])).abs() * word(n[0]).abs().max(word(n[1]).abs()));
        push_words(&mut source, &[p[0], p[1], p[2], w[3], n[0], n[1], n[2], 0]);
    }
    let mut placed = 0f64;
    let mut instance_bytes = Vec::with_capacity(instances * INSTANCE_BYTES);
    for placement in &validated.placements {
        let mut rows = [0f32; 12];
        for r in 0..3 {
            for c in 0..3 {
                rows[4 * r + c] = narrow(
                    placement.scale * placement.rotation[r][c],
                    "instance placement",
                )?;
            }
            rows[4 * r + 3] = narrow(placement.offset[r], "instance offset")?;
            placed = placed.max(placement.scale.abs() * extent + placement.offset[r].abs());
        }
        push(&mut instance_bytes, &rows);
    }
    let displacement = (validated.amplitudes[0].abs() + validated.amplitudes[1].abs()) * sway;
    let phase_extent = validated.phase.abs() + 3.0 * placed;
    if placed + displacement > MAGNITUDE_LIMIT || phase_extent > MAGNITUDE_LIMIT {
        return Err(GpuRefusal::Magnitude);
    }

    let mut parameters = Vec::with_capacity(PARAMETER_BYTES);
    push_words(
        &mut parameters,
        &[
            vertices as u32,
            instances as u32,
            inputs.colour_frequency,
            inputs.batch_max,
            (columns * workgroup as u64) as u32,
            0,
            0,
            0,
        ],
    );
    push(
        &mut parameters,
        &[
            frequency,
            amplitudes[0],
            amplitudes[1],
            phase,
            KA1,
            KB1,
            KC,
            0.0,
        ],
    );
    for byte in 0..=255u8 {
        parameters.extend_from_slice(&colour_row([byte; 4])[0].to_le_bytes());
    }
    let mut pool = Vec::with_capacity(4 * validated.pool_length as usize);
    for entry in &inputs.colour_pool[..validated.pool_length as usize] {
        pool.extend_from_slice(entry);
    }

    let mut indices = Vec::with_capacity(instances * inputs.indices.len());
    for instance in 0..instances {
        let base = (instance * vertices) as u32;
        indices.extend(inputs.indices.iter().map(|k| base + *k as u32));
    }
    Ok(GpuGroupZeroWork {
        parameters,
        instances: instance_bytes,
        vertices: source,
        pool,
        groups: [columns as u32, rows as u32, 1],
        vertex_count: total,
        indices,
    })
}

impl GpuGroupZeroWork {
    /// Bytes of the produced vertex rows.
    pub fn output_bytes(&self) -> u64 {
        self.vertex_count as u64 * STRIDE as u64
    }

    /// Adds the kernel inputs, the produced vertex buffer, the CPU index buffer and the dispatch
    /// of `program` (from [`kernel_program`]) to `builder`.
    pub fn append(&self, builder: &mut SceneBuilder, program: u32) -> ProducedVertices {
        let input = |builder: &mut SceneBuilder, bytes: &[u8]| {
            (
                builder.add_buffer(OwnedBuffer::Bytes(bytes.to_vec())),
                bytes.len() as u64,
            )
        };
        let (parameters, parameters_length) = input(builder, &self.parameters);
        let (instances, instances_length) = input(builder, &self.instances);
        let (vertices, vertices_length) = input(builder, &self.vertices);
        let (pool, pool_length) = input(builder, &self.pool);
        let output = builder.add_buffer(OwnedBuffer::Produced(self.output_bytes()));
        let index_bytes: Vec<u8> = self.indices.iter().flat_map(|i| i.to_ne_bytes()).collect();
        let (indices, indices_length) = input(builder, &index_bytes);
        let dispatch = builder.add_dispatch(OwnedDispatch {
            program,
            bindings: vec![
                (0, parameters, 0, parameters_length),
                (1, instances, 0, instances_length),
                (2, vertices, 0, vertices_length),
                (3, pool, 0, pool_length),
                (4, output, 0, self.output_bytes()),
            ],
            groups: self.groups,
        });
        ProducedVertices {
            vertices: (output, 0, self.output_bytes()),
            indices: (indices, 0, indices_length),
            index_count: self.indices.len() as u32,
            stride: STRIDE,
            dispatch,
        }
    }

    /// The vertex-input layout plan for a draw that reads `required` slots, with the item's
    /// output bindings and group-zero auxiliary attributes. The kernel's rows serve the position
    /// in slot 0 and the prepared colours in slot 12, in that order, so `required` must be slot 0
    /// alone or slots 0 and 12; anything else is declined, before the item's deformation is
    /// committed to the GPU for any pass.
    pub fn layout_plan(
        &self,
        outputs: &[VertexBinding],
        auxiliary: &AuxiliarySet<'_>,
        required: u16,
    ) -> Result<GpuLayoutPlan, GpuRefusal> {
        layout_plan(outputs, auxiliary, required, self.vertex_count)
    }
}

/// See [`GpuGroupZeroWork::layout_plan`].
pub fn layout_plan(
    outputs: &[VertexBinding],
    auxiliary: &AuxiliarySet<'_>,
    required: u16,
    vertex_count: usize,
) -> Result<GpuLayoutPlan, GpuRefusal> {
    let mut array_mask = 1u16 << 12;
    let mut auxiliary_slots = 0u32;
    for binding in auxiliary
        .decoded
        .iter()
        .map(|a| &a.binding)
        .chain(&auxiliary.failed)
    {
        if binding.slot >= 16 {
            return Err(GpuRefusal::Layout("auxiliary slot out of range"));
        }
        auxiliary_slots |= 1 << binding.slot;
        if binding.components != 0 {
            array_mask |= 1 << binding.slot;
        }
    }
    let mut output_slots = 0u32;
    for binding in outputs {
        if binding.slot >= 16 {
            return Err(GpuRefusal::Layout("output slot out of range"));
        }
        output_slots |= 1 << binding.slot;
        if binding.components != 0 {
            array_mask |= 1 << binding.slot;
        }
    }
    let fields = match required {
        0x0001 => vec![0],
        0x1001 => vec![0, 12],
        _ => return Err(GpuRefusal::Layout("required slots other than 0 and 12")),
    };
    // Slot 0 is the output position binding (output bindings overwrite their slots), and its
    // lane count must keep all three coordinates, as the kernel writes them with w = 1.
    let position = outputs
        .iter()
        .rfind(|b| b.slot == 0)
        .ok_or(GpuRefusal::Layout("no slot-0 output binding"))?;
    if position.semantic != 1 {
        return Err(GpuRefusal::Layout("slot 0 is not the position"));
    }
    if !(1..=4).contains(&position.components) || !(1..=7).contains(&position.format) {
        return Err(GpuRefusal::Layout("position binding"));
    }
    if position.format != 6 && position.components < 3 {
        return Err(GpuRefusal::Layout("position with fewer than three lanes"));
    }
    if fields.contains(&12) && (output_slots | auxiliary_slots) & (1 << 12) != 0 {
        return Err(GpuRefusal::Layout("slot 12 is not the prepared colour"));
    }
    Ok(GpuLayoutPlan {
        array_mask,
        stride: STRIDE,
        fields,
        vertex_count,
    })
}

/// The kernel's arithmetic evaluated on the CPU in binary32, operation for operation, from the
/// same input bytes. Used to check the kernel's translation and the encoding of its inputs; a GPU
/// may still contract or reorder operations within the tolerance.
pub fn reference(work: &GpuGroupZeroWork) -> Vec<[f32; 4]> {
    let u = |bytes: &[u8], at: usize| u32::from_le_bytes(bytes[at..at + 4].try_into().unwrap());
    let f = |bytes: &[u8], at: usize| f32::from_bits(u(bytes, at));
    let p = &work.parameters;
    let (vertices, frequency_colour, batch) = (u(p, 0) as usize, u(p, 8), u(p, 12));
    let (frequency, a1, a2, theta) = (f(p, 32), f(p, 36), f(p, 40), f(p, 44));
    let (ka1, kb1, kc) = (f(p, 48), f(p, 52), f(p, 56));
    let fraction = |x: f32| {
        let v = x - x.floor();
        if v >= 1.0 { 0.0 } else { v }
    };
    let triangle = |x: f32| (2.0 * fraction(x) - 1.0).abs();
    let smoothed = |t: f32| t * t * (3.0 - 2.0 * t);
    let wave =
        |phase: f32, freq: f32| smoothed(triangle(freq * (2.0 * fraction(phase) - 1.0) + 0.5));
    let mut out = Vec::with_capacity(work.vertex_count * 2);
    for index in 0..work.vertex_count {
        let instance = index / vertices;
        let vertex = index - instance * vertices;
        let row = |r: usize| -> [f32; 4] {
            std::array::from_fn(|l| f(&work.instances, instance * INSTANCE_BYTES + 16 * r + 4 * l))
        };
        let s = |l: usize| f(&work.vertices, vertex * SOURCE_VERTEX_BYTES + 4 * l);
        let position = [s(0), s(1), s(2)];
        let placed: [f32; 3] = std::array::from_fn(|r| {
            let m = row(r);
            m[0] * position[0] + m[1] * position[1] + m[2] * position[2] + m[3]
        });
        let alpha = ka1 * (theta + placed[0] + placed[1]);
        let beta = kb1 * (theta + kc * (placed[0] + placed[1] + placed[2]));
        let displacement =
            (a1 * wave(beta, frequency) + a2 * wave(alpha, frequency)) * (1.0 - s(3));
        out.push([
            placed[0] + displacement * s(4),
            placed[1] + displacement * s(5),
            placed[2] + displacement * s(4),
            1.0,
        ]);
        let first = (instance as u32 / batch) * batch;
        let entry = first
            + ((instance as u32 - first) * vertices as u32 + vertex as u32) / frequency_colour;
        let packed = u(&work.pool, 4 * entry as usize);
        let colour = |shift: u32| {
            let byte = ((packed >> shift) & 255) as usize;
            f(p, 64 + 4 * byte)
        };
        out.push([colour(0), colour(8), colour(16), colour(24)]);
    }
    out
}

/// The validated terms of `inputs`, for tolerance checks.
pub fn validated(inputs: &GroupZeroInputs<'_>) -> Result<ValidatedGroupZero, GroupZeroError> {
    validate_group_zero(inputs, u32::MAX as u64)
}
