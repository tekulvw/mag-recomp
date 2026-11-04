//! Exact immutable FP translation retention; dynamic inline values stay per draw.
use super::{CompileError, EffectiveShaderControls, TechniquePrograms, compiler};
use compiler::native_compile::CacheLimits;
use std::{
    hash::{Hash, Hasher},
    sync::Arc,
};
pub(super) struct DecodedFragment {
    pub program: compiler::HlslProgram,
    pub outputs: Option<[u8; 16]>,
    /// Native lowering of exactly `program`, per lowering kind. Lowering is a
    /// deterministic function of the program text, so a warm decode reuses it
    /// instead of re-hashing and comparing the full text in the native cache.
    pub native: super::NativeMemo<2>,
}
impl DecodedFragment {
    pub(super) fn source(program: compiler::HlslProgram) -> Self {
        Self {
            program,
            outputs: None,
            native: Default::default(),
        }
    }
}
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct FragmentDecodeStats {
    pub hits: u64,
    pub decode_invocations: u64,
    pub evictions: u64,
}
#[derive(PartialEq, Eq, Hash)]
struct Controls {
    shader: u32,
    texcoords: [u32; 10],
    cube: u32,
    volume: u32,
    shadow: [[u8; 4]; 16],
    gamma: [u8; 16],
    expand: [u8; 16],
    alpha: Option<u32>,
    srgb: bool,
    grid: [u32; 2],
    unnormalized: u32,
    dimensions: [[u32; 2]; 16],
    unlit_color: bool,
}
impl Controls {
    fn new(source: &TechniquePrograms, c: &EffectiveShaderControls, unlit_color: bool) -> Self {
        Self {
            shader: source.fragment.shader_control,
            texcoords: source.fragment.texcoord_controls,
            cube: c.cube_mask,
            volume: c.volume_mask,
            shadow: c
                .shadow
                .map(|s| [s.enabled, s.function, s.linear, s.unnormalized]),
            gamma: c.gamma,
            expand: c.expand,
            alpha: c.alpha_function,
            srgb: c.srgb_output,
            grid: c.sample_grid,
            unnormalized: c.unnormalized_mask,
            dimensions: c.texture_dimensions,
            unlit_color,
        }
    }
}
struct Entry {
    bytes: Vec<u8>,
    controls: Controls,
    // Only immutable Unsupported is memoized as a negative capability result.
    // Invalid source, allocation, values and native failures never enter here.
    result: Option<Arc<DecodedFragment>>,
    payload: usize,
    touched: u64,
}
pub(super) struct FragmentDecodeCache {
    buckets: crate::fast_hash::FastHashMap<u64, Vec<Entry>>,
    limits: CacheLimits,
    pub(super) entries: usize,
    pub(super) bytes: usize,
    clock: u64,
    pub(super) stats: FragmentDecodeStats,
}
impl FragmentDecodeCache {
    pub(super) fn new(limits: CacheLimits) -> Self {
        Self {
            buckets: Default::default(),
            limits,
            entries: 0,
            bytes: 0,
            clock: 0,
            stats: FragmentDecodeStats::default(),
        }
    }
    pub(super) fn clear(&mut self) {
        self.buckets.clear();
        self.entries = 0;
        self.bytes = 0;
        self.clock = 0;
    }
    fn evict(&mut self) {
        if let Some((_, hash, index)) = self
            .buckets
            .iter()
            .flat_map(|(&hash, entries)| {
                entries
                    .iter()
                    .enumerate()
                    .map(move |(i, e)| (e.touched, hash, i))
            })
            .min()
        {
            let bucket = self.buckets.get_mut(&hash).expect("live FP bucket");
            let removed = bucket.swap_remove(index);
            self.bytes -= removed.payload;
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
        bytes: &[u8],
        c: &EffectiveShaderControls,
        unlit_color: bool,
        decode: impl FnOnce() -> Result<DecodedFragment, CompileError>,
    ) -> Result<Arc<DecodedFragment>, CompileError> {
        if self.clock == u64::MAX {
            self.clear();
        }
        self.clock += 1;
        let controls = Controls::new(source, c, unlit_color);
        let mut hash = crate::fast_hash::FastHasher::default();
        (2u32, bytes, &controls).hash(&mut hash);
        let hash = hash.finish();
        if let Some(e) = self.buckets.get_mut(&hash).and_then(|b| {
            b.iter_mut()
                .find(|e| e.bytes == bytes && e.controls == controls)
        }) {
            e.touched = self.clock;
            self.stats.hits = self.stats.hits.saturating_add(1);
            return e
                .result
                .as_ref()
                .map(Arc::clone)
                .ok_or(CompileError::Decode(compiler::ProgramError::Unsupported));
        }
        self.stats.decode_invocations = self.stats.decode_invocations.saturating_add(1);
        let result = match decode() {
            Ok(program) => Some(Arc::new(program)),
            Err(CompileError::Decode(compiler::ProgramError::Unsupported)) => None,
            Err(error) => return Err(error),
        };
        let return_result = || {
            result
                .as_ref()
                .map(Arc::clone)
                .ok_or(CompileError::Decode(compiler::ProgramError::Unsupported))
        };
        let bytes = bytes.to_vec();
        let payload = bytes
            .capacity()
            .saturating_add(result.as_ref().map_or(0, |p| p.program.source.capacity()));
        if self.limits.entries == 0 || payload > self.limits.retained_payload_bytes {
            return return_result();
        }
        while self.entries >= self.limits.entries
            || self.bytes > self.limits.retained_payload_bytes - payload
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
            bytes,
            controls,
            result: result.as_ref().map(Arc::clone),
            payload,
            touched: self.clock,
        });
        self.entries += 1;
        self.bytes += payload;
        return_result()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::material_compile::poc_unlit_tests::{controls, source};
    fn decoded() -> Result<DecodedFragment, CompileError> {
        Ok(DecodedFragment::source(compiler::HlslProgram {
            source: "owned-emission".into(),
            instructions: 1,
            constant_slots: 0,
            input_mask: 0,
            current_attribute_mask: 0,
            constant_buffer_bytes: 0,
        }))
    }
    #[test]
    fn exact_source_controls_and_policy_are_keyed_before_decode() {
        let (source, bytes) = source();
        let c = controls();
        let mut cache = FragmentDecodeCache::new(CacheLimits {
            entries: 64,
            retained_payload_bytes: 65536,
        });
        let a = cache.get(&source, &bytes, &c, false, decoded).unwrap();
        let b = cache
            .get(&source, &bytes, &c, false, || {
                panic!("warm FP must not decode")
            })
            .unwrap();
        assert!(Arc::ptr_eq(&a, &b));
        let mut variants = Vec::new();
        for index in 0..12 {
            let mut c = c.clone();
            match index {
                0 => c.cube_mask = 1,
                1 => c.volume_mask = 1,
                2 => c.shadow[3].function = 1,
                3 => c.gamma[2] = 1,
                4 => c.expand[4] = 1,
                5 => c.alpha_function = Some(1),
                6 => c.srgb_output = true,
                7 => c.sample_grid = [2, 1],
                8 => c.unnormalized_mask = 4,
                9 => c.texture_dimensions[1] = [2, 3],
                10 => c.shadow[0].linear = 1,
                _ => c.shadow[0].enabled = 1,
            };
            variants.push(c);
        }
        for c in variants {
            cache.get(&source, &bytes, &c, false, decoded).unwrap();
        }
        cache.get(&source, &bytes, &c, true, decoded).unwrap();
        let mut changed = source.clone();
        changed.fragment.shader_control ^= 1;
        cache.get(&changed, &bytes, &c, false, decoded).unwrap();
        changed = source.clone();
        changed.fragment.texcoord_controls[1] ^= 1;
        cache.get(&changed, &bytes, &c, false, decoded).unwrap();
        let mut changed_bytes = bytes.clone();
        changed_bytes[0] ^= 1;
        cache
            .get(&source, &changed_bytes, &c, false, decoded)
            .unwrap();
        assert_eq!(cache.stats.hits, 1);
        assert_eq!(cache.stats.decode_invocations, 17);
        assert!(cache.bytes <= 65536);
    }
    #[test]
    fn failure_not_cached_eviction_and_clear_preserve_bounds() {
        let (source, bytes) = source();
        let c = controls();
        let mut cache = FragmentDecodeCache::new(CacheLimits {
            entries: 1,
            retained_payload_bytes: 1024,
        });
        for _ in 0..2 {
            assert!(
                cache
                    .get(&source, &bytes, &c, false, || Err(
                        CompileError::SourceExtent
                    ))
                    .is_err()
            );
        }
        cache.get(&source, &bytes, &c, false, decoded).unwrap();
        cache.get(&source, &bytes, &c, true, decoded).unwrap();
        assert_eq!(cache.entries, 1);
        assert_eq!(cache.stats.evictions, 1);
        assert_eq!(cache.stats.decode_invocations, 4);
        cache.clear();
        assert_eq!((cache.bytes, cache.entries), (0, 0));
        let mut tiny = FragmentDecodeCache::new(CacheLimits {
            entries: 0,
            retained_payload_bytes: 0,
        });
        tiny.get(&source, &bytes, &c, false, decoded).unwrap();
        assert_eq!(tiny.entries, 0);
    }
    /// A per-frame working set of distinct fragment byte strings larger than
    /// the retention re-decodes every entry every frame under LRU; retention
    /// sized for the working set decodes each once and returns the same result.
    #[test]
    fn retention_sized_for_the_frame_working_set_stops_steady_state_decodes() {
        let (source, bytes) = source();
        let c = controls();
        let distinct: Vec<Vec<u8>> = (0..300u32)
            .map(|i| {
                let mut b = bytes.clone();
                b[16..20].copy_from_slice(&i.to_be_bytes());
                b
            })
            .collect();
        let decoded_for = |b: &[u8]| {
            let tag = u32::from_be_bytes(b[16..20].try_into().unwrap());
            move || {
                Ok(DecodedFragment::source(compiler::HlslProgram {
                    source: format!("owned-emission-{tag}"),
                    instructions: 1,
                    constant_slots: 0,
                    input_mask: 0,
                    current_attribute_mask: 0,
                    constant_buffer_bytes: 0,
                }))
            }
        };
        let frame = |cache: &mut FragmentDecodeCache| {
            let before = cache.stats.decode_invocations;
            let out: Vec<String> = distinct
                .iter()
                .map(|b| {
                    cache
                        .get(&source, b, &c, false, decoded_for(b))
                        .unwrap()
                        .program
                        .source
                        .clone()
                })
                .collect();
            (cache.stats.decode_invocations - before, out)
        };
        let mut shared = FragmentDecodeCache::new(CacheLimits {
            entries: 256,
            retained_payload_bytes: 8 * 1024 * 1024,
        });
        let mut sized = FragmentDecodeCache::new(CacheLimits {
            entries: 4096,
            retained_payload_bytes: 64 * 1024 * 1024,
        });
        let (_, first) = frame(&mut shared);
        assert_eq!(frame(&mut shared).0, 300);
        assert_eq!(frame(&mut sized).0, 300);
        let (warm, out) = frame(&mut sized);
        assert_eq!(warm, 0);
        assert_eq!(out, first);
    }
    #[test]
    fn immutable_unsupported_specialization_is_bounded_and_not_redecoded_warm() {
        let (source, bytes) = source();
        let c = controls();
        let mut cache = FragmentDecodeCache::new(CacheLimits {
            entries: 2,
            retained_payload_bytes: 1024,
        });
        assert!(
            cache
                .get(&source, &bytes, &c, true, || Err(CompileError::Decode(
                    compiler::ProgramError::Unsupported
                )))
                .is_err()
        );
        assert!(
            cache
                .get(&source, &bytes, &c, true, || panic!(
                    "unchanged unsupported semantic form must be memoized"
                ))
                .is_err()
        );
        assert_eq!(cache.stats.decode_invocations, 1);
        assert_eq!(cache.stats.hits, 1);
        assert_eq!(cache.entries, 1);
        assert!(cache.bytes <= 1024);
        assert!(cache.get(&source, &bytes, &c, false, decoded).is_ok());
        assert_eq!(cache.stats.decode_invocations, 2);
    }
}
