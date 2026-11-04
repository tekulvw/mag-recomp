//! Raw packed-input ownership from mesh asset headers, before child preparation.
//!
//! This implements noninstanced and Main group-zero instance source readsets.
//! A bundle owns bytes, not a live asset lease, codec admission, visibility
//! result, complete draw, or original-work removal certificate. Compact mapped
//! attributes and other instance/deformation modes remain separate consumers.
use crate::{
    error::Error,
    mesh_sources::{MeshSource, PartitionSource},
};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct StreamRequest {
    pub address: u32,
    pub bytes: usize,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct NonInstancedSourceBases {
    pub palette_base: u32,
    pub extra_stream: u32,
    pub environment: u32,
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum PartitionInputError {
    BudgetExceeded,
    WrongDispatch,
    InvalidInstanceBatch,
    UnresolvedInstanceLocation,
    IndexMappingRange,
    UnsupportedSourceControl {
        slot: usize,
        control: u32,
    },
    InvalidRange {
        slot: usize,
    },
    Source {
        slot: usize,
        error: Error,
    },
    ReadExtent {
        slot: usize,
        expected: usize,
        actual: usize,
    },
}
#[derive(Clone, Debug)]
pub struct PartitionInputPlan {
    requests: [StreamRequest; 24],
    total_bytes: usize,
}
impl PartitionInputPlan {
    pub fn requests(&self) -> &[StreamRequest; 24] {
        &self.requests
    }
    pub fn total_bytes(&self) -> usize {
        self.total_bytes
    }
}
fn word(bytes: &[u8; 544], at: usize) -> u32 {
    u32::from_be_bytes([bytes[at], bytes[at + 1], bytes[at + 2], bytes[at + 3]])
}
fn half(bytes: &[u8; 544], at: usize) -> u16 {
    u16::from_be_bytes([bytes[at], bytes[at + 1]])
}
/// Plan source requests directly from the asset header and real semantic bases.
/// No child descriptor/template, output buffer, allocator or hole is an input.
/// Unsupported list controls remain an explicit variant to implement. Legacy
/// worker scratch limits and paired/chunk-size admission predicates are not
/// imposed on raw source copying; codec qualification remains separate.
pub fn plan_noninstanced_inputs(
    partition: &PartitionSource,
    bases: NonInstancedSourceBases,
    byte_budget: usize,
) -> Result<PartitionInputPlan, PartitionInputError> {
    let header = partition.bytes();
    let mut raw: [(u32, u32); 24] =
        core::array::from_fn(|slot| (word(header, 0x30 + 8 * slot), word(header, 0x34 + 8 * slot)));
    raw[3].1 = bases
        .palette_base
        .wrapping_add(u32::from(half(header, 0x14)));
    raw[4].1 = bases
        .palette_base
        .wrapping_add(u32::from(half(header, 0x16)));
    for (slot, entry) in raw.iter_mut().enumerate().take(13).skip(10) {
        entry.0 = u32::from(half(header, 0x0a + 2 * (slot - 10)));
    }
    for entry in &mut raw[13..17] {
        *entry = (0, bases.extra_stream);
    }
    raw[17].1 = bases.environment;
    raw[18] = (0, 0);
    let mut requests = [StreamRequest {
        address: 0,
        bytes: 0,
    }; 24];
    let mut total = 0usize;
    for (slot, (size, address)) in raw.into_iter().enumerate() {
        if size >> 16 != 0 {
            return Err(PartitionInputError::UnsupportedSourceControl {
                slot,
                control: size,
            });
        }
        let bytes = size as usize;
        if bytes != 0 && u64::from(address) + bytes as u64 > 1u64 << 32 {
            return Err(PartitionInputError::InvalidRange { slot });
        }
        total = total
            .checked_add(bytes)
            .filter(|&value| value <= byte_budget)
            .ok_or(PartitionInputError::BudgetExceeded)?;
        requests[slot] = StreamRequest { address, bytes };
    }
    Ok(PartitionInputPlan {
        requests,
        total_bytes: total,
    })
}
#[derive(Debug)]
pub struct OwnedPackedInput {
    source_address: u32,
    bytes: Vec<u8>,
}
impl OwnedPackedInput {
    pub fn source_address(&self) -> u32 {
        self.source_address
    }
    pub fn bytes(&self) -> &[u8] {
        &self.bytes
    }
}
#[derive(Debug)]
pub struct OwnedPartitionInputs {
    inputs: [OwnedPackedInput; 24],
}
impl OwnedPartitionInputs {
    pub fn inputs(&self) -> &[OwnedPackedInput; 24] {
        &self.inputs
    }
}
/// Own the complete planned readset before exposing any bundle. The reader
/// supplies owned checked bytes under actual source lifetimes; the callback
/// itself grants no completion, streaming generation or scene-epoch proof.
/// Empty inputs retain their source addresses and issue no fake source read.
pub fn copy_noninstanced_inputs(
    partition: &PartitionSource,
    bases: NonInstancedSourceBases,
    byte_budget: usize,
    reader: &mut impl FnMut(StreamRequest) -> Result<Vec<u8>, Error>,
) -> Result<OwnedPartitionInputs, PartitionInputError> {
    let plan = plan_noninstanced_inputs(partition, bases, byte_budget)?;
    copy_planned_inputs(&plan, reader)
}
fn copy_planned_inputs(
    plan: &PartitionInputPlan,
    reader: &mut impl FnMut(StreamRequest) -> Result<Vec<u8>, Error>,
) -> Result<OwnedPartitionInputs, PartitionInputError> {
    let mut inputs = core::array::from_fn(|slot| OwnedPackedInput {
        source_address: plan.requests[slot].address,
        bytes: Vec::new(),
    });
    for (slot, (input, &request)) in inputs.iter_mut().zip(plan.requests.iter()).enumerate() {
        if request.bytes == 0 {
            continue;
        }
        let bytes = reader(request).map_err(|error| PartitionInputError::Source { slot, error })?;
        if bytes.len() != request.bytes {
            return Err(PartitionInputError::ReadExtent {
                slot,
                expected: request.bytes,
                actual: bytes.len(),
            });
        }
        input.bytes = bytes;
    }
    Ok(OwnedPartitionInputs { inputs })
}

/// Encoded RSX source mapping, not a native buffer or allocation generation.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct IndexStreamSource {
    pub location: u8,
    pub offset: u32,
    pub bytes: usize,
}
#[derive(Clone, Debug)]
pub struct GroupZeroInputPlan {
    pub first_instance: u32,
    pub instances: u8,
    pub index: IndexStreamSource,
    pub inputs: PartitionInputPlan,
}
/// Derive Main group-zero batch sources from actual owned mesh/transform/header
/// inputs. No original parent-derived register, child descriptor or scratch seed
/// supplies a source. The unproven extra-stream location variant is explicit.
pub fn plan_group_zero_inputs(
    partition: &PartitionSource,
    mesh: &MeshSource,
    transform: &[u8; 112],
    resource_base: u32,
    environment: u32,
    batch_ordinal: u32,
    byte_budget: usize,
) -> Result<GroupZeroInputPlan, PartitionInputError> {
    if mesh.material_group() != 0 {
        return Err(PartitionInputError::WrongDispatch);
    }
    let field = |at: usize| {
        u32::from_be_bytes([
            transform[at],
            transform[at + 1],
            transform[at + 2],
            transform[at + 3],
        ])
    };
    let extra = field(0x60);
    if extra & 0x8000_0000 == 0 {
        return Err(PartitionInputError::UnresolvedInstanceLocation);
    }
    let has_rows = transform[0x6c] != 0;
    let total = if has_rows {
        u32::from(transform[0x6c])
    } else {
        1
    };
    let maximum = if has_rows {
        word(partition.bytes(), 0xfc)
    } else {
        1
    };
    if maximum == 0 {
        return Err(PartitionInputError::InvalidInstanceBatch);
    }
    let first = batch_ordinal
        .checked_mul(maximum)
        .filter(|&n| n < total)
        .ok_or(PartitionInputError::InvalidInstanceBatch)?;
    let count = (total - first).min(maximum);
    let instances = u8::try_from(count).map_err(|_| PartitionInputError::InvalidInstanceBatch)?;
    let bank = resource_base.wrapping_add(extra & 0x7fff_ffff);
    let mut inputs = plan_noninstanced_inputs(
        partition,
        NonInstancedSourceBases {
            palette_base: 0,
            extra_stream: bank,
            environment,
        },
        usize::MAX,
    )?;
    if has_rows {
        let start = u64::from(bank) + u64::from(first) * 16;
        let end = start + u64::from(count) * 16;
        // Preserve guest wrapping for an empty one-past address. The actual
        // nonempty row span must still fit without crossing the guest boundary.
        if end > 1u64 << 32 {
            return Err(PartitionInputError::InvalidRange { slot: 13 });
        }
        inputs.requests[13] = StreamRequest {
            address: start as u32,
            bytes: count as usize * 16,
        };
        for request in &mut inputs.requests[14..17] {
            *request = StreamRequest {
                address: end as u32,
                bytes: 0,
            };
        }
        inputs.total_bytes = inputs
            .total_bytes
            .checked_add(count as usize * 16)
            .ok_or(PartitionInputError::BudgetExceeded)?;
    }
    if inputs.total_bytes > byte_budget {
        return Err(PartitionInputError::BudgetExceeded);
    }
    let payload = field(0x64);
    let offset = (payload & 0x7fff_ffff)
        .checked_add(first * 4)
        .filter(|&n| u64::from(n) + u64::from(count) * 4 <= 1u64 << 31)
        .ok_or(PartitionInputError::IndexMappingRange)?;
    Ok(GroupZeroInputPlan {
        first_instance: first,
        instances,
        index: IndexStreamSource {
            location: (payload >> 31) as u8,
            offset,
            bytes: count as usize * 4,
        },
        inputs,
    })
}
#[derive(Debug)]
pub struct OwnedGroupZeroInputs {
    pub first_instance: u32,
    pub instances: u8,
    pub index: IndexStreamSource,
    pub inputs: OwnedPartitionInputs,
}
pub fn copy_group_zero_inputs(
    plan: GroupZeroInputPlan,
    reader: &mut impl FnMut(StreamRequest) -> Result<Vec<u8>, Error>,
) -> Result<OwnedGroupZeroInputs, PartitionInputError> {
    let inputs = copy_planned_inputs(&plan.inputs, reader)?;
    Ok(OwnedGroupZeroInputs {
        first_instance: plan.first_instance,
        instances: plan.instances,
        index: plan.index,
        inputs,
    })
}
