//! Main producer input acquisition (`material-producers.md`).
//!
//! Acquisition reads one immutable input set for a Main invocation. Every value keeps the guest
//! address it came from so later steps can prove provenance. Floating-point words are kept as
//! their bit patterns; validating their numeric domain belongs to the producer that uses them.

use crate::guest::{GuestMemory, SourceError, be_u16, be_u32, be_words};
use crate::surfaces::SurfaceRecord;

/// A value and the guest address it was read from.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Tagged<T> {
    pub address: u32,
    pub value: T,
}

/// Displacements of the pointer and scalar globals below `TOC`.
pub mod globals {
    pub const RENDERER: u32 = 0x2294;
    pub const PARAMS: u32 = 0x227c;
    pub const JITTER: u32 = 0x6fc;
    pub const COMMON_VC: u32 = 0x2b9c;
    pub const VP_TABLE: u32 = 0x550;
    pub const FP_TABLE: u32 = 0x54c;
    pub const V_ONE: u32 = 0x21e8;
    pub const S_DEPTH: u32 = 0x21e4;
    pub const BASIS_B: u32 = 0x243c;
    pub const BASIS_A: u32 = 0x2438;
    pub const BASIS_C: u32 = 0x2434;
    pub const Z0: u32 = 0x22a0;
    pub const Z1: u32 = 0x22a8;
    pub const ZH: u32 = 0x22a4;
    pub const PA: u32 = 0x221c;
    pub const PB: u32 = 0x2218;
    pub const PC: u32 = 0x2214;
    pub const PD: u32 = 0x2210;
    pub const LOFF: u32 = 0x21e0;
    pub const FTOP: u32 = 0x220c;
    pub const T_LO: u32 = 0x754;
    pub const T_HI: u32 = 0x750;
    pub const T_FAR: u32 = 0x748;
    pub const T_UVFAR: u32 = 0x74c;
    pub const VP_HX: u32 = 0x6f8;
    pub const VP_HY: u32 = 0x6f4;
    pub const CLIP_MIN: u32 = 0x27d8;
    pub const CLIP_MAX: u32 = 0x27dc;
    /// Largest displacement used; `TOC` must be at least this.
    pub const LARGEST: u32 = 0x2b9c;
}

/// Renderer object offsets.
pub mod renderer {
    pub const DEPTH_DISTANCE: u32 = 0x30;
    pub const FOG_BLOCK: u32 = 0x50;
    pub const PROJECTION: u32 = 0x260;
    pub const PROJECTION_SIZE: u32 = 0xa28;
    pub const DOWNSAMPLE_RECORD: u32 = 0x2bac;
    pub const SURFACE_TABLE: u32 = 0x2c10;
    pub const TARGET_STATE: u32 = 0x2f58;
    pub const TARGET_STATE_SIZE: u32 = 0x220;
    pub const CURRENT_INDEX: u32 = 0x2fb0;
    pub const SURFACE_DESCRIPTORS: u32 = 0x2fc0;
    pub const LIGHTING_SOURCE_BANK_ZERO: u32 = 0x3120;
    pub const LIGHTING_SOURCE_NO_BANK_ZERO: u32 = 0x3140;
    pub const SURFACE_BRANCH: u32 = 0x3160;
    pub const EXPLICIT_WIDTH: u32 = 0x3164;
    pub const EXPLICIT_HEIGHT: u32 = 0x3168;
    pub const SELECTION_CONTROLS: u32 = 0x3174;
    pub const FOG_CONTROLS: u32 = 0x3290;
    pub const SRGB: u32 = 0x329c;
    pub const STENCIL_BRANCH: u32 = 0x329d;
    pub const AA_ENABLE: u32 = 0x329f;
}

/// Projection block fields (offsets from `RENDERER+0x260`), as bit patterns.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ProjectionBlock {
    pub address: u32,
    pub scale: u32,
    pub far: u32,
    pub slope: u32,
    /// Row-major camera matrix.
    pub matrix: [u32; 16],
    pub aspect: u32,
}

/// The renderer fields a producer may read.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct RendererObject {
    pub address: u32,
    /// `f32` at `+0x34`.
    pub depth_distance_a: Tagged<u32>,
    /// `f32` at `+0x40`.
    pub depth_distance_b: Tagged<u32>,
    pub fog_block: Tagged<[u32; 16]>,
    pub projection: ProjectionBlock,
    pub downsample_record: Tagged<SurfaceRecord>,
    /// Role `r`, index `i` at position `2r + i`.
    pub surface_table: [Tagged<SurfaceRecord>; 8],
    pub current_index: u32,
    /// Compact descriptor of each surface record, same order as the table.
    pub surface_descriptors: [Tagged<[u32; 8]>; 8],
    pub lighting_source_bank_zero: Tagged<[u32; 8]>,
    pub lighting_source_no_bank_zero: Tagged<[u32; 8]>,
    pub surface_branch: u32,
    pub explicit_width: u32,
    pub explicit_height: u32,
    pub selection_controls: [u32; 3],
    pub fog_controls: u32,
    pub srgb: u8,
    pub stencil_branch: u8,
    pub aa_enable: u8,
}

impl RendererObject {
    /// Byte `+0x329D` and word `+0x3160` both nonzero.
    pub fn bank_zero_enabled(&self) -> bool {
        self.stencil_branch != 0 && self.surface_branch != 0
    }

    /// Word `+0x3174` nonzero.
    pub fn role_forcing(&self) -> bool {
        self.selection_controls[0] != 0
    }

    /// The compact lighting source descriptor: `+0x3120` with bank zero, `+0x3140` without.
    pub fn lighting_source(&self) -> Tagged<[u32; 8]> {
        if self.bank_zero_enabled() {
            self.lighting_source_bank_zero
        } else {
            self.lighting_source_no_bank_zero
        }
    }
}

/// The shadow/parameter object fields.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ParamsObject {
    pub address: u32,
    pub transform0: Tagged<[u32; 16]>,
    pub transform1: Tagged<[u32; 16]>,
    /// Full descriptor for lighting texture unit 1.
    pub lighting_unit1: Tagged<[u32; 12]>,
}

/// A four-lane vector constant that the game loads with 16-byte alignment.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum VectorConstant {
    Read(Tagged<[u32; 4]>),
    /// The pointer is not 16-byte aligned; the value was not read. Producers that use it decline.
    Misaligned(u32),
}

impl VectorConstant {
    pub fn lanes(&self) -> Option<[u32; 4]> {
        match self {
            Self::Read(tagged) => Some(tagged.value),
            Self::Misaligned(_) => None,
        }
    }
}

/// Scalar constants, as bit patterns tagged with their global addresses.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Scalars {
    pub z0: Tagged<u32>,
    pub z1: Tagged<u32>,
    pub zh: Tagged<u32>,
    pub pa: Tagged<u32>,
    pub pb: Tagged<u32>,
    pub pc: Tagged<u32>,
    pub pd: Tagged<u32>,
    pub loff: Tagged<u32>,
    pub ftop: Tagged<u32>,
    pub t_lo: Tagged<u32>,
    pub t_hi: Tagged<u32>,
    pub t_far: Tagged<u32>,
    pub t_uvfar: Tagged<u32>,
    pub vp_hx: Tagged<u32>,
    pub vp_hy: Tagged<u32>,
    pub clip_min: Tagged<u32>,
    pub clip_max: Tagged<u32>,
}

/// A vertex program object read whole.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct VertexProgramObject {
    pub registry_index: u32,
    pub address: u32,
    pub instruction_count: u32,
    pub instruction_offset: u32,
    pub constant_count: u32,
    /// The whole extent, header included.
    pub bytes: Vec<u8>,
}

/// A fragment program file read whole.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct FragmentProgramFile {
    pub registry_index: u32,
    pub address: u32,
    pub microcode_length: u32,
    /// Header `+8`; any of bits 1–3 means unsupported outputs such as depth export.
    pub control_flags: u32,
    /// Header `+12` as stored in the original file.
    pub storage_word: u32,
    pub microcode_offset: u32,
    pub bytes: Vec<u8>,
}

impl FragmentProgramFile {
    /// The microcode bytes inside the original file.
    pub fn microcode(&self) -> &[u8] {
        let start = self.microcode_offset as usize;
        &self.bytes[start..start + self.microcode_length as usize]
    }

    /// Bits 1–3 of the control flags.
    pub fn has_unsupported_outputs(&self) -> bool {
        self.control_flags & 0xe != 0
    }
}

/// A producer's programs. `None` is an absent program, not an error.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct ProgramPair {
    pub vertex: Option<VertexProgramObject>,
    pub fragment: Option<FragmentProgramFile>,
}

impl ProgramPair {
    pub fn both_present(&self) -> bool {
        self.vertex.is_some() && self.fragment.is_some()
    }
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct FogPrograms {
    pub vertex: Option<VertexProgramObject>,
    pub untinted: Option<FragmentProgramFile>,
    pub tinted: Option<FragmentProgramFile>,
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct ProducerPrograms {
    pub stencil: ProgramPair,
    pub lighting: ProgramPair,
    pub downsample: ProgramPair,
    pub quincunx: ProgramPair,
    pub fog: FogPrograms,
}

/// Registry indices of the producer programs.
pub mod registry {
    pub const STENCIL: (u32, u32) = (3, 0x41);
    pub const LIGHTING: (u32, u32) = (0x12, 0x44);
    pub const DOWNSAMPLE: (u32, u32) = (0x0e, 0x0e);
    pub const QUINCUNX: (u32, u32) = (0x0c, 0x3c);
    pub const FOG_VERTEX: u32 = 0x0d;
    pub const FOG_UNTINTED: u32 = 3;
    pub const FOG_TINTED: u32 = 4;
}

/// The immutable input set of one Main invocation.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ProducerInputs {
    pub toc: u32,
    pub renderer: RendererObject,
    pub params: ParamsObject,
    /// Jitter record words; `f32` at `+4` and `+8` are used.
    pub jitter: Tagged<[u32; 3]>,
    pub common_vc: u32,
    /// Rows 21, 22 and 43 of the common vertex-constant block.
    pub common_rows: [(u32, Tagged<[u32; 4]>); 3],
    pub v_one: VectorConstant,
    pub basis_a: VectorConstant,
    pub basis_b: VectorConstant,
    pub basis_c: VectorConstant,
    pub s_depth: Tagged<u32>,
    pub scalars: Scalars,
    pub programs: ProducerPrograms,
}

/// The common vertex-constant rows that are qualified inputs.
pub const COMMON_ROWS: [u32; 3] = [21, 22, 43];

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum AcquisitionError {
    InvalidToc(u32),
    NullPointer { global: &'static str },
    Misaligned { global: &'static str, address: u32 },
    OutOfRange { address: u32, length: u32 },
    Source(SourceError),
    Short { address: u32, length: u32 },
    Budget,
    VertexProgram { index: u32, reason: ProgramError },
    FragmentProgram { index: u32, reason: ProgramError },
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ProgramError {
    InstructionCount(u32),
    ConstantCount(u32),
    InstructionOffset(u32),
    FileLength(u32),
    MicrocodeLength(u32),
    MicrocodeOffset(u32),
    MicrocodeExtent { offset: u32, length: u32, file: u32 },
    ChangedHeader,
}

/// Default read budget in bytes for one acquisition. Its size is not behaviour.
pub const DEFAULT_READ_BUDGET: u64 = 4 << 20;

struct Reader<'a, M: GuestMemory + ?Sized> {
    memory: &'a M,
    budget: u64,
}

impl<M: GuestMemory + ?Sized> Reader<'_, M> {
    fn bytes(&mut self, address: u32, length: u32) -> Result<Vec<u8>, AcquisitionError> {
        if address == 0 || address as u64 + length as u64 > 1 << 32 {
            return Err(AcquisitionError::OutOfRange { address, length });
        }
        if length as u64 > self.budget {
            return Err(AcquisitionError::Budget);
        }
        self.budget -= length as u64;
        let bytes = self
            .memory
            .read(address, length)
            .map_err(AcquisitionError::Source)?;
        if bytes.len() != length as usize {
            return Err(AcquisitionError::Short { address, length });
        }
        Ok(bytes)
    }

    fn word(&mut self, address: u32) -> Result<u32, AcquisitionError> {
        Ok(be_u32(&self.bytes(address, 4)?, 0))
    }

    fn words<const N: usize>(
        &mut self,
        address: u32,
    ) -> Result<Tagged<[u32; N]>, AcquisitionError> {
        let bytes = self.bytes(address, 4 * N as u32)?;
        Ok(Tagged {
            address,
            value: be_words(&bytes, 0),
        })
    }

    fn pointer(
        &mut self,
        toc: u32,
        displacement: u32,
        global: &'static str,
    ) -> Result<u32, AcquisitionError> {
        let value = self.word(toc - displacement)?;
        if value == 0 {
            return Err(AcquisitionError::NullPointer { global });
        }
        Ok(value)
    }

    fn scalar(&mut self, toc: u32, displacement: u32) -> Result<Tagged<u32>, AcquisitionError> {
        let address = toc - displacement;
        Ok(Tagged {
            address,
            value: self.word(address)?,
        })
    }

    fn vector(
        &mut self,
        toc: u32,
        displacement: u32,
        global: &'static str,
    ) -> Result<VectorConstant, AcquisitionError> {
        let address = self.pointer(toc, displacement, global)?;
        if address % 16 != 0 {
            return Ok(VectorConstant::Misaligned(address));
        }
        Ok(VectorConstant::Read(self.words(address)?))
    }

    fn surface(&mut self, address: u32) -> Result<Tagged<SurfaceRecord>, AcquisitionError> {
        let words = self.words::<21>(address)?;
        Ok(Tagged {
            address,
            value: SurfaceRecord { words: words.value },
        })
    }

    fn vertex_program(
        &mut self,
        table: u32,
        index: u32,
    ) -> Result<Option<VertexProgramObject>, AcquisitionError> {
        let entry = self.bytes(table + 12 * index, 12)?;
        let address = be_u32(&entry, 4);
        if address == 0 {
            return Ok(None);
        }
        let fail = |reason| AcquisitionError::VertexProgram { index, reason };
        let header = self.bytes(address, 24)?;
        let n = be_u16(&header, 0) as u32;
        let o = be_u16(&header, 18) as u32;
        let k = be_u16(&header, 22) as u32;
        if n > 512 {
            return Err(fail(ProgramError::InstructionCount(n)));
        }
        if k > 512 {
            return Err(fail(ProgramError::ConstantCount(k)));
        }
        if n != 0 && (o < 24 || !o.is_multiple_of(4)) {
            return Err(fail(ProgramError::InstructionOffset(o)));
        }
        let extent = vertex_program_extent(n, o, k);
        let bytes = self.bytes(address, extent)?;
        if bytes[..24] != header[..] {
            return Err(fail(ProgramError::ChangedHeader));
        }
        Ok(Some(VertexProgramObject {
            registry_index: index,
            address,
            instruction_count: n,
            instruction_offset: o,
            constant_count: k,
            bytes,
        }))
    }

    fn fragment_program(
        &mut self,
        table: u32,
        index: u32,
    ) -> Result<Option<FragmentProgramFile>, AcquisitionError> {
        let entry = self.bytes(table + 16 * index, 16)?;
        let address = be_u32(&entry, 4);
        let length = be_u32(&entry, 8);
        if address == 0 {
            return Ok(None);
        }
        let fail = |reason| AcquisitionError::FragmentProgram { index, reason };
        if length < 24 {
            return Err(fail(ProgramError::FileLength(length)));
        }
        let header = self.bytes(address, 24)?;
        let m = be_u32(&header, 0);
        let flags = be_u32(&header, 8);
        let storage = be_u32(&header, 12);
        let q = be_u32(&header, 16);
        if m == 0 || !m.is_multiple_of(16) {
            return Err(fail(ProgramError::MicrocodeLength(m)));
        }
        if q < 24 || !q.is_multiple_of(4) {
            return Err(fail(ProgramError::MicrocodeOffset(q)));
        }
        if q as u64 + m as u64 > length as u64 {
            return Err(fail(ProgramError::MicrocodeExtent {
                offset: q,
                length: m,
                file: length,
            }));
        }
        let bytes = self.bytes(address, length)?;
        if bytes[..24] != header[..] {
            return Err(fail(ProgramError::ChangedHeader));
        }
        Ok(Some(FragmentProgramFile {
            registry_index: index,
            address,
            microcode_length: m,
            control_flags: flags,
            storage_word: storage,
            microcode_offset: q,
            bytes,
        }))
    }
}

/// Extent of a vertex program object: `max(E_c, E_i)`.
pub fn vertex_program_extent(instructions: u32, offset: u32, constants: u32) -> u32 {
    let constant_extent = if constants == 0 {
        24
    } else {
        (24 + 4 * constants).next_multiple_of(16) + 16 * constants
    };
    let instruction_extent = if instructions == 0 {
        0
    } else {
        offset + 16 * instructions
    };
    constant_extent.max(instruction_extent)
}

/// Acquires the producer input set with the default read budget.
pub fn acquire<M: GuestMemory + ?Sized>(
    memory: &M,
    toc: u32,
) -> Result<ProducerInputs, AcquisitionError> {
    acquire_with_budget(memory, toc, DEFAULT_READ_BUDGET)
}

/// Acquires the producer input set, failing as a whole on any error.
pub fn acquire_with_budget<M: GuestMemory + ?Sized>(
    memory: &M,
    toc: u32,
    budget: u64,
) -> Result<ProducerInputs, AcquisitionError> {
    use globals as g;
    use renderer as r;
    if !toc.is_multiple_of(4) || toc < g::LARGEST {
        return Err(AcquisitionError::InvalidToc(toc));
    }
    let mut reader = Reader { memory, budget };
    let reader = &mut reader;

    // Renderer object.
    let base = reader.pointer(toc, g::RENDERER, "RENDERER")?;
    let at = |offset: u32| -> Result<u32, AcquisitionError> {
        base.checked_add(offset)
            .ok_or(AcquisitionError::OutOfRange {
                address: base,
                length: offset,
            })
    };
    let depth_distance = reader.bytes(at(r::DEPTH_DISTANCE)?, 20)?;
    let fog_block = reader.words::<16>(at(r::FOG_BLOCK)?)?;
    let projection_address = at(r::PROJECTION)?;
    let projection = reader.bytes(projection_address, r::PROJECTION_SIZE)?;
    let downsample_record = reader.surface(at(r::DOWNSAMPLE_RECORD)?)?;
    let mut surface_table = Vec::with_capacity(8);
    for position in 0..8 {
        surface_table.push(reader.surface(at(r::SURFACE_TABLE + 84 * position)?)?);
    }
    let target_state_address = at(r::TARGET_STATE)?;
    let target_state = reader.bytes(target_state_address, r::TARGET_STATE_SIZE)?;
    let field = |offset: u32| (offset - r::TARGET_STATE) as usize;
    let descriptor = |offset: u32| Tagged {
        address: base + offset,
        value: be_words::<8>(&target_state, field(offset)),
    };
    let selection = reader.words::<3>(at(r::SELECTION_CONTROLS)?)?;
    let fog_controls = reader.word(at(r::FOG_CONTROLS)?)?;
    let bytes_329c = reader.bytes(at(r::SRGB)?, 4)?;
    let renderer = RendererObject {
        address: base,
        depth_distance_a: Tagged {
            address: base + 0x34,
            value: be_u32(&depth_distance, 4),
        },
        depth_distance_b: Tagged {
            address: base + 0x40,
            value: be_u32(&depth_distance, 0x10),
        },
        fog_block,
        projection: ProjectionBlock {
            address: projection_address,
            scale: be_u32(&projection, 0x54),
            far: be_u32(&projection, 0x58),
            slope: be_u32(&projection, 0x60),
            matrix: be_words(&projection, 0x70),
            aspect: be_u32(&projection, 0xa24),
        },
        downsample_record,
        surface_table: surface_table.try_into().expect("eight records"),
        current_index: be_u32(&target_state, field(r::CURRENT_INDEX)),
        surface_descriptors: std::array::from_fn(|position| {
            descriptor(r::SURFACE_DESCRIPTORS + 32 * position as u32)
        }),
        lighting_source_bank_zero: descriptor(r::LIGHTING_SOURCE_BANK_ZERO),
        lighting_source_no_bank_zero: descriptor(r::LIGHTING_SOURCE_NO_BANK_ZERO),
        surface_branch: be_u32(&target_state, field(r::SURFACE_BRANCH)),
        explicit_width: be_u32(&target_state, field(r::EXPLICIT_WIDTH)),
        explicit_height: be_u32(&target_state, field(r::EXPLICIT_HEIGHT)),
        selection_controls: selection.value,
        fog_controls,
        srgb: bytes_329c[0],
        stencil_branch: bytes_329c[1],
        aa_enable: bytes_329c[3],
    };

    // Jitter, parameters and common vertex constants.
    let jitter_address = reader.pointer(toc, g::JITTER, "JITTER")?;
    let jitter = reader.words::<3>(jitter_address)?;
    let params_address = reader.pointer(toc, g::PARAMS, "PARAMS")?;
    let params_bytes = reader.bytes(params_address, 0x15ac)?;
    let params = ParamsObject {
        address: params_address,
        transform0: Tagged {
            address: params_address + 0xd0,
            value: be_words(&params_bytes, 0xd0),
        },
        transform1: Tagged {
            address: params_address + 0xb50,
            value: be_words(&params_bytes, 0xb50),
        },
        lighting_unit1: Tagged {
            address: params_address + 0x1520,
            value: be_words(&params_bytes, 0x1520),
        },
    };
    let common_vc = reader.pointer(toc, g::COMMON_VC, "COMMON_VC")?;
    if common_vc % 16 != 0 {
        return Err(AcquisitionError::Misaligned {
            global: "COMMON_VC",
            address: common_vc,
        });
    }
    let mut common_rows = Vec::with_capacity(3);
    for row in COMMON_ROWS {
        let address = common_vc
            .checked_add(16 * row)
            .ok_or(AcquisitionError::OutOfRange {
                address: common_vc,
                length: 16 * row,
            })?;
        common_rows.push((row, reader.words::<4>(address)?));
    }

    // Programs.
    let bank_zero = renderer.bank_zero_enabled();
    let role_forcing = renderer.role_forcing();
    let vp_table = reader.pointer(toc, g::VP_TABLE, "VP_TABLE")?;
    let fp_table = reader.pointer(toc, g::FP_TABLE, "FP_TABLE")?;
    let pair = |reader: &mut Reader<'_, M>, (vertex, fragment): (u32, u32), condition: bool| {
        if !condition {
            return Ok(ProgramPair::default());
        }
        Ok::<_, AcquisitionError>(ProgramPair {
            vertex: reader.vertex_program(vp_table, vertex)?,
            fragment: reader.fragment_program(fp_table, fragment)?,
        })
    };
    let stencil = pair(reader, registry::STENCIL, bank_zero)?;
    let lighting = pair(reader, registry::LIGHTING, true)?;
    let downsample = pair(reader, registry::DOWNSAMPLE, bank_zero)?;
    let quincunx = pair(reader, registry::QUINCUNX, bank_zero)?;
    let fog = if role_forcing {
        FogPrograms::default()
    } else {
        FogPrograms {
            vertex: reader.vertex_program(vp_table, registry::FOG_VERTEX)?,
            untinted: reader.fragment_program(fp_table, registry::FOG_UNTINTED)?,
            tinted: reader.fragment_program(fp_table, registry::FOG_TINTED)?,
        }
    };

    // Scalars and indirect constants.
    let scalars = Scalars {
        z0: reader.scalar(toc, g::Z0)?,
        z1: reader.scalar(toc, g::Z1)?,
        zh: reader.scalar(toc, g::ZH)?,
        pa: reader.scalar(toc, g::PA)?,
        pb: reader.scalar(toc, g::PB)?,
        pc: reader.scalar(toc, g::PC)?,
        pd: reader.scalar(toc, g::PD)?,
        loff: reader.scalar(toc, g::LOFF)?,
        ftop: reader.scalar(toc, g::FTOP)?,
        t_lo: reader.scalar(toc, g::T_LO)?,
        t_hi: reader.scalar(toc, g::T_HI)?,
        t_far: reader.scalar(toc, g::T_FAR)?,
        t_uvfar: reader.scalar(toc, g::T_UVFAR)?,
        vp_hx: reader.scalar(toc, g::VP_HX)?,
        vp_hy: reader.scalar(toc, g::VP_HY)?,
        clip_min: reader.scalar(toc, g::CLIP_MIN)?,
        clip_max: reader.scalar(toc, g::CLIP_MAX)?,
    };
    let v_one = reader.vector(toc, g::V_ONE, "V_ONE")?;
    let s_depth_address = reader.pointer(toc, g::S_DEPTH, "S_DEPTH")?;
    let s_depth = Tagged {
        address: s_depth_address,
        value: reader.word(s_depth_address)?,
    };
    let basis_b = reader.vector(toc, g::BASIS_B, "BASIS_B")?;
    let basis_a = reader.vector(toc, g::BASIS_A, "BASIS_A")?;
    let basis_c = reader.vector(toc, g::BASIS_C, "BASIS_C")?;

    Ok(ProducerInputs {
        toc,
        renderer,
        params,
        jitter,
        common_vc,
        common_rows: common_rows.try_into().expect("three rows"),
        v_one,
        basis_a,
        basis_b,
        basis_c,
        s_depth,
        scalars,
        programs: ProducerPrograms {
            stencil,
            lighting,
            downsample,
            quincunx,
            fog,
        },
    })
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;
    use crate::guest::SparseMemory;

    pub(crate) const TOC: u32 = 0x0010_0000;
    pub(crate) const RENDERER: u32 = 0x0020_0000;
    pub(crate) const PARAMS: u32 = 0x0030_0000;
    pub(crate) const JITTER: u32 = 0x0031_0000;
    pub(crate) const COMMON_VC: u32 = 0x0032_0000;
    pub(crate) const VP_TABLE: u32 = 0x0033_0000;
    pub(crate) const FP_TABLE: u32 = 0x0034_0000;
    pub(crate) const VECTORS: u32 = 0x0035_0000;
    pub(crate) const PROGRAMS: u32 = 0x0036_0000;

    /// A synthetic guest image with every pointer valid and every program absent.
    pub(crate) fn image() -> SparseMemory {
        let mut m = SparseMemory::new();
        m.put(RENDERER, &vec![0u8; 0x32a0]);
        m.put(PARAMS, &vec![0u8; 0x15ac]);
        m.put(JITTER, &[0u8; 12]);
        m.put(COMMON_VC, &[0u8; 48 * 16]);
        m.put(VP_TABLE, &[0u8; 12 * 0x20]);
        m.put(FP_TABLE, &[0u8; 16 * 0x50]);
        m.put(VECTORS, &[0u8; 0x100]);
        m.put(
            TOC - globals::LARGEST,
            &vec![0u8; globals::LARGEST as usize],
        );
        for (displacement, value) in [
            (globals::RENDERER, RENDERER),
            (globals::PARAMS, PARAMS),
            (globals::JITTER, JITTER),
            (globals::COMMON_VC, COMMON_VC),
            (globals::VP_TABLE, VP_TABLE),
            (globals::FP_TABLE, FP_TABLE),
            (globals::V_ONE, VECTORS),
            (globals::S_DEPTH, VECTORS + 0x10),
            (globals::BASIS_A, VECTORS + 0x20),
            (globals::BASIS_B, VECTORS + 0x30),
            (globals::BASIS_C, VECTORS + 0x40),
        ] {
            m.put_u32(TOC - displacement, value);
        }
        m
    }

    fn put_vertex_program(m: &mut SparseMemory, index: u32, address: u32, n: u16, o: u16, k: u16) {
        m.put_u32(VP_TABLE + 12 * index + 4, address);
        let extent = vertex_program_extent(n as u32, o as u32, k as u32);
        let mut bytes = vec![0xabu8; extent as usize];
        bytes[..24].fill(0);
        bytes[0..2].copy_from_slice(&n.to_be_bytes());
        bytes[18..20].copy_from_slice(&o.to_be_bytes());
        bytes[22..24].copy_from_slice(&k.to_be_bytes());
        m.put(address, &bytes);
    }

    fn put_fragment_program(
        m: &mut SparseMemory,
        index: u32,
        address: u32,
        length: u32,
        mc: u32,
        q: u32,
    ) {
        m.put_u32(FP_TABLE + 16 * index + 4, address);
        m.put_u32(FP_TABLE + 16 * index + 8, length);
        let mut bytes: Vec<u8> = (0..length).map(|i| i as u8).collect();
        bytes[..24].fill(0);
        bytes[0..4].copy_from_slice(&mc.to_be_bytes());
        bytes[16..20].copy_from_slice(&q.to_be_bytes());
        m.put(address, &bytes);
    }

    fn enable_bank_zero(m: &mut SparseMemory) {
        m.put(RENDERER + renderer::STENCIL_BRANCH, &[1]);
        m.put_u32(RENDERER + renderer::SURFACE_BRANCH, 1);
    }

    #[test]
    fn baseline_acquires_with_absent_programs() {
        let inputs = acquire(&image(), TOC).unwrap();
        assert!(!inputs.renderer.bank_zero_enabled());
        assert_eq!(inputs.programs.lighting, ProgramPair::default());
        assert_eq!(inputs.common_rows[1].1.address, COMMON_VC + 16 * 22);
        assert_eq!(inputs.renderer.lighting_source().address, RENDERER + 0x3140);
        assert!(matches!(inputs.v_one, VectorConstant::Read(_)));
    }

    #[test]
    fn vector_1_absent_program() {
        let mut m = image();
        enable_bank_zero(&mut m);
        put_fragment_program(&mut m, 0x41, PROGRAMS, 64, 32, 24);
        let inputs = acquire(&m, TOC).unwrap();
        assert!(inputs.renderer.bank_zero_enabled());
        assert!(inputs.programs.stencil.vertex.is_none());
        assert!(inputs.programs.stencil.fragment.is_some());
        assert!(!inputs.programs.stencil.both_present());
    }

    #[test]
    fn vectors_2_to_4_vertex_extent() {
        assert_eq!(vertex_program_extent(2, 0x40, 3), 96);
        assert_eq!(vertex_program_extent(0, 0, 1), 48);
        assert_eq!(vertex_program_extent(0, 0, 0), 24);
        let mut m = image();
        put_vertex_program(&mut m, 0x12, PROGRAMS, 2, 0x40, 3);
        let program = acquire(&m, TOC).unwrap().programs.lighting.vertex.unwrap();
        assert_eq!(program.bytes.len(), 96);
        let mut m = image();
        put_vertex_program(&mut m, 0x12, PROGRAMS, 1, 22, 0);
        assert_eq!(
            acquire(&m, TOC),
            Err(AcquisitionError::VertexProgram {
                index: 0x12,
                reason: ProgramError::InstructionOffset(22)
            })
        );
        let mut m = image();
        put_vertex_program(&mut m, 0x12, PROGRAMS, 513, 24, 0);
        assert!(matches!(
            acquire(&m, TOC),
            Err(AcquisitionError::VertexProgram {
                reason: ProgramError::InstructionCount(513),
                ..
            })
        ));
    }

    #[test]
    fn vector_5_fragment_bounds() {
        let mut m = image();
        put_fragment_program(&mut m, 0x44, PROGRAMS, 64, 32, 24);
        let program = acquire(&m, TOC)
            .unwrap()
            .programs
            .lighting
            .fragment
            .unwrap();
        let expected: Vec<u8> = (24..56).map(|i| i as u8).collect();
        assert_eq!(program.microcode(), &expected[..]);
        let mut m = image();
        put_fragment_program(&mut m, 0x44, PROGRAMS, 64, 48, 24);
        assert!(matches!(
            acquire(&m, TOC),
            Err(AcquisitionError::FragmentProgram {
                index: 0x44,
                reason: ProgramError::MicrocodeExtent { .. }
            })
        ));
        let mut m = image();
        put_fragment_program(&mut m, 0x44, PROGRAMS, 64, 20, 24);
        assert!(matches!(
            acquire(&m, TOC),
            Err(AcquisitionError::FragmentProgram {
                reason: ProgramError::MicrocodeLength(20),
                ..
            })
        ));
    }

    #[test]
    fn vector_6_changed_header() {
        /// Returns a different header on the second read of the program address.
        struct Changing(SparseMemory, std::cell::Cell<u32>);
        impl GuestMemory for Changing {
            fn read(&self, address: u32, length: u32) -> Result<Vec<u8>, SourceError> {
                let mut bytes = self.0.read(address, length)?;
                if address == PROGRAMS {
                    self.1.set(self.1.get() + 1);
                    if self.1.get() == 2 {
                        bytes[5] ^= 1;
                    }
                }
                Ok(bytes)
            }
        }
        let mut m = image();
        put_fragment_program(&mut m, 0x44, PROGRAMS, 64, 32, 24);
        let changing = Changing(m, std::cell::Cell::new(0));
        assert!(matches!(
            acquire(&changing, TOC),
            Err(AcquisitionError::FragmentProgram {
                reason: ProgramError::ChangedHeader,
                ..
            })
        ));
    }

    #[test]
    fn vector_7_conditional_reads() {
        /// Fails any read of the registry entries that bank zero would need.
        struct Guarded(SparseMemory);
        impl GuestMemory for Guarded {
            fn read(&self, address: u32, length: u32) -> Result<Vec<u8>, SourceError> {
                let forbidden = [
                    VP_TABLE + 12 * 3,
                    VP_TABLE + 12 * 0x0e,
                    VP_TABLE + 12 * 0x0c,
                    FP_TABLE + 16 * 0x41,
                    FP_TABLE + 16 * 0x0e,
                    FP_TABLE + 16 * 0x3c,
                ];
                if forbidden.contains(&address) {
                    return Err(SourceError {
                        address,
                        length,
                        reason: "registry read while bank zero is disabled".into(),
                    });
                }
                self.0.read(address, length)
            }
        }
        let mut m = image();
        m.put_u32(RENDERER + renderer::SURFACE_BRANCH, 1);
        m.put_u32(RENDERER + 0x2bac + 4, 0x1234_0000);
        let inputs = acquire(&Guarded(m), TOC).unwrap();
        assert_eq!(inputs.programs.stencil, ProgramPair::default());
        assert_eq!(inputs.programs.downsample, ProgramPair::default());
        assert_eq!(inputs.programs.quincunx, ProgramPair::default());
        assert_eq!(
            inputs.renderer.downsample_record.value.words[1],
            0x1234_0000
        );
    }

    #[test]
    fn role_forcing_skips_fog_programs() {
        let mut m = image();
        m.put_u32(RENDERER + renderer::SELECTION_CONTROLS, 1);
        put_vertex_program(&mut m, 0x0d, PROGRAMS, 1, 24, 0);
        let inputs = acquire(&m, TOC).unwrap();
        assert!(inputs.programs.fog.vertex.is_none());
        m.put_u32(RENDERER + renderer::SELECTION_CONTROLS, 0);
        assert!(acquire(&m, TOC).unwrap().programs.fog.vertex.is_some());
    }

    #[test]
    fn vector_8_misaligned_vector_constant() {
        let mut m = image();
        m.put_u32(TOC - globals::BASIS_A, VECTORS + 0x28);
        let inputs = acquire(&m, TOC).unwrap();
        assert_eq!(inputs.basis_a, VectorConstant::Misaligned(VECTORS + 0x28));
        assert_eq!(inputs.basis_a.lanes(), None);
    }

    #[test]
    fn vector_9_common_rows() {
        let mut m = image();
        m.put_u32(COMMON_VC + 0x160, 0x3f80_0000);
        let inputs = acquire(&m, TOC).unwrap();
        assert_eq!(inputs.common_rows[1].0, 22);
        assert_eq!(inputs.common_rows[1].1.address, COMMON_VC + 0x160);
        assert_eq!(inputs.common_rows[1].1.value[0], 0x3f80_0000);
        m.put_u32(TOC - globals::COMMON_VC, COMMON_VC + 8);
        assert_eq!(
            acquire(&m, TOC),
            Err(AcquisitionError::Misaligned {
                global: "COMMON_VC",
                address: COMMON_VC + 8
            })
        );
    }

    #[test]
    fn whole_acquisition_failures() {
        assert_eq!(
            acquire(&image(), TOC + 2),
            Err(AcquisitionError::InvalidToc(TOC + 2))
        );
        assert_eq!(
            acquire(&image(), 0x2b98),
            Err(AcquisitionError::InvalidToc(0x2b98))
        );
        let mut m = image();
        m.put_u32(TOC - globals::PARAMS, 0);
        assert_eq!(
            acquire(&m, TOC),
            Err(AcquisitionError::NullPointer { global: "PARAMS" })
        );
        assert_eq!(
            acquire_with_budget(&image(), TOC, 1000),
            Err(AcquisitionError::Budget)
        );
        let mut m = image();
        m.put_u32(TOC - globals::JITTER, 0xffff_fffc);
        assert_eq!(
            acquire(&m, TOC),
            Err(AcquisitionError::OutOfRange {
                address: 0xffff_fffc,
                length: 12
            })
        );
    }

    #[test]
    fn values_are_copied_bit_for_bit_with_addresses() {
        let mut m = image();
        m.put_u32(TOC - globals::Z1, 0x7fc0_1234);
        m.put_u32(RENDERER + 0x260 + 0x70, 0xffff_ffff);
        m.put_u32(PARAMS + 0x1520 + 4 * 11, 0x55);
        let inputs = acquire(&m, TOC).unwrap();
        assert_eq!(inputs.scalars.z1.value, 0x7fc0_1234);
        assert_eq!(inputs.scalars.z1.address, TOC - globals::Z1);
        assert_eq!(inputs.renderer.projection.matrix[0], 0xffff_ffff);
        assert_eq!(inputs.params.lighting_unit1.value[11], 0x55);
        assert_eq!(inputs.params.lighting_unit1.address, PARAMS + 0x1520);
    }
}
