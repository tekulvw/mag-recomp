//! Bounded CPU compiler output retention, independent of devices and frames.
use super::{BufferBinding, NativeProgram, SamplerBinding, Stage, TextureBinding, lower};
use crate::HlslProgram;
use std::{
    collections::HashMap,
    hash::{DefaultHasher, Hash, Hasher},
    mem::size_of,
    sync::Arc,
};
#[derive(Clone, Copy, Debug)]
pub struct CacheLimits {
    pub entries: usize,
    /// Retained source/output allocation capacities. Hash-table/entry overhead
    /// is separately bounded by `entries`; temporary compiler memory is not
    /// retained here and follows the existing compiler input/output bounds.
    pub retained_payload_bytes: usize,
}
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct CacheStats {
    pub hits: u64,
    pub compilations: u64,
    pub evictions: u64,
}
struct Entry {
    source: String,
    stage: Stage,
    uniform_bytes: u32,
    result: Arc<NativeProgram>,
    payload_bytes: usize,
    touched: u64,
}
/// Exact content comparison follows hashing, so neither address reuse nor a
/// hash collision can select another shader. Uniform VALUES are intentionally
/// absent: the caller owns them per draw; only the reflected layout is keyed.
/// Eviction releases this cache's Arc, never an existing scene's ownership.
pub struct NativeCompilerCache {
    buckets: HashMap<u64, Vec<Entry>>,
    limits: CacheLimits,
    entries: usize,
    payload_bytes: usize,
    clock: u64,
    stats: CacheStats,
}
impl NativeCompilerCache {
    pub fn new(limits: CacheLimits) -> Self {
        Self {
            buckets: HashMap::new(),
            limits,
            entries: 0,
            payload_bytes: 0,
            clock: 0,
            stats: CacheStats::default(),
        }
    }
    pub fn stats(&self) -> CacheStats {
        self.stats
    }
    pub fn retained_entries(&self) -> usize {
        self.entries
    }
    pub fn retained_payload_bytes(&self) -> usize {
        self.payload_bytes
    }
    pub fn clear(&mut self) {
        self.buckets.clear();
        self.entries = 0;
        self.payload_bytes = 0;
    }
    fn evict_oldest(&mut self) {
        let oldest = self
            .buckets
            .iter()
            .flat_map(|(&hash, entries)| {
                entries
                    .iter()
                    .enumerate()
                    .map(move |(i, e)| (e.touched, hash, i))
            })
            .min();
        if let Some((_, hash, index)) = oldest {
            let bucket = self.buckets.get_mut(&hash).expect("selected live bucket");
            let removed = bucket.swap_remove(index);
            self.payload_bytes -= removed.payload_bytes;
            self.entries -= 1;
            self.stats.evictions = self.stats.evictions.saturating_add(1);
            if bucket.is_empty() {
                self.buckets.remove(&hash);
            }
        }
    }
    pub fn compile(
        &mut self,
        program: &HlslProgram,
        stage: Stage,
    ) -> Result<Arc<NativeProgram>, String> {
        if self.clock == u64::MAX {
            self.clear();
            self.clock = 0;
        }
        self.clock += 1;
        let mut hasher = DefaultHasher::new();
        (
            stage,
            program.constant_buffer_bytes,
            program.source.as_str(),
        )
            .hash(&mut hasher);
        let hash = hasher.finish();
        if let Some(entry) = self.buckets.get_mut(&hash).and_then(|b| {
            b.iter_mut().find(|e| {
                e.stage == stage
                    && e.uniform_bytes == program.constant_buffer_bytes
                    && e.source == program.source
            })
        }) {
            entry.touched = self.clock;
            self.stats.hits = self.stats.hits.saturating_add(1);
            return Ok(Arc::clone(&entry.result));
        }
        self.stats.compilations = self.stats.compilations.saturating_add(1);
        let result = Arc::new(lower(program, stage)?);
        let mut source = String::new();
        source
            .try_reserve_exact(program.source.len())
            .map_err(|_| "compiler cache allocation")?;
        source.push_str(&program.source);
        let payload_bytes = source
            .capacity()
            .saturating_add(result.msl.capacity())
            .saturating_add(result.msl_entry.capacity())
            .saturating_add(result.hlsl.capacity())
            .saturating_add(result.hlsl_entry.capacity())
            .saturating_add(result.spirv_entry.capacity())
            .saturating_add(result.spirv.capacity() * size_of::<u32>())
            .saturating_add(result.buffers.capacity() * size_of::<BufferBinding>())
            .saturating_add(result.textures.capacity() * size_of::<u32>())
            .saturating_add(result.samplers.capacity() * size_of::<u32>())
            .saturating_add(result.texture_bindings.capacity() * size_of::<TextureBinding>())
            .saturating_add(result.sampler_bindings.capacity() * size_of::<SamplerBinding>())
            .saturating_add(result.vertex_locations.capacity() * size_of::<u32>());
        if self.limits.entries == 0 || payload_bytes > self.limits.retained_payload_bytes {
            return Ok(result);
        }
        while self.entries >= self.limits.entries
            || self.payload_bytes > self.limits.retained_payload_bytes - payload_bytes
        {
            self.evict_oldest();
        }
        self.buckets
            .try_reserve(1)
            .map_err(|_| "compiler cache allocation")?;
        let bucket = self.buckets.entry(hash).or_default();
        bucket
            .try_reserve(1)
            .map_err(|_| "compiler cache allocation")?;
        bucket.push(Entry {
            source,
            stage,
            uniform_bytes: program.constant_buffer_bytes,
            result: Arc::clone(&result),
            payload_bytes,
            touched: self.clock,
        });
        self.entries += 1;
        self.payload_bytes += payload_bytes;
        Ok(result)
    }
}
