//! Owned raw mesh assets selected by a retained visibility cohort.
//!
//! Partition headers are mesh-asset data, not original child-job descriptors.
//! This decoder neither builds child DMA lists nor qualifies resource lifetime,
//! deformation, shader inputs, draw execution, or original-work removal.
use crate::{error::Error, guest_memory::GuestSnapshot};

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum MeshSourceError {
    NullMesh,
    Snapshot(Error),
    BudgetExceeded,
    Cycle,
    AllocationFailed,
    ReadExtent { expected: usize, actual: usize },
}
fn word(bytes: &[u8], at: usize) -> u32 {
    u32::from_be_bytes([bytes[at], bytes[at + 1], bytes[at + 2], bytes[at + 3]])
}
pub struct MeshSource {
    source_address: u32,
    bytes: [u8; 192],
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum GeometrySource {
    StaticPartitions {
        first: u32,
        count: u8,
    },
    SkinnedPartitions {
        first: u32,
        count: u8,
        palette_address: u32,
    },
    /// Still has material, vertex-state and draw responsibilities.
    DirectDraw,
}
/// Main's source dispatch is distinct from ordinary Depth dispatch. This only
/// identifies required source work; it does not prepare or execute a draw.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum MainGeometrySource {
    GroupZeroPartitions {
        first: u32,
        recorded_count: u8,
    },
    StaticPartitions {
        first: u32,
        recorded_count: u8,
    },
    SkinnedPartitions {
        first: u32,
        recorded_count: u8,
        palette_address: u32,
    },
    DirectDraw {
        instanced: bool,
    },
}
impl MeshSource {
    pub fn copy(snapshot: &GuestSnapshot<'_>, address: u32) -> Result<Self, MeshSourceError> {
        if address == 0 {
            return Err(MeshSourceError::NullMesh);
        }
        let words = snapshot
            .words::<48>(u64::from(address))
            .map_err(MeshSourceError::Snapshot)?;
        let mut bytes = [0; 192];
        for (target, word) in bytes.as_chunks_mut::<4>().0.iter_mut().zip(words) {
            target.copy_from_slice(&word.to_be_bytes());
        }
        Ok(Self {
            source_address: address,
            bytes,
        })
    }
    pub fn source_address(&self) -> u32 {
        self.source_address
    }
    pub fn bytes(&self) -> &[u8; 192] {
        &self.bytes
    }
    pub fn vertex_layout_address(&self) -> u32 {
        word(&self.bytes, 0x3c)
    }
    pub fn material_group(&self) -> u16 {
        u16::from_be_bytes([self.bytes[0xbc], self.bytes[0xbd]])
    }
    /// Main's group-zero path wins independently of the static/skinning flags.
    /// The other instanced-direct route is selected by the exact source control
    /// byte, not a nonzero test. Recorded count remains metadata, not a chain
    /// length invariant.
    pub fn main_geometry_source(
        &self,
        primary_material_flags: u32,
        item: &[u8; 32],
        transform_control_6f: u8,
        resource_base: u32,
    ) -> MainGeometrySource {
        let first = (word(&self.bytes, 0x50) & 0x7fff_ffff).wrapping_add(resource_base);
        let recorded_count = self.bytes[0xa2];
        if self.material_group() == 0 {
            MainGeometrySource::GroupZeroPartitions {
                first,
                recorded_count,
            }
        } else if word(&self.bytes, 0x9c) & 0x1000 != 0 {
            MainGeometrySource::StaticPartitions {
                first,
                recorded_count,
            }
        } else if transform_control_6f != 1 && primary_material_flags & 0x20 != 0 {
            let palette_address = if item[27] & 1 == 0 {
                word(item, 20) & !15
            } else {
                0
            };
            MainGeometrySource::SkinnedPartitions {
                first,
                recorded_count,
                palette_address,
            }
        } else {
            MainGeometrySource::DirectDraw {
                instanced: transform_control_6f == 1,
            }
        }
    }
    /// Established ordinary Depth dispatch only. Main group-zero instanced
    /// geometry uses main_geometry_source; Shadow still needs its own rule.
    pub fn depth_geometry_source(
        &self,
        primary_material_flags: u32,
        item: &[u8; 32],
        resource_base: u32,
    ) -> GeometrySource {
        let first = (word(&self.bytes, 0x50) & 0x7fff_ffff).wrapping_add(resource_base);
        let count = self.bytes[0xa2];
        if word(&self.bytes, 0x9c) & 0x1000 != 0 {
            GeometrySource::StaticPartitions { first, count }
        } else if primary_material_flags & 0x20 != 0 {
            let palette_address = if item[27] & 1 == 0 {
                word(item, 20) & !15
            } else {
                0
            };
            GeometrySource::SkinnedPartitions {
                first,
                count,
                palette_address,
            }
        } else {
            GeometrySource::DirectDraw
        }
    }
}
#[derive(Debug)]
pub struct PartitionSource {
    pub source_address: u32,
    bytes: [u8; 544],
}
impl PartitionSource {
    pub fn bytes(&self) -> &[u8; 544] {
        &self.bytes
    }
}
#[derive(Debug)]
pub struct PartitionChain {
    partitions: Vec<PartitionSource>,
}
impl PartitionChain {
    /// Follow the actual links under an explicit native header budget and own
    /// all headers before returning. Mesh byte 0xA2 is metadata, not a proven
    /// chain-length invariant. Copying headers does not acquire their streams.
    pub fn copy(
        snapshot: &GuestSnapshot<'_>,
        first: u32,
        header_budget: usize,
    ) -> Result<Self, MeshSourceError> {
        Self::copy_with_reader(first, header_budget, &mut |address| {
            snapshot.copy_bytes(u64::from(address), 544)
        })
    }
    /// Follow sparse live source reads synchronously into owned headers. The
    /// caller supplies the actual read lifetime; this function creates none.
    /// Every extent and native traversal budget is checked before issuing a
    /// request. No guest pointer or callback is retained in the result.
    pub fn copy_with_reader(
        first: u32,
        header_budget: usize,
        reader: &mut impl FnMut(u32) -> Result<Vec<u8>, Error>,
    ) -> Result<Self, MeshSourceError> {
        let mut partitions: Vec<PartitionSource> = Vec::new();
        let mut address = first;
        while address != 0 {
            if partitions.iter().any(|p| p.source_address == address) {
                return Err(MeshSourceError::Cycle);
            }
            if partitions.len() >= header_budget {
                return Err(MeshSourceError::BudgetExceeded);
            }
            if u64::from(address) + 544 > 1u64 << 32 {
                return Err(MeshSourceError::Snapshot(Error::InvalidRange));
            }
            let read = reader(address).map_err(MeshSourceError::Snapshot)?;
            let actual = read.len();
            let bytes: [u8; 544] = read.try_into().map_err(|_| MeshSourceError::ReadExtent {
                expected: 544,
                actual,
            })?;
            let next = word(&bytes, 0xf0);
            partitions
                .try_reserve(1)
                .map_err(|_| MeshSourceError::AllocationFailed)?;
            partitions.push(PartitionSource {
                source_address: address,
                bytes,
            });
            address = next;
        }
        Ok(Self { partitions })
    }
    pub fn partitions(&self) -> &[PartitionSource] {
        &self.partitions
    }
}
