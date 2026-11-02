//! SDK-independent ordered GPU dataflow. CPU uploads are immutable inputs;
//! produced values are batch-local and every output has exactly one writer.
use crate::resources::{ResourceCacheKey, ResourceLease};
use std::{
    collections::{BTreeMap, BTreeSet},
    sync::atomic::{AtomicU64, Ordering},
};
static NEXT_BATCH: AtomicU64 = AtomicU64::new(1);
#[derive(Clone, Copy, Debug)]
pub struct BatchLimits {
    pub max_commands: usize,
    pub max_gpu_buffers: usize,
    pub max_gpu_bytes: u64,
    pub max_uploads: usize,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum BatchError {
    InvalidLimits,
    Exhausted,
    Capacity,
    InvalidRange,
    WrongBatch,
    ReadBeforeWrite,
    MultipleWriters,
    Alias,
    UnknownPipeline,
    WrongOwner,
    InvalidBindings,
    MissingDependency,
    Empty,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ByteRange {
    pub offset: u64,
    pub length: u64,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct GpuBuffer {
    batch: u64,
    slot: usize,
    bytes: u64,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct GpuValue {
    buffer: GpuBuffer,
    producer: usize,
    epoch: u64,
}
impl GpuValue {
    pub fn byte_length(self) -> u64 {
        self.buffer.bytes
    }
    pub fn producer(self) -> usize {
        self.producer
    }
    pub fn epoch(self) -> u64 {
        self.epoch
    }
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Upload {
    batch: u64,
    slot: usize,
    bytes: u64,
    key: ResourceCacheKey,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ReadSource {
    Upload(Upload),
    Produced(GpuValue),
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Binding {
    Read {
        slot: u32,
        source: ReadSource,
        range: ByteRange,
    },
    Write {
        slot: u32,
        buffer: GpuBuffer,
        range: ByteRange,
    },
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Access {
    Read,
    ReadWrite,
}
#[derive(Clone, Copy, Debug)]
pub struct BindingRequirement {
    pub slot: u32,
    pub access: Access,
    pub minimum_bytes: u64,
    pub alignment: u64,
}
/// Native owner implements this from its actual compiled pipeline registry;
/// neither caller metadata nor a prepared batch authenticates a shader/device.
pub trait PipelineCatalog {
    fn owner_generation(&self) -> u64;
    fn bindings(&self, pipeline: u64) -> Option<&[BindingRequirement]>;
}
#[derive(Clone, Debug)]
pub struct Dispatch {
    pub pipeline: u64,
    pub bindings: Vec<Binding>,
    pub groups: [u32; 3],
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Dependency {
    pub buffer: GpuBuffer,
    pub producer: usize,
    pub consumer: usize,
    pub range: ByteRange,
}
#[derive(Clone, Copy, Debug)]
pub struct GpuBufferPlan {
    pub byte_length: u64,
    pub zero_initialize: bool,
}
struct Allocation {
    plan: GpuBufferPlan,
    writer: Option<usize>,
}
pub struct BatchBuilder {
    id: u64,
    owner: u64,
    limits: BatchLimits,
    gpu_bytes: u64,
    allocations: Vec<Allocation>,
    uploads: Vec<ResourceLease>,
    dispatches: Vec<Dispatch>,
    dependencies: Vec<Dependency>,
}
pub struct PreparedBatch {
    pub(crate) id: u64,
    pub(crate) owner: u64,
    pub(crate) buffers: Vec<GpuBufferPlan>,
    pub(crate) uploads: Vec<ResourceLease>,
    pub(crate) dispatches: Vec<Dispatch>,
    pub(crate) dependencies: Vec<Dependency>,
}
impl PreparedBatch {
    pub fn owner_generation(&self) -> u64 {
        self.owner
    }
    pub fn buffers(&self) -> &[GpuBufferPlan] {
        &self.buffers
    }
    pub fn uploads(&self) -> &[ResourceLease] {
        &self.uploads
    }
    pub fn dispatches(&self) -> &[Dispatch] {
        &self.dispatches
    }
    pub fn dependencies(&self) -> &[Dependency] {
        &self.dependencies
    }
    pub fn batch_id(&self) -> u64 {
        self.id
    }
}
impl BatchBuilder {
    pub fn new(owner: u64, limits: BatchLimits) -> Result<Self, BatchError> {
        if owner == 0
            || limits.max_commands == 0
            || limits.max_gpu_buffers == 0
            || limits.max_gpu_bytes == 0
            || limits.max_uploads == 0
        {
            return Err(BatchError::InvalidLimits);
        }
        let id = NEXT_BATCH
            .try_update(Ordering::Relaxed, Ordering::Relaxed, |n| n.checked_add(1))
            .map_err(|_| BatchError::Exhausted)?;
        Ok(Self {
            id,
            owner,
            limits,
            gpu_bytes: 0,
            allocations: vec![],
            uploads: vec![],
            dispatches: vec![],
            dependencies: vec![],
        })
    }
    pub fn upload(&mut self, lease: ResourceLease) -> Result<Upload, BatchError> {
        let key = lease.cache_key();
        if let Some(slot) = self.uploads.iter().position(|l| l.cache_key() == key) {
            return Ok(Upload {
                batch: self.id,
                slot,
                bytes: lease.bytes().len() as u64,
                key,
            });
        }
        if self.uploads.len() >= self.limits.max_uploads {
            return Err(BatchError::Capacity);
        }
        self.uploads
            .try_reserve(1)
            .map_err(|_| BatchError::Capacity)?;
        let upload = Upload {
            batch: self.id,
            slot: self.uploads.len(),
            bytes: lease.bytes().len() as u64,
            key,
        };
        self.uploads.push(lease);
        Ok(upload)
    }
    pub fn allocate(&mut self, bytes: u64) -> Result<GpuBuffer, BatchError> {
        if bytes == 0 {
            return Err(BatchError::InvalidRange);
        }
        let total = self
            .gpu_bytes
            .checked_add(bytes)
            .ok_or(BatchError::Capacity)?;
        if total > self.limits.max_gpu_bytes
            || self.allocations.len() >= self.limits.max_gpu_buffers
        {
            return Err(BatchError::Capacity);
        }
        self.allocations
            .try_reserve(1)
            .map_err(|_| BatchError::Capacity)?;
        let buffer = GpuBuffer {
            batch: self.id,
            slot: self.allocations.len(),
            bytes,
        };
        self.allocations.push(Allocation {
            plan: GpuBufferPlan {
                byte_length: bytes,
                zero_initialize: true,
            },
            writer: None,
        });
        self.gpu_bytes = total;
        Ok(buffer)
    }
    fn allocation(&self, buffer: GpuBuffer) -> Result<&Allocation, BatchError> {
        if buffer.batch != self.id {
            return Err(BatchError::WrongBatch);
        }
        self.allocations
            .get(buffer.slot)
            .filter(|a| a.plan.byte_length == buffer.bytes)
            .ok_or(BatchError::WrongBatch)
    }
    pub fn value(&self, buffer: GpuBuffer) -> Result<GpuValue, BatchError> {
        let producer = self
            .allocation(buffer)?
            .writer
            .ok_or(BatchError::ReadBeforeWrite)?;
        Ok(GpuValue {
            buffer,
            producer,
            epoch: producer as u64 + 1,
        })
    }
    fn read(
        &self,
        source: ReadSource,
        range: ByteRange,
        node: usize,
    ) -> Result<Option<Dependency>, BatchError> {
        match source {
            ReadSource::Upload(upload) => {
                if upload.batch != self.id
                    || self.uploads.get(upload.slot).is_none_or(|l| {
                        l.cache_key() != upload.key || l.bytes().len() as u64 != upload.bytes
                    })
                {
                    return Err(BatchError::WrongBatch);
                }
                check_range(range, upload.bytes)?;
                Ok(None)
            }
            ReadSource::Produced(value) => {
                let actual = self.value(value.buffer)?;
                if actual != value || value.producer >= node {
                    return Err(BatchError::ReadBeforeWrite);
                }
                check_range(range, value.buffer.bytes)?;
                Ok(Some(Dependency {
                    buffer: value.buffer,
                    producer: value.producer,
                    consumer: node,
                    range: ByteRange {
                        offset: 0,
                        length: value.buffer.bytes,
                    },
                }))
            }
        }
    }
    pub fn dispatch(
        &mut self,
        pipeline: u64,
        bindings: Vec<Binding>,
        groups: [u32; 3],
    ) -> Result<Vec<GpuValue>, BatchError> {
        if bindings.is_empty() || groups.contains(&0) {
            return Err(BatchError::InvalidBindings);
        }
        let node = self.dispatches.len();
        let mut reads = BTreeSet::new();
        let mut writes = BTreeMap::new();
        let mut deps = BTreeMap::new();
        let mut slots = BTreeSet::new();
        for binding in &bindings {
            let slot = match binding {
                Binding::Read {
                    slot,
                    source,
                    range,
                } => {
                    if let Some(dep) = self.read(*source, *range, node)? {
                        reads.insert(dep.buffer.slot);
                        deps.insert(dep.buffer.slot, dep);
                    }
                    *slot
                }
                Binding::Write {
                    slot,
                    buffer,
                    range,
                } => {
                    self.allocation(*buffer)?;
                    check_range(*range, buffer.bytes)?;
                    if writes.insert(buffer.slot, *buffer).is_some() {
                        return Err(BatchError::Alias);
                    }
                    *slot
                }
            };
            if !slots.insert(slot) {
                return Err(BatchError::InvalidBindings);
            }
        }
        if reads.iter().any(|slot| writes.contains_key(slot)) {
            return Err(BatchError::Alias);
        }
        if writes
            .values()
            .any(|b| self.allocations[b.slot].writer.is_some())
        {
            return Err(BatchError::MultipleWriters);
        }
        let count = self
            .dispatches
            .len()
            .checked_add(self.dependencies.len())
            .and_then(|n| n.checked_add(deps.len()))
            .and_then(|n| n.checked_add(1))
            .ok_or(BatchError::Capacity)?;
        if count > self.limits.max_commands {
            return Err(BatchError::Capacity);
        }
        self.dispatches
            .try_reserve(1)
            .map_err(|_| BatchError::Capacity)?;
        self.dependencies
            .try_reserve(deps.len())
            .map_err(|_| BatchError::Capacity)?;
        let mut values = Vec::new();
        values
            .try_reserve(writes.len())
            .map_err(|_| BatchError::Capacity)?;
        for buffer in writes.values() {
            values.push(GpuValue {
                buffer: *buffer,
                producer: node,
                epoch: node as u64 + 1,
            });
        }
        for buffer in writes.values() {
            self.allocations[buffer.slot].writer = Some(node);
        }
        self.dependencies.extend(deps.into_values());
        self.dispatches.push(Dispatch {
            pipeline,
            bindings,
            groups,
        });
        Ok(values)
    }
    pub fn prepare(self, catalog: &impl PipelineCatalog) -> Result<PreparedBatch, BatchError> {
        if catalog.owner_generation() != self.owner {
            return Err(BatchError::WrongOwner);
        }
        if self.dispatches.is_empty() {
            return Err(BatchError::Empty);
        }
        let mut expected = Vec::new();
        for (node, dispatch) in self.dispatches.iter().enumerate() {
            let requirements = catalog
                .bindings(dispatch.pipeline)
                .ok_or(BatchError::UnknownPipeline)?;
            if requirements.len() != dispatch.bindings.len() {
                return Err(BatchError::InvalidBindings);
            }
            let mut slots = BTreeSet::new();
            let mut deps = BTreeMap::new();
            for binding in &dispatch.bindings {
                let (slot, range, access) = match binding {
                    Binding::Read {
                        slot,
                        source,
                        range,
                    } => {
                        if let Some(dep) = self.read(*source, *range, node)? {
                            deps.insert(dep.buffer.slot, dep);
                        }
                        (*slot, *range, Access::Read)
                    }
                    Binding::Write {
                        slot,
                        buffer,
                        range,
                    } => {
                        if self.allocation(*buffer)?.writer != Some(node) {
                            return Err(BatchError::MultipleWriters);
                        }
                        check_range(*range, buffer.bytes)?;
                        (*slot, *range, Access::ReadWrite)
                    }
                };
                if !slots.insert(slot) {
                    return Err(BatchError::InvalidBindings);
                }
                let requirement = requirements
                    .iter()
                    .find(|r| r.slot == slot)
                    .ok_or(BatchError::InvalidBindings)?;
                if access != requirement.access
                    || range.length < requirement.minimum_bytes
                    || !requirement.alignment.is_power_of_two()
                    || range.offset % requirement.alignment != 0
                {
                    return Err(BatchError::InvalidBindings);
                }
            }
            expected.extend(deps.into_values());
        }
        if expected != self.dependencies {
            return Err(BatchError::MissingDependency);
        }
        Ok(PreparedBatch {
            id: self.id,
            owner: self.owner,
            buffers: self.allocations.into_iter().map(|a| a.plan).collect(),
            uploads: self.uploads,
            dispatches: self.dispatches,
            dependencies: self.dependencies,
        })
    }
}
fn check_range(range: ByteRange, length: u64) -> Result<(), BatchError> {
    if range.length == 0
        || range
            .offset
            .checked_add(range.length)
            .is_none_or(|end| end > length)
    {
        Err(BatchError::InvalidRange)
    } else {
        Ok(())
    }
}
