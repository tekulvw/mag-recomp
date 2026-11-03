//! Content-bound immutable CPU snapshots for the fixed scene worker. Hashes
//! accelerate lookup only; exact owned bytes decide equality. No guest address,
//! source epoch or allocation generation is invented by this cache.
use mag_render::{resources::*, types::ResourceVersion};
use std::hash::{Hash, Hasher};
struct Entry {
    hash: u64,
    version: ResourceVersion,
    last: u64,
}
pub(super) struct UploadCache {
    store: ResourceStore,
    entries: Vec<Entry>,
    max_bytes: usize,
    max_entries: usize,
    clock: u64,
    hits: u64,
    misses: u64,
}
impl UploadCache {
    pub(super) fn new(max_entries: usize, max_bytes: usize) -> Result<Self, i32> {
        Ok(Self {
            store: ResourceStore::new(ResourceLimits {
                max_resources: max_entries,
                max_bytes,
            })
            .map_err(|_| -5)?,
            entries: vec![],
            max_bytes,
            max_entries,
            clock: 0,
            hits: 0,
            misses: 0,
        })
    }
    pub(super) fn lease(&mut self, bytes: &[u8]) -> Result<ResourceLease, i32> {
        let mut h = std::collections::hash_map::DefaultHasher::new();
        bytes.hash(&mut h);
        self.lease_hashed(bytes, h.finish())
    }
    fn lease_hashed(&mut self, bytes: &[u8], hash: u64) -> Result<ResourceLease, i32> {
        if bytes.is_empty() {
            return Err(-1);
        }
        if bytes.len() > self.max_bytes {
            return Err(-5);
        }
        self.clock = self.clock.checked_add(1).ok_or(-5)?;
        for e in &mut self.entries {
            if e.hash == hash {
                let lease = self.store.lease(e.version).map_err(|_| -6)?;
                if lease.bytes() == bytes {
                    e.last = self.clock;
                    self.hits = self.hits.checked_add(1).ok_or(-5)?;
                    return Ok(lease);
                }
            }
        }
        self.entries.try_reserve(1).map_err(|_| -5)?;
        loop {
            let stats = self.store.stats();
            if stats.resident_resources < self.max_entries
                && bytes.len() <= self.max_bytes.saturating_sub(stats.resident_bytes)
            {
                break;
            }
            let i = self
                .entries
                .iter()
                .enumerate()
                .min_by_key(|(_, e)| e.last)
                .map(|(i, _)| i)
                .ok_or(-5)?;
            self.store.retire(self.entries[i].version).map_err(|_| -5)?;
            self.entries.remove(i);
        }
        let mut copy = Vec::new();
        copy.try_reserve_exact(bytes.len()).map_err(|_| -5)?;
        copy.extend_from_slice(bytes);
        let version = self
            .store
            .apply(ResourceDelta::Create { bytes: copy })
            .map_err(|_| -5)?;
        self.entries.push(Entry {
            hash,
            version,
            last: self.clock,
        });
        self.misses = self.misses.checked_add(1).ok_or(-5)?;
        self.store.lease(version).map_err(|_| -6)
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn exact_content_reuses_immutable_identity_but_mutation_does_not() {
        let mut c = UploadCache::new(4, 64).unwrap();
        let a = c.lease(&[1, 2, 3, 4]).unwrap();
        let b = c.lease(&[1, 2, 3, 4]).unwrap();
        assert_eq!(a.cache_key(), b.cache_key());
        let d = c.lease(&[1, 2, 3, 5]).unwrap();
        assert_ne!(a.cache_key(), d.cache_key());
        assert_eq!(a.bytes(), [1, 2, 3, 4]);
        assert_eq!(c.store.stats().resident_bytes, 8);
    }
    #[test]
    fn retained_pending_leases_count_after_eviction_and_release_restores_admission() {
        let mut c = UploadCache::new(2, 8).unwrap();
        let a = c.lease(&[1; 4]).unwrap();
        let b = c.lease(&[2; 4]).unwrap();
        assert!(c.lease(&[3; 4]).is_err());
        assert_eq!(c.store.stats().resident_bytes, 8);
        drop(a);
        let d = c.lease(&[3; 4]).unwrap();
        assert_eq!(d.bytes(), [3; 4]);
        assert_eq!(b.bytes(), [2; 4]);
        assert!(c.store.stats().resident_bytes <= 8);
    }
    #[test]
    fn hash_collision_does_not_alias_different_owned_bytes() {
        let mut c = UploadCache::new(4, 64).unwrap();
        let a = c.lease_hashed(&[1; 4], 7).unwrap();
        let b = c.lease_hashed(&[2; 4], 7).unwrap();
        assert_ne!(a.cache_key(), b.cache_key());
        assert_eq!(
            a.cache_key(),
            c.lease_hashed(&[1; 4], 7).unwrap().cache_key()
        );
    }
    #[test]
    fn unchanged_content_from_unrelated_callers_is_safe_but_caches_have_distinct_owner_domains() {
        let mut a = UploadCache::new(2, 16).unwrap();
        let mut b = UploadCache::new(2, 16).unwrap();
        let x = a.lease(&[0; 4]).unwrap();
        let y = b.lease(&[0; 4]).unwrap();
        assert_ne!(x.cache_key(), y.cache_key());
    }
}
