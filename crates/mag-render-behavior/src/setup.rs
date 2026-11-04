//! Main setup command banks and register knowledge (`material-setup.md`).
//!
//! Bank reads go through a [`GuestMemory`] reader whose addresses are IO offsets in main memory.

use crate::guest::{GuestMemory, SourceError, be_u16, be_u32};
use crate::texture::TextureUnitRecord;
use std::collections::{BTreeMap, HashSet};

/// Exclusive upper bound of valid IO offsets.
const IO_LIMIT: u32 = 0x1000_0000;
/// Budgets used for the eight banks of a Main pass.
pub const MAIN_BANK_WORD_BUDGET: u32 = 8192;
pub const MAIN_BANK_BRANCH_BUDGET: u32 = 32;

/// Preparation status carried by every copied bank. A copy never qualifies removal of the game's
/// own preparation of that bank.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PreparationStatus {
    PreparedByRetainedGameCode,
}

/// One method write of a bank.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct MethodWrite {
    /// IO offset of the payload word.
    pub offset: u32,
    pub subchannel: u32,
    pub method: u32,
    pub value: u32,
    /// Ordinal of the method packet this write came from, counted from 0 within the bank.
    pub packet: u32,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct OwnedSetupBank {
    pub target: u32,
    pub methods: Vec<MethodWrite>,
    /// Every request, in order: (IO offset, bytes copied).
    pub reads: Vec<(u32, Vec<u8>)>,
    pub status: PreparationStatus,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum BankError {
    Target(u32),
    ControlCycle(u32),
    Budget,
    Source(SourceError),
    Extent,
    NestedCall,
    Header(u32),
    MethodExtent,
}

fn valid_address(address: u32) -> bool {
    address != 0 && address.is_multiple_of(4) && address < IO_LIMIT
}

struct BankCopier<'a, M: GuestMemory + ?Sized> {
    memory: &'a M,
    words_left: u32,
    branches_left: u32,
    reads: Vec<(u32, Vec<u8>)>,
}

impl<M: GuestMemory + ?Sized> BankCopier<'_, M> {
    fn read_words(&mut self, start: u32, count: u32) -> Result<Vec<u32>, BankError> {
        if count > self.words_left {
            return Err(BankError::Budget);
        }
        let end = (count as u64) * 4 + start as u64;
        if !valid_address(start) || end > IO_LIMIT as u64 {
            return Err(BankError::Target(start));
        }
        self.words_left -= count;
        let bytes = self
            .memory
            .read(start, count * 4)
            .map_err(BankError::Source)?;
        if bytes.len() != (count * 4) as usize {
            return Err(BankError::Extent);
        }
        let words = (0..count as usize).map(|i| be_u32(&bytes, 4 * i)).collect();
        self.reads.push((start, bytes));
        Ok(words)
    }

    fn branch(&mut self) -> Result<(), BankError> {
        if self.branches_left == 0 {
            return Err(BankError::Budget);
        }
        self.branches_left -= 1;
        Ok(())
    }
}

/// Copies one setup bank into an owned, ordered list of method writes. Any failure returns no
/// bank.
pub fn copy_setup_bank<M: GuestMemory + ?Sized>(
    memory: &M,
    target: u32,
    word_budget: u32,
    branch_budget: u32,
) -> Result<OwnedSetupBank, BankError> {
    if !valid_address(target) {
        return Err(BankError::Target(target));
    }
    let mut copier = BankCopier {
        memory,
        words_left: word_budget,
        branches_left: branch_budget,
        reads: Vec::new(),
    };
    let mut methods = Vec::new();
    let mut visited: HashSet<(u32, Option<u32>)> = HashSet::new();
    let mut position = target;
    let mut pending_return: Option<u32> = None;
    let mut packet = 0u32;
    loop {
        if !visited.insert((position, pending_return)) {
            return Err(BankError::ControlCycle(position));
        }
        let header = copier.read_words(position, 1)?[0];
        let after_header = position.checked_add(4).ok_or(BankError::Target(position))?;
        let kind = header >> 29;
        if header == 0x0002_0000 {
            copier.branch()?;
            match pending_return.take() {
                Some(resume) => position = resume,
                None => break,
            }
        } else if kind == 1 {
            copier.branch()?;
            let destination = header & 0x1fff_fffc;
            if !valid_address(destination) {
                return Err(BankError::Target(destination));
            }
            position = destination;
        } else if header & 3 == 1 {
            copier.branch()?;
            let destination = header & !3;
            if !valid_address(destination) {
                return Err(BankError::Target(destination));
            }
            position = destination;
        } else if header & 3 == 2 {
            copier.branch()?;
            if pending_return.is_some() {
                return Err(BankError::NestedCall);
            }
            let destination = header & !3;
            if !valid_address(destination) {
                return Err(BankError::Target(destination));
            }
            pending_return = Some(after_header);
            position = destination;
        } else if (kind == 0 || kind == 2) && header & 3 == 0 {
            let count = (header >> 18) & 0x7ff;
            let subchannel = (header >> 13) & 31;
            let first = header & 0x1ffc;
            let incrementing = kind == 0;
            if count == 0 {
                position = after_header;
                continue;
            }
            if incrementing && first + 4 * (count - 1) > 0x1ffc {
                return Err(BankError::MethodExtent);
            }
            let payload = copier.read_words(after_header, count)?;
            for (index, value) in payload.into_iter().enumerate() {
                let index = index as u32;
                methods.push(MethodWrite {
                    offset: after_header + 4 * index,
                    subchannel,
                    method: if incrementing {
                        first + 4 * index
                    } else {
                        first
                    },
                    value,
                    packet,
                });
            }
            packet += 1;
            position = after_header
                .checked_add(4 * count)
                .ok_or(BankError::Target(after_header))?;
        } else {
            return Err(BankError::Header(header));
        }
    }
    Ok(OwnedSetupBank {
        target,
        methods,
        reads: copier.reads,
        status: PreparationStatus::PreparedByRetainedGameCode,
    })
}

/// Render side effects that a planner must still perform.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ObligationKind {
    Clear,
    PrimitiveBegin,
    PrimitiveEnd,
    Draw,
    Attachment,
    Program,
    Synchronisation,
    TextureBinding,
    VertexInput,
    UntranslatedState,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Obligation {
    pub kind: ObligationKind,
    pub write: MethodWrite,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum MethodClass {
    Obligation(ObligationKind),
    StateOnly,
}

fn classify(method: u32, value: u32) -> Option<MethodClass> {
    use ObligationKind::*;
    let obligation = |kind| Some(MethodClass::Obligation(kind));
    match method {
        0x1d94 => obligation(Clear),
        0x1808 if value == 0 => obligation(PrimitiveEnd),
        0x1808 => obligation(PrimitiveBegin),
        0x180c | 0x1810 | 0x1814 | 0x1818 => obligation(Draw),
        0x200..=0x2fc | 0x8c0 | 0x8c4 | 0xa00..=0xa5c => obligation(Attachment),
        0xb80..=0xbfc | 0x1d60 | 0x1d64 | 0x1e9c | 0x1ea0 | 0x1ef8 | 0x1ff0 | 0x1ff4 | 0x1ff8 => {
            obligation(Program)
        }
        0x50 | 0x64 | 0x68 | 0x6c | 0x1d6c | 0x1d70 | 0x1fc0 | 0x1fd8 => {
            obligation(Synchronisation)
        }
        0x1a00..=0x1bfc | 0x1840..=0x187c | 0xb00..=0xb3c | 0x1d00..=0x1d3c => {
            obligation(TextureBinding)
        }
        0x1680..=0x17bc | 0x1e00..=0x1e40 => obligation(VertexInput),
        0x300 | 0x31c | 0x368 | 0x36c | 0x378 | 0x37c | 0xa60 | 0xa64 | 0x1d8c | 0x1d90 => {
            obligation(UntranslatedState)
        }
        0x304..=0x318
        | 0x320
        | 0x324
        | 0x328
        | 0x32c..=0x344
        | 0x348
        | 0x34c..=0x364
        | 0x370
        | 0x380
        | 0x384
        | 0x388
        | 0xa68..=0xa7c
        | 0x1830
        | 0x1834
        | 0x183c
        | 0x1fec
        | 0x1efc
        | 0x1f00..=0x1f7c => Some(MethodClass::StateOnly),
        _ => None,
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ApplyError {
    UnqualifiedMethod {
        subchannel: u32,
        method: u32,
    },
    MissingConstantLoad,
    /// A constant-data packet follows an earlier one in the same bank without an intervening
    /// load-row write. Refused until hardware load-row advancement is settled (open question 1).
    UnconfirmedConstantLoad {
        packet: u32,
    },
    Value {
        method: u32,
        value: u32,
    },
}

/// Sparse register knowledge. Absence means unknown, never zero.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct KnownRegisters {
    methods: BTreeMap<u32, u32>,
    constants: BTreeMap<(u32, u32), u32>,
}

const CONSTANT_LOAD: u32 = 0x1efc;

impl KnownRegisters {
    pub fn new() -> Self {
        Self::default()
    }

    /// The last known value of `method`.
    pub fn get(&self, method: u32) -> Option<u32> {
        self.methods.get(&method).copied()
    }

    /// Makes `method` known with `value`.
    pub fn set(&mut self, method: u32, value: u32) {
        self.methods.insert(method, value);
    }

    /// Makes `method` unknown.
    pub fn forget(&mut self, method: u32) {
        self.methods.remove(&method);
    }

    /// A vertex constant lane (row 0..511, lane 0..3).
    pub fn constant(&self, row: u32, lane: u32) -> Option<u32> {
        self.constants.get(&(row, lane)).copied()
    }

    /// Makes every register and every vertex-constant lane unknown.
    pub fn invalidate(&mut self) {
        self.methods.clear();
        self.constants.clear();
    }

    /// Applies a bank's method writes. On any failure the knowledge is unchanged and no
    /// obligation is returned.
    pub fn apply(&mut self, bank: &OwnedSetupBank) -> Result<Vec<Obligation>, ApplyError> {
        let mut next = self.clone();
        let mut obligations = Vec::new();
        let mut last_constant_packet: Option<u32> = None;
        let mut load_written_since = false;
        for write in &bank.methods {
            if write.subchannel != 0 {
                return Err(ApplyError::UnqualifiedMethod {
                    subchannel: write.subchannel,
                    method: write.method,
                });
            }
            let class =
                classify(write.method, write.value).ok_or(ApplyError::UnqualifiedMethod {
                    subchannel: 0,
                    method: write.method,
                })?;
            if write.method == CONSTANT_LOAD {
                load_written_since = true;
            }
            if (0x1f00..=0x1f7c).contains(&write.method) {
                if last_constant_packet.is_some_and(|packet| packet != write.packet)
                    && !load_written_since
                {
                    return Err(ApplyError::UnconfirmedConstantLoad {
                        packet: write.packet,
                    });
                }
                if last_constant_packet != Some(write.packet) {
                    last_constant_packet = Some(write.packet);
                    load_written_since = false;
                }
                let load = next
                    .get(CONSTANT_LOAD)
                    .ok_or(ApplyError::MissingConstantLoad)?;
                let k = (write.method - 0x1f00) / 4;
                let row = load as u64 + (k / 4) as u64;
                if row >= 512 {
                    return Err(ApplyError::Value {
                        method: CONSTANT_LOAD,
                        value: load,
                    });
                }
                next.constants.insert((row as u32, k % 4), write.value);
            }
            next.set(write.method, write.value);
            if let MethodClass::Obligation(kind) = class {
                obligations.push(Obligation {
                    kind,
                    write: *write,
                });
            }
        }
        next.pipeline().map_err(|error| match error {
            PipelineError::Value { method, value } => ApplyError::Value { method, value },
        })?;
        *self = next;
        Ok(obligations)
    }

    /// Decodes the fixed-function pipeline state from the known registers.
    pub fn pipeline(&self) -> Result<PipelineState, PipelineError> {
        decode_pipeline(self)
    }

    /// Assembles texture unit `unit` (0..15).
    pub fn texture_unit(&self, unit: u32) -> Result<TextureUnitRecord, PipelineError> {
        if unit >= 16 {
            return Err(PipelineError::Value {
                method: 0x1a00,
                value: unit,
            });
        }
        let base = 0x1a00 + 32 * unit;
        let words = [
            self.get(base),
            self.get(base + 4),
            self.get(base + 8),
            self.get(base + 12),
            self.get(base + 16),
            self.get(base + 20),
            self.get(base + 24),
            self.get(0x1840 + 4 * unit),
        ];
        let compact = if words.iter().all(Option::is_some) {
            Some(words.map(Option::unwrap))
        } else {
            None
        };
        Ok(TextureUnitRecord {
            compact,
            border: self.get(base + 28),
            texcoord_control: self.get(0xb00 + 4 * unit),
            vertex_lod_control: self.get(0x1d00 + 4 * unit),
        })
    }

    /// Rows whose four lanes are all known.
    pub fn vertex_constant_bank(&self) -> BTreeMap<u32, [u32; 4]> {
        let mut rows = BTreeMap::new();
        for &(row, _) in self.constants.keys() {
            if rows.contains_key(&row) {
                continue;
            }
            let lanes = [0, 1, 2, 3].map(|lane| self.constant(row, lane));
            if lanes.iter().all(Option::is_some) {
                rows.insert(row, lanes.map(Option::unwrap));
            }
        }
        rows
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PipelineError {
    Value { method: u32, value: u32 },
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Comparison {
    Never,
    Less,
    Equal,
    LessOrEqual,
    Greater,
    NotEqual,
    GreaterOrEqual,
    Always,
}

impl Comparison {
    pub fn from_code(code: u32) -> Option<Self> {
        Some(match code {
            0x200 => Self::Never,
            0x201 => Self::Less,
            0x202 => Self::Equal,
            0x203 => Self::LessOrEqual,
            0x204 => Self::Greater,
            0x205 => Self::NotEqual,
            0x206 => Self::GreaterOrEqual,
            0x207 => Self::Always,
            _ => return None,
        })
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum BlendFactor {
    Zero,
    One,
    SourceColour,
    OneMinusSourceColour,
    SourceAlpha,
    OneMinusSourceAlpha,
    DestinationAlpha,
    OneMinusDestinationAlpha,
    DestinationColour,
    OneMinusDestinationColour,
    SourceAlphaSaturate,
    ConstantColour,
    OneMinusConstantColour,
    ConstantAlpha,
    OneMinusConstantAlpha,
}

impl BlendFactor {
    pub fn from_code(code: u32) -> Option<Self> {
        Some(match code {
            0 => Self::Zero,
            1 => Self::One,
            0x300 => Self::SourceColour,
            0x301 => Self::OneMinusSourceColour,
            0x302 => Self::SourceAlpha,
            0x303 => Self::OneMinusSourceAlpha,
            0x304 => Self::DestinationAlpha,
            0x305 => Self::OneMinusDestinationAlpha,
            0x306 => Self::DestinationColour,
            0x307 => Self::OneMinusDestinationColour,
            0x308 => Self::SourceAlphaSaturate,
            0x8001 => Self::ConstantColour,
            0x8002 => Self::OneMinusConstantColour,
            0x8003 => Self::ConstantAlpha,
            0x8004 => Self::OneMinusConstantAlpha,
            _ => return None,
        })
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum BlendEquation {
    Add,
    Min,
    Max,
    Subtract,
    ReverseSubtract,
}

impl BlendEquation {
    pub fn from_code(code: u32) -> Option<Self> {
        Some(match code {
            0x8006 => Self::Add,
            0x8007 => Self::Min,
            0x8008 => Self::Max,
            0x800a => Self::Subtract,
            0x800b => Self::ReverseSubtract,
            _ => return None,
        })
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum StencilOperation {
    Zero,
    Keep,
    Replace,
    Increment,
    Decrement,
    Invert,
    IncrementWrap,
    DecrementWrap,
}

impl StencilOperation {
    pub fn from_code(code: u32) -> Option<Self> {
        Some(match code {
            0 => Self::Zero,
            0x1e00 => Self::Keep,
            0x1e01 => Self::Replace,
            0x1e02 => Self::Increment,
            0x1e03 => Self::Decrement,
            0x150a => Self::Invert,
            0x8507 => Self::IncrementWrap,
            0x8508 => Self::DecrementWrap,
            _ => return None,
        })
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CullFace {
    Front,
    Back,
    FrontAndBack,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum FrontFace {
    Clockwise,
    CounterClockwise,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct BlendFactors {
    pub source_colour: BlendFactor,
    pub source_alpha: BlendFactor,
    pub destination_colour: BlendFactor,
    pub destination_alpha: BlendFactor,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct BlendEquations {
    pub colour: BlendEquation,
    pub alpha: BlendEquation,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct StencilFace {
    pub write_mask: u32,
    pub comparison: Comparison,
    pub reference: u32,
    pub read_mask: u32,
    pub fail: StencilOperation,
    pub depth_fail: StencilOperation,
    pub pass: StencilOperation,
}

/// Decoded fixed-function state. `None` means unknown.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct PipelineState {
    pub srgb_output: Option<bool>,
    pub alpha_test: Option<bool>,
    pub alpha_comparison: Option<Comparison>,
    pub alpha_reference: Option<u32>,
    pub cull_enable: Option<bool>,
    pub cull_face: Option<CullFace>,
    pub front_face: Option<FrontFace>,
    pub blend_enable: Option<bool>,
    pub blend_factors: Option<BlendFactors>,
    pub blend_equations: Option<BlendEquations>,
    /// Four 4-bit masks (bit 0 red, 1 green, 2 blue, 3 alpha) for targets 0..3.
    pub colour_masks: Option<[u8; 4]>,
    pub depth_test: Option<bool>,
    pub depth_write: Option<bool>,
    pub depth_comparison: Option<Comparison>,
    pub depth_bounds_enable: Option<bool>,
    /// Raw words of `0x384` and `0x388`.
    pub depth_bounds: Option<[u32; 2]>,
    pub polygon_offset_fill: Option<bool>,
    pub offset_factor: Option<u32>,
    pub offset_units: Option<u32>,
    pub stencil_enable: Option<bool>,
    pub two_sided_stencil: Option<bool>,
    pub front_stencil: Option<StencilFace>,
    pub back_stencil: Option<StencilFace>,
}

fn code<T>(
    registers: &KnownRegisters,
    method: u32,
    decode: impl Fn(u32) -> Option<T>,
) -> Result<Option<T>, PipelineError> {
    let Some(value) = registers.get(method) else {
        return Ok(None);
    };
    decode(value)
        .map(Some)
        .ok_or(PipelineError::Value { method, value })
}

fn split<T>(
    method: u32,
    value: u32,
    decode: impl Fn(u32) -> Option<T>,
) -> Result<(T, T), PipelineError> {
    let low = value & 0xffff;
    let high = value >> 16;
    let low_value = decode(low).ok_or(PipelineError::Value { method, value: low })?;
    let high_value = decode(high).ok_or(PipelineError::Value {
        method,
        value: high,
    })?;
    Ok((low_value, high_value))
}

fn stencil_face(
    registers: &KnownRegisters,
    base: u32,
) -> Result<Option<StencilFace>, PipelineError> {
    let words: [Option<u32>; 7] = std::array::from_fn(|i| registers.get(base + 4 * i as u32));
    if words.iter().any(Option::is_none) {
        return Ok(None);
    }
    let words = words.map(Option::unwrap);
    let operation = |index: usize| {
        StencilOperation::from_code(words[index]).ok_or(PipelineError::Value {
            method: base + 4 * index as u32,
            value: words[index],
        })
    };
    Ok(Some(StencilFace {
        write_mask: words[0],
        comparison: Comparison::from_code(words[1]).ok_or(PipelineError::Value {
            method: base + 4,
            value: words[1],
        })?,
        reference: words[2],
        read_mask: words[3],
        fail: operation(4)?,
        depth_fail: operation(5)?,
        pass: operation(6)?,
    }))
}

fn decode_pipeline(registers: &KnownRegisters) -> Result<PipelineState, PipelineError> {
    let flag = |method| registers.get(method).map(|value| value != 0);
    let raw = |method| registers.get(method);

    let blend_factors = match (raw(0x314), raw(0x318)) {
        (Some(source), Some(destination)) => {
            let (source_colour, source_alpha) = split(0x314, source, BlendFactor::from_code)?;
            let (destination_colour, destination_alpha) =
                split(0x318, destination, BlendFactor::from_code)?;
            Some(BlendFactors {
                source_colour,
                source_alpha,
                destination_colour,
                destination_alpha,
            })
        }
        _ => None,
    };
    let blend_equations = match raw(0x320) {
        Some(value) => {
            let (colour, alpha) = split(0x320, value, BlendEquation::from_code)?;
            Some(BlendEquations { colour, alpha })
        }
        None => None,
    };
    let colour_masks = match (raw(0x324), raw(0x370)) {
        (Some(target0), Some(others)) => {
            let bit =
                |value: u32, shift: u32, out: u8| if value >> shift & 1 != 0 { out } else { 0 };
            let mut masks = [0u8; 4];
            masks[0] =
                bit(target0, 16, 1) | bit(target0, 8, 2) | bit(target0, 0, 4) | bit(target0, 24, 8);
            for (target, mask) in masks.iter_mut().enumerate().skip(1) {
                let nibble = (others >> (4 * target as u32)) & 0xf;
                *mask =
                    bit(nibble, 1, 1) | bit(nibble, 2, 2) | bit(nibble, 3, 4) | bit(nibble, 0, 8);
            }
            Some(masks)
        }
        _ => None,
    };
    let depth_bounds = match (raw(0x384), raw(0x388)) {
        (Some(near), Some(far)) => Some([near, far]),
        _ => None,
    };
    Ok(PipelineState {
        srgb_output: flag(0x1fec),
        alpha_test: flag(0x304),
        alpha_comparison: code(registers, 0x308, Comparison::from_code)?,
        alpha_reference: raw(0x30c),
        cull_enable: flag(0x183c),
        cull_face: code(registers, 0x1830, |value| match value {
            0x404 => Some(CullFace::Front),
            0x405 => Some(CullFace::Back),
            0x408 => Some(CullFace::FrontAndBack),
            _ => None,
        })?,
        front_face: code(registers, 0x1834, |value| match value {
            0x900 => Some(FrontFace::Clockwise),
            0x901 => Some(FrontFace::CounterClockwise),
            _ => None,
        })?,
        blend_enable: flag(0x310),
        blend_factors,
        blend_equations,
        colour_masks,
        depth_test: flag(0xa74),
        depth_write: flag(0xa70),
        depth_comparison: code(registers, 0xa6c, Comparison::from_code)?,
        depth_bounds_enable: flag(0x380),
        depth_bounds,
        polygon_offset_fill: flag(0xa68),
        offset_factor: raw(0xa78),
        offset_units: raw(0xa7c),
        stencil_enable: flag(0x328),
        two_sided_stencil: flag(0x348),
        front_stencil: stencil_face(registers, 0x32c)?,
        back_stencil: stencil_face(registers, 0x34c)?,
    })
}

/// The fields of a Main pass's 160-byte setup record used by the setup banks.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct MainSetupRecord {
    pub enabled_group_mask: u16,
    pub lists_enabled: bool,
    pub depth_control: u8,
    /// Initial setup-bank references 0..2.
    pub initial: [u32; 3],
    pub group_thirteen: u32,
    pub group_fifteen: [u32; 2],
    pub auxiliary: u32,
    pub restore: u32,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum RecordError {
    /// The record copy was not exactly 160 bytes.
    Bounds { length: usize },
}

impl MainSetupRecord {
    pub const SIZE: usize = 160;

    pub fn decode(bytes: &[u8]) -> Result<Self, RecordError> {
        if bytes.len() != Self::SIZE {
            return Err(RecordError::Bounds {
                length: bytes.len(),
            });
        }
        Ok(Self {
            enabled_group_mask: be_u16(bytes, 0x48),
            lists_enabled: bytes[0x4c] != 0,
            depth_control: bytes[0x4d],
            initial: [
                be_u32(bytes, 0x50),
                be_u32(bytes, 0x54),
                be_u32(bytes, 0x58),
            ],
            group_thirteen: be_u32(bytes, 0x5c),
            group_fifteen: [be_u32(bytes, 0x60), be_u32(bytes, 0x64)],
            auxiliary: be_u32(bytes, 0x68),
            restore: be_u32(bytes, 0x6c),
        })
    }
}

/// The eight independent bank results of one Main pass.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct RetainedMainSetupBanks {
    pub initial: [Result<OwnedSetupBank, BankError>; 3],
    pub group_thirteen: Result<OwnedSetupBank, BankError>,
    pub group_fifteen: [Result<OwnedSetupBank, BankError>; 2],
    pub auxiliary: Result<OwnedSetupBank, BankError>,
    pub restore: Result<OwnedSetupBank, BankError>,
}

/// Copies every bank named by a Main pass. Returns `None` when lists are disabled.
pub fn copy_main_setup_banks<M: GuestMemory + ?Sized>(
    memory: &M,
    record: &MainSetupRecord,
) -> Option<RetainedMainSetupBanks> {
    if !record.lists_enabled {
        return None;
    }
    let copy = |target| {
        copy_setup_bank(
            memory,
            target,
            MAIN_BANK_WORD_BUDGET,
            MAIN_BANK_BRANCH_BUDGET,
        )
    };
    Some(RetainedMainSetupBanks {
        initial: record.initial.map(copy),
        group_thirteen: copy(record.group_thirteen),
        group_fifteen: record.group_fifteen.map(copy),
        auxiliary: copy(record.auxiliary),
        restore: copy(record.restore),
    })
}

/// The method list of an inherited register snapshot, in order.
pub fn inherited_snapshot_methods() -> Vec<u32> {
    let mut methods = vec![
        0x220, 0x304, 0x308, 0x30c, 0x310, 0x314, 0x318, 0x31c, 0x320, 0x324, 0x328, 0x348, 0x370,
        0x380, 0x384, 0x388, 0xa68, 0xa6c, 0xa70, 0xa74, 0xa78, 0xa7c, 0x1830, 0x1834, 0x183c,
        0x1fec, 0x1738, 0x173c,
    ];
    methods.extend((0..7).map(|i| 0x32c + 4 * i));
    methods.extend((0..7).map(|i| 0x34c + 4 * i));
    for unit in 0..16 {
        methods.extend((0..8).map(|i| 0x1a00 + 32 * unit + 4 * i));
        methods.extend([0x1840 + 4 * unit, 0xb00 + 4 * unit, 0x1d00 + 4 * unit]);
    }
    methods
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SnapshotError {
    Shape,
    State(PipelineError),
}

/// State inherited from before Main.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct InheritedMainState {
    pub pipeline: PipelineState,
    pub texture_units: [TextureUnitRecord; 16],
    pub blend_constant: u32,
    pub colour_target: u32,
    pub vertex_base_offset: u32,
    pub vertex_base_index: u32,
    /// The registers as decoded, for consumers that track raw knowledge.
    pub registers: KnownRegisters,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DirectGeometryError {
    DirectVertexBases { offset: u32, index: u32 },
}

impl InheritedMainState {
    /// Prepared direct geometry requires both vertex bases to be zero.
    pub fn require_direct_vertex_bases(&self) -> Result<(), DirectGeometryError> {
        if self.vertex_base_offset != 0 || self.vertex_base_index != 0 {
            return Err(DirectGeometryError::DirectVertexBases {
                offset: self.vertex_base_offset,
                index: self.vertex_base_index,
            });
        }
        Ok(())
    }
}

/// The enable whose exact value 0 lets an invalid code in `method` be tolerated.
fn controlling_enable(registers: &KnownRegisters, method: u32) -> Option<u32> {
    match method {
        0x308 => Some(0x304),
        0x314 | 0x318 | 0x320 => Some(0x310),
        0xa6c => Some(0xa74),
        0x1830 => Some(0x183c),
        0x32c..=0x344 => Some(0x328),
        0x34c..=0x364 => {
            if registers.get(0x328) == Some(0) {
                Some(0x328)
            } else {
                Some(0x348)
            }
        }
        _ => None,
    }
}

/// Decodes a register snapshot taken just before Main.
pub fn decode_inherited_snapshot(
    pairs: &[(u32, u32)],
) -> Result<InheritedMainState, SnapshotError> {
    let expected = inherited_snapshot_methods();
    if pairs.len() != expected.len()
        || pairs
            .iter()
            .zip(&expected)
            .any(|((method, _), expected)| method != expected)
    {
        return Err(SnapshotError::Shape);
    }
    let mut registers = KnownRegisters::new();
    for &(method, value) in pairs {
        let value = match method {
            0x304 | 0x310 | 0x328 | 0x348 | 0x380 | 0xa68 | 0xa70 | 0xa74 | 0x183c | 0x1fec => {
                value & 1
            }
            _ => value,
        };
        registers.set(method, value);
    }
    let pipeline = loop {
        match registers.pipeline() {
            Ok(pipeline) => break pipeline,
            Err(PipelineError::Value { method, value }) => {
                let tolerated = controlling_enable(&registers, method)
                    .is_some_and(|enable| registers.get(enable) == Some(0));
                if !tolerated {
                    return Err(SnapshotError::State(PipelineError::Value { method, value }));
                }
                registers.forget(method);
            }
        }
    };
    let texture_units = std::array::from_fn(|unit| {
        registers
            .texture_unit(unit as u32)
            .expect("unit index is in range")
    });
    let value = |method| registers.get(method).expect("snapshot register is known");
    Ok(InheritedMainState {
        pipeline,
        texture_units,
        blend_constant: value(0x31c),
        colour_target: value(0x220),
        vertex_base_offset: value(0x1738),
        vertex_base_index: value(0x173c),
        registers,
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::guest::SparseMemory;
    use crate::testutil::Rng;

    fn memory(words: &[(u32, u32)]) -> SparseMemory {
        let mut memory = SparseMemory::new();
        for &(address, word) in words {
            memory.put_u32(address, word);
        }
        memory
    }

    fn write(offset: u32, method: u32, value: u32) -> (u32, u32, u32) {
        (offset, method, value)
    }

    fn triples(bank: &OwnedSetupBank) -> Vec<(u32, u32, u32)> {
        bank.methods
            .iter()
            .map(|m| (m.offset, m.method, m.value))
            .collect()
    }

    /// A bank built directly from method writes, one packet per write unless grouped.
    fn bank(writes: &[(u32, u32, u32)]) -> OwnedSetupBank {
        OwnedSetupBank {
            target: 0x1000,
            methods: writes
                .iter()
                .enumerate()
                .map(|(index, &(subchannel, method, value))| MethodWrite {
                    offset: 0x1000 + 4 * index as u32,
                    subchannel,
                    method,
                    value,
                    packet: index as u32,
                })
                .collect(),
            reads: Vec::new(),
            status: PreparationStatus::PreparedByRetainedGameCode,
        }
    }

    fn vector_one_memory() -> SparseMemory {
        memory(&[
            (0x1000, 0x0008_0a70),
            (0x1004, 0),
            (0x1008, 1),
            (0x100c, 0x0004_1830),
            (0x1010, 0x405),
            (0x1014, 0x0004_1d94),
            (0x1018, 0xf1),
            (0x101c, 0x0004_1808),
            (0x1020, 5),
            (0x1024, 0x0004_1814),
            (0x1028, 0x0200_0000),
            (0x102c, 0x0004_1808),
            (0x1030, 0),
            (0x1034, 0x0002_0000),
        ])
    }

    #[test]
    fn vector_1_ordered_state_and_obligations() {
        let memory = vector_one_memory();
        let bank = copy_setup_bank(&memory, 0x1000, 14, 1).unwrap();
        assert_eq!(
            triples(&bank),
            vec![
                write(0x1004, 0xa70, 0),
                write(0x1008, 0xa74, 1),
                write(0x1010, 0x1830, 0x405),
                write(0x1018, 0x1d94, 0xf1),
                write(0x1020, 0x1808, 5),
                write(0x1028, 0x1814, 0x0200_0000),
                write(0x1030, 0x1808, 0),
            ]
        );
        assert_eq!(bank.reads.len(), 13);
        assert_eq!(bank.status, PreparationStatus::PreparedByRetainedGameCode);
        assert_eq!(
            copy_setup_bank(&memory, 0x1000, 13, 1),
            Err(BankError::Budget)
        );
        assert_eq!(
            copy_setup_bank(&memory, 0x1000, 14, 0),
            Err(BankError::Budget)
        );

        let mut known = KnownRegisters::new();
        let obligations = known.apply(&bank).unwrap();
        let kinds: Vec<_> = obligations.iter().map(|o| o.kind).collect();
        assert_eq!(
            kinds,
            vec![
                ObligationKind::Clear,
                ObligationKind::PrimitiveBegin,
                ObligationKind::Draw,
                ObligationKind::PrimitiveEnd
            ]
        );
        let state = known.pipeline().unwrap();
        assert_eq!(state.depth_test, Some(true));
        assert_eq!(state.depth_write, Some(false));
        assert_eq!(state.cull_face, Some(CullFace::Back));
        let expected = PipelineState {
            depth_test: Some(true),
            depth_write: Some(false),
            cull_face: Some(CullFace::Back),
            ..Default::default()
        };
        assert_eq!(state, expected);
    }

    #[test]
    fn vector_2_call_nonincrementing_and_return() {
        let memory = memory(&[
            (0x2000, 0x0000_3002),
            (0x2004, 0x4008_0a70),
            (0x2008, 1),
            (0x200c, 0),
            (0x2010, 0x0002_0000),
            (0x3000, 0x0004_0a74),
            (0x3004, 1),
            (0x3008, 0x0002_0000),
        ]);
        let bank = copy_setup_bank(&memory, 0x2000, 100, 3).unwrap();
        assert_eq!(
            triples(&bank),
            vec![
                write(0x3004, 0xa74, 1),
                write(0x2008, 0xa70, 1),
                write(0x200c, 0xa70, 0)
            ]
        );
        assert_eq!(
            copy_setup_bank(&memory, 0x2000, 100, 2),
            Err(BankError::Budget)
        );
        let mut known = KnownRegisters::new();
        known.apply(&bank).unwrap();
        assert_eq!(known.pipeline().unwrap().depth_write, Some(false));
    }

    #[test]
    fn vector_3_errors() {
        let empty = SparseMemory::new();
        assert_eq!(
            copy_setup_bank(&empty, 0, 10, 10),
            Err(BankError::Target(0))
        );
        assert_eq!(
            copy_setup_bank(&empty, 0x1002, 10, 10),
            Err(BankError::Target(0x1002))
        );
        assert_eq!(
            copy_setup_bank(&empty, 0x1000_0000, 10, 10),
            Err(BankError::Target(0x1000_0000))
        );
        let cycle = memory(&[(0x4000, 0x0000_4001)]);
        assert_eq!(
            copy_setup_bank(&cycle, 0x4000, 100, 100),
            Err(BankError::ControlCycle(0x4000))
        );
        let nested = memory(&[(0x2000, 0x0000_3002), (0x3000, 0x0000_4002)]);
        assert_eq!(
            copy_setup_bank(&nested, 0x2000, 100, 100),
            Err(BankError::NestedCall)
        );
        let jump = memory(&[(0x5000, 0x2000_6000), (0x6000, 0x0002_0000)]);
        let bank = copy_setup_bank(&jump, 0x5000, 100, 100).unwrap();
        assert!(bank.methods.is_empty());
        assert_eq!(
            copy_setup_bank(&memory(&[(0x5000, 3)]), 0x5000, 100, 100),
            Err(BankError::Header(3))
        );
        assert_eq!(
            copy_setup_bank(&memory(&[(0x5000, 0x6000_0000)]), 0x5000, 100, 100),
            Err(BankError::Header(0x6000_0000))
        );
        assert_eq!(
            copy_setup_bank(&memory(&[(0x5000, 0x0008_1ffc)]), 0x5000, 100, 100),
            Err(BankError::MethodExtent)
        );
        assert_eq!(
            copy_setup_bank(&memory(&[(0x5000, 0)]), 0x5000, 1, 100),
            Err(BankError::Budget)
        );
    }

    #[test]
    fn reader_failures_and_short_reads() {
        // The first request fails: one request, no retry.
        struct Counting<'a>(&'a SparseMemory, std::cell::Cell<u32>);
        impl GuestMemory for Counting<'_> {
            fn read(&self, address: u32, length: u32) -> Result<Vec<u8>, SourceError> {
                self.1.set(self.1.get() + 1);
                self.0.read(address, length)
            }
        }
        let empty = SparseMemory::new();
        let counting = Counting(&empty, std::cell::Cell::new(0));
        assert!(matches!(
            copy_setup_bank(&counting, 0x1000, 10, 10),
            Err(BankError::Source(_))
        ));
        assert_eq!(counting.1.get(), 1);
        // A payload that runs off the mapped image is a short read.
        let short = memory(&[(0x1000, 0x0008_0a70), (0x1004, 0)]);
        assert_eq!(
            copy_setup_bank(&short, 0x1000, 10, 10),
            Err(BankError::Extent)
        );
        // A read whose end passes the IO limit.
        let edge = memory(&[(0x0fff_fffc, 0x0008_0a70)]);
        assert_eq!(
            copy_setup_bank(&edge, 0x0fff_fffc, 10, 10),
            Err(BankError::Target(0x1000_0000))
        );
        // A jump to an invalid target.
        let bad_jump = memory(&[(0x1000, 0x0000_0001 | 0x1002_0000)]);
        assert!(matches!(
            copy_setup_bank(&bad_jump, 0x1000, 10, 10),
            Err(BankError::Target(_))
        ));
    }

    #[test]
    fn subroutine_called_twice_is_not_a_cycle() {
        let memory = memory(&[
            (0x1000, 0x0000_3002),
            (0x1004, 0x0000_3002),
            (0x1008, 0x0002_0000),
            (0x3000, 0x0004_0a74),
            (0x3004, 1),
            (0x3008, 0x0002_0000),
        ]);
        let bank = copy_setup_bank(&memory, 0x1000, 100, 100).unwrap();
        assert_eq!(bank.methods.len(), 2);
    }

    #[test]
    fn vector_4_vertex_constants() {
        let mut writes = vec![(0, 0x1efc, 510)];
        writes.extend((0..8).map(|i| (0, 0x1f00 + 4 * i, i + 1)));
        let mut constants = bank(&writes);
        // The eight data writes form one packet.
        for write in constants.methods.iter_mut().skip(1) {
            write.packet = 1;
        }
        let mut known = KnownRegisters::new();
        known.apply(&constants).unwrap();
        let rows = known.vertex_constant_bank();
        assert_eq!(rows.get(&510), Some(&[1, 2, 3, 4]));
        assert_eq!(rows.get(&511), Some(&[5, 6, 7, 8]));
        assert_eq!(rows.get(&0), None);
        assert_eq!(known.constant(0, 0), None);

        constants.methods[0].value = 511;
        let mut fresh = KnownRegisters::new();
        assert_eq!(
            fresh.apply(&constants),
            Err(ApplyError::Value {
                method: 0x1efc,
                value: 511
            })
        );
        assert_eq!(fresh, KnownRegisters::new());

        let missing = bank(&[(0, 0x1f00, 1)]);
        assert_eq!(
            KnownRegisters::new().apply(&missing),
            Err(ApplyError::MissingConstantLoad)
        );
    }

    #[test]
    fn second_constant_packet_without_load_write_is_refused() {
        let writes = [(0, 0x1efc, 0), (0, 0x1f00, 1), (0, 0x1f00, 2)];
        let mut known = KnownRegisters::new();
        assert_eq!(
            known.apply(&bank(&writes)),
            Err(ApplyError::UnconfirmedConstantLoad { packet: 2 })
        );
        let writes = [
            (0, 0x1efc, 0),
            (0, 0x1f00, 1),
            (0, 0x1efc, 4),
            (0, 0x1f00, 2),
        ];
        known.apply(&bank(&writes)).unwrap();
        assert_eq!(known.constant(0, 0), Some(1));
        assert_eq!(known.constant(4, 0), Some(2));
    }

    #[test]
    fn vector_5_colour_masks() {
        let mut known = KnownRegisters::new();
        known
            .apply(&bank(&[(0, 0x324, 0x0100_0100), (0, 0x370, 0x0000_1230)]))
            .unwrap();
        assert_eq!(known.pipeline().unwrap().colour_masks, Some([10, 9, 1, 8]));
        let mut partial = KnownRegisters::new();
        partial.apply(&bank(&[(0, 0x324, 0x0100_0100)])).unwrap();
        assert_eq!(partial.pipeline().unwrap().colour_masks, None);
    }

    #[test]
    fn vector_6_blend() {
        let mut known = KnownRegisters::new();
        known
            .apply(&bank(&[
                (0, 0x314, 0x0303_0302),
                (0, 0x318, 0x0001_0001),
                (0, 0x320, 0x800b_8006),
            ]))
            .unwrap();
        let state = known.pipeline().unwrap();
        assert_eq!(
            state.blend_factors,
            Some(BlendFactors {
                source_colour: BlendFactor::SourceAlpha,
                source_alpha: BlendFactor::OneMinusSourceAlpha,
                destination_colour: BlendFactor::One,
                destination_alpha: BlendFactor::One,
            })
        );
        assert_eq!(
            state.blend_equations,
            Some(BlendEquations {
                colour: BlendEquation::Add,
                alpha: BlendEquation::ReverseSubtract
            })
        );
        let before = known.clone();
        assert_eq!(
            known.apply(&bank(&[(0, 0x320, 0x8009_8006)])),
            Err(ApplyError::Value {
                method: 0x320,
                value: 0x8009
            })
        );
        assert_eq!(known, before);
    }

    #[test]
    fn vector_7_unqualified_writes() {
        let mut known = KnownRegisters::new();
        known.apply(&bank(&[(0, 0xa74, 1)])).unwrap();
        let before = known.clone();
        assert_eq!(
            known.apply(&bank(&[(0, 0xa70, 1), (1, 0xa70, 1)])),
            Err(ApplyError::UnqualifiedMethod {
                subchannel: 1,
                method: 0xa70
            })
        );
        assert_eq!(
            known.apply(&bank(&[(0, 0xa70, 1), (0, 0x1234, 1)])),
            Err(ApplyError::UnqualifiedMethod {
                subchannel: 0,
                method: 0x1234
            })
        );
        assert_eq!(known, before);
    }

    #[test]
    fn vector_8_border_survives_compact_updates() {
        let mut writes: Vec<_> = (0..7).map(|i| (0, 0x1a00 + 4 * i, 0x100 + i)).collect();
        writes.push((0, 0x1a1c, 0x1122_3344));
        writes.push((0, 0x1840, 8));
        writes.extend((0..7).map(|i| (0, 0x1a00 + 4 * i, 0x200 + i)));
        let mut known = KnownRegisters::new();
        let obligations = known.apply(&bank(&writes)).unwrap();
        assert!(
            obligations
                .iter()
                .all(|o| o.kind == ObligationKind::TextureBinding)
        );
        let unit = known.texture_unit(0).unwrap();
        assert_eq!(
            unit.compact,
            Some([0x200, 0x201, 0x202, 0x203, 0x204, 0x205, 0x206, 8])
        );
        assert_eq!(unit.border, Some(0x1122_3344));
        known.invalidate();
        let unit = known.texture_unit(0).unwrap();
        assert_eq!(unit.border, None);
        assert_eq!(unit.compact, None);
        assert_eq!(
            known.texture_unit(16),
            Err(PipelineError::Value {
                method: 0x1a00,
                value: 16
            })
        );
    }

    fn snapshot(overrides: &[(u32, u32)]) -> Vec<(u32, u32)> {
        inherited_snapshot_methods()
            .into_iter()
            .map(|method| {
                let value = overrides
                    .iter()
                    .find(|(m, _)| *m == method)
                    .map_or(0, |(_, v)| *v);
                (method, value)
            })
            .collect()
    }

    #[test]
    fn vector_9_inherited_snapshot() {
        let state =
            decode_inherited_snapshot(&snapshot(&[(0x1834, 0x901), (0x308, 0x999)])).unwrap();
        let p = state.pipeline;
        assert_eq!(p.front_face, Some(FrontFace::CounterClockwise));
        assert_eq!(p.alpha_comparison, None);
        assert_eq!(p.blend_equations, None);
        assert_eq!(p.depth_comparison, None);
        assert_eq!(p.cull_face, None);
        assert_eq!(p.front_stencil, None);
        assert_eq!(p.back_stencil, None);
        assert_eq!(
            p.blend_factors,
            Some(BlendFactors {
                source_colour: BlendFactor::Zero,
                source_alpha: BlendFactor::Zero,
                destination_colour: BlendFactor::Zero,
                destination_alpha: BlendFactor::Zero,
            })
        );
        for unit in &state.texture_units {
            assert_eq!(unit.compact, Some([0; 8]));
            assert!(matches!(
                crate::texture::decode(unit),
                Ok(crate::texture::DecodedUnit::Disabled(_))
            ));
        }
        assert_eq!(state.require_direct_vertex_bases(), Ok(()));

        assert_eq!(
            decode_inherited_snapshot(&snapshot(&[(0x1834, 0x901), (0x308, 0x999), (0x304, 3)])),
            Err(SnapshotError::State(PipelineError::Value {
                method: 0x308,
                value: 0x999
            }))
        );
        assert_eq!(
            decode_inherited_snapshot(&snapshot(&[(0x308, 0x999)])),
            Err(SnapshotError::State(PipelineError::Value {
                method: 0x1834,
                value: 0
            }))
        );
    }

    #[test]
    fn snapshot_raw_values_and_shape() {
        let state = decode_inherited_snapshot(&snapshot(&[
            (0x1834, 0x900),
            (0x31c, 0xdead_beef),
            (0x220, 1),
            (0x1738, 4),
            (0x173c, 2),
        ]))
        .unwrap();
        assert_eq!(state.blend_constant, 0xdead_beef);
        assert_eq!(state.colour_target, 1);
        assert_eq!(
            state.require_direct_vertex_bases(),
            Err(DirectGeometryError::DirectVertexBases {
                offset: 4,
                index: 2
            })
        );
        let mut short = snapshot(&[(0x1834, 0x900)]);
        short.pop();
        assert_eq!(decode_inherited_snapshot(&short), Err(SnapshotError::Shape));
        let mut swapped = snapshot(&[(0x1834, 0x900)]);
        swapped.swap(0, 1);
        assert_eq!(
            decode_inherited_snapshot(&swapped),
            Err(SnapshotError::Shape)
        );
    }

    #[test]
    fn back_stencil_controlling_enable() {
        // Stencil enabled, two-sided off: an invalid back comparison is tolerated through 0x348.
        let mut overrides = vec![(0x1834, 0x900), (0x328, 1)];
        overrides.extend([
            (0x330, 0x207),
            (0x33c, 0x1e00),
            (0x340, 0x1e00),
            (0x344, 0x1e00),
        ]);
        let state = decode_inherited_snapshot(&snapshot(&overrides)).unwrap();
        assert!(state.pipeline.front_stencil.is_some());
        assert_eq!(state.pipeline.back_stencil, None);
        // Two-sided on: the invalid back comparison fails.
        overrides.push((0x348, 1));
        assert_eq!(
            decode_inherited_snapshot(&snapshot(&overrides)),
            Err(SnapshotError::State(PipelineError::Value {
                method: 0x350,
                value: 0
            }))
        );
    }

    #[test]
    fn main_setup_record_and_bank_slots() {
        let mut bytes = vec![0u8; 160];
        bytes[0x48..0x4a].copy_from_slice(&0x1234u16.to_be_bytes());
        bytes[0x4c] = 1;
        bytes[0x4d] = 7;
        bytes[0x54..0x58].copy_from_slice(&0x1000u32.to_be_bytes());
        let record = MainSetupRecord::decode(&bytes).unwrap();
        assert_eq!(record.enabled_group_mask, 0x1234);
        assert_eq!(record.depth_control, 7);
        assert_eq!(record.initial, [0, 0x1000, 0]);
        assert_eq!(
            MainSetupRecord::decode(&bytes[..159]),
            Err(RecordError::Bounds { length: 159 })
        );
        let banks = copy_main_setup_banks(&vector_one_memory(), &record).unwrap();
        assert_eq!(banks.initial[0], Err(BankError::Target(0)));
        assert_eq!(banks.initial[1].as_ref().unwrap().methods.len(), 7);
        assert_eq!(banks.restore, Err(BankError::Target(0)));
        let disabled = MainSetupRecord {
            lists_enabled: false,
            ..record
        };
        assert!(copy_main_setup_banks(&vector_one_memory(), &disabled).is_none());
    }

    #[test]
    fn classification_examples() {
        use ObligationKind::*;
        let kind = |method, value| match classify(method, value) {
            Some(MethodClass::Obligation(kind)) => Some(kind),
            _ => None,
        };
        assert_eq!(kind(0x31c, 0), Some(UntranslatedState));
        assert_eq!(kind(0x318, 0), None);
        assert_eq!(classify(0x318, 0), Some(MethodClass::StateOnly));
        assert_eq!(kind(0x208, 0), Some(Attachment));
        assert_eq!(kind(0x1d60, 0), Some(Program));
        assert_eq!(kind(0x1fd8, 0), Some(Synchronisation));
        assert_eq!(kind(0x1e00, 0), Some(VertexInput));
        assert_eq!(classify(0x1f80, 0), None);
    }

    /// Property: a failed apply never changes knowledge, and a successful apply always leaves a
    /// decodable pipeline.
    #[test]
    fn property_apply_is_atomic() {
        let methods = [
            0x304, 0x308, 0x310, 0x314, 0x318, 0x320, 0x1830, 0x1834, 0xa6c, 0x330, 0x1d94, 0x1234,
            0x1efc, 0x1f00, 0x1a00,
        ];
        let values = [
            0,
            1,
            0x201,
            0x405,
            0x901,
            0x8006_8006,
            0x0302_0302,
            0x999,
            600,
        ];
        let mut rng = Rng::new(7);
        let mut known = KnownRegisters::new();
        for _ in 0..5_000 {
            let count = 1 + rng.below(4);
            let writes: Vec<_> = (0..count)
                .map(|_| {
                    (
                        u32::from(rng.below(20) == 0),
                        methods[rng.below(methods.len() as u32) as usize],
                        values[rng.below(values.len() as u32) as usize],
                    )
                })
                .collect();
            let before = known.clone();
            match known.apply(&bank(&writes)) {
                Ok(_) => assert!(known.pipeline().is_ok()),
                Err(_) => assert_eq!(known, before),
            }
        }
    }
}
