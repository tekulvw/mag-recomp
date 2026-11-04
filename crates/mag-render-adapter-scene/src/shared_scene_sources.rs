//! Content-qualified owned source bytes shared by Depth and Main.
//! Every publication still performs its genuine source read. Equal EAs, an
//! execution update or a callback never stand in for a guest writer generation.
use sha2::{Digest, Sha256};
use std::sync::{Arc, Mutex, OnceLock, Weak};

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum SourceDomain {
    Guest,
    Local,
    Main,
}
#[derive(Clone, Debug)]
pub struct SourceSpan {
    pub domain: SourceDomain,
    pub address: u32,
    pub content: Arc<SourceContent>,
}
#[derive(Debug)]
pub struct SourceContent {
    pub identity: u64,
    bytes: Arc<[u8]>,
    digest: [u8; 32],
}
impl SourceContent {
    pub fn bytes(&self) -> &[u8] {
        &self.bytes
    }
    pub fn sha256(&self) -> &[u8; 32] {
        &self.digest
    }
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CatalogError {
    InvalidRange,
    Budget,
    Exhausted,
    Unavailable,
}
#[derive(Clone, Copy)]
pub struct CatalogBudget {
    pub entries: usize,
    pub bytes: usize,
    pub observations: usize,
}
impl Default for CatalogBudget {
    fn default() -> Self {
        Self {
            entries: 32768,
            bytes: 256 * 1024 * 1024,
            observations: 1048576,
        }
    }
}
#[derive(Default)]
pub struct SourceCatalog {
    next: u64,
    count: usize,
    resident: usize,
    entries: crate::fast_hash::FastHashMap<(usize, u64), Vec<Weak<SourceContent>>>,
    /// Optional sampled-key candidates for long contents. A probe key only
    /// selects candidates; reuse is still authorised by exact byte equality.
    probes: crate::fast_hash::FastHashMap<(usize, u64), Vec<Weak<SourceContent>>>,
    probe_stats: ProbeStats,
}
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct ProbeStats {
    /// Exact-equal content found by sampled key, without a full fingerprint.
    pub hits: u64,
    /// Sampled-key lookups that fell back to the full fingerprint path.
    pub fallbacks: u64,
}
/// Bucket selector only: exact comparison authorises every reuse, so this
/// needs speed and spread, not resistance to crafted collisions. Four
/// independent multiply-rotate lanes over 32-byte blocks, then the tail.
pub(crate) fn fingerprint(bytes: &[u8]) -> (usize, u64) {
    const K: [u64; 4] = [
        0x9e37_79b9_7f4a_7c15,
        0xc2b2_ae3d_27d4_eb4f,
        0x1656_67b1_9e37_79f9,
        0xff51_afd7_ed55_8ccd,
    ];
    let word = |b: &[u8]| u64::from_le_bytes(b.try_into().expect("8 bytes"));
    let mut lanes = K;
    let (blocks, tail) = bytes.as_chunks::<32>();
    for block in blocks {
        for i in 0..4 {
            lanes[i] = (lanes[i] ^ word(&block[i * 8..i * 8 + 8]))
                .wrapping_mul(K[i])
                .rotate_left(31);
        }
    }
    let mut h = (bytes.len() as u64).wrapping_mul(K[0]);
    for lane in lanes {
        h = (h ^ lane).wrapping_mul(K[3]).rotate_left(27);
    }
    let (words, rest) = tail.as_chunks::<8>();
    for w in words {
        h = (h ^ u64::from_le_bytes(*w))
            .wrapping_mul(K[1])
            .rotate_left(29);
    }
    let mut last = [0u8; 8];
    last[..rest.len()].copy_from_slice(rest);
    h = (h ^ u64::from_le_bytes(last) ^ rest.len() as u64).wrapping_mul(K[2]);
    h ^= h >> 32;
    (bytes.len(), h)
}
/// Shorter contents use the full fingerprint directly; it is already cheap.
const PROBE_MIN: usize = 512;
const PROBE_BUCKET: usize = 4;
/// Length plus 2x64 edge bytes and 15 interior words. Deliberately partial:
/// equal keys never imply equal content, they only nominate candidates.
fn probe_key(bytes: &[u8]) -> (usize, u64) {
    let n = bytes.len();
    debug_assert!(n >= PROBE_MIN);
    let mut h = (n as u64).wrapping_mul(0x9e37_79b9_7f4a_7c15);
    let mut mix = |at: usize| {
        let w = u64::from_le_bytes(bytes[at..at + 8].try_into().expect("8 bytes"));
        h = (h ^ w).wrapping_mul(0xff51_afd7_ed55_8ccd);
        h ^= h >> 29;
    };
    for at in (0..64).step_by(8) {
        mix(at);
        mix(n - 64 + at);
    }
    for i in 1..16 {
        mix((n - 8) * i / 16);
    }
    (n, h)
}
impl SourceCatalog {
    pub fn intern(
        &mut self,
        bytes: &[u8],
        budget: CatalogBudget,
    ) -> Result<Arc<SourceContent>, CatalogError> {
        self.intern_with(bytes, budget, false)
    }
    pub fn probe_stats(&self) -> ProbeStats {
        self.probe_stats
    }
    /// `probe` adds a sampled-key candidate lookup before the full path. The
    /// catalog keeps at most one live content per exact byte string, so an
    /// exact-equal probe candidate is the same content the full path returns.
    pub fn intern_with(
        &mut self,
        bytes: &[u8],
        budget: CatalogBudget,
        probe: bool,
    ) -> Result<Arc<SourceContent>, CatalogError> {
        let probe_key = (probe && bytes.len() >= PROBE_MIN).then(|| probe_key(bytes));
        if let Some(key) = probe_key {
            if let Some(bucket) = self.probes.get(&key) {
                for old in bucket.iter().filter_map(Weak::upgrade) {
                    if old.bytes() == bytes {
                        self.probe_stats.hits += 1;
                        return Ok(old);
                    }
                }
            }
            self.probe_stats.fallbacks += 1;
        }
        let content = self.intern_full(bytes, budget)?;
        if let Some(key) = probe_key {
            if self.probes.len() >= budget.entries {
                self.probes.retain(|_, bucket| {
                    bucket.retain(|entry| entry.strong_count() != 0);
                    !bucket.is_empty()
                });
            }
            if self.probes.len() < budget.entries || self.probes.contains_key(&key) {
                let bucket = self.probes.entry(key).or_default();
                bucket.retain(|entry| entry.strong_count() != 0);
                if !bucket
                    .iter()
                    .any(|entry| std::ptr::eq(entry.as_ptr(), Arc::as_ptr(&content)))
                {
                    if bucket.len() >= PROBE_BUCKET {
                        bucket.remove(0);
                    }
                    bucket.push(Arc::downgrade(&content));
                }
            }
        }
        Ok(content)
    }
    /// Live contents of one candidate bucket (concurrent capture caches).
    fn bucket(
        map: &crate::fast_hash::FastHashMap<(usize, u64), Vec<Weak<SourceContent>>>,
        key: (usize, u64),
    ) -> Vec<Arc<SourceContent>> {
        map.get(&key)
            .map(|b| b.iter().filter_map(Weak::upgrade).collect())
            .unwrap_or_default()
    }
    fn intern_full(
        &mut self,
        bytes: &[u8],
        budget: CatalogBudget,
    ) -> Result<Arc<SourceContent>, CatalogError> {
        let key = fingerprint(bytes);
        if let Some(bucket) = self.entries.get(&key) {
            for old in bucket.iter().filter_map(Weak::upgrade) {
                // The fingerprint only selects candidates. Every hit still
                // reads and compares all source bytes; collisions cannot
                // authorize reuse. Hashes are never source generations.
                if old.bytes() == bytes {
                    return Ok(old);
                }
            }
        }
        if self.count >= budget.entries
            || self
                .resident
                .checked_add(bytes.len())
                .is_none_or(|n| n > budget.bytes)
        {
            let mut count = 0usize;
            let mut resident = 0usize;
            self.entries.retain(|_, bucket| {
                bucket.retain(|entry| entry.strong_count() != 0);
                for entry in bucket.iter().filter_map(Weak::upgrade) {
                    count = count.saturating_add(1);
                    resident = resident.saturating_add(entry.bytes.len());
                }
                !bucket.is_empty()
            });
            self.count = count;
            self.resident = resident;
        }
        if self.count >= budget.entries
            || self
                .resident
                .checked_add(bytes.len())
                .is_none_or(|n| n > budget.bytes)
        {
            return Err(CatalogError::Budget);
        }
        let identity = self.next.checked_add(1).ok_or(CatalogError::Exhausted)?;
        let content = Arc::new(SourceContent {
            identity,
            bytes: Arc::from(bytes),
            // Retain the stable digest for new immutable content only. Warm
            // observations need no cryptographic rehash before exact equality.
            digest: Sha256::digest(bytes).into(),
        });
        self.entries
            .entry(key)
            .or_default()
            .push(Arc::downgrade(&content));
        self.next = identity;
        self.count += 1;
        self.resident += bytes.len();
        Ok(content)
    }
}
static CATALOG: OnceLock<Mutex<SourceCatalog>> = OnceLock::new();
/// Retain exact owned content for prepared records without inventing a source
/// observation/address. Real snapshots retain their own span provenance.
pub(crate) fn retain_content(bytes: &[u8]) -> Result<Arc<SourceContent>, CatalogError> {
    if crate::source_acquisition::concurrent_caches_enabled() {
        return intern_unlocked(
            CATALOG.get_or_init(|| Mutex::new(SourceCatalog::default())),
            bytes,
            CatalogBudget::default(),
            crate::source_acquisition::content_probe_enabled(),
        );
    }
    CATALOG
        .get_or_init(|| Mutex::new(SourceCatalog::default()))
        .lock()
        .map_err(|_| CatalogError::Unavailable)?
        .intern_with(
            bytes,
            CatalogBudget::default(),
            crate::source_acquisition::content_probe_enabled(),
        )
}
/// Concurrent capture caches: the lookups `intern_with` makes, with the
/// hashing and byte comparisons outside the lock, in the same order and with
/// the same laziness (the full fingerprint only after a sampled-key miss).
/// An exact-equal live candidate is the one content the catalog keeps for
/// these bytes. Otherwise `intern_with` runs under the lock and re-checks, so
/// a content another thread just added is still the one returned.
fn intern_unlocked(
    catalog: &Mutex<SourceCatalog>,
    bytes: &[u8],
    budget: CatalogBudget,
    probe: bool,
) -> Result<Arc<SourceContent>, CatalogError> {
    let lock = || catalog.lock().map_err(|_| CatalogError::Unavailable);
    let found =
        |candidates: Vec<Arc<SourceContent>>| candidates.into_iter().find(|c| c.bytes() == bytes);
    if probe && bytes.len() >= PROBE_MIN {
        let key = probe_key(bytes);
        let candidates = SourceCatalog::bucket(&lock()?.probes, key);
        if let Some(hit) = found(candidates) {
            UNLOCKED_HITS.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
            return Ok(hit);
        }
    }
    let key = fingerprint(bytes);
    let candidates = SourceCatalog::bucket(&lock()?.entries, key);
    if let Some(hit) = found(candidates) {
        UNLOCKED_HITS.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
        return Ok(hit);
    }
    lock()?.intern_with(bytes, budget, probe)
}
/// The live content byte-identical to `bytes`, if the catalog holds one,
/// found in `intern_with`'s lookup order. Never inserts: a caller that gets
/// `None` interns through the ordinary path.
#[cfg_attr(
    not(test),
    expect(
        dead_code,
        reason = "called by the source FFI layer, which is migrated later"
    )
)]
pub(crate) fn find_content(bytes: &[u8]) -> Option<Arc<SourceContent>> {
    let catalog = CATALOG.get()?;
    let probe = crate::source_acquisition::content_probe_enabled();
    if crate::source_acquisition::concurrent_caches_enabled() {
        let lock = || catalog.lock().ok();
        if probe && bytes.len() >= PROBE_MIN {
            let candidates = SourceCatalog::bucket(&lock()?.probes, probe_key(bytes));
            if let Some(hit) = candidates.into_iter().find(|c| c.bytes() == bytes) {
                return Some(hit);
            }
        }
        let candidates = SourceCatalog::bucket(&lock()?.entries, fingerprint(bytes));
        return candidates.into_iter().find(|c| c.bytes() == bytes);
    }
    let c = catalog.lock().ok()?;
    if probe && bytes.len() >= PROBE_MIN {
        let key = probe_key(bytes);
        if let Some(hit) = c
            .probes
            .get(&key)
            .into_iter()
            .flatten()
            .filter_map(Weak::upgrade)
            .find(|old| old.bytes() == bytes)
        {
            return Some(hit);
        }
    }
    c.entries
        .get(&fingerprint(bytes))
        .into_iter()
        .flatten()
        .filter_map(Weak::upgrade)
        .find(|old| old.bytes() == bytes)
}
/// Catalog hits found outside the lock (concurrent capture caches only).
static UNLOCKED_HITS: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);
pub fn unlocked_hits() -> u64 {
    UNLOCKED_HITS.load(std::sync::atomic::Ordering::Relaxed)
}
/// Cumulative sampled-key statistics of the live catalog.
pub fn probe_stats() -> ProbeStats {
    CATALOG
        .get()
        .and_then(|c| c.try_lock().ok())
        .map_or(ProbeStats::default(), |c| c.probe_stats())
}
/// The catalog owns weak lookup entries only. Actual Depth/Main snapshots and
/// later consumers own the immutable bytes; eviction cannot revoke a live Arc.
#[derive(Default, Debug)]
pub struct SourceSnapshot {
    pub spans: Vec<SourceSpan>,
    pub incomplete: bool,
}
impl SourceSnapshot {
    pub fn observe(
        &mut self,
        domain: SourceDomain,
        address: u32,
        bytes: &[u8],
    ) -> Result<Arc<SourceContent>, CatalogError> {
        let result = (|| {
            if u64::from(address)
                .checked_add(bytes.len() as u64)
                .is_none_or(|end| end > 1u64 << 32)
            {
                return Err(CatalogError::InvalidRange);
            }
            let budget = CatalogBudget::default();
            if self.spans.len() >= budget.observations {
                return Err(CatalogError::Budget);
            }
            let content = retain_content(bytes)?;
            self.spans
                .try_reserve(1)
                .map_err(|_| CatalogError::Budget)?;
            self.spans.push(SourceSpan {
                domain,
                address,
                content: content.clone(),
            });
            Ok(content)
        })();
        if result.is_err() {
            self.incomplete = true;
        }
        result
    }
}
/// Separate opt-in for input observation; this never enables Depth cutover.
pub fn enabled() -> bool {
    std::env::var_os("PS3_RENDER_DEPTH_SOURCES").is_some_and(|s| s == "1")
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Synthetic warm-hit interning cost (run with --ignored --nocapture in
    /// release): locked `intern_with`, the same with an eager full
    /// fingerprint (an earlier regression), and `intern_unlocked`.
    #[test]
    #[ignore]
    fn bench_warm_interning() {
        let contents: Vec<Vec<u8>> = (0..330u32)
            .map(|i| {
                (0..(2048 + (i as usize % 7) * 1024))
                    .map(|j| (j as u32 ^ i) as u8)
                    .collect()
            })
            .collect();
        let per_thread = 7000;
        for probe in [true, false] {
            for threads in [1usize, 4] {
                let catalog = Mutex::new(SourceCatalog::default());
                // Held, as the decoded cache holds them: the catalog is weak.
                let _live: Vec<_> = contents
                    .iter()
                    .map(|c| {
                        catalog
                            .lock()
                            .unwrap()
                            .intern_with(c, CatalogBudget::default(), probe)
                            .unwrap()
                    })
                    .collect();
                let time = |f: &(dyn Fn(&[u8]) + Sync)| {
                    let start = std::time::Instant::now();
                    std::thread::scope(|scope| {
                        for t in 0..threads {
                            let contents = &contents;
                            scope.spawn(move || {
                                for i in 0..per_thread {
                                    f(&contents[(i * 7 + t * 13) % contents.len()]);
                                }
                            });
                        }
                    });
                    start.elapsed().as_secs_f64() * 1e9 / (per_thread * threads) as f64
                };
                let locked = time(&|b| {
                    catalog
                        .lock()
                        .unwrap()
                        .intern_with(b, CatalogBudget::default(), probe)
                        .unwrap();
                });
                let eager = time(&|b| {
                    let mut c = catalog.lock().unwrap();
                    std::hint::black_box(fingerprint(b));
                    c.intern_with(b, CatalogBudget::default(), probe).unwrap();
                });
                let unlocked = time(&|b| {
                    intern_unlocked(&catalog, b, CatalogBudget::default(), probe).unwrap();
                });
                println!(
                    "probe={probe} threads={threads} ns/intern: locked {locked:.0} eager-fingerprint {eager:.0} unlocked {unlocked:.0}"
                );
            }
        }
    }
    #[test]
    fn unlocked_interning_keeps_one_content_per_byte_string_across_threads() {
        let catalog = Mutex::new(SourceCatalog::default());
        let contents: Vec<Vec<u8>> = (0..24u8)
            .map(|i| {
                (0..(64 + usize::from(i) * 97))
                    .map(|j| (j as u8) ^ i)
                    .collect()
            })
            .collect();
        let results: Vec<Vec<Arc<SourceContent>>> = std::thread::scope(|scope| {
            let handles: Vec<_> = (0..8)
                .map(|t| {
                    let (catalog, contents) = (&catalog, &contents);
                    scope.spawn(move || {
                        let mut out = Vec::new();
                        for round in 0..20 {
                            for (i, bytes) in contents.iter().enumerate() {
                                if (i + t + round) % 3 == 0 {
                                    continue;
                                }
                                let c = intern_unlocked(
                                    catalog,
                                    bytes,
                                    CatalogBudget::default(),
                                    t % 2 == 0,
                                )
                                .unwrap();
                                assert_eq!(c.bytes(), bytes.as_slice());
                                out.push(c);
                            }
                        }
                        out
                    })
                })
                .collect();
            handles.into_iter().map(|h| h.join().unwrap()).collect()
        });
        let all: Vec<&Arc<SourceContent>> = results.iter().flatten().collect();
        for a in &all {
            for b in &all {
                assert_eq!(a.bytes() == b.bytes(), Arc::ptr_eq(a, b));
            }
        }
        let locked = catalog
            .lock()
            .unwrap()
            .intern(&contents[1], CatalogBudget::default())
            .unwrap();
        assert!(all.iter().any(|c| Arc::ptr_eq(c, &locked)));
    }
    #[test]
    fn fingerprint_collision_still_requires_exact_bytes() {
        let mut catalog = SourceCatalog::default();
        let budget = CatalogBudget::default();
        let first = catalog.intern(b"first", budget).unwrap();
        let second = catalog.intern(b"other", budget).unwrap();
        // Put different, live, same-length content in the selected bucket.
        // A matching fingerprint is deliberately insufficient for reuse.
        catalog.entries.remove(&fingerprint(b"first"));
        catalog.entries.insert(
            fingerprint(b"other"),
            vec![Arc::downgrade(&first), Arc::downgrade(&second)],
        );
        let found = catalog.intern(b"other", budget).unwrap();
        assert!(Arc::ptr_eq(&found, &second));
        assert!(!Arc::ptr_eq(&found, &first));
        assert_eq!(*found.sha256(), <[u8; 32]>::from(Sha256::digest(b"other")));
        assert_eq!(catalog.next, 2);
    }

    #[test]
    fn collision_without_equal_content_allocates_fresh_identity() {
        let mut catalog = SourceCatalog::default();
        let budget = CatalogBudget::default();
        let first = catalog.intern(b"first", budget).unwrap();
        catalog.entries.remove(&fingerprint(b"first"));
        catalog
            .entries
            .insert(fingerprint(b"other"), vec![Arc::downgrade(&first)]);
        let second = catalog.intern(b"other", budget).unwrap();
        assert_ne!(first.identity, second.identity);
        assert_eq!(first.bytes(), b"first");
        assert_eq!(second.bytes(), b"other");
        assert!(Arc::ptr_eq(
            &second,
            &catalog.intern(b"other", budget).unwrap()
        ));
    }

    #[test]
    fn probe_returns_the_same_identity_as_the_full_path() {
        let budget = CatalogBudget::default();
        let bytes: Vec<u8> = (0..4096u32).map(|i| (i * 7 + 3) as u8).collect();
        let mut plain = SourceCatalog::default();
        let mut probed = SourceCatalog::default();
        let a = plain.intern(&bytes, budget).unwrap();
        let b = probed.intern_with(&bytes, budget, true).unwrap();
        assert_eq!(a.identity, b.identity);
        assert_eq!(
            probed.probe_stats(),
            ProbeStats {
                hits: 0,
                fallbacks: 1
            }
        );
        let again = probed.intern_with(&bytes.clone(), budget, true).unwrap();
        assert!(Arc::ptr_eq(&again, &b));
        assert_eq!(probed.probe_stats().hits, 1);
        // Content interned without probing is still found by a probed call,
        // and vice versa: probing never creates a second live identity.
        let c = plain.intern_with(&bytes, budget, true).unwrap();
        assert!(Arc::ptr_eq(&c, &a));
        assert!(Arc::ptr_eq(&probed.intern(&bytes, budget).unwrap(), &b));
        // Short contents never use the sampled key.
        probed.intern_with(&[1; 64], budget, true).unwrap();
        assert_eq!(
            probed.probe_stats(),
            ProbeStats {
                hits: 1,
                fallbacks: 1
            }
        );
    }

    #[test]
    fn equal_probe_key_with_different_unsampled_bytes_is_not_reused() {
        let budget = CatalogBudget::default();
        let base = vec![5u8; 4096];
        let mut changed = base.clone();
        // An interior byte outside every sampled word: identical probe key.
        let at = (0..4096)
            .find(|&at| {
                let mut probe = base.clone();
                probe[at] ^= 0xff;
                at > 64 && at < 4096 - 64 && probe_key(&probe) == probe_key(&base)
            })
            .unwrap();
        changed[at] ^= 0xff;
        assert_eq!(probe_key(&changed), probe_key(&base));
        let mut catalog = SourceCatalog::default();
        let first = catalog.intern_with(&base, budget, true).unwrap();
        let second = catalog.intern_with(&changed, budget, true).unwrap();
        assert_ne!(first.identity, second.identity);
        assert_eq!(second.bytes(), changed.as_slice());
        assert_eq!(catalog.probe_stats().hits, 0);
        // Both remain individually reachable through the shared bucket.
        assert!(Arc::ptr_eq(
            &catalog.intern_with(&base, budget, true).unwrap(),
            &first
        ));
        assert!(Arc::ptr_eq(
            &catalog.intern_with(&changed, budget, true).unwrap(),
            &second
        ));
        assert_eq!(catalog.probe_stats().hits, 2);
    }

    #[test]
    fn dropped_content_is_never_revived_by_a_probe() {
        let budget = CatalogBudget::default();
        let bytes = vec![9u8; 2048];
        let mut catalog = SourceCatalog::default();
        let first = catalog.intern_with(&bytes, budget, true).unwrap();
        let identity = first.identity;
        drop(first);
        let second = catalog.intern_with(&bytes, budget, true).unwrap();
        assert_ne!(second.identity, identity);
        assert_eq!(catalog.probe_stats().hits, 0);
    }

    #[test]
    fn fingerprint_covers_every_byte_position_and_length() {
        let base: Vec<u8> = (0..203u32).map(|i| (i * 31 + 7) as u8).collect();
        let mut seen = std::collections::HashSet::new();
        assert!(seen.insert(fingerprint(&base)));
        for at in 0..base.len() {
            let mut changed = base.clone();
            changed[at] ^= 0x01;
            assert!(seen.insert(fingerprint(&changed)), "byte {at}");
        }
        for n in 0..base.len() {
            assert!(seen.insert(fingerprint(&base[..n])), "length {n}");
        }
        // Zero tails of different lengths stay distinct.
        assert_ne!(fingerprint(&[0; 9]), fingerprint(&[0; 10]));
    }
}
