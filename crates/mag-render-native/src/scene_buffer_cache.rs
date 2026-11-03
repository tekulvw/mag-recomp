//! Device-local immutable upload buffers (`MAG_RENDER_RESIDENT_BUFFERS=1`).
//! One GPU buffer per immutable upload identity: it is created with the
//! lease's bytes and never written again. Every submission that binds it pins
//! it until that submission's genuine terminal event (no further GPU access);
//! only unpinned buffers are ever released or evicted.
use mag_render::{resources::ResourceCacheKey, sdk_compute::Buffer};
use std::collections::HashMap;
#[derive(Clone, Copy, Default, Debug, PartialEq, Eq)]
pub struct ResidentBufferStats {
    pub hits: u64,
    pub misses: u64,
    pub hit_bytes: u64,
    pub retired: u64,
    pub entries: usize,
    pub resident_bytes: u64,
    pub pinned_bytes: u64,
}
struct Entry {
    buffer: Buffer,
    bytes: u64,
    pins: u32,
    last: u64,
}
pub(super) struct Cache {
    entries: HashMap<ResourceCacheKey, Entry>,
    by_buffer: HashMap<(u64, u64), ResourceCacheKey>,
    max_entries: usize,
    max_bytes: u64,
    resident: u64,
    generation: u64,
    clock: u64,
    stats: ResidentBufferStats,
}
impl Cache {
    pub(super) fn new(generation: u64, max_entries: usize, max_bytes: u64) -> Self {
        Self {
            entries: HashMap::new(),
            by_buffer: HashMap::new(),
            max_entries,
            max_bytes,
            resident: 0,
            generation,
            clock: 0,
            stats: ResidentBufferStats::default(),
        }
    }
    pub(super) fn enabled(&self) -> bool {
        self.max_entries != 0
    }
    /// The resident buffer for `key`, pinned once more, or a new one created by
    /// `allocate` (pinned once). `None` when it cannot be kept within bounds;
    /// the caller then allocates an ordinary single-use buffer.
    pub(super) fn acquire(
        &mut self,
        key: ResourceCacheKey,
        bytes: u64,
        allocate: impl FnOnce() -> Result<Buffer, i32>,
        mut release: impl FnMut(Buffer),
    ) -> Option<Result<Buffer, i32>> {
        if !self.enabled() || bytes == 0 || bytes > self.max_bytes {
            return None;
        }
        self.clock += 1;
        if let Some(e) = self.entries.get_mut(&key) {
            if e.bytes != bytes {
                return Some(Err(-3));
            }
            e.pins = e.pins.checked_add(1)?;
            e.last = self.clock;
            self.stats.hits += 1;
            self.stats.hit_bytes += bytes;
            return Some(Ok(e.buffer));
        }
        while self.entries.len() >= self.max_entries || self.resident + bytes > self.max_bytes {
            let victim = self
                .entries
                .iter()
                .filter(|(_, e)| e.pins == 0)
                .min_by_key(|(_, e)| e.last)
                .map(|(k, _)| *k)?;
            let e = self.entries.remove(&victim).expect("victim");
            self.by_buffer.remove(&(e.buffer.id, e.buffer.version));
            self.resident -= e.bytes;
            self.stats.retired += 1;
            release(e.buffer);
        }
        let buffer = match allocate() {
            Ok(b) => b,
            Err(code) => return Some(Err(code)),
        };
        if buffer.device_generation != self.generation || buffer.id == 0 || buffer.version == 0 {
            release(buffer);
            return Some(Err(-3));
        }
        self.entries.insert(
            key,
            Entry {
                buffer,
                bytes,
                pins: 1,
                last: self.clock,
            },
        );
        self.by_buffer.insert((buffer.id, buffer.version), key);
        self.resident += bytes;
        self.stats.misses += 1;
        Some(Ok(buffer))
    }
    /// Drop one pin. `true` when the buffer is resident (the caller must not
    /// release the native handle); `false` for an ordinary buffer.
    pub(super) fn unpin(&mut self, buffer: Buffer) -> bool {
        let Some(key) = self.by_buffer.get(&(buffer.id, buffer.version)) else {
            return false;
        };
        if buffer.device_generation != self.generation {
            return false;
        }
        if let Some(e) = self.entries.get_mut(key) {
            // A double unpin must never release a resident handle early.
            e.pins = e.pins.saturating_sub(1);
        }
        true
    }
    pub(super) fn clear(&mut self, mut release: impl FnMut(Buffer)) {
        for (_, e) in self.entries.drain() {
            release(e.buffer);
        }
        self.by_buffer.clear();
        self.resident = 0;
    }
    pub(super) fn stats(&self) -> ResidentBufferStats {
        ResidentBufferStats {
            entries: self.entries.len(),
            resident_bytes: self.resident,
            pinned_bytes: self
                .entries
                .values()
                .filter(|e| e.pins != 0)
                .map(|e| e.bytes)
                .sum(),
            ..self.stats
        }
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    use mag_render::resources::*;
    fn keys(n: usize) -> Vec<(ResourceCacheKey, ResourceLease)> {
        let mut s = ResourceStore::new(ResourceLimits {
            max_resources: n,
            max_bytes: 4 * n,
        })
        .unwrap();
        (0..n)
            .map(|i| {
                let v = s
                    .apply(ResourceDelta::Create {
                        bytes: vec![i as u8; 4],
                    })
                    .unwrap();
                let l = s.lease(v).unwrap();
                (l.cache_key(), l)
            })
            .collect()
    }
    #[test]
    fn pinned_buffers_are_never_evicted_and_hits_skip_allocation() {
        let k = keys(3);
        let mut c = Cache::new(7, 2, 8);
        let mut next = 0;
        let mut alloc = || {
            next += 1;
            Ok(Buffer {
                device_generation: 7,
                id: next,
                version: 1,
            })
        };
        let mut released = Vec::new();
        let a = c
            .acquire(k[0].0, 4, &mut alloc, |b| released.push(b.id))
            .unwrap()
            .unwrap();
        let again = c
            .acquire(k[0].0, 4, &mut alloc, |b| released.push(b.id))
            .unwrap()
            .unwrap();
        assert_eq!(a.id, again.id); // hit: no new allocation
        let b = c
            .acquire(k[1].0, 4, &mut alloc, |b| released.push(b.id))
            .unwrap()
            .unwrap();
        // Full and every entry pinned (two in-flight uses of a, one of b).
        assert!(
            c.acquire(k[2].0, 4, &mut alloc, |b| released.push(b.id))
                .is_none()
        );
        assert!(released.is_empty());
        assert!(c.unpin(a) && c.unpin(a)); // both submissions completed
        assert!(!c.unpin(Buffer {
            device_generation: 7,
            id: 99,
            version: 1
        }));
        let d = c
            .acquire(k[2].0, 4, &mut alloc, |b| released.push(b.id))
            .unwrap()
            .unwrap();
        assert_eq!(released, vec![a.id]); // only the unpinned, least recent one
        assert_ne!(d.id, b.id);
        let s = c.stats();
        assert_eq!((s.hits, s.misses, s.retired, s.entries), (1, 3, 1, 2));
        assert_eq!(s.pinned_bytes, 8);
        // Size mismatch for an existing identity is a refusal, not a reuse.
        assert!(matches!(
            c.acquire(k[1].0, 8, &mut alloc, |_| {}),
            Some(Err(-3))
        ));
        let mut all = Vec::new();
        c.clear(|b| all.push(b.id));
        all.sort();
        assert_eq!(all, vec![b.id, d.id]);
    }
}
