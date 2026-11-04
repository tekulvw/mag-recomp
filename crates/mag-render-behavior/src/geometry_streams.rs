//! Partition streams and attribute codecs (`geometry-streams.md`).
//!
//! Reading is split from decoding. [`read_partition_streams`] turns a partition record into 24
//! slot reads, applying the slot replacements and the large-stream rule. [`decode_partition`]
//! turns those bytes into the configuration, the index list, the positions and the primary and
//! secondary attribute lists. [`PartitionGeometry::lookup`] applies the semantic lookup rule.
//!
//! The descriptor codecs are the partition's own source formats. They are not the RSX
//! vertex-fetch formats with the same numbers, so nothing here uses a GPU fetch decoder.

use crate::group_zero::rebased_half;
use crate::guest::{GuestMemory, SourceError, be_u16, be_u32};
use crate::normals::{NormalError, decode_packed_normal};
use crate::vertex_inputs::{AttributeList, SemanticBank};

/// Size of a partition record.
pub const PARTITION_BYTES: usize = 544;
/// The control of the largest single transfer; see the large-stream rule.
pub const LARGE_CONTROL: u32 = 0x4000;
/// The largest length the large-stream rule may substitute.
pub const LARGE_CAP: u64 = 4 << 20;

/// Slot roles named by the specification.
pub mod slot {
    pub const OUTPUT_LAYOUT: usize = 0;
    pub const INDICES: usize = 1;
    pub const PALETTE_0: usize = 3;
    pub const PALETTE_1: usize = 4;
    pub const INFLUENCES: usize = 5;
    pub const VERTICES: usize = 7;
    pub const SECONDARY: usize = 10;
    pub const ENVIRONMENT: usize = 17;
    pub const CONFIGURATION: usize = 19;
    pub const PRIMARY_FORMAT: usize = 20;
    pub const SECONDARY_FORMAT: usize = 21;
}

/// Semantic identifiers used by this specification.
pub mod semantic {
    pub const UNCLASSIFIED: u8 = 0;
    pub const POSITION: u8 = 1;
    pub const NORMAL: u8 = 2;
    pub const TANGENT: u8 = 3;
    pub const SECONDARY_DIRECTION: u8 = 4;
    pub const WEIGHTS: u8 = 9;
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum StreamError {
    /// A control with any of bits 16–31 set.
    UnsupportedControl {
        slot: usize,
        control: u32,
    },
    /// `address + length` passes `2^32`.
    InvalidRange {
        slot: usize,
        address: u32,
        length: u32,
    },
    /// Bytes needed lie outside what was read, or an extent computation overflowed.
    SourceExtent(&'static str),
    UnsupportedPositionEncoding(u8),
    MissingPositionAttribute,
    UnsupportedEncoding(u8),
    UnclassifiedSemantic,
    SplitInputChunks,
    BuiltinSecondary(u8),
    PartitionCycle(u32),
    PartitionRange(u32),
    /// The partition chain or the read budget was exhausted.
    Budget,
    Read(SourceError),
    Normal(NormalError),
}

impl From<NormalError> for StreamError {
    fn from(error: NormalError) -> Self {
        Self::Normal(error)
    }
}

// --- Partition chain ---------------------------------------------------------------------------

/// One copied partition record.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct PartitionRecord {
    pub address: u32,
    pub bytes: [u8; PARTITION_BYTES],
}

impl PartitionRecord {
    fn u16(&self, at: usize) -> u16 {
        be_u16(&self.bytes, at)
    }
    fn u32(&self, at: usize) -> u32 {
        be_u32(&self.bytes, at)
    }
    /// Control word of slot `k`.
    pub fn control(&self, k: usize) -> u32 {
        self.u32(0x30 + 8 * k)
    }
    /// Address word of slot `k`.
    pub fn slot_address(&self, k: usize) -> u32 {
        self.u32(0x34 + 8 * k)
    }
    /// The next partition, 0 at the end of the chain.
    pub fn next(&self) -> u32 {
        self.u32(0xf0)
    }
    /// The three secondary chunk sizes at `+0x0A`, `+0x0C` and `+0x0E`.
    pub fn secondary_chunks(&self) -> [u16; 3] {
        [self.u16(0x0a), self.u16(0x0c), self.u16(0x0e)]
    }
    /// Palette segment offsets at `+0x14` and `+0x16`.
    pub fn palette_offsets(&self) -> [u16; 2] {
        [self.u16(0x14), self.u16(0x16)]
    }
    /// Instance batch maximum at `+0xFC`.
    pub fn batch_max(&self) -> u32 {
        self.u32(0xfc)
    }
}

/// The first partition of a mesh: `(u32 mesh+0x50 AND 0x7FFFFFFF) + G`, modulo `2^32`.
pub fn first_partition(mesh_word_0x50: u32, geometry_base: u32) -> u32 {
    (mesh_word_0x50 & 0x7fff_ffff).wrapping_add(geometry_base)
}

/// Follows the chain from `first` for at most `budget` records.
pub fn partition_chain<M: GuestMemory + ?Sized>(
    memory: &M,
    first: u32,
    budget: usize,
) -> Result<Vec<PartitionRecord>, StreamError> {
    let mut records: Vec<PartitionRecord> = Vec::new();
    let mut address = first;
    while address != 0 {
        if records.iter().any(|r| r.address == address) {
            return Err(StreamError::PartitionCycle(address));
        }
        if records.len() == budget {
            return Err(StreamError::Budget);
        }
        if address as u64 + PARTITION_BYTES as u64 > 1 << 32 {
            return Err(StreamError::PartitionRange(address));
        }
        let bytes = read_exact(memory, address, PARTITION_BYTES as u32)?;
        let record = PartitionRecord {
            address,
            bytes: bytes.try_into().expect("exact read"),
        };
        address = record.next();
        records.push(record);
    }
    Ok(records)
}

fn read_exact<M: GuestMemory + ?Sized>(
    memory: &M,
    address: u32,
    length: u32,
) -> Result<Vec<u8>, StreamError> {
    let bytes = memory.read(address, length).map_err(StreamError::Read)?;
    if bytes.len() != length as usize {
        return Err(StreamError::Read(SourceError {
            address,
            length,
            reason: format!("short read of {} bytes", bytes.len()),
        }));
    }
    Ok(bytes)
}

// --- Slot reads --------------------------------------------------------------------------------

/// Bases that belong to the consuming preparation.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct StreamBases {
    /// Skinning palette base (slots 3 and 4).
    pub palette_base: u32,
    /// Extra input stream base (slots 13–16, kept as identity).
    pub extra_stream: u32,
    /// View environment record (slot 17).
    pub environment: u32,
}

/// The planned read of one slot, before the large-stream rule.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct SlotPlan {
    pub address: u32,
    pub length: u32,
    /// The length is the slot's control word (so the large-stream rule can apply).
    pub from_control: bool,
}

/// One slot as read: its address and bytes (empty for a zero length).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Stream {
    pub address: u32,
    pub bytes: Vec<u8>,
}

/// Plans slot `k` of `partition`: length and address replacements and the control check.
///
/// The unsupported-control rule applies where the slot's length is its control word: slots 0–9,
/// 17 and 19–23. The control words of slots 10–16 and 18 are never checked or used.
pub fn plan_slot(
    partition: &PartitionRecord,
    bases: &StreamBases,
    k: usize,
) -> Result<SlotPlan, StreamError> {
    let control = partition.control(k);
    let address = partition.slot_address(k);
    let chunks = partition.secondary_chunks();
    let offsets = partition.palette_offsets();
    let (address, length, from_control) = match k {
        3 => (
            bases.palette_base.wrapping_add(offsets[0] as u32),
            control,
            true,
        ),
        4 => (
            bases.palette_base.wrapping_add(offsets[1] as u32),
            control,
            true,
        ),
        10..=12 => (address, chunks[k - 10] as u32, false),
        13..=16 => (bases.extra_stream, 0, false),
        17 => (bases.environment, control, true),
        18 => (0, 0, false),
        _ => (address, control, true),
    };
    if from_control && control >> 16 != 0 {
        return Err(StreamError::UnsupportedControl { slot: k, control });
    }
    if length != 0 && address as u64 + length as u64 > 1 << 32 {
        return Err(StreamError::InvalidRange {
            slot: k,
            address,
            length,
        });
    }
    Ok(SlotPlan {
        address,
        length,
        from_control,
    })
}

/// The large-stream rule: the length to read for a slot with this control and need.
pub fn large_stream_length(control: u32, needed: Option<u64>) -> u32 {
    match needed {
        Some(n) if control == LARGE_CONTROL && n > LARGE_CONTROL as u64 && n <= LARGE_CAP => {
            n as u32
        }
        _ => control,
    }
}

/// The 24 slots of one partition, each read or failed on its own.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct PartitionStreams {
    pub slots: Vec<Result<Stream, StreamError>>,
}

impl PartitionStreams {
    /// Slot `k`, or its error.
    pub fn slot(&self, k: usize) -> Result<&Stream, StreamError> {
        self.slots[k].as_ref().map_err(Clone::clone)
    }
}

/// Reads every slot of `partition` within `budget` bytes in total.
///
/// Slots 19 and 20 are read first, because the configuration and the primary format give the
/// counts the large-stream rule needs for slots 1 and 7.
pub fn read_partition_streams<M: GuestMemory + ?Sized>(
    memory: &M,
    partition: &PartitionRecord,
    bases: &StreamBases,
    budget: usize,
) -> PartitionStreams {
    let mut remaining = budget;
    let mut read = |k: usize, plan: SlotPlan, length: u32| -> Result<Stream, StreamError> {
        if length == 0 {
            return Ok(Stream {
                address: plan.address,
                bytes: Vec::new(),
            });
        }
        if plan.address as u64 + length as u64 > 1 << 32 {
            return Err(StreamError::InvalidRange {
                slot: k,
                address: plan.address,
                length,
            });
        }
        if length as usize > remaining {
            return Err(StreamError::Budget);
        }
        remaining -= length as usize;
        Ok(Stream {
            address: plan.address,
            bytes: read_exact(memory, plan.address, length)?,
        })
    };
    let mut slots: Vec<Option<Result<Stream, StreamError>>> = vec![None; 24];
    for k in [slot::CONFIGURATION, slot::PRIMARY_FORMAT] {
        slots[k] = Some(plan_slot(partition, bases, k).and_then(|p| read(k, p, p.length)));
    }
    let configuration = slots[slot::CONFIGURATION]
        .as_ref()
        .and_then(|s| s.as_ref().ok())
        .and_then(|s| Configuration::parse(&s.bytes).ok());
    let table = slots[slot::PRIMARY_FORMAT]
        .as_ref()
        .and_then(|s| s.as_ref().ok())
        .and_then(|s| DescriptorTable::parse(&s.bytes).ok());
    for k in 0..24 {
        if slots[k].is_some() {
            continue;
        }
        let result = plan_slot(partition, bases, k).and_then(|plan| {
            let needed = match (k, configuration.as_ref()) {
                (slot::INDICES, Some(c)) => Some(2 * c.index_count as u64),
                (slot::VERTICES, Some(c)) => match c.position_codec {
                    0 => Some(12 * c.vertex_count as u64),
                    6 => Some(6 * c.vertex_count as u64),
                    255 => table
                        .as_ref()
                        .map(|t| t.furthest_byte(plan.address, c.vertex_count as usize)),
                    _ => None,
                },
                _ => None,
            };
            let length = if plan.from_control {
                large_stream_length(plan.length, needed)
            } else {
                plan.length
            };
            read(k, plan, length)
        });
        slots[k] = Some(result);
    }
    PartitionStreams {
        slots: slots
            .into_iter()
            .map(|s| s.expect("every slot read"))
            .collect(),
    }
}

// --- Configuration and descriptor tables -------------------------------------------------------

/// The configuration stream (slot 19).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Configuration {
    /// The first 16 bytes.
    pub bytes: [u8; 16],
    /// Byte 2: 0, 6 or 255.
    pub position_codec: u8,
    /// Byte 3: 255 when the slot-21 format describes the secondary attributes.
    pub secondary_layout: u8,
    /// Byte 4: 0–3 built-in output layout, above 3 custom (slot 0).
    pub output_selector: u8,
    /// Bytes 8–9.
    pub vertex_count: u16,
    /// Bytes 10–11.
    pub index_count: u16,
}

impl Configuration {
    pub fn parse(bytes: &[u8]) -> Result<Self, StreamError> {
        let bytes: [u8; 16] = bytes
            .get(..16)
            .ok_or(StreamError::SourceExtent("configuration"))?
            .try_into()
            .expect("sixteen bytes");
        Ok(Self {
            bytes,
            position_codec: bytes[2],
            secondary_layout: bytes[3],
            output_selector: bytes[4],
            vertex_count: be_u16(&bytes, 8),
            index_count: be_u16(&bytes, 10),
        })
    }
}

/// One 8-byte declaration.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Declaration {
    pub offset: u8,
    pub codec: u8,
    pub components: u8,
    pub semantic: u8,
    pub element_size: u8,
    pub raw: [u8; 8],
}

/// A descriptor table (slots 20 and 21).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct DescriptorTable {
    pub stride: u8,
    pub declarations: Vec<Declaration>,
}

impl DescriptorTable {
    pub fn parse(bytes: &[u8]) -> Result<Self, StreamError> {
        let header = bytes
            .get(..8)
            .ok_or(StreamError::SourceExtent("descriptor header"))?;
        let count = header[0] as usize;
        let rows = bytes
            .get(8..8 + 8 * count)
            .ok_or(StreamError::SourceExtent("descriptor table"))?;
        let declarations = rows
            .as_chunks::<8>()
            .0
            .iter()
            .map(|r| Declaration {
                offset: r[0],
                codec: r[1],
                components: r[2],
                semantic: r[3],
                element_size: r[4],
                raw: *r,
            })
            .collect();
        Ok(Self {
            stride: header[1],
            declarations,
        })
    }

    /// The furthest byte (as a length from the stream address) any declaration reads over
    /// `vertices` vertices of a stream at `address`. Declarations that read nothing count 0.
    pub fn furthest_byte(&self, address: u32, vertices: usize) -> u64 {
        let mut furthest = 0u64;
        for d in &self.declarations {
            for i in 0..vertices {
                if let Some((offsets, count)) = byte_offsets(d, self.stride, address, i) {
                    for offset in &offsets[..count] {
                        furthest = furthest.max((offset + 1).max(0) as u64);
                    }
                }
            }
        }
        furthest
    }
}

// --- Codecs ------------------------------------------------------------------------------------

/// A rebased half as binary32 (exact).
pub fn rebased_half_f32(h: u16) -> f32 {
    rebased_half(h) as f32
}

fn lane_bytes(codec: u8) -> Option<usize> {
    Some(match codec {
        1 | 3 | 5 => 2,
        2 => 4,
        4 | 7 => 1,
        _ => return None,
    })
}

/// Byte count a declaration reads per vertex, or `None` for an unsupported codec. Codecs 6 and 9
/// always read their whole packed field, whatever the component count, so a codec-9 refusal
/// fails the declaration even when every lane it affects would be discarded.
fn bytes_read(d: &Declaration) -> Option<usize> {
    match d.codec {
        6 => Some(4),
        9 => Some(3),
        codec => {
            let lanes = if d.semantic == semantic::POSITION {
                (d.components as usize).min(4)
            } else {
                4
            };
            lane_bytes(codec).map(|size| size * lanes)
        }
    }
}

/// The offsets, relative to the stream address, of the bytes a declaration reads for vertex
/// `i`, in decode order (at most 16); `None` for an unsupported codec or an overflowing extent.
fn byte_offsets(d: &Declaration, stride: u8, address: u32, i: usize) -> Option<([i64; 16], usize)> {
    let count = bytes_read(d)?;
    let logical = (i as u64)
        .checked_mul(stride as u64)?
        .checked_add(d.offset as u64)?;
    let phase = logical % 16;
    let block = (address as u64 + logical) & !15;
    let start = block + phase;
    let base = address as i64;
    // Codec 2 lanes beyond the component count of a non-position semantic may wrap.
    let wraps =
        d.codec == 2 && d.semantic != semantic::POSITION && phase + d.element_size as u64 <= 16;
    let declared = 4 * d.components as usize;
    let mut offsets = [0i64; 16];
    for (j, offset) in offsets.iter_mut().enumerate().take(count) {
        let at = if wraps && j >= declared {
            block + (phase + j as u64) % 16
        } else {
            start + j as u64
        };
        *offset = at as i64 - base;
    }
    Some((offsets, count))
}

fn decode_lane(codec: u8, bytes: &[u8]) -> f32 {
    match codec {
        1 => {
            let v = i16::from_be_bytes([bytes[0], bytes[1]]) as f64;
            ((2.0 * v + 1.0) / 65535.0) as f32
        }
        2 => f32::from_bits(be_u32(bytes, 0)),
        3 => rebased_half_f32(be_u16(bytes, 0)),
        4 => (bytes[0] as f64 / 255.0) as f32,
        5 => i16::from_be_bytes([bytes[0], bytes[1]]) as f32,
        7 => bytes[0] as f32,
        _ => unreachable!("lane codecs only"),
    }
}

/// Codec 6: x bits 0–10, y bits 11–21, z bits 22–31, each two's complement.
pub fn decode_packed_vector(word: u32) -> [f32; 4] {
    let field = |shift: u32, bits: u32| -> f64 {
        let raw = (word >> shift) & ((1 << bits) - 1);
        ((raw << (32 - bits)) as i32 >> (32 - bits)) as f64
    };
    let x = field(0, 11);
    let y = field(11, 11);
    let z = field(22, 10);
    [
        ((2.0 * x + 1.0) / 2047.0) as f32,
        ((2.0 * y + 1.0) / 2047.0) as f32,
        ((2.0 * z + 1.0) / 1023.0) as f32,
        1.0,
    ]
}

/// Decodes one declaration over `vertices` vertices of `stream`, as binary32 bit patterns.
pub fn decode_declaration(
    d: &Declaration,
    stride: u8,
    stream: &Stream,
    vertices: usize,
) -> Result<Vec<[u32; 4]>, StreamError> {
    let unsupported = if d.semantic == semantic::POSITION {
        StreamError::UnsupportedPositionEncoding(d.codec)
    } else {
        StreamError::UnsupportedEncoding(d.codec)
    };
    if bytes_read(d).is_none() {
        return Err(unsupported);
    }
    let mut rows = Vec::with_capacity(vertices);
    let mut gathered = Vec::with_capacity(16);
    for i in 0..vertices {
        let (offsets, count) = byte_offsets(d, stride, stream.address, i)
            .ok_or(StreamError::SourceExtent("declaration offset"))?;
        gathered.clear();
        for &offset in &offsets[..count] {
            let byte = usize::try_from(offset)
                .ok()
                .and_then(|o| stream.bytes.get(o))
                .ok_or(StreamError::SourceExtent("declaration bytes"))?;
            gathered.push(*byte);
        }
        let mut lanes = match d.codec {
            6 => decode_packed_vector(be_u32(&gathered, 0)),
            9 => decode_packed_normal([gathered[0], gathered[1], gathered[2]])?,
            codec => {
                let size = lane_bytes(codec).expect("supported");
                let mut lanes = [0.0f32; 4];
                for (lane, chunk) in gathered.chunks_exact(size).enumerate() {
                    lanes[lane] = decode_lane(codec, chunk);
                }
                lanes
            }
        };
        if d.semantic == semantic::POSITION {
            let c = d.components as usize;
            for lane in c.min(3)..3 {
                lanes[lane] = 0.0;
            }
            if c < 4 {
                lanes[3] = 1.0;
            }
        }
        rows.push(lanes.map(f32::to_bits));
    }
    Ok(rows)
}

// --- Partition decode --------------------------------------------------------------------------

/// One recorded declaration of an attribute list.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct DecodedDeclaration {
    pub ordinal: usize,
    pub semantic: u8,
    pub rows: Result<Vec<[u32; 4]>, StreamError>,
}

/// A primary or secondary attribute list.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum DecodedList {
    Absent,
    /// Failed as a whole (secondary only).
    Failed(StreamError),
    Declarations(Vec<DecodedDeclaration>),
}

/// The result of looking up a semantic.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Lookup<'a> {
    Found(&'a [[u32; 4]]),
    Failed(&'a StreamError),
    Missing,
}

impl DecodedList {
    fn lookup(&self, semantic: u8) -> Option<Lookup<'_>> {
        match self {
            Self::Absent => None,
            Self::Failed(error) => Some(Lookup::Failed(error)),
            Self::Declarations(list) => {
                list.iter()
                    .find(|d| d.semantic == semantic)
                    .map(|d| match &d.rows {
                        Ok(rows) => Lookup::Found(rows),
                        Err(error) => Lookup::Failed(error),
                    })
            }
        }
    }

    /// The list as vertex-input banks.
    pub fn attribute_list(&self) -> AttributeList<'_> {
        match self {
            Self::Absent => AttributeList::Absent,
            Self::Failed(_) => AttributeList::Failed,
            Self::Declarations(list) => AttributeList::Banks(
                list.iter()
                    .map(|d| SemanticBank {
                        semantic: d.semantic as u32,
                        rows: d.rows.as_deref().ok(),
                    })
                    .collect(),
            ),
        }
    }
}

fn decode_list(
    table: &DescriptorTable,
    stream: &Stream,
    vertices: usize,
) -> Vec<DecodedDeclaration> {
    table
        .declarations
        .iter()
        .enumerate()
        .map(|(ordinal, d)| DecodedDeclaration {
            ordinal,
            semantic: d.semantic,
            rows: if d.semantic == semantic::UNCLASSIFIED {
                Err(StreamError::UnclassifiedSemantic)
            } else {
                decode_declaration(d, table.stride, stream, vertices)
            },
        })
        .collect()
}

/// The decoded geometry of one partition.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct PartitionGeometry {
    pub configuration: Configuration,
    /// `V`: the number of rows every bank has.
    pub vertex_count: usize,
    /// The first `K` indices, in order.
    pub indices: Vec<u16>,
    /// Positions; lane 3 is `1.0` for the fixed codecs.
    pub positions: Vec<[u32; 4]>,
    pub primary: DecodedList,
    pub secondary: DecodedList,
}

impl PartitionGeometry {
    /// The bank for `semantic`: the primary list first, then the secondary list.
    pub fn lookup(&self, semantic: u8) -> Lookup<'_> {
        self.primary
            .lookup(semantic)
            .or_else(|| self.secondary.lookup(semantic))
            .unwrap_or(Lookup::Missing)
    }
}

/// Decodes a partition's streams. An error means the partition has no geometry.
pub fn decode_partition(
    partition: &PartitionRecord,
    streams: &PartitionStreams,
) -> Result<PartitionGeometry, StreamError> {
    let configuration = Configuration::parse(&streams.slot(slot::CONFIGURATION)?.bytes)?;
    let vertices = configuration.vertex_count as usize;

    // Indices.
    let index_stream = streams.slot(slot::INDICES)?;
    let count = configuration.index_count as usize;
    let index_bytes = index_stream
        .bytes
        .get(..2 * count)
        .ok_or(StreamError::SourceExtent("index list"))?;
    let indices: Vec<u16> = index_bytes
        .as_chunks::<2>()
        .0
        .iter()
        .map(|c| u16::from_be_bytes(*c))
        .collect();
    if indices.iter().any(|&i| i as usize >= vertices) {
        return Err(StreamError::SourceExtent("index out of range"));
    }

    // Positions and the primary list.
    let data = streams.slot(slot::VERTICES)?;
    let (positions, primary) = match configuration.position_codec {
        0 => {
            let bytes = data
                .bytes
                .get(..12 * vertices)
                .ok_or(StreamError::SourceExtent("positions"))?;
            let rows = bytes
                .as_chunks::<12>()
                .0
                .iter()
                .map(|r| [be_u32(r, 0), be_u32(r, 4), be_u32(r, 8), 1f32.to_bits()])
                .collect();
            (rows, DecodedList::Absent)
        }
        6 => {
            let bytes = data
                .bytes
                .get(..6 * vertices)
                .ok_or(StreamError::SourceExtent("positions"))?;
            let rows = bytes
                .as_chunks::<6>()
                .0
                .iter()
                .map(|r| {
                    let h = |at| rebased_half_f32(be_u16(r, at)).to_bits();
                    [h(0), h(2), h(4), 1f32.to_bits()]
                })
                .collect();
            (rows, DecodedList::Absent)
        }
        255 => {
            let table = DescriptorTable::parse(&streams.slot(slot::PRIMARY_FORMAT)?.bytes)?;
            let declarations = decode_list(&table, data, vertices);
            let position = declarations
                .iter()
                .find(|d| d.semantic == semantic::POSITION)
                .ok_or(StreamError::MissingPositionAttribute)?;
            let rows = position.rows.clone()?;
            (rows, DecodedList::Declarations(declarations))
        }
        other => return Err(StreamError::UnsupportedPositionEncoding(other)),
    };

    // The secondary list.
    let chunks = partition.secondary_chunks();
    let secondary = if chunks == [0, 0, 0] {
        DecodedList::Absent
    } else if chunks[1] != 0 || chunks[2] != 0 {
        DecodedList::Failed(StreamError::SplitInputChunks)
    } else if configuration.secondary_layout != 255 {
        DecodedList::Failed(StreamError::BuiltinSecondary(
            configuration.secondary_layout,
        ))
    } else {
        let decoded = streams
            .slot(slot::SECONDARY_FORMAT)
            .and_then(|format| DescriptorTable::parse(&format.bytes))
            .and_then(|table| {
                let data = streams.slot(slot::SECONDARY)?;
                Ok(decode_list(&table, data, vertices))
            });
        match decoded {
            Ok(list) => DecodedList::Declarations(list),
            Err(error) => DecodedList::Failed(error),
        }
    };

    Ok(PartitionGeometry {
        configuration,
        vertex_count: vertices,
        indices,
        positions,
        primary,
        secondary,
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::guest::SparseMemory;

    fn bits(values: [f32; 4]) -> [u32; 4] {
        values.map(f32::to_bits)
    }

    fn record() -> PartitionRecord {
        PartitionRecord {
            address: 0x1000,
            bytes: [0; PARTITION_BYTES],
        }
    }

    fn set(record: &mut PartitionRecord, at: usize, word: u32) {
        record.bytes[at..at + 4].copy_from_slice(&word.to_be_bytes());
    }

    fn set_slot(record: &mut PartitionRecord, k: usize, control: u32, address: u32) {
        set(record, 0x30 + 8 * k, control);
        set(record, 0x34 + 8 * k, address);
    }

    fn configuration(codec: u8, vertices: u16, indices: u16) -> Vec<u8> {
        let mut c = vec![0u8; 16];
        c[2] = codec;
        c[3] = 255;
        c[8..10].copy_from_slice(&vertices.to_be_bytes());
        c[10..12].copy_from_slice(&indices.to_be_bytes());
        c
    }

    fn stream(address: u32, bytes: &[u8]) -> Stream {
        Stream {
            address,
            bytes: bytes.to_vec(),
        }
    }

    fn decl(offset: u8, codec: u8, components: u8, semantic: u8, size: u8) -> Declaration {
        Declaration {
            offset,
            codec,
            components,
            semantic,
            element_size: size,
            raw: [offset, codec, components, semantic, size, 0, 0, 0],
        }
    }

    #[test]
    fn v1_slot_read() {
        let mut p = record();
        set_slot(&mut p, 7, 0x30, 0x10000);
        let bases = StreamBases::default();
        assert_eq!(
            plan_slot(&p, &bases, 7),
            Ok(SlotPlan {
                address: 0x10000,
                length: 0x30,
                from_control: true
            })
        );
        set_slot(&mut p, 7, 0x10000, 0x10000);
        assert_eq!(
            plan_slot(&p, &bases, 7),
            Err(StreamError::UnsupportedControl {
                slot: 7,
                control: 0x10000
            })
        );
        // Control 0: empty, no read (the memory is empty, so any read would fail).
        set_slot(&mut p, 7, 0, 0x10000);
        let streams = read_partition_streams(&SparseMemory::new(), &p, &bases, 1 << 20);
        assert_eq!(streams.slot(7), Ok(&stream(0x10000, &[])));
        // Invalid range.
        set_slot(&mut p, 7, 0x20, 0xffff_fff0);
        assert!(matches!(
            plan_slot(&p, &bases, 7),
            Err(StreamError::InvalidRange { slot: 7, .. })
        ));
    }

    #[test]
    fn v2_palette_and_replacements() {
        let mut p = record();
        set_slot(&mut p, 3, 0x60, 0xdead_0000);
        p.bytes[0x14..0x16].copy_from_slice(&0x40u16.to_be_bytes());
        p.bytes[0x0a..0x0c].copy_from_slice(&0x20u16.to_be_bytes());
        set_slot(&mut p, 10, 0xffff_ffff, 0x5000);
        set_slot(&mut p, 13, 0x40, 0x6000);
        set_slot(&mut p, 17, 0x80, 0x7000);
        set_slot(&mut p, 18, 0x40, 0x8000);
        let bases = StreamBases {
            palette_base: 0x20000,
            extra_stream: 0x30000,
            environment: 0x40000,
        };
        assert_eq!(plan_slot(&p, &bases, 3).unwrap().address, 0x20040);
        assert_eq!(plan_slot(&p, &bases, 3).unwrap().length, 0x60);
        // Slot 10 takes its length from +0x0A; its control is not a length.
        assert_eq!(
            plan_slot(&p, &bases, 10),
            Ok(SlotPlan {
                address: 0x5000,
                length: 0x20,
                from_control: false
            })
        );
        assert_eq!(plan_slot(&p, &bases, 13).unwrap().length, 0);
        assert_eq!(plan_slot(&p, &bases, 13).unwrap().address, 0x30000);
        assert_eq!(plan_slot(&p, &bases, 17).unwrap().address, 0x40000);
        assert_eq!(
            plan_slot(&p, &bases, 18),
            Ok(SlotPlan {
                address: 0,
                length: 0,
                from_control: false
            })
        );
    }

    #[test]
    fn v3_large_stream() {
        assert_eq!(large_stream_length(0x4000, Some(24000)), 24000);
        assert_eq!(large_stream_length(0x3ff0, Some(24000)), 0x3ff0);
        assert_eq!(large_stream_length(0x4000, Some(12000)), 0x4000);
        assert_eq!(large_stream_length(0x4000, Some(18000)), 18000);
        assert_eq!(
            large_stream_length(0x4000, Some(LARGE_CAP)),
            LARGE_CAP as u32
        );
        assert_eq!(large_stream_length(0x4000, Some(LARGE_CAP + 1)), 0x4000);

        // Through the reader: codec 0, V = 2000, K = 9000.
        let mut memory = SparseMemory::new();
        memory.put(0x10_0000, &vec![0x3f; 24000]);
        memory.put(0x20_0000, &vec![0; 18000]);
        memory.put(0x30_0000, &configuration(0, 2000, 9000));
        let mut p = record();
        set_slot(&mut p, 7, 0x4000, 0x10_0000);
        set_slot(&mut p, 1, 0x4000, 0x20_0000);
        set_slot(&mut p, 19, 16, 0x30_0000);
        let streams = read_partition_streams(&memory, &p, &StreamBases::default(), 1 << 20);
        assert_eq!(streams.slot(7).unwrap().bytes.len(), 24000);
        assert_eq!(streams.slot(1).unwrap().bytes.len(), 18000);
        let geometry = decode_partition(&p, &streams).unwrap();
        assert_eq!(geometry.positions.len(), 2000);
        assert_eq!(geometry.indices.len(), 9000);

        // Control 0x3FF0: read 0x3FF0 bytes, and the position decode fails.
        set_slot(&mut p, 7, 0x3ff0, 0x10_0000);
        let streams = read_partition_streams(&memory, &p, &StreamBases::default(), 1 << 20);
        assert_eq!(streams.slot(7).unwrap().bytes.len(), 0x3ff0);
        assert_eq!(
            decode_partition(&p, &streams),
            Err(StreamError::SourceExtent("positions"))
        );

        // V = 1000: 12000 needed, so 16 KiB is read.
        memory.put(0x30_0000, &configuration(0, 1000, 9000));
        set_slot(&mut p, 7, 0x4000, 0x10_0000);
        memory.put(0x10_0000, &vec![0x3f; 0x4000]);
        let streams = read_partition_streams(&memory, &p, &StreamBases::default(), 1 << 20);
        assert_eq!(streams.slot(7).unwrap().bytes.len(), 0x4000);
    }

    #[test]
    fn large_descriptor_stream_uses_the_furthest_declaration_byte() {
        // Stride 16, one float position (12 bytes) and unsigned-byte weights at 12: the furthest
        // byte of vertex V−1 is 16·V.
        let mut table = vec![2u8, 16, 0, 0, 0, 0, 0, 0];
        table.extend([0, 2, 3, 1, 12, 0, 0, 0]);
        table.extend([12, 4, 4, 9, 4, 0, 0, 0]);
        let mut memory = SparseMemory::new();
        memory.put(0x10_0000, &vec![0; 16 * 2000]);
        memory.put(0x30_0000, &configuration(255, 2000, 0));
        memory.put(0x31_0000, &table);
        let mut p = record();
        set_slot(&mut p, 7, 0x4000, 0x10_0000);
        set_slot(&mut p, 19, 16, 0x30_0000);
        set_slot(&mut p, 20, table.len() as u32, 0x31_0000);
        let streams = read_partition_streams(&memory, &p, &StreamBases::default(), 1 << 20);
        assert_eq!(streams.slot(7).unwrap().bytes.len(), 32000);
        let geometry = decode_partition(&p, &streams).unwrap();
        assert_eq!(geometry.positions.len(), 2000);
        assert!(matches!(geometry.lookup(9), Lookup::Found(rows) if rows.len() == 2000));
    }

    #[test]
    fn read_budget_is_visible() {
        let mut memory = SparseMemory::new();
        memory.put(0x10_0000, &[0; 0x100]);
        let mut p = record();
        set_slot(&mut p, 7, 0x100, 0x10_0000);
        let streams = read_partition_streams(&memory, &p, &StreamBases::default(), 0xff);
        assert_eq!(streams.slot(7), Err(StreamError::Budget));
    }

    #[test]
    fn v4_codec_zero_position() {
        let mut bytes = Vec::new();
        for w in [0x3f80_0000u32, 0x4000_0000, 0xc040_0000] {
            bytes.extend(w.to_be_bytes());
        }
        let mut memory = SparseMemory::new();
        memory.put(0x10_0000, &bytes);
        memory.put(0x20_0000, &[0, 0]);
        memory.put(0x30_0000, &configuration(0, 1, 1));
        let mut p = record();
        set_slot(&mut p, 7, 12, 0x10_0000);
        set_slot(&mut p, 1, 2, 0x20_0000);
        set_slot(&mut p, 19, 16, 0x30_0000);
        let streams = read_partition_streams(&memory, &p, &StreamBases::default(), 1 << 20);
        let geometry = decode_partition(&p, &streams).unwrap();
        assert_eq!(geometry.positions, vec![bits([1.0, 2.0, -3.0, 1.0])]);
        assert_eq!(geometry.primary, DecodedList::Absent);
        assert_eq!(geometry.indices, vec![0]);
    }

    #[test]
    fn v5_rebased_half() {
        assert_eq!(rebased_half_f32(0x3c00).to_bits(), 0x3f80_0000);
        assert_eq!(rebased_half_f32(0x8000).to_bits(), 0x8000_0000);
        assert_eq!(rebased_half_f32(0x0001).to_bits(), 0x3800_2000);
        assert_eq!(rebased_half_f32(0x7c00).to_bits(), 0x4780_0000);
        // Exact for every field: sign, exponent − 15 + 127 and the mantissa shifted by 13.
        for h in 0..=u16::MAX {
            let expected = if h & 0x7fff == 0 {
                (h as u32) << 16
            } else {
                ((h as u32 & 0x8000) << 16)
                    | ((((h as u32 >> 10) & 0x1f) + 112) << 23)
                    | ((h as u32 & 0x3ff) << 13)
            };
            assert_eq!(rebased_half_f32(h).to_bits(), expected, "{h:#06x}");
        }
        // Codec 6 positions use the same field.
        let mut memory = SparseMemory::new();
        memory.put(0x10_0000, &[0x3c, 0x00, 0x80, 0x00, 0x7c, 0x00]);
        memory.put(0x30_0000, &configuration(6, 1, 0));
        let mut p = record();
        set_slot(&mut p, 7, 6, 0x10_0000);
        set_slot(&mut p, 19, 16, 0x30_0000);
        let streams = read_partition_streams(&memory, &p, &StreamBases::default(), 1 << 20);
        let geometry = decode_partition(&p, &streams).unwrap();
        assert_eq!(geometry.positions, vec![bits([1.0, -0.0, 65536.0, 1.0])]);
    }

    /// The exact rational `n/d` is within one binary32 ulp of `value`.
    fn within_ulp(value: f32, n: f64, d: f64) {
        let exact = n / d;
        let ulp = (value.abs().next_up() - value.abs()) as f64;
        assert!((value as f64 - exact).abs() <= ulp, "{value} vs {n}/{d}");
    }

    fn one(codec: u8, bytes: &[u8], semantic: u8, components: u8) -> [f32; 4] {
        let d = decl(0, codec, components, semantic, 4 * components);
        decode_declaration(&d, 16, &stream(0x10_0000, bytes), 1).unwrap()[0].map(f32::from_bits)
    }

    #[test]
    fn v6_v7_normalised_codecs() {
        for v in [32767i16, -32768, 0, -1, 1234, -4321] {
            let bytes: Vec<u8> = [v, 0, 0, 0].iter().flat_map(|x| x.to_be_bytes()).collect();
            let lanes = one(1, &bytes, 2, 1);
            within_ulp(lanes[0], 2.0 * v as f64 + 1.0, 65535.0);
        }
        assert_eq!(one(1, &[0x7f, 0xff, 0, 0, 0, 0, 0, 0], 2, 1)[0], 1.0);
        assert_eq!(one(1, &[0x80, 0x00, 0, 0, 0, 0, 0, 0], 2, 1)[0], -1.0);
        assert_eq!(one(1, &[0; 8], 2, 1)[0].to_bits(), 0x3780_0080);
        for v in 0..=255u8 {
            within_ulp(one(4, &[v, 0, 0, 0], 2, 1)[0], v as f64, 255.0);
        }
        assert_eq!(one(4, &[255, 128, 0, 0], 2, 1)[0], 1.0);
        assert_eq!(one(4, &[0, 0, 0, 0], 2, 1)[0].to_bits(), 0);
    }

    #[test]
    fn v8_packed_vector() {
        let lanes = one(6, &0x0020_03ffu32.to_be_bytes(), 2, 3);
        assert_eq!(lanes[0], 1.0);
        assert_eq!(lanes[1], -1.0);
        within_ulp(lanes[2], 1.0, 1023.0);
        assert_eq!(lanes[3], 1.0);
        // Every z field is within one ulp of (2s + 1)/1023.
        for z in 0..1024u32 {
            let s = ((z << 22) as i32 >> 22) as f64;
            within_ulp(decode_packed_vector(z << 22)[2], 2.0 * s + 1.0, 1023.0);
        }
        // Position semantic: lanes at or beyond the component count are zeroed, lane 3 is 1.
        assert_eq!(
            one(6, &0x0020_03ffu32.to_be_bytes(), 1, 2),
            [1.0, -1.0, 0.0, 1.0]
        );
    }

    #[test]
    fn v9_integer_codecs() {
        assert_eq!(
            one(5, &[0xff, 0xfe, 0, 1, 0, 2, 0, 3], 2, 1),
            [-2.0, 1.0, 2.0, 3.0]
        );
        assert_eq!(one(7, &[0xc8, 1, 2, 3], 2, 1), [200.0, 1.0, 2.0, 3.0]);
        assert_eq!(
            one(3, &[0x3c, 0, 0xbc, 0, 0, 0, 0x7c, 0], 2, 4),
            [1.0, -1.0, 0.0, 65536.0]
        );
    }

    fn vector_ten(position_components: u8) -> (Vec<u8>, Vec<u8>) {
        let mut table = vec![2u8, 16, 0, 0, 0, 0, 0, 0];
        table.extend([0, 2, position_components, 1, 12, 0, 0, 0]);
        table.extend([12, 4, 4, 9, 4, 0, 0, 0]);
        let mut vertex = Vec::new();
        for w in [0x3f80_0000u32, 0, 0x3f00_0000] {
            vertex.extend(w.to_be_bytes());
        }
        vertex.extend([0xff, 0x80, 0x00, 0x40]);
        (table, vertex)
    }

    fn descriptor_partition(
        table: &[u8],
        vertex: &[u8],
        address: u32,
    ) -> (PartitionRecord, PartitionStreams) {
        let mut memory = SparseMemory::new();
        memory.put(address, vertex);
        memory.put(0x30_0000, &configuration(255, 1, 0));
        memory.put(0x31_0000, table);
        let mut p = record();
        set_slot(&mut p, 7, vertex.len() as u32, address);
        set_slot(&mut p, 19, 16, 0x30_0000);
        set_slot(&mut p, 20, table.len() as u32, 0x31_0000);
        let streams = read_partition_streams(&memory, &p, &StreamBases::default(), 1 << 20);
        (p, streams)
    }

    #[test]
    fn v10_v11_descriptor_table() {
        let (table, vertex) = vector_ten(3);
        let (p, streams) = descriptor_partition(&table, &vertex, 0x10_0000);
        let geometry = decode_partition(&p, &streams).unwrap();
        assert_eq!(geometry.positions, vec![bits([1.0, 0.0, 0.5, 1.0])]);
        let Lookup::Found(weights) = geometry.lookup(9) else {
            panic!("weights")
        };
        let w = weights[0].map(f32::from_bits);
        assert_eq!((w[0], w[2]), (1.0, 0.0));
        within_ulp(w[1], 128.0, 255.0);
        within_ulp(w[3], 64.0, 255.0);

        let (table, vertex) = vector_ten(2);
        let (p, streams) = descriptor_partition(&table, &vertex, 0x10_0000);
        let geometry = decode_partition(&p, &streams).unwrap();
        assert_eq!(geometry.positions, vec![bits([1.0, 0.0, 0.0, 1.0])]);
        assert_eq!(geometry.positions[0][2], 0);
    }

    #[test]
    fn lanes_beyond_the_component_count_decode_the_following_bytes() {
        // A two-component unsigned-byte normal still decodes four lanes.
        assert_eq!(one(7, &[1, 2, 3, 4], 2, 2), [1.0, 2.0, 3.0, 4.0]);
        // Codec 2, three components at offset 4 of a 16-byte stride, element size 12:
        // 4 + 12 ≤ 16, so lane 3 wraps to the start of the 16-byte block.
        let mut vertex = Vec::new();
        for v in [9.0f32, 1.0, 2.0, 3.0] {
            vertex.extend(v.to_bits().to_be_bytes());
        }
        let d = decl(4, 2, 3, 2, 12);
        let rows = decode_declaration(&d, 16, &stream(0x10_0000, &vertex), 1).unwrap();
        assert_eq!(rows[0].map(f32::from_bits), [1.0, 2.0, 3.0, 9.0]);
        // Element size 16 at offset 4: no wrap, bytes run straight on.
        let mut long = vertex.clone();
        long.extend(7.0f32.to_bits().to_be_bytes());
        let d = decl(4, 2, 3, 2, 16);
        let rows = decode_declaration(&d, 16, &stream(0x10_0000, &long), 1).unwrap();
        assert_eq!(rows[0].map(f32::from_bits), [1.0, 2.0, 3.0, 7.0]);
        // The furthest byte follows the wrapped reads.
        let table = DescriptorTable {
            stride: 16,
            declarations: vec![decl(4, 2, 3, 2, 12)],
        };
        assert_eq!(table.furthest_byte(0x10_0000, 1), 16);
    }

    #[test]
    fn v12_primary_first() {
        let rows = vec![[0u32; 4]];
        let geometry = PartitionGeometry {
            configuration: Configuration::parse(&configuration(255, 1, 0)).unwrap(),
            vertex_count: 1,
            indices: Vec::new(),
            positions: rows.clone(),
            primary: DecodedList::Declarations(vec![
                DecodedDeclaration {
                    ordinal: 0,
                    semantic: 1,
                    rows: Ok(rows.clone()),
                },
                DecodedDeclaration {
                    ordinal: 1,
                    semantic: 2,
                    rows: Err(StreamError::UnsupportedEncoding(8)),
                },
                DecodedDeclaration {
                    ordinal: 2,
                    semantic: 2,
                    rows: Ok(rows.clone()),
                },
            ]),
            secondary: DecodedList::Declarations(vec![
                DecodedDeclaration {
                    ordinal: 0,
                    semantic: 2,
                    rows: Ok(rows.clone()),
                },
                DecodedDeclaration {
                    ordinal: 1,
                    semantic: 3,
                    rows: Ok(vec![[3; 4]]),
                },
            ]),
        };
        assert_eq!(
            geometry.lookup(2),
            Lookup::Failed(&StreamError::UnsupportedEncoding(8))
        );
        assert_eq!(geometry.lookup(3), Lookup::Found(&[[3; 4]]));
        assert_eq!(geometry.lookup(9), Lookup::Missing);
        // A secondary list that failed as a whole fails the lookup.
        let mut failed = geometry.clone();
        failed.secondary = DecodedList::Failed(StreamError::SplitInputChunks);
        assert_eq!(
            failed.lookup(3),
            Lookup::Failed(&StreamError::SplitInputChunks)
        );
        assert!(matches!(
            failed.lookup(2),
            Lookup::Failed(StreamError::UnsupportedEncoding(8))
        ));
        // The vertex-input view keeps ordinals and failures.
        let AttributeList::Banks(banks) = geometry.primary.attribute_list() else {
            panic!("banks")
        };
        assert_eq!(banks.len(), 3);
        assert_eq!(banks[1].rows, None);
        assert_eq!(failed.secondary.attribute_list(), AttributeList::Failed);
    }

    fn indexed(vertices: u16, indices: &[u16]) -> Result<PartitionGeometry, StreamError> {
        let bytes: Vec<u8> = indices.iter().flat_map(|i| i.to_be_bytes()).collect();
        let mut memory = SparseMemory::new();
        memory.put(0x10_0000, &vec![0; 12 * vertices as usize + 1]);
        memory.put(0x20_0000, &bytes);
        memory.put(0x30_0000, &configuration(0, vertices, indices.len() as u16));
        let mut p = record();
        set_slot(&mut p, 7, 12 * vertices as u32 + 1, 0x10_0000);
        set_slot(&mut p, 1, bytes.len() as u32, 0x20_0000);
        set_slot(&mut p, 19, 16, 0x30_0000);
        let streams = read_partition_streams(&memory, &p, &StreamBases::default(), 1 << 20);
        decode_partition(&p, &streams)
    }

    #[test]
    fn v13_index_range() {
        assert_eq!(
            indexed(2, &[0, 1, 2]),
            Err(StreamError::SourceExtent("index out of range"))
        );
        assert_eq!(indexed(3, &[0, 1, 2, 2]).unwrap().indices, vec![0, 1, 2, 2]);
        // V = 0 and K = 0: empty banks, no indices.
        let empty = indexed(0, &[]).unwrap();
        assert!(empty.positions.is_empty() && empty.indices.is_empty());
    }

    #[test]
    fn v14_misaligned_stream() {
        let d = decl(0, 2, 3, 2, 12);
        assert_eq!(
            decode_declaration(&d, 16, &stream(0x10004, &[0; 32]), 1),
            Err(StreamError::SourceExtent("declaration bytes"))
        );
        // Offset 8: guest address align_down_16(0x1000C) + 8 = 0x10008, stream byte 4.
        let mut bytes = vec![0u8; 32];
        bytes[4..8].copy_from_slice(&5.0f32.to_bits().to_be_bytes());
        let d = decl(8, 2, 1, 1, 4);
        let rows = decode_declaration(&d, 16, &stream(0x10004, &bytes), 1).unwrap();
        assert_eq!(rows[0].map(f32::from_bits), [5.0, 0.0, 0.0, 1.0]);
        // A non-position semantic also reads the lanes past its element; here they wrap to the
        // start of the 16-byte block, which lies before the stream.
        let d = decl(8, 2, 1, 2, 4);
        assert_eq!(
            decode_declaration(&d, 16, &stream(0x10004, &bytes), 1),
            Err(StreamError::SourceExtent("declaration bytes"))
        );
    }

    #[test]
    fn v15_unsupported_codecs() {
        let mut table = vec![3u8, 16, 0, 0, 0, 0, 0, 0];
        table.extend([0, 2, 3, 1, 12, 0, 0, 0]);
        table.extend([12, 8, 4, 2, 4, 0, 0, 0]);
        table.extend([12, 4, 4, 9, 4, 0, 0, 0]);
        let (_, vertex) = vector_ten(3);
        let (p, streams) = descriptor_partition(&table, &vertex, 0x10_0000);
        let geometry = decode_partition(&p, &streams).unwrap();
        assert_eq!(
            geometry.lookup(2),
            Lookup::Failed(&StreamError::UnsupportedEncoding(8))
        );
        assert!(matches!(geometry.lookup(9), Lookup::Found(_)));
        // Codec 8 on the position: the partition has no geometry.
        let mut table = vec![1u8, 16, 0, 0, 0, 0, 0, 0];
        table.extend([0, 8, 3, 1, 12, 0, 0, 0]);
        let (p, streams) = descriptor_partition(&table, &vertex, 0x10_0000);
        assert_eq!(
            decode_partition(&p, &streams),
            Err(StreamError::UnsupportedPositionEncoding(8))
        );
        // No position declaration; semantic 0 is unclassified.
        let mut table = vec![1u8, 16, 0, 0, 0, 0, 0, 0];
        table.extend([0, 2, 3, 0, 12, 0, 0, 0]);
        let (p, streams) = descriptor_partition(&table, &vertex, 0x10_0000);
        assert_eq!(
            decode_partition(&p, &streams),
            Err(StreamError::MissingPositionAttribute)
        );
        // Unsupported fixed position codec and a short configuration.
        let mut memory = SparseMemory::new();
        memory.put(0x30_0000, &configuration(3, 0, 0));
        let mut p = record();
        set_slot(&mut p, 19, 16, 0x30_0000);
        let streams = read_partition_streams(&memory, &p, &StreamBases::default(), 1 << 20);
        assert_eq!(
            decode_partition(&p, &streams),
            Err(StreamError::UnsupportedPositionEncoding(3))
        );
        set_slot(&mut p, 19, 15, 0x30_0000);
        let streams = read_partition_streams(&memory, &p, &StreamBases::default(), 1 << 20);
        assert_eq!(
            decode_partition(&p, &streams),
            Err(StreamError::SourceExtent("configuration"))
        );
    }

    #[test]
    fn v16_secondary_admission() {
        let (table, vertex) = vector_ten(3);
        let (mut p, streams) = descriptor_partition(&table, &vertex, 0x10_0000);
        p.bytes[0x0a..0x0c].copy_from_slice(&0x40u16.to_be_bytes());
        p.bytes[0x0c..0x0e].copy_from_slice(&0x10u16.to_be_bytes());
        assert_eq!(
            decode_partition(&p, &streams).unwrap().secondary,
            DecodedList::Failed(StreamError::SplitInputChunks)
        );
        p.bytes[0x0c..0x0e].copy_from_slice(&0u16.to_be_bytes());
        let mut built_in = streams.clone();
        let mut config = configuration(255, 1, 0);
        config[3] = 0;
        built_in.slots[19] = Ok(stream(0x30_0000, &config));
        assert_eq!(
            decode_partition(&p, &built_in).unwrap().secondary,
            DecodedList::Failed(StreamError::BuiltinSecondary(0))
        );
    }

    #[test]
    fn v17_control_check_scope() {
        let mut memory = SparseMemory::new();
        memory.put(0x5000, &[7; 0x40]);
        let mut p = record();
        set_slot(&mut p, 10, 0x0001_0000, 0x5000);
        p.bytes[0x0a..0x0c].copy_from_slice(&0x40u16.to_be_bytes());
        set_slot(&mut p, 5, 0x0001_0000, 0x6000);
        let streams = read_partition_streams(&memory, &p, &StreamBases::default(), 1 << 20);
        assert_eq!(streams.slot(10), Ok(&stream(0x5000, &[7; 0x40])));
        assert_eq!(
            streams.slot(5),
            Err(StreamError::UnsupportedControl {
                slot: 5,
                control: 0x0001_0000
            })
        );
        // The replaced-length slots never check their control words.
        for k in [11, 12, 13, 14, 15, 16, 18] {
            set_slot(&mut p, k, 0xffff_ffff, 0);
            assert!(
                plan_slot(&p, &StreamBases::default(), k).is_ok(),
                "slot {k}"
            );
        }
    }

    #[test]
    fn v18_secondary_per_declaration() {
        let (table, vertex) = vector_ten(3);
        let mut primary = table[..16].to_vec();
        primary[0] = 1;
        let mut secondary_table = vec![2u8, 16, 0, 0, 0, 0, 0, 0];
        secondary_table.extend([0, 8, 3, 2, 12, 0, 0, 0]);
        secondary_table.extend([0, 2, 4, 3, 16, 0, 0, 0]);
        let mut memory = SparseMemory::new();
        memory.put(0x10_0000, &vertex);
        memory.put(0x30_0000, &configuration(255, 1, 0));
        memory.put(0x31_0000, &primary);
        memory.put(0x32_0000, &secondary_table);
        memory.put(0x33_0000, &[0; 16]);
        let mut p = record();
        set_slot(&mut p, 7, vertex.len() as u32, 0x10_0000);
        set_slot(&mut p, 19, 16, 0x30_0000);
        set_slot(&mut p, 20, 16, 0x31_0000);
        set_slot(&mut p, 21, secondary_table.len() as u32, 0x32_0000);
        set_slot(&mut p, 10, 0, 0x33_0000);
        p.bytes[0x0a..0x0c].copy_from_slice(&16u16.to_be_bytes());
        let streams = read_partition_streams(&memory, &p, &StreamBases::default(), 1 << 20);
        let geometry = decode_partition(&p, &streams).unwrap();
        let DecodedList::Declarations(list) = &geometry.secondary else {
            panic!("the list succeeds")
        };
        assert_eq!(list[0].rows, Err(StreamError::UnsupportedEncoding(8)));
        assert_eq!(list[1].rows, Ok(vec![[0; 4]]));
        // An unreadable slot-21 table fails the whole list.
        set_slot(&mut p, 21, secondary_table.len() as u32, 0x34_0000);
        let streams = read_partition_streams(&memory, &p, &StreamBases::default(), 1 << 20);
        assert!(matches!(
            decode_partition(&p, &streams).unwrap().secondary,
            DecodedList::Failed(StreamError::Read(_))
        ));
    }

    #[test]
    fn v19_packed_position_below_three_components() {
        let word = 0x0020_03ffu32.to_be_bytes();
        assert_eq!(one(6, &word, 1, 1), [1.0, 0.0, 0.0, 1.0]);
        // The whole 4-byte field must lie in the stream although only x is kept.
        let d = decl(0, 6, 1, 1, 4);
        assert_eq!(
            decode_declaration(&d, 16, &stream(0x10_0000, &word[..2]), 1),
            Err(StreamError::SourceExtent("declaration bytes"))
        );
        // Codec 9: the whole field is decoded, so a selector refusal fails the declaration even
        // when no decoded lane is kept, and the field must fit.
        let d = decl(0, 9, 0, 1, 3);
        assert_eq!(
            decode_declaration(&d, 16, &stream(0x10_0000, &[0, 0, 12]), 1),
            Err(StreamError::Normal(NormalError::UnqualifiedSelector(12)))
        );
        assert_eq!(
            decode_declaration(&d, 16, &stream(0x10_0000, &[0, 0]), 1),
            Err(StreamError::SourceExtent("declaration bytes"))
        );
    }

    #[test]
    fn secondary_list_decodes_over_slot_ten() {
        // Primary: position only. Secondary: a float normal and a tangent, stride 32.
        let (table, vertex) = vector_ten(3);
        let mut secondary_table = vec![2u8, 32, 0, 0, 0, 0, 0, 0];
        secondary_table.extend([0, 2, 3, 2, 12, 0, 0, 0]);
        secondary_table.extend([16, 2, 4, 3, 16, 0, 0, 0]);
        let mut data = Vec::new();
        for v in [0.0f32, 0.0, 1.0, 0.0, 1.0, 0.0, 0.0, -1.0] {
            data.extend(v.to_bits().to_be_bytes());
        }
        let mut memory = SparseMemory::new();
        memory.put(0x10_0000, &vertex);
        memory.put(0x30_0000, &configuration(255, 1, 0));
        memory.put(0x32_0000, &secondary_table);
        memory.put(0x33_0000, &data);
        let mut p = record();
        let mut primary = table[..16].to_vec();
        primary[0] = 1;
        memory.put(0x31_0000, &primary);
        set_slot(&mut p, 7, vertex.len() as u32, 0x10_0000);
        set_slot(&mut p, 19, 16, 0x30_0000);
        set_slot(&mut p, 20, 16, 0x31_0000);
        set_slot(&mut p, 21, secondary_table.len() as u32, 0x32_0000);
        set_slot(&mut p, 10, 0, 0x33_0000);
        p.bytes[0x0a..0x0c].copy_from_slice(&(data.len() as u16).to_be_bytes());
        let streams = read_partition_streams(&memory, &p, &StreamBases::default(), 1 << 20);
        let geometry = decode_partition(&p, &streams).unwrap();
        assert_eq!(
            geometry.lookup(2),
            Lookup::Found(&[bits([0.0, 0.0, 1.0, 0.0])])
        );
        assert_eq!(
            geometry.lookup(3),
            Lookup::Found(&[bits([1.0, 0.0, 0.0, -1.0])])
        );
        // A failed slot-10 read fails the secondary list as a whole.
        let mut broken = streams.clone();
        broken.slots[10] = Err(StreamError::Budget);
        assert_eq!(
            decode_partition(&p, &broken).unwrap().secondary,
            DecodedList::Failed(StreamError::Budget)
        );
    }

    #[test]
    fn packed_normal_codec_and_position_defaults() {
        // Selector 11: axis z, positive, handedness +1; u = v = 511 are near zero.
        let n = (511u32 << 14) | (511 << 4) | 11;
        let bytes = &n.to_be_bytes()[1..];
        let normal = one(9, bytes, 2, 3);
        assert!(normal[2] > 0.999 && normal[3] == 1.0);
        // As a two-component position: lane 2 zeroed, lane 3 is 1.
        let position = one(9, bytes, 1, 2);
        assert_eq!((position[2], position[3]), (0.0, 1.0));
        // Selector 12 is refused by the normal codec.
        let bad = [0u8, 0, 12];
        assert_eq!(
            decode_declaration(&decl(0, 9, 3, 2, 3), 16, &stream(0x10_0000, &bad), 1),
            Err(StreamError::Normal(NormalError::UnqualifiedSelector(12)))
        );
    }

    #[test]
    fn partition_chain_follows_links_and_detects_cycles() {
        let mut memory = SparseMemory::new();
        let mut a = [0u8; PARTITION_BYTES];
        a[0xf0..0xf4].copy_from_slice(&0x2000u32.to_be_bytes());
        memory.put(0x1000, &a);
        memory.put(0x2000, &[0u8; PARTITION_BYTES]);
        let chain = partition_chain(&memory, first_partition(0x8000_0800, 0x800), 8).unwrap();
        assert_eq!(
            chain.iter().map(|p| p.address).collect::<Vec<_>>(),
            vec![0x1000, 0x2000]
        );
        let mut b = [0u8; PARTITION_BYTES];
        b[0xf0..0xf4].copy_from_slice(&0x1000u32.to_be_bytes());
        memory.put(0x2000, &b);
        assert_eq!(
            partition_chain(&memory, 0x1000, 8),
            Err(StreamError::PartitionCycle(0x1000))
        );
        assert_eq!(
            partition_chain(&memory, 0x1000, 1),
            Err(StreamError::Budget)
        );
        assert_eq!(
            partition_chain(&memory, 0xffff_ff00, 8),
            Err(StreamError::PartitionRange(0xffff_ff00))
        );
        assert_eq!(first_partition(0x8000_0400, 0x1000_0000), 0x1000_0400);
    }
}
