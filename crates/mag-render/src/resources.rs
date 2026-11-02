//! Owned resource snapshots; never interprets guest memory or GPU completion.
use crate::types::{ResourceId, ResourceVersion};
use std::sync::{
    Arc, Weak,
    atomic::{AtomicU64, Ordering},
};
static NEXT_NAMESPACE: AtomicU64 = AtomicU64::new(1);

#[derive(Clone, Copy, Debug)]
/// Limits cover logical owned snapshot bytes and live/retained versions.
/// Allocator metadata, caller-owned deltas and backend storage are separate.
pub struct ResourceLimits {
    pub max_resources: usize,
    pub max_bytes: usize,
}
#[derive(Debug, PartialEq, Eq)]
pub enum ResourceError {
    InvalidLimits,
    EmptyResource,
    StaleVersion,
    InvalidRange,
    OverlappingRanges,
    CapacityExceeded,
    VersionExhausted,
    AllocationFailed,
    CounterOverflow,
    NamespaceExhausted,
}
#[derive(Debug)]
pub struct DirtyRange {
    pub offset: u64,
    pub bytes: Vec<u8>,
}
#[derive(Debug)]
pub enum ResourceDelta {
    Create {
        bytes: Vec<u8>,
    },
    Update {
        expected: ResourceVersion,
        ranges: Vec<DirtyRange>,
    },
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct DirtySpan {
    pub offset: usize,
    pub length: usize,
}
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
/// CPU ownership diagnostics, not upload or GPU statistics. `snapshot_allocations`
/// counts created/adopted snapshots, not individual allocator calls.
/// `bytes_copied` counts explicit whole-resource snapshot copies; allocator
/// relocation while trimming caller Vec capacity is outside that counter.
pub struct ResourceStats {
    pub resident_resources: usize,
    pub resident_bytes: usize,
    pub snapshot_allocations: u64,
    pub bytes_copied: u64,
    pub patch_bytes_written: u64,
}
/// Process-local non-reused resource owner domain; never reconstructed from a slot.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct ResourceNamespace(u64);
impl ResourceNamespace {
    pub fn get(self) -> u64 {
        self.0
    }
}
/// Upload/cache identity. Native device/context generation is an additional key.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct ResourceCacheKey {
    pub namespace: ResourceNamespace,
    pub resource_id: ResourceId,
    pub content_version: u64,
}
#[derive(Debug)]
struct Snapshot {
    namespace: ResourceNamespace,
    version: ResourceVersion,
    bytes: Box<[u8]>,
    dirty: Vec<DirtySpan>,
}
#[derive(Clone, Debug)]
pub struct ResourceLease {
    snapshot: Arc<Snapshot>,
}
impl ResourceLease {
    /// Include this full key, plus native device/context generation, in every
    /// shared upload cache. ResourceVersion alone is local to its ResourceStore.
    pub fn cache_key(&self) -> ResourceCacheKey {
        ResourceCacheKey {
            namespace: self.snapshot.namespace,
            resource_id: self.snapshot.version.id,
            content_version: self.snapshot.version.content_version,
        }
    }
    pub fn version(&self) -> ResourceVersion {
        self.snapshot.version
    }
    pub fn bytes(&self) -> &[u8] {
        &self.snapshot.bytes
    }
    /// Changes from the preceding content version (full range on creation).
    /// Consumers cache ResourceVersion; repeated leasing is not a new upload.
    pub fn dirty_ranges(&self) -> &[DirtySpan] {
        &self.snapshot.dirty
    }
}
struct Slot {
    generation: u32,
    current: Option<Arc<Snapshot>>,
}
/// Single-owner mutable store. Leases may be cloned/sent to other threads and
/// independently retain immutable bytes until their actual work is terminal.
/// max_resources includes *versions* retained by leases, not just live slots.
pub struct ResourceStore {
    namespace: ResourceNamespace,
    limits: ResourceLimits,
    slots: Vec<Slot>,
    retained: Vec<(u32, Weak<Snapshot>)>,
    counters: ResourceStats,
}
impl ResourceStore {
    pub fn new(limits: ResourceLimits) -> Result<Self, ResourceError> {
        if limits.max_resources == 0
            || limits.max_bytes == 0
            || limits.max_resources > u32::MAX as usize
        {
            return Err(ResourceError::InvalidLimits);
        }
        let namespace = ResourceNamespace(
            NEXT_NAMESPACE
                .try_update(Ordering::Relaxed, Ordering::Relaxed, |n| n.checked_add(1))
                .map_err(|_| ResourceError::NamespaceExhausted)?,
        );
        Ok(Self {
            namespace,
            limits,
            slots: Vec::new(),
            retained: Vec::new(),
            counters: ResourceStats::default(),
        })
    }
    pub fn apply(&mut self, delta: ResourceDelta) -> Result<ResourceVersion, ResourceError> {
        // Dead weak bookkeeping can be discarded without changing resource
        // identity, contents, capacity usage or observable allocation counters.
        self.retained.retain(|(_, weak)| weak.strong_count() != 0);
        match delta {
            ResourceDelta::Create { bytes } => self.create(bytes),
            ResourceDelta::Update { expected, ranges } => self.update(expected, ranges),
        }
    }
    fn create(&mut self, bytes: Vec<u8>) -> Result<ResourceVersion, ResourceError> {
        if bytes.is_empty() {
            return Err(ResourceError::EmptyResource);
        }
        self.reserve_capacity(1, bytes.len())?;
        let reuse = self
            .slots
            .iter()
            .enumerate()
            .find(|(i, slot)| {
                slot.current.is_none()
                    && slot.generation < u32::MAX
                    && !self
                        .retained
                        .iter()
                        .any(|(id, weak)| *id == *i as u32 && weak.strong_count() != 0)
            })
            .map(|(i, _)| i);
        let index = match reuse {
            Some(i) => i,
            None if self.slots.len() < self.limits.max_resources => {
                self.slots
                    .try_reserve(1)
                    .map_err(|_| ResourceError::AllocationFailed)?;
                self.slots.len()
            }
            None => return Err(ResourceError::CapacityExceeded),
        };
        let generation = if index < self.slots.len() {
            self.slots[index].generation + 1
        } else {
            1
        };
        let version = ResourceVersion {
            id: ResourceId {
                slot: index as u32,
                generation,
            },
            content_version: 1,
        };
        let allocations = self
            .counters
            .snapshot_allocations
            .checked_add(1)
            .ok_or(ResourceError::CounterOverflow)?;
        let length = bytes.len();
        // Boxed storage does not retain caller Vec spare capacity.
        let snapshot = Arc::new(Snapshot {
            namespace: self.namespace,
            version,
            bytes: bytes.into_boxed_slice(),
            dirty: vec![DirtySpan { offset: 0, length }],
        });
        let slot = Slot {
            generation,
            current: Some(snapshot),
        };
        if index == self.slots.len() {
            self.slots.push(slot);
        } else {
            self.slots[index] = slot;
        }
        self.counters.snapshot_allocations = allocations;
        Ok(version)
    }
    fn index(&self, expected: ResourceVersion) -> Result<usize, ResourceError> {
        let index = expected.id.slot as usize;
        match self.slots.get(index).and_then(|s| s.current.as_ref()) {
            Some(snapshot) if snapshot.version == expected => Ok(index),
            _ => Err(ResourceError::StaleVersion),
        }
    }
    fn update(
        &mut self,
        expected: ResourceVersion,
        mut ranges: Vec<DirtyRange>,
    ) -> Result<ResourceVersion, ResourceError> {
        let index = self.index(expected)?;
        let current = self.slots[index]
            .current
            .as_ref()
            .ok_or(ResourceError::StaleVersion)?;
        ranges.sort_unstable_by_key(|r| r.offset);
        let mut previous_end = 0;
        for range in &ranges {
            let start = usize::try_from(range.offset).map_err(|_| ResourceError::InvalidRange)?;
            let end = start
                .checked_add(range.bytes.len())
                .ok_or(ResourceError::InvalidRange)?;
            if range.bytes.is_empty() || end > current.bytes.len() {
                return Err(ResourceError::InvalidRange);
            }
            if start < previous_end {
                return Err(ResourceError::OverlappingRanges);
            }
            previous_end = end;
        }
        ranges.retain(|r| {
            current.bytes[r.offset as usize..r.offset as usize + r.bytes.len()] != r.bytes
        });
        if ranges.is_empty() {
            return Ok(expected);
        }
        let content_version = expected
            .content_version
            .checked_add(1)
            .ok_or(ResourceError::VersionExhausted)?;
        let version = ResourceVersion {
            id: expected.id,
            content_version,
        };
        let shared = Arc::strong_count(current) > 1;
        if shared {
            self.reserve_capacity(1, current.bytes.len())?;
        }
        let mut dirty = Vec::new();
        dirty
            .try_reserve_exact(ranges.len())
            .map_err(|_| ResourceError::AllocationFailed)?;
        let mut written = 0u64;
        for range in &ranges {
            written = written
                .checked_add(range.bytes.len() as u64)
                .ok_or(ResourceError::CounterOverflow)?;
            dirty.push(DirtySpan {
                offset: range.offset as usize,
                length: range.bytes.len(),
            });
        }
        let mut counters = self.counters;
        counters.patch_bytes_written = counters
            .patch_bytes_written
            .checked_add(written)
            .ok_or(ResourceError::CounterOverflow)?;
        if shared {
            counters.snapshot_allocations = counters
                .snapshot_allocations
                .checked_add(1)
                .ok_or(ResourceError::CounterOverflow)?;
            counters.bytes_copied = counters
                .bytes_copied
                .checked_add(current.bytes.len() as u64)
                .ok_or(ResourceError::CounterOverflow)?;
            self.retained
                .try_reserve(1)
                .map_err(|_| ResourceError::AllocationFailed)?;
            let mut bytes = Vec::new();
            bytes
                .try_reserve_exact(current.bytes.len())
                .map_err(|_| ResourceError::AllocationFailed)?;
            bytes.extend_from_slice(&current.bytes);
            for range in ranges {
                let start = range.offset as usize;
                bytes[start..start + range.bytes.len()].copy_from_slice(&range.bytes);
            }
            let snapshot = Arc::new(Snapshot {
                namespace: current.namespace,
                version,
                bytes: bytes.into_boxed_slice(),
                dirty,
            });
            self.retained.push((index as u32, Arc::downgrade(current)));
            self.slots[index].current = Some(snapshot);
        } else {
            // No external Arc or Weak can point at a current unleased version.
            // All validation/reservations precede the first byte write.
            let current = Arc::get_mut(
                self.slots[index]
                    .current
                    .as_mut()
                    .ok_or(ResourceError::StaleVersion)?,
            )
            .ok_or(ResourceError::StaleVersion)?;
            for range in ranges {
                let start = range.offset as usize;
                current.bytes[start..start + range.bytes.len()].copy_from_slice(&range.bytes);
            }
            current.version = version;
            current.dirty = dirty;
        }
        self.counters = counters;
        Ok(version)
    }
    /// Lease the exact current version; retain/clone this lease for in-flight work.
    pub fn lease(&self, expected: ResourceVersion) -> Result<ResourceLease, ResourceError> {
        let index = self.index(expected)?;
        Ok(ResourceLease {
            snapshot: Arc::clone(
                self.slots[index]
                    .current
                    .as_ref()
                    .ok_or(ResourceError::StaleVersion)?,
            ),
        })
    }
    /// Retire the store owner, without pretending outstanding work completed.
    /// Every content-version lease must drop before this slot can be reused.
    pub fn retire(&mut self, expected: ResourceVersion) -> Result<(), ResourceError> {
        let index = self.index(expected)?;
        self.retained.retain(|(_, weak)| weak.strong_count() != 0);
        let current = self.slots[index]
            .current
            .as_ref()
            .ok_or(ResourceError::StaleVersion)?;
        if Arc::strong_count(current) > 1 {
            self.retained
                .try_reserve(1)
                .map_err(|_| ResourceError::AllocationFailed)?;
            self.retained.push((index as u32, Arc::downgrade(current)));
        }
        self.slots[index].current = None;
        Ok(())
    }
    /// Exact logical owned snapshot bytes/versions; excludes allocator metadata,
    /// caller-owned deltas and backend storage. Dropped leases release capacity
    /// immediately; no polling or simulated GPU completion is involved.
    pub fn stats(&self) -> ResourceStats {
        let mut stats = self.counters;
        for current in self.slots.iter().filter_map(|s| s.current.as_ref()) {
            stats.resident_resources += 1;
            stats.resident_bytes += current.bytes.len();
        }
        for (_, weak) in &self.retained {
            if let Some(snapshot) = weak.upgrade() {
                stats.resident_resources += 1;
                stats.resident_bytes += snapshot.bytes.len();
            }
        }
        stats
    }
    fn reserve_capacity(&self, resources: usize, bytes: usize) -> Result<(), ResourceError> {
        let stats = self.stats();
        if stats
            .resident_resources
            .checked_add(resources)
            .is_none_or(|n| n > self.limits.max_resources)
            || stats
                .resident_bytes
                .checked_add(bytes)
                .is_none_or(|n| n > self.limits.max_bytes)
        {
            Err(ResourceError::CapacityExceeded)
        } else {
            Ok(())
        }
    }
}
