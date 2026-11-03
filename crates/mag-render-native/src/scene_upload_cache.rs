//! Content-bound immutable upload snapshots kept across scene submissions
//! (`MAG_RENDER_RESIDENT_BUFFERS=1`). A sampled key only nominates candidates;
//! a lease is reused only when every byte is equal. Equal bytes keep one stable
//! resource identity, which the device maps to one immutable GPU buffer.
use mag_render::{resources::*, types::ResourceVersion};
use std::collections::HashMap;

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct UploadCacheStats {
    pub hits: u64,
    pub misses: u64,
    pub hit_bytes: u64,
    pub evictions: u64,
    pub entries: usize,
    pub resident_bytes: usize,
}
struct Entry {
    version: ResourceVersion,
    bytes: usize,
    last: u64,
}
pub(crate) struct UploadCache {
    store: ResourceStore,
    buckets: HashMap<(usize, u64), Vec<Entry>>,
    max_entries: usize,
    max_bytes: usize,
    entries: usize,
    resident: usize,
    clock: u64,
    stats: UploadCacheStats,
}
/// Length plus edge bytes and 30 interior words; short contents are mixed in
/// full. Deliberately partial: it selects candidates, never equality.
fn sampled_key(bytes: &[u8]) -> (usize, u64) {
    let n = bytes.len();
    let mut h = (n as u64).wrapping_mul(0x9e37_79b9_7f4a_7c15);
    let mut mix = |w: u64| {
        h = (h ^ w).wrapping_mul(0xff51_afd7_ed55_8ccd);
        h ^= h >> 29;
    };
    if n < 512 {
        for chunk in bytes.chunks(8) {
            let mut w = [0u8; 8];
            w[..chunk.len()].copy_from_slice(chunk);
            mix(u64::from_le_bytes(w));
        }
    } else {
        let word = |at: usize| u64::from_le_bytes(bytes[at..at + 8].try_into().expect("8"));
        for at in (0..64).step_by(8) {
            mix(word(at));
            mix(word(n - 64 + at));
        }
        for i in 1..31 {
            mix(word((n - 8) * i / 31));
        }
    }
    (n, h)
}
impl UploadCache {
    pub(crate) fn new(max_entries: usize, max_bytes: usize) -> Result<Self, i32> {
        Ok(Self {
            // Retained snapshots still held by in-flight submissions count
            // against the store; allow them on top of the cache's own budget.
            store: ResourceStore::new(ResourceLimits {
                max_resources: max_entries.checked_mul(2).ok_or(-5)?,
                max_bytes: max_bytes.checked_mul(2).ok_or(-5)?,
            })
            .map_err(|_| -5)?,
            buckets: HashMap::new(),
            max_entries,
            max_bytes,
            entries: 0,
            resident: 0,
            clock: 0,
            stats: UploadCacheStats::default(),
        })
    }
    pub(crate) fn stats(&self) -> UploadCacheStats {
        UploadCacheStats {
            entries: self.entries,
            resident_bytes: self.resident,
            ..self.stats
        }
    }
    /// A lease of exactly `bytes`, or `None` when the bytes cannot be kept
    /// (empty, too large, or store capacity); the caller then uses its
    /// ordinary single-use snapshot.
    pub(crate) fn lease(&mut self, bytes: &[u8]) -> Option<ResourceLease> {
        if bytes.is_empty() || bytes.len() > self.max_bytes {
            return None;
        }
        self.clock += 1;
        let key = sampled_key(bytes);
        if let Some(bucket) = self.buckets.get_mut(&key) {
            for e in bucket.iter_mut() {
                if let Ok(lease) = self.store.lease(e.version)
                    && lease.bytes() == bytes
                {
                    e.last = self.clock;
                    self.stats.hits += 1;
                    self.stats.hit_bytes += bytes.len() as u64;
                    return Some(lease);
                }
            }
        }
        while self.entries >= self.max_entries || self.resident + bytes.len() > self.max_bytes {
            if !self.evict_oldest() {
                return None;
            }
        }
        let mut copy = Vec::new();
        copy.try_reserve_exact(bytes.len()).ok()?;
        copy.extend_from_slice(bytes);
        let version = self
            .store
            .apply(ResourceDelta::Create { bytes: copy })
            .ok()?;
        let lease = self.store.lease(version).ok()?;
        self.buckets.entry(key).or_default().push(Entry {
            version,
            bytes: bytes.len(),
            last: self.clock,
        });
        self.entries += 1;
        self.resident += bytes.len();
        self.stats.misses += 1;
        Some(lease)
    }
    fn evict_oldest(&mut self) -> bool {
        let Some((key, index)) = self
            .buckets
            .iter()
            .flat_map(|(k, b)| b.iter().enumerate().map(move |(i, e)| (*k, i, e.last)))
            .min_by_key(|(_, _, last)| *last)
            .map(|(k, i, _)| (k, i))
        else {
            return false;
        };
        let bucket = self.buckets.get_mut(&key).expect("victim bucket");
        let e = bucket.swap_remove(index);
        if bucket.is_empty() {
            self.buckets.remove(&key);
        }
        // Outstanding leases keep their snapshot; retiring never mutates it.
        let _ = self.store.retire(e.version);
        self.entries -= 1;
        self.resident -= e.bytes;
        self.stats.evictions += 1;
        true
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn exact_bytes_share_one_identity_and_any_change_gets_a_new_one() {
        let mut c = UploadCache::new(4, 4096).unwrap();
        let data: Vec<u8> = (0..2048u32).map(|i| (i * 7) as u8).collect();
        let a = c.lease(&data).unwrap();
        let b = c.lease(&data.clone()).unwrap();
        assert_eq!(a.cache_key(), b.cache_key());
        // An unsampled interior byte: equal sampled key, different content.
        let mut changed = data.clone();
        changed[1000] ^= 1;
        assert_eq!(sampled_key(&changed), sampled_key(&data));
        let d = c.lease(&changed).unwrap();
        assert_ne!(d.cache_key(), a.cache_key());
        assert_eq!(d.bytes(), changed.as_slice());
        assert_eq!(c.stats().hits, 1);
        assert_eq!(c.stats().misses, 2);
    }
    #[test]
    fn eviction_never_changes_an_outstanding_lease_and_keys_are_not_reused() {
        let mut c = UploadCache::new(2, 1 << 20).unwrap();
        let first = c.lease(&[1; 600]).unwrap();
        let key = first.cache_key();
        c.lease(&[2; 600]).unwrap();
        c.lease(&[3; 600]).unwrap(); // evicts [1; 600]
        assert_eq!(c.stats().evictions, 1);
        assert_eq!(first.bytes(), &[1; 600][..]);
        let again = c.lease(&[1; 600]).unwrap(); // reloaded: a fresh identity
        assert_ne!(again.cache_key(), key);
        for n in 0..20u8 {
            let lease = c.lease(&[n.wrapping_add(9); 600]).unwrap();
            assert_ne!(lease.cache_key(), key);
        }
        assert!(c.lease(&[]).is_none());
        assert!(c.lease(&vec![0; (1 << 20) + 1]).is_none());
    }
}
