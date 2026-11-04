//! Content-qualified immutable component decoding shared by all source families.
//! Cache retention is additional to caller/source snapshots and GPU allocations.
//! Every caller still copies current source bytes under its genuine read lease.
use crate::{
    geometry_bindings,
    geometry_streams::{self, GeometryStreamError, PositionEncoding},
    shared_scene_sources::{self, SourceContent},
};
use std::sync::{Arc, Mutex, OnceLock};
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct CacheLimits {
    pub record: usize,
    pub total: usize,
    pub entries: usize,
}
impl Default for CacheLimits {
    fn default() -> Self {
        Self {
            record: 32 * 1024 * 1024,
            total: 64 * 1024 * 1024,
            entries: 4096,
        }
    }
}
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum RowCodec {
    Float32x3,
    Rebased16x3,
    Descriptor { ordinal: usize, phase: u8 },
}
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
enum Kind {
    Rows(RowCodec),
    Indices,
    /// Every binding field that `decode_auxiliary_attribute` reads.
    Auxiliary {
        format: u8,
        components: u8,
        stride: u8,
        frequency: u16,
        modulo: bool,
    },
}
#[derive(Clone, Copy, Debug, Default)]
pub struct CacheStats {
    pub hits: u64,
    pub misses: u64,
    pub evictions: u64,
    pub retained_bytes: usize,
    pub entries: usize,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
struct Key {
    kind: Kind,
    count: usize,
    format: u64,
    source: u64,
}
struct Entry {
    touched: u64,
    _format: Arc<SourceContent>,
    _source: Arc<SourceContent>,
    value: Value,
    bytes: usize,
}
enum Value {
    Rows(Arc<Vec<[u32; 4]>>),
    Indices(Arc<Vec<u16>>),
}
#[derive(Default)]
pub struct DecodedGeometryCache {
    entries: crate::fast_hash::FastHashMap<Key, Entry>,
    clock: u64,
    bytes: usize,
    stats: CacheStats,
    last_limits: Option<CacheLimits>,
}
impl DecodedGeometryCache {
    fn evict_oldest(&mut self) -> bool {
        let Some(key) = self
            .entries
            .iter()
            .min_by_key(|(_, entry)| entry.touched)
            .map(|(key, _)| *key)
        else {
            return false;
        };
        let old = self.entries.remove(&key).expect("existing victim");
        self.bytes -= old.bytes;
        self.stats.evictions = self.stats.evictions.saturating_add(1);
        true
    }
    fn stamp(&mut self) -> u64 {
        if self.clock == u64::MAX {
            self.clear();
            self.clock = 0;
        }
        self.clock += 1;
        self.clock
    }
    fn trim(&mut self, limits: CacheLimits) {
        if self.last_limits == Some(limits) {
            return;
        }
        self.last_limits = Some(limits);
        self.entries.retain(|_, entry| {
            if entry.bytes <= limits.record {
                return true;
            }
            self.bytes -= entry.bytes;
            self.stats.evictions = self.stats.evictions.saturating_add(1);
            false
        });
        while self.entries.len() > limits.entries || self.bytes > limits.total {
            if !self.evict_oldest() {
                break;
            }
        }
    }
    fn find(
        &mut self,
        kind: Kind,
        format: &Arc<SourceContent>,
        source: &Arc<SourceContent>,
        count: usize,
    ) -> Option<Value> {
        let key = Key {
            kind,
            count,
            format: format.identity,
            source: source.identity,
        };
        // Hash lookup and a scalar touch only on hits; victim selection scans
        // the bounded map solely when actual retention eviction is required.
        let touched = self.stamp();
        let entry = self.entries.get_mut(&key)?;
        entry.touched = touched;
        self.stats.hits = self.stats.hits.saturating_add(1);
        Some(match &entry.value {
            Value::Rows(rows) => Value::Rows(rows.clone()),
            Value::Indices(rows) => Value::Indices(rows.clone()),
        })
    }
    fn insert(
        &mut self,
        kind: Kind,
        format: Arc<SourceContent>,
        source: Arc<SourceContent>,
        count: usize,
        value: Value,
        limits: CacheLimits,
    ) {
        let output = match &value {
            Value::Rows(r) => r.capacity().checked_mul(16),
            Value::Indices(r) => r.capacity().checked_mul(2),
        };
        let Some(bytes) = output
            .and_then(|n| n.checked_add(format.bytes().len()))
            .and_then(|n| n.checked_add(source.bytes().len()))
            // Charge record/vector/source Arc metadata as well as the payload.
            // Encoded content shared by several records is conservatively
            // charged once per record; no fictitious free cache headroom.
            .and_then(|n| {
                n.checked_add(
                    size_of::<Entry>()
                        + size_of::<Key>()
                        + 2 * size_of::<usize>()
                        + size_of::<Vec<u16>>()
                        + 2 * size_of::<SourceContent>()
                        + 6 * size_of::<usize>(),
                )
            })
        else {
            return;
        };
        if bytes > limits.record || bytes > limits.total || limits.entries == 0 {
            return;
        }
        while self.entries.len() >= limits.entries || self.bytes > limits.total - bytes {
            if !self.evict_oldest() {
                break;
            }
        }
        let touched = self.stamp();
        self.entries.insert(
            Key {
                kind,
                count,
                format: format.identity,
                source: source.identity,
            },
            Entry {
                touched,
                _format: format,
                _source: source,
                value,
                bytes,
            },
        );
        self.bytes += bytes;
    }
    pub fn rows(
        &mut self,
        codec: RowCodec,
        format: &[u8],
        source: &[u8],
        count: usize,
        budget: usize,
        limits: CacheLimits,
    ) -> Result<Arc<Vec<[u32; 4]>>, GeometryStreamError> {
        self.rows_with(codec, None, format, source, count, budget, limits)
    }
    /// `interned` must be `intern_pair(format, source)` of these same slices;
    /// it only saves re-interning, never changes the result.
    #[allow(clippy::too_many_arguments)]
    fn rows_with(
        &mut self,
        codec: RowCodec,
        interned: Option<&InternedPair<'_>>,
        format: &[u8],
        source: &[u8],
        count: usize,
        budget: usize,
        limits: CacheLimits,
    ) -> Result<Arc<Vec<[u32; 4]>>, GeometryStreamError> {
        self.trim(limits);
        if source
            .len()
            .checked_add(format.len())
            .is_none_or(|n| n > limits.record)
        {
            self.stats.misses = self.stats.misses.saturating_add(1);
            return decode_rows(codec, format, source, count, budget).map(Arc::new);
        }
        let content = match interned {
            // The pair borrows these exact slices (same address and length,
            // immutably borrowed), so its contents are these bytes.
            Some(pair)
                if std::ptr::eq(pair.source_slice, source)
                    && std::ptr::eq(pair.format_slice, format) =>
            {
                Some((pair.source.clone(), pair.format.clone()))
            }
            _ => shared_scene_sources::retain_content(source)
                .ok()
                .zip(shared_scene_sources::retain_content(format).ok()),
        };
        if let Some((source, format)) = &content
            && let Some(Value::Rows(rows)) = self.find(Kind::Rows(codec), format, source, count)
        {
            if rows.len().checked_mul(16).is_none_or(|n| n > budget) {
                return Err(GeometryStreamError::BudgetExceeded);
            }
            return Ok(rows);
        }
        self.stats.misses = self.stats.misses.saturating_add(1);
        let rows = Arc::new(decode_rows(codec, format, source, count, budget)?);
        if let Some((source, format)) = content {
            self.insert(
                Kind::Rows(codec),
                format,
                source,
                count,
                Value::Rows(rows.clone()),
                limits,
            )
        }
        Ok(rows)
    }
    pub fn indices(
        &mut self,
        source: &[u8],
        count: usize,
        budget: usize,
        limits: CacheLimits,
    ) -> Result<Arc<Vec<u16>>, GeometryStreamError> {
        self.indices_known(source, None, count, budget, limits)
    }
    fn indices_known(
        &mut self,
        source: &[u8],
        known: Option<&Arc<SourceContent>>,
        count: usize,
        budget: usize,
        limits: CacheLimits,
    ) -> Result<Arc<Vec<u16>>, GeometryStreamError> {
        self.trim(limits);
        if source.len() > limits.record {
            self.stats.misses = self.stats.misses.saturating_add(1);
            return geometry_streams::decode_indices(source, count, budget).map(Arc::new);
        }
        let content = known
            .cloned()
            .or_else(|| shared_scene_sources::retain_content(source).ok())
            .zip(shared_scene_sources::retain_content(&[]).ok());
        if let Some((source, format)) = &content
            && let Some(Value::Indices(rows)) = self.find(Kind::Indices, format, source, count)
        {
            if rows.len().checked_mul(2).is_none_or(|n| n > budget) {
                return Err(GeometryStreamError::BudgetExceeded);
            }
            return Ok(rows);
        }
        self.stats.misses = self.stats.misses.saturating_add(1);
        let rows = Arc::new(geometry_streams::decode_indices(source, count, budget)?);
        if let Some((source, format)) = content {
            self.insert(
                Kind::Indices,
                format,
                source,
                count,
                Value::Indices(rows.clone()),
                limits,
            )
        }
        Ok(rows)
    }
    pub fn auxiliary(
        &mut self,
        binding: &geometry_bindings::AuxiliaryBinding,
        source: &[u8],
        count: usize,
        budget: usize,
        limits: CacheLimits,
    ) -> Result<Arc<Vec<[u32; 4]>>, geometry_bindings::BindingError> {
        self.trim(limits);
        let decode = || {
            geometry_bindings::decode_auxiliary_attribute(binding, source, count, budget)
                .map(Arc::new)
        };
        if source.len() > limits.record {
            self.stats.misses = self.stats.misses.saturating_add(1);
            return decode();
        }
        let kind = Kind::Auxiliary {
            format: binding.binding.format,
            components: binding.binding.components,
            stride: binding.binding.stride,
            frequency: binding.frequency,
            modulo: binding.modulo,
        };
        let content = shared_scene_sources::retain_content(source)
            .ok()
            .zip(shared_scene_sources::retain_content(&[]).ok());
        if let Some((source, format)) = &content
            && let Some(Value::Rows(rows)) = self.find(kind, format, source, count)
        {
            // The same refusal the decoder makes before touching any byte.
            if count.checked_mul(16).is_none_or(|n| n > budget) {
                return Err(geometry_bindings::BindingError::Budget);
            }
            return Ok(rows);
        }
        self.stats.misses = self.stats.misses.saturating_add(1);
        let rows = decode()?;
        if let Some((source, format)) = content {
            self.insert(
                kind,
                format,
                source,
                count,
                Value::Rows(rows.clone()),
                limits,
            )
        }
        Ok(rows)
    }
    pub fn clear(&mut self) {
        self.entries = Default::default();
        self.bytes = 0;
    }
    pub fn stats(&self) -> CacheStats {
        CacheStats {
            retained_bytes: self.bytes,
            entries: self.entries.len(),
            ..self.stats
        }
    }
}
fn decode_rows(
    codec: RowCodec,
    format: &[u8],
    source: &[u8],
    count: usize,
    budget: usize,
) -> Result<Vec<[u32; 4]>, GeometryStreamError> {
    match codec {
        RowCodec::Float32x3 => {
            geometry_streams::decode_positions(source, count, PositionEncoding::Float32x3, budget)
        }
        RowCodec::Rebased16x3 => {
            geometry_streams::decode_positions(source, count, PositionEncoding::Rebased16x3, budget)
        }
        RowCodec::Descriptor { ordinal, phase } => {
            geometry_streams::decode_descriptor_declaration_at(
                format,
                source,
                u32::from(phase),
                ordinal,
                count,
                budget,
            )
        }
    }
}
/// The cache with its lock held only for lookup and insert (concurrent
/// capture caches). Same outcome as the locked methods: an oversized record
/// is decoded uncached, a hit reproduces the decoder's budget refusal, and a
/// miss decodes and inserts. Two threads missing the same key both decode the
/// same content to the same value; the later insert replaces an equal entry.
#[allow(clippy::too_many_arguments)]
fn cached_unlocked<T: Clone, E>(
    kind: Kind,
    oversize: bool,
    content: impl FnOnce() -> Option<(Arc<SourceContent>, Arc<SourceContent>)>,
    count: usize,
    take: fn(&Value) -> Option<T>,
    on_hit: impl FnOnce(T) -> Result<T, E>,
    decode: impl FnOnce() -> Result<T, E>,
    put: fn(T) -> Value,
) -> Result<T, E> {
    let limits = CacheLimits::default();
    let cache = CACHE.get_or_init(|| Mutex::new(DecodedGeometryCache::default()));
    let content = if oversize { None } else { content() };
    let found = cache.lock().ok().and_then(|mut c| {
        c.trim(limits);
        let found = content
            .as_ref()
            .and_then(|(source, format)| c.find(kind, format, source, count))
            .and_then(|v| take(&v));
        if found.is_none() {
            c.stats.misses = c.stats.misses.saturating_add(1);
        }
        found
    });
    if let Some(value) = found {
        return on_hit(value);
    }
    let value = decode()?;
    if let Some((source, format)) = content
        && let Ok(mut c) = cache.lock()
    {
        c.insert(kind, format, source, count, put(value.clone()), limits);
    }
    Ok(value)
}
fn take_rows(v: &Value) -> Option<Arc<Vec<[u32; 4]>>> {
    match v {
        Value::Rows(r) => Some(r.clone()),
        Value::Indices(_) => None,
    }
}
fn take_indices(v: &Value) -> Option<Arc<Vec<u16>>> {
    match v {
        Value::Indices(r) => Some(r.clone()),
        Value::Rows(_) => None,
    }
}
static CACHE: OnceLock<Mutex<DecodedGeometryCache>> = OnceLock::new();
/// Lookups that found the cache busy and decoded without it.
static BUSY: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);
pub fn busy_decodes() -> u64 {
    BUSY.load(std::sync::atomic::Ordering::Relaxed)
}
/// The locked-mode cache. A busy cache is skipped (and counted) as before;
/// with `PS3_RENDER_CAPTURE_VIEW_VALIDATION=1` the lookup waits for it
/// instead, so concurrent capture threads do not decode content again.
fn locked_cache() -> Option<std::sync::MutexGuard<'static, DecodedGeometryCache>> {
    let cache = CACHE.get_or_init(|| Mutex::new(DecodedGeometryCache::default()));
    if crate::source_acquisition::view_validation_enabled() {
        return cache.lock().ok();
    }
    match cache.try_lock() {
        Ok(guard) => Some(guard),
        Err(std::sync::TryLockError::WouldBlock) => {
            BUSY.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
            None
        }
        Err(std::sync::TryLockError::Poisoned(_)) => None,
    }
}
pub fn enabled() -> bool {
    static ENABLED: OnceLock<bool> = OnceLock::new();
    *ENABLED.get_or_init(|| {
        std::env::var_os("PS3_RENDER_DECODED_GEOMETRY_CACHE").is_some_and(|v| v == "1")
    })
}
/// Exact catalog contents of one (format, source) pair, interned once and
/// reused by every row decode of that pair (positions and each declaration).
pub struct InternedPair<'a> {
    format_slice: &'a [u8],
    source_slice: &'a [u8],
    format: Arc<SourceContent>,
    source: Arc<SourceContent>,
}
/// `None` when the cache is off or the pair is too large to retain; callers
/// then use the ordinary per-call path with identical results.
pub fn intern_pair<'a>(format: &'a [u8], source: &'a [u8]) -> Option<InternedPair<'a>> {
    intern_pair_known(format, source, None, None)
}
/// `intern_pair`, reusing contents the caller already holds for exactly these
/// slices (each `known` content's bytes are the slice), instead of looking
/// them up again.
pub fn intern_pair_known<'a>(
    format: &'a [u8],
    source: &'a [u8],
    known_format: Option<&Arc<SourceContent>>,
    known_source: Option<&Arc<SourceContent>>,
) -> Option<InternedPair<'a>> {
    debug_assert!(known_format.is_none_or(|c| c.bytes() == format));
    debug_assert!(known_source.is_none_or(|c| c.bytes() == source));
    if !enabled()
        || source
            .len()
            .checked_add(format.len())
            .is_none_or(|n| n > CacheLimits::default().record)
    {
        return None;
    }
    Some(InternedPair {
        format_slice: format,
        source_slice: source,
        source: match known_source {
            Some(c) => c.clone(),
            None => shared_scene_sources::retain_content(source).ok()?,
        },
        format: match known_format {
            Some(c) => c.clone(),
            None => shared_scene_sources::retain_content(format).ok()?,
        },
    })
}
/// `rows` for a pair already interned by `intern_pair` on these same slices.
pub fn rows_interned(
    codec: RowCodec,
    interned: Option<&InternedPair<'_>>,
    format: &[u8],
    source: &[u8],
    count: usize,
    budget: usize,
) -> Result<Arc<Vec<[u32; 4]>>, GeometryStreamError> {
    let _timer =
        crate::source_acquisition::section(crate::source_acquisition::Section::DecodedCacheLookup);
    if enabled() && crate::source_acquisition::concurrent_caches_enabled() {
        return rows_unlocked(codec, interned, format, source, count, budget);
    }
    if enabled()
        && let Some(mut cache) = locked_cache()
    {
        return cache.rows_with(
            codec,
            interned,
            format,
            source,
            count,
            budget,
            CacheLimits::default(),
        );
    }
    decode_rows(codec, format, source, count, budget).map(Arc::new)
}
fn rows_unlocked(
    codec: RowCodec,
    interned: Option<&InternedPair<'_>>,
    format: &[u8],
    source: &[u8],
    count: usize,
    budget: usize,
) -> Result<Arc<Vec<[u32; 4]>>, GeometryStreamError> {
    cached_unlocked(
        Kind::Rows(codec),
        source
            .len()
            .checked_add(format.len())
            .is_none_or(|n| n > CacheLimits::default().record),
        || match interned {
            Some(pair)
                if std::ptr::eq(pair.source_slice, source)
                    && std::ptr::eq(pair.format_slice, format) =>
            {
                Some((pair.source.clone(), pair.format.clone()))
            }
            _ => shared_scene_sources::retain_content(source)
                .ok()
                .zip(shared_scene_sources::retain_content(format).ok()),
        },
        count,
        take_rows,
        |rows| {
            if rows.len().checked_mul(16).is_none_or(|n| n > budget) {
                return Err(GeometryStreamError::BudgetExceeded);
            }
            Ok(rows)
        },
        || decode_rows(codec, format, source, count, budget).map(Arc::new),
        Value::Rows,
    )
}
pub fn rows(
    codec: RowCodec,
    format: &[u8],
    source: &[u8],
    count: usize,
    budget: usize,
) -> Result<Arc<Vec<[u32; 4]>>, GeometryStreamError> {
    let _timer =
        crate::source_acquisition::section(crate::source_acquisition::Section::DecodedCacheLookup);
    if enabled() && crate::source_acquisition::concurrent_caches_enabled() {
        return rows_unlocked(codec, None, format, source, count, budget);
    }
    if enabled()
        && let Some(mut cache) = locked_cache()
    {
        return cache.rows(codec, format, source, count, budget, CacheLimits::default());
    }
    decode_rows(codec, format, source, count, budget).map(Arc::new)
}
pub fn indices(
    source: &[u8],
    count: usize,
    budget: usize,
) -> Result<Arc<Vec<u16>>, GeometryStreamError> {
    indices_known(source, None, count, budget)
}
/// `indices`, with the caller's content for exactly `source` when it has one.
pub fn indices_known(
    source: &[u8],
    known: Option<&Arc<SourceContent>>,
    count: usize,
    budget: usize,
) -> Result<Arc<Vec<u16>>, GeometryStreamError> {
    debug_assert!(known.is_none_or(|c| c.bytes() == source));
    let _timer =
        crate::source_acquisition::section(crate::source_acquisition::Section::DecodedCacheLookup);
    if enabled() && crate::source_acquisition::concurrent_caches_enabled() {
        return cached_unlocked(
            Kind::Indices,
            source.len() > CacheLimits::default().record,
            || {
                known
                    .cloned()
                    .or_else(|| shared_scene_sources::retain_content(source).ok())
                    .zip(shared_scene_sources::retain_content(&[]).ok())
            },
            count,
            take_indices,
            |rows| {
                if rows.len().checked_mul(2).is_none_or(|n| n > budget) {
                    return Err(GeometryStreamError::BudgetExceeded);
                }
                Ok(rows)
            },
            || geometry_streams::decode_indices(source, count, budget).map(Arc::new),
            Value::Indices,
        );
    }
    if enabled()
        && let Some(mut cache) = locked_cache()
    {
        return cache.indices_known(source, known, count, budget, CacheLimits::default());
    }
    geometry_streams::decode_indices(source, count, budget).map(Arc::new)
}
/// Decoded auxiliary rows, shared by exact source content and every binding
/// field the decoder reads. Results equal `decode_auxiliary_attribute`.
pub fn auxiliary(
    binding: &geometry_bindings::AuxiliaryBinding,
    source: &[u8],
    count: usize,
    budget: usize,
) -> Result<Arc<Vec<[u32; 4]>>, geometry_bindings::BindingError> {
    let _timer =
        crate::source_acquisition::section(crate::source_acquisition::Section::DecodedCacheLookup);
    if enabled() && crate::source_acquisition::concurrent_caches_enabled() {
        return cached_unlocked(
            Kind::Auxiliary {
                format: binding.binding.format,
                components: binding.binding.components,
                stride: binding.binding.stride,
                frequency: binding.frequency,
                modulo: binding.modulo,
            },
            source.len() > CacheLimits::default().record,
            || {
                shared_scene_sources::retain_content(source)
                    .ok()
                    .zip(shared_scene_sources::retain_content(&[]).ok())
            },
            count,
            take_rows,
            |rows| {
                // The same refusal the decoder makes before touching any byte.
                if count.checked_mul(16).is_none_or(|n| n > budget) {
                    return Err(geometry_bindings::BindingError::Budget);
                }
                Ok(rows)
            },
            || {
                geometry_bindings::decode_auxiliary_attribute(binding, source, count, budget)
                    .map(Arc::new)
            },
            Value::Rows,
        );
    }
    if enabled()
        && let Some(mut cache) = locked_cache()
    {
        return cache.auxiliary(binding, source, count, budget, CacheLimits::default());
    }
    geometry_bindings::decode_auxiliary_attribute(binding, source, count, budget).map(Arc::new)
}
pub fn vm_changed() {
    if let Some(cache) = CACHE.get()
        && let Ok(mut cache) = cache.lock()
    {
        cache.clear()
    }
}
pub fn stats() -> CacheStats {
    CACHE
        .get()
        .and_then(|c| c.try_lock().ok())
        .map_or(CacheStats::default(), |c| c.stats())
}

/// Match the source's first semantic-one declaration before caching its rows.
pub fn position_codec(format: &[u8], source_address: u32) -> Result<RowCodec, GeometryStreamError> {
    let header = format.get(..8).ok_or(GeometryStreamError::SourceExtent)?;
    let declarations = format
        .get(8..8 + usize::from(header[0]) * 8)
        .ok_or(GeometryStreamError::SourceExtent)?;
    let ordinal = declarations
        .as_chunks::<8>()
        .0
        .iter()
        .position(|d| d[3] == 1)
        .ok_or(GeometryStreamError::MissingPositionAttribute)?;
    Ok(RowCodec::Descriptor {
        ordinal,
        phase: (source_address & 15) as u8,
    })
}

#[cfg(test)]
mod interned_tests {
    use super::*;

    #[test]
    fn interned_pair_reuses_identity_only_for_the_borrowed_slices() {
        let format = vec![1, 16, 0, 0, 0, 0, 0, 0, 0, 2, 3, 1, 16, 0, 0, 0];
        let source: Vec<u8> = [1f32, 2., 3., 4.]
            .into_iter()
            .flat_map(f32::to_be_bytes)
            .collect();
        let pair = InternedPair {
            format_slice: &format,
            source_slice: &source,
            format: shared_scene_sources::retain_content(&format).unwrap(),
            source: shared_scene_sources::retain_content(&source).unwrap(),
        };
        let codec = RowCodec::Descriptor {
            ordinal: 0,
            phase: 0,
        };
        let limits = CacheLimits::default();
        let mut cache = DecodedGeometryCache::default();
        let plain = cache.rows(codec, &format, &source, 1, 16, limits).unwrap();
        let shared = cache
            .rows_with(codec, Some(&pair), &format, &source, 1, 16, limits)
            .unwrap();
        assert!(Arc::ptr_eq(&plain, &shared));
        // Equal bytes elsewhere are not "the same slices": the ordinary path
        // interns them, and still finds the same exact content.
        let copy = source.clone();
        let other = cache
            .rows_with(codec, Some(&pair), &format, &copy, 1, 16, limits)
            .unwrap();
        assert!(Arc::ptr_eq(&plain, &other));
        // Different bytes under a stale pair can never reuse its identity.
        let mut changed = source.clone();
        changed[3] ^= 1;
        let fresh = cache
            .rows_with(codec, Some(&pair), &format, &changed, 1, 16, limits)
            .unwrap();
        assert!(!Arc::ptr_eq(&plain, &fresh));
        assert_eq!(
            *fresh,
            decode_rows(codec, &format, &changed, 1, 16).unwrap()
        );
    }
}

#[cfg(test)]
mod view_bench {
    use super::*;
    /// Synthetic warm geometry validation sized like a large scene (release,
    /// `--ignored --nocapture`): 2,000 partitions with about 15 MB of vertex
    /// and index streams. Copy path: copy both streams, intern the
    /// (format, source) pair and the indices, two decoded-cache hits. View
    /// path: find each stream's retained content in place, then the same two
    /// cache hits through the known contents.
    #[test]
    #[ignore]
    #[allow(unsafe_code)]
    fn bench_warm_geometry_validation() {
        // SAFETY: an ignored benchmark, run alone in its own process.
        unsafe {
            std::env::set_var("PS3_RENDER_DECODED_GEOMETRY_CACHE", "1");
            std::env::set_var("PS3_RENDER_RESIDENT_SOURCES", "1");
        }
        assert!(enabled());
        let partitions = 2000usize;
        let vertices = 500usize; // 12-byte float positions: 6,000 bytes
        let indices = 750usize; // 1,500 bytes
        let guest: Vec<(Vec<u8>, Vec<u8>)> = (0..partitions)
            .map(|p| {
                let source = (0..vertices * 12).map(|i| (i * 7 + p * 13) as u8).collect();
                let index = (0..indices)
                    .flat_map(|i| (((i + p) % vertices) as u16).to_be_bytes())
                    .collect();
                (source, index)
            })
            .collect();
        let format: Vec<u8> = vec![];
        let copy_pass = || {
            let start = std::time::Instant::now();
            for (source, index) in &guest {
                let source = source.to_vec();
                let index = index.to_vec();
                let pair = intern_pair_known(&format, &source, None, None);
                let rows = rows_interned(
                    RowCodec::Float32x3,
                    pair.as_ref(),
                    &format,
                    &source,
                    vertices,
                    usize::MAX,
                )
                .unwrap();
                let idx = indices_known(&index, None, indices, usize::MAX).unwrap();
                std::hint::black_box((rows, idx));
            }
            start.elapsed().as_secs_f64() * 1e3
        };
        let view_pass = || {
            let start = std::time::Instant::now();
            for (source, index) in &guest {
                let source = shared_scene_sources::find_content(source).expect("warm");
                let index = shared_scene_sources::find_content(index).expect("warm");
                let pair = intern_pair_known(&format, source.bytes(), None, Some(&source));
                let rows = rows_interned(
                    RowCodec::Float32x3,
                    pair.as_ref(),
                    &format,
                    source.bytes(),
                    vertices,
                    usize::MAX,
                )
                .unwrap();
                let idx = indices_known(index.bytes(), Some(&index), indices, usize::MAX).unwrap();
                std::hint::black_box((rows, idx));
            }
            start.elapsed().as_secs_f64() * 1e3
        };
        copy_pass(); // first sight: decode and retain everything
        let mb = guest.iter().map(|(s, i)| s.len() + i.len()).sum::<usize>() as f64 / 1e6;
        for _ in 0..3 {
            let copy = copy_pass();
            let view = view_pass();
            println!(
                "warm geometry, {partitions} partitions / {mb:.1} MB: copy path {copy:.2} ms, in-place {view:.2} ms"
            );
        }
    }
}
