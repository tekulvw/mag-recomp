//! Immutable source decode retention owned by the existing per-VM compiler.
//! Values, viewport and constant/current knowledge are never cached as values.
use super::{CompileError, EffectiveShaderControls, TechniquePrograms, compiler, native_compile};
use native_compile::{CacheLimits, CacheStats, NativeProgram, Stage};
use std::{
    hash::{Hash, Hasher},
    mem::size_of,
    sync::Arc,
};

const DECODE_POLICY_VERSION: u32 = 2;
pub(super) struct DecodedVertex {
    pub program: compiler::HlslProgram,
    pub reads: compiler::VertexConstantReads,
    pub approximated_outputs: bool,
    /// Native lowering of exactly `program` (vertex lowering has one kind).
    pub native: super::NativeMemo<1>,
}
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct VertexDecodeStats {
    pub hits: u64,
    /// Actual immutable decode/slice/read-reflection builds, including failures.
    pub decode_invocations: u64,
    pub evictions: u64,
}
#[derive(Clone, Copy, PartialEq, Eq, Hash)]
pub(super) enum VertexPolicy {
    Full,
    Position,
    Outputs([u8; 16]),
}
struct Entry {
    instructions: Vec<[u32; 4]>,
    load_slot: u16,
    textures: u32,
    arrays: u16,
    known: u16,
    policy: VertexPolicy,
    result: Arc<DecodedVertex>,
    bytes: usize,
    touched: u64,
}
pub(super) struct VertexDecodeCache {
    buckets: crate::fast_hash::FastHashMap<u64, Vec<Entry>>,
    limits: CacheLimits,
    entries: usize,
    bytes: usize,
    clock: u64,
    stats: VertexDecodeStats,
}
impl VertexDecodeCache {
    fn new(limits: CacheLimits) -> Self {
        Self {
            buckets: Default::default(),
            limits,
            entries: 0,
            bytes: 0,
            clock: 0,
            stats: VertexDecodeStats::default(),
        }
    }
    fn clear(&mut self) {
        self.buckets.clear();
        self.entries = 0;
        self.bytes = 0;
        self.clock = 0;
    }
    fn evict(&mut self) {
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
            let bucket = self.buckets.get_mut(&hash).expect("live bucket");
            let removed = bucket.swap_remove(index);
            self.bytes -= removed.bytes;
            self.entries -= 1;
            self.stats.evictions = self.stats.evictions.saturating_add(1);
            if bucket.is_empty() {
                self.buckets.remove(&hash);
            }
        }
    }
    pub(super) fn get(
        &mut self,
        source: &TechniquePrograms,
        controls: &EffectiveShaderControls,
        known: u16,
        poc: bool,
        decode: impl FnOnce() -> Result<DecodedVertex, CompileError>,
    ) -> Result<Arc<DecodedVertex>, CompileError> {
        self.get_policy(
            source,
            controls,
            known,
            if poc {
                VertexPolicy::Position
            } else {
                VertexPolicy::Full
            },
            decode,
        )
    }
    pub(super) fn get_policy(
        &mut self,
        source: &TechniquePrograms,
        controls: &EffectiveShaderControls,
        known: u16,
        policy: VertexPolicy,
        decode: impl FnOnce() -> Result<DecodedVertex, CompileError>,
    ) -> Result<Arc<DecodedVertex>, CompileError> {
        if self.clock == u64::MAX {
            self.clear();
        }
        self.clock += 1;
        let mut hash = crate::fast_hash::FastHasher::default();
        (
            DECODE_POLICY_VERSION,
            source.vertex.instructions.as_slice(),
            source.vertex.load_slot,
            controls.vertex_texture_mask,
            controls.enabled_arrays,
            known,
            policy,
        )
            .hash(&mut hash);
        let hash = hash.finish();
        if let Some(entry) = self.buckets.get_mut(&hash).and_then(|b| {
            b.iter_mut().find(|e| {
                e.instructions == source.vertex.instructions
                    && e.load_slot == source.vertex.load_slot
                    && e.textures == controls.vertex_texture_mask
                    && e.arrays == controls.enabled_arrays
                    && e.known == known
                    && e.policy == policy
            })
        }) {
            entry.touched = self.clock;
            self.stats.hits = self.stats.hits.saturating_add(1);
            return Ok(Arc::clone(&entry.result));
        }
        self.stats.decode_invocations = self.stats.decode_invocations.saturating_add(1);
        let result = Arc::new(decode()?);
        let instructions = source.vertex.instructions.clone();
        let bytes = instructions
            .capacity()
            .saturating_mul(size_of::<[u32; 4]>())
            .saturating_add(result.program.source.capacity())
            .saturating_add(
                result
                    .reads
                    .rows
                    .capacity()
                    .saturating_mul(size_of::<u16>()),
            );
        if self.limits.entries == 0 || bytes > self.limits.retained_payload_bytes {
            return Ok(result);
        }
        while self.entries >= self.limits.entries
            || self.bytes > self.limits.retained_payload_bytes - bytes
        {
            self.evict();
        }
        self.buckets
            .try_reserve(1)
            .map_err(|_| CompileError::Allocation)?;
        let bucket = self.buckets.entry(hash).or_default();
        bucket
            .try_reserve(1)
            .map_err(|_| CompileError::Allocation)?;
        bucket.push(Entry {
            instructions,
            load_slot: source.vertex.load_slot,
            textures: controls.vertex_texture_mask,
            arrays: controls.enabled_arrays,
            known,
            policy,
            result: Arc::clone(&result),
            bytes,
            touched: self.clock,
        });
        self.entries += 1;
        self.bytes += bytes;
        Ok(result)
    }
}
/// One owner for both immutable cache layers, reset with the existing VM owner.
/// A quarter of the existing payload budget (at most8MiB) is reserved for each
/// source-decoding stage; the remaining native-output budget bounds the aggregate.
/// Each layer separately bounds entry overhead by the requested entry count.
pub struct NativeCompilerCache {
    pub(super) native: native_compile::NativeCompilerCache,
    pub(super) vertex: VertexDecodeCache,
    pub(super) fragment: super::FragmentDecodeCache,
    /// Opt-in cross-plan preparation reuse (`MAG_RENDER_PLAN_REUSE=1`).
    pub(super) reuse: super::plan_reuse::PlanReuse,
}
impl NativeCompilerCache {
    /// Whether cross-plan preparation reuse is on (default: environment).
    pub fn plan_reuse_enabled(&self) -> bool {
        self.reuse.enabled()
    }
    /// Turn cross-plan preparation reuse on or off for this cache (turning it
    /// off drops everything it retained). Results never depend on it.
    pub fn set_plan_reuse(&mut self, enabled: bool) {
        self.reuse.set_enabled(enabled);
    }
    pub fn plan_reuse_stats(&self) -> super::PlanReuseStats {
        self.reuse.stats
    }
    /// The exact identity of one technique's sources for reuse across plans,
    /// or None when reuse is off. Equal content always yields the same id.
    pub fn reuse_technique<'a>(
        &mut self,
        source: &'a TechniquePrograms,
        fragment: &'a [u8],
    ) -> Option<super::ReusedTechnique<'a>> {
        self.reuse.technique(source, fragment)
    }
    pub fn new(limits: CacheLimits) -> Self {
        let decode_bytes = (limits.retained_payload_bytes / 4).min(8 * 1024 * 1024);
        Self {
            native: native_compile::NativeCompilerCache::new(CacheLimits {
                entries: limits.entries,
                retained_payload_bytes: limits.retained_payload_bytes - 2 * decode_bytes,
            }),
            vertex: VertexDecodeCache::new(CacheLimits {
                entries: limits.entries,
                retained_payload_bytes: decode_bytes,
            }),
            fragment: super::FragmentDecodeCache::new(CacheLimits {
                entries: limits.entries,
                retained_payload_bytes: decode_bytes,
            }),
            reuse: super::plan_reuse::PlanReuse::new(),
        }
    }
    /// Same layers, with a separately sized fragment decode layer. Fragment
    /// decode entries are keyed by the exact source byte string, which embeds
    /// per-material inline constants, so one native program can need many
    /// decode entries. Only retention changes; results are identical.
    pub fn with_fragment_decode(limits: CacheLimits, fragment: CacheLimits) -> Self {
        let mut cache = Self::new(limits);
        cache.fragment = super::FragmentDecodeCache::new(fragment);
        cache
    }
    pub fn stats(&self) -> CacheStats {
        self.native.stats()
    }
    pub fn vertex_decode_stats(&self) -> VertexDecodeStats {
        self.vertex.stats
    }
    pub fn fragment_decode_stats(&self) -> super::FragmentDecodeStats {
        self.fragment.stats
    }
    pub fn retained_entries(&self) -> usize {
        self.native.retained_entries() + self.vertex.entries + self.fragment.entries
    }
    pub fn retained_payload_bytes(&self) -> usize {
        self.native.retained_payload_bytes() + self.vertex.bytes + self.fragment.bytes
    }
    pub fn clear(&mut self) {
        self.native.clear();
        self.vertex.clear();
        self.fragment.clear();
        self.reuse.clear();
    }
    pub fn compile(
        &mut self,
        program: &compiler::HlslProgram,
        stage: Stage,
    ) -> Result<Arc<NativeProgram>, String> {
        self.native.compile(program, stage)
    }
    pub fn compile_metal_stored_depth(
        &mut self,
        program: &compiler::HlslProgram,
    ) -> Result<Arc<NativeProgram>, String> {
        self.native.compile_metal_stored_depth(program)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn controls() -> EffectiveShaderControls {
        EffectiveShaderControls {
            vertex_texture_mask: 0,
            enabled_arrays: 1,
            cube_mask: 0,
            volume_mask: 0,
            shadow: [compiler::ShadowTexture::default(); 16],
            gamma: [0; 16],
            expand: [0; 16],
            alpha_function: None,
            srgb_output: false,
            sample_grid: [1, 1],
            unnormalized_mask: 0,
            texture_dimensions: [[1, 1]; 16],
        }
    }
    fn decoded() -> DecodedVertex {
        DecodedVertex {
            program: compiler::HlslProgram {
                source: "authored cache payload".into(),
                instructions: 1,
                constant_slots: 0,
                input_mask: 1,
                current_attribute_mask: 0,
                constant_buffer_bytes: 0,
            },
            native: Default::default(),
            reads: compiler::VertexConstantReads {
                rows: vec![7],
                indexed: false,
            },
            approximated_outputs: false,
        }
    }
    #[test]
    fn exact_source_controls_and_policy_are_cache_identity() {
        let (mut source, _) = super::super::poc_unlit_tests::source();
        let mut controls = controls();
        let mut cache = VertexDecodeCache::new(CacheLimits {
            entries: 16,
            retained_payload_bytes: 4096,
        });
        let a = cache
            .get(&source, &controls, 0, false, || Ok(decoded()))
            .unwrap();
        assert!(Arc::ptr_eq(
            &a,
            &cache
                .get(&source, &controls, 0, false, || panic!("must hit"))
                .unwrap()
        ));
        source.vertex.instructions[0][0] ^= 1;
        cache
            .get(&source, &controls, 0, false, || Ok(decoded()))
            .unwrap();
        source.vertex.load_slot += 1;
        cache
            .get(&source, &controls, 0, false, || Ok(decoded()))
            .unwrap();
        controls.vertex_texture_mask = 1;
        cache
            .get(&source, &controls, 0, false, || Ok(decoded()))
            .unwrap();
        controls.enabled_arrays = 2;
        cache
            .get(&source, &controls, 0, false, || Ok(decoded()))
            .unwrap();
        cache
            .get(&source, &controls, 1, false, || Ok(decoded()))
            .unwrap();
        cache
            .get(&source, &controls, 1, true, || Ok(decoded()))
            .unwrap();
        assert_eq!(cache.stats.decode_invocations, 7);
        assert_eq!(cache.stats.hits, 1);
    }
    #[test]
    fn eviction_clear_and_disabled_cache_preserve_live_owners_and_budget() {
        let (mut source, _) = super::super::poc_unlit_tests::source();
        let controls = controls();
        let mut cache = VertexDecodeCache::new(CacheLimits {
            entries: 1,
            retained_payload_bytes: 128,
        });
        let a = cache
            .get(&source, &controls, 0, false, || Ok(decoded()))
            .unwrap();
        source.vertex.instructions[0][0] ^= 1;
        cache
            .get(&source, &controls, 0, false, || Ok(decoded()))
            .unwrap();
        assert_eq!(cache.entries, 1);
        assert!(cache.bytes <= 128);
        assert_eq!(cache.stats.evictions, 1);
        assert_eq!(a.reads.rows, [7]);
        cache.clear();
        assert_eq!((cache.entries, cache.bytes), (0, 0));
        cache
            .get(&source, &controls, 0, false, || Ok(decoded()))
            .unwrap();
        assert_eq!(cache.stats.decode_invocations, 3);
        let mut disabled = VertexDecodeCache::new(CacheLimits {
            entries: 0,
            retained_payload_bytes: 128,
        });
        for _ in 0..2 {
            disabled
                .get(&source, &controls, 0, false, || Ok(decoded()))
                .unwrap();
        }
        assert_eq!(disabled.entries, 0);
        assert_eq!(disabled.stats.decode_invocations, 2);
        let bounded = NativeCompilerCache::new(CacheLimits {
            entries: 2,
            retained_payload_bytes: 128,
        });
        assert_eq!(
            bounded.native.retained_payload_bytes() + bounded.vertex.bytes,
            0
        );
        assert_eq!(bounded.vertex.limits.retained_payload_bytes, 32);
    }
    #[test]
    fn required_varying_lanes_are_exact_cache_identity() {
        let (source, _) = super::super::poc_unlit_tests::source();
        let c = controls();
        let mut cache = VertexDecodeCache::new(CacheLimits {
            entries: 8,
            retained_payload_bytes: 4096,
        });
        let mut outputs = [0; 16];
        outputs[0] = 15;
        outputs[7] = 3;
        let a = cache
            .get_policy(&source, &c, 0, VertexPolicy::Outputs(outputs), || {
                Ok(decoded())
            })
            .unwrap();
        assert!(Arc::ptr_eq(
            &a,
            &cache
                .get_policy(&source, &c, 0, VertexPolicy::Outputs(outputs), || panic!(
                    "must hit"
                ))
                .unwrap()
        ));
        outputs[11] = 8;
        let b = cache
            .get_policy(&source, &c, 0, VertexPolicy::Outputs(outputs), || {
                Ok(decoded())
            })
            .unwrap();
        assert!(!Arc::ptr_eq(&a, &b));
        cache.get(&source, &c, 0, true, || Ok(decoded())).unwrap();
        assert_eq!(cache.stats.decode_invocations, 3);
        assert_eq!(cache.stats.hits, 1);
    }
}
