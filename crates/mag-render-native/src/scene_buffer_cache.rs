//! Context-local immutable upload buffers. Pins are charged once per allocation
//! until actual terminal handling releases each submission's acquisitions.
use mag_render::{resources::ResourceCacheKey, sdk_compute::Buffer};
#[derive(Clone, Copy, Default, Debug, PartialEq, Eq)]
pub struct Stats {
    pub resident_bytes: u64,
    pub pinned_bytes: u64,
    pub entries: usize,
    pub hits: u64,
    pub uploads: u64,
}
struct Entry {
    key: ResourceCacheKey,
    buffer: Buffer,
    bytes: u64,
    pins: u32,
    last: u64,
}
pub(super) struct Cache {
    entries: Vec<Entry>,
    max_entries: usize,
    max_bytes: u64,
    generation: u64,
    clock: u64,
    hits: u64,
    uploads: u64,
}
impl Cache {
    pub(super) fn new(generation: u64, max_entries: usize, max_bytes: u64) -> Self {
        Self {
            entries: vec![],
            max_entries,
            max_bytes,
            generation,
            clock: 0,
            hits: 0,
            uploads: 0,
        }
    }
    pub(super) fn cacheable(&self, bytes: u64) -> bool {
        self.max_entries != 0 && bytes != 0 && bytes <= self.max_bytes
    }
    pub(super) fn acquire(
        &mut self,
        key: ResourceCacheKey,
        bytes: u64,
        allocate: impl FnOnce() -> Result<Buffer, i32>,
        mut release: impl FnMut(Buffer),
    ) -> Result<Buffer, i32> {
        if !self.cacheable(bytes) {
            return Err(-5);
        }
        self.clock = self.clock.checked_add(1).ok_or(-5)?;
        if let Some(e) = self.entries.iter_mut().find(|e| e.key == key) {
            if e.bytes != bytes {
                return Err(-3);
            }
            e.pins = e.pins.checked_add(1).ok_or(-5)?;
            e.last = self.clock;
            self.hits = self.hits.checked_add(1).ok_or(-5)?;
            return Ok(e.buffer);
        }
        self.entries.try_reserve(1).map_err(|_| -5)?;
        while self.entries.len() >= self.max_entries
            || bytes > self.max_bytes - self.stats().resident_bytes
        {
            let i = self
                .entries
                .iter()
                .enumerate()
                .filter(|(_, e)| e.pins == 0)
                .min_by_key(|(_, e)| e.last)
                .map(|(i, _)| i)
                .ok_or(-4)?;
            let e = self.entries.remove(i);
            release(e.buffer);
        }
        let buffer = allocate()?;
        if buffer.device_generation != self.generation || buffer.id == 0 || buffer.version == 0 {
            release(buffer);
            return Err(-3);
        }
        self.entries.push(Entry {
            key,
            buffer,
            bytes,
            pins: 1,
            last: self.clock,
        });
        self.uploads = self.uploads.checked_add(1).ok_or(-5)?;
        Ok(buffer)
    }
    pub(super) fn release(&mut self, buffer: Buffer) -> Result<bool, i32> {
        if let Some(e) = self.entries.iter_mut().find(|e| {
            (e.buffer.device_generation, e.buffer.id, e.buffer.version)
                == (buffer.device_generation, buffer.id, buffer.version)
        }) {
            e.pins = e.pins.checked_sub(1).ok_or(-6)?;
            return Ok(true);
        }
        Ok(false)
    }
    pub(super) fn clear(&mut self, mut release: impl FnMut(Buffer)) {
        for e in self.entries.drain(..) {
            release(e.buffer);
        }
    }
    pub(super) fn stats(&self) -> Stats {
        Stats {
            resident_bytes: self.entries.iter().map(|e| e.bytes).sum(),
            pinned_bytes: self
                .entries
                .iter()
                .filter(|e| e.pins != 0)
                .map(|e| e.bytes)
                .sum(),
            entries: self.entries.len(),
            hits: self.hits,
            uploads: self.uploads,
        }
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    use mag_render::resources::*;
    fn key(bytes: u8) -> ResourceCacheKey {
        let mut s = ResourceStore::new(ResourceLimits {
            max_resources: 1,
            max_bytes: 4,
        })
        .unwrap();
        let v = s
            .apply(ResourceDelta::Create {
                bytes: vec![bytes; 4],
            })
            .unwrap();
        s.lease(v).unwrap().cache_key()
    }
    #[test]
    fn warm_hit_skips_allocation_and_double_pending_pins_count_one_allocation() {
        let k = key(1);
        let mut c = Cache::new(7, 2, 8);
        let b = c
            .acquire(
                k,
                4,
                || {
                    Ok(Buffer {
                        device_generation: 7,
                        id: 1,
                        version: 1,
                    })
                },
                |_| panic!(),
            )
            .unwrap();
        let again = c
            .acquire(k, 4, || panic!("warm allocation"), |_| panic!())
            .unwrap();
        assert_eq!(
            (b.device_generation, b.id, b.version),
            (again.device_generation, again.id, again.version)
        );
        assert_eq!(c.stats().resident_bytes, 4);
        assert_eq!(c.stats().pinned_bytes, 4);
        assert_eq!(c.stats().hits, 1);
        assert!(c.release(b).unwrap());
        assert_eq!(c.stats().pinned_bytes, 4);
        c.release(b).unwrap();
        assert_eq!(c.stats().pinned_bytes, 0);
        assert_eq!(c.release(b), Err(-6));
    }
    #[test]
    fn pinned_capacity_cannot_be_evicted_and_failed_allocation_never_publishes() {
        let mut c = Cache::new(7, 1, 4);
        let k = key(1);
        let other = key(2);
        let b = c
            .acquire(
                k,
                4,
                || {
                    Ok(Buffer {
                        device_generation: 7,
                        id: 1,
                        version: 1,
                    })
                },
                |_| panic!(),
            )
            .unwrap();
        assert_eq!(
            c.acquire(other, 4, || panic!(), |_| panic!()).err(),
            Some(-4)
        );
        assert_eq!(c.stats().entries, 1);
        c.release(b).unwrap();
        let mut retired = vec![];
        assert_eq!(
            c.acquire(other, 4, || Err(-5), |b| retired.push(b)).err(),
            Some(-5)
        );
        assert_eq!(retired.len(), 1);
        assert_eq!(retired[0].id, b.id);
        assert_eq!(c.stats().entries, 0);
    }
    #[test]
    fn foreign_context_allocation_is_not_cached() {
        let mut c = Cache::new(7, 2, 8);
        let mut retired = vec![];
        assert_eq!(
            c.acquire(
                key(1),
                4,
                || Ok(Buffer {
                    device_generation: 8,
                    id: 1,
                    version: 1
                }),
                |b| retired.push(b)
            )
            .err(),
            Some(-3)
        );
        assert_eq!(c.stats().resident_bytes, 0);
        assert_eq!(retired.len(), 1);
    }
}
