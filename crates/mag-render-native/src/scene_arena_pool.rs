//! Completion-gated reuse of per-frame upload arena buffers
//! (`MAG_RENDER_RESIDENT_ARENAS=1`). A buffer returns to the free list only
//! when its submission's genuine terminal event released it; the native
//! in-place write additionally refuses while any use is pending, so a buffer
//! the GPU may still read is never overwritten.
use mag_render::sdk_compute::Buffer;
use std::collections::HashMap;
#[derive(Clone, Copy, Default, Debug, PartialEq, Eq)]
pub struct ArenaStats {
    /// Free buffers rewritten in place instead of created.
    pub reuses: u64,
    /// New ring buffers created (zeroed, then written).
    pub creates: u64,
    pub written_bytes: u64,
    /// In-place writes the backend refused (UNSUPPORTED/BUSY/...).
    pub write_refusals: u64,
    /// Buffers released natively because the free list was full.
    pub retired: u64,
    pub free: usize,
    pub free_bytes: u64,
    pub owned: usize,
}
/// Capacity class: 4 KiB steps up to 64 KiB, then 64 KiB steps, so a slightly
/// larger arena next frame can reuse a buffer. Bindings carry explicit
/// ranges; the tail beyond the written bytes is never read.
pub(super) fn capacity(bytes: u64) -> Option<u64> {
    let step: u64 = if bytes <= 64 * 1024 { 4096 } else { 64 * 1024 };
    bytes
        .checked_add(step - 1)
        .map(|n| n / step * step)
        .filter(|&n| n != 0)
}
pub(super) struct Pool {
    enabled: bool,
    owned: HashMap<(u64, u64), u64>,
    free: Vec<(Buffer, u64)>,
    free_bytes: u64,
    max_free: usize,
    max_free_bytes: u64,
    stats: ArenaStats,
}
impl Pool {
    pub(super) fn new(enabled: bool) -> Self {
        Self {
            enabled,
            owned: HashMap::new(),
            free: Vec::new(),
            free_bytes: 0,
            max_free: 16,
            max_free_bytes: 512 * 1024 * 1024,
            stats: ArenaStats::default(),
        }
    }
    pub(super) fn enabled(&self) -> bool {
        self.enabled
    }
    pub(super) fn disable(&mut self) {
        self.enabled = false;
    }
    /// The smallest free buffer of at least `bytes`, removed from the list.
    pub(super) fn take(&mut self, bytes: u64) -> Option<(Buffer, u64)> {
        let index = self
            .free
            .iter()
            .enumerate()
            .filter(|(_, (_, c))| *c >= bytes)
            .min_by_key(|(_, (_, c))| *c)
            .map(|(i, _)| i)?;
        let (buffer, cap) = self.free.swap_remove(index);
        self.free_bytes -= cap;
        Some((buffer, cap))
    }
    pub(super) fn adopt(&mut self, buffer: Buffer, cap: u64) {
        self.owned.insert((buffer.id, buffer.version), cap);
        self.stats.creates += 1;
    }
    pub(super) fn reused(&mut self, bytes: u64) {
        self.stats.reuses += 1;
        self.stats.written_bytes += bytes;
    }
    pub(super) fn written(&mut self, bytes: u64) {
        self.stats.written_bytes += bytes;
    }
    pub(super) fn refused(&mut self) {
        self.stats.write_refusals += 1;
    }
    /// `None`: not a ring buffer. `Some(true)`: kept for reuse. `Some(false)`:
    /// a ring buffer the caller must now release natively.
    pub(super) fn released(&mut self, buffer: Buffer) -> Option<bool> {
        let cap = *self.owned.get(&(buffer.id, buffer.version))?;
        if self.free.len() < self.max_free && self.free_bytes + cap <= self.max_free_bytes {
            self.free.push((buffer, cap));
            self.free_bytes += cap;
            Some(true)
        } else {
            self.owned.remove(&(buffer.id, buffer.version));
            self.stats.retired += 1;
            Some(false)
        }
    }
    /// Forget a ring buffer (it is being released natively by the caller).
    pub(super) fn forget(&mut self, buffer: Buffer) {
        self.owned.remove(&(buffer.id, buffer.version));
    }
    pub(super) fn drain(&mut self) -> Vec<Buffer> {
        self.free_bytes = 0;
        self.owned.clear();
        self.free.drain(..).map(|(b, _)| b).collect()
    }
    pub(super) fn stats(&self) -> ArenaStats {
        ArenaStats {
            free: self.free.len(),
            free_bytes: self.free_bytes,
            owned: self.owned.len(),
            ..self.stats
        }
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    fn b(id: u64) -> Buffer {
        Buffer {
            device_generation: 1,
            id,
            version: 1,
        }
    }
    #[test]
    fn only_released_ring_buffers_are_reused_smallest_fit_first() {
        let mut p = Pool::new(true);
        assert!(p.take(10).is_none());
        p.adopt(b(1), capacity(70_000).unwrap());
        p.adopt(b(2), capacity(10).unwrap());
        // In flight: nothing is free yet.
        assert!(p.take(10).is_none());
        assert_eq!(p.released(b(9)), None); // not a ring buffer
        assert_eq!(p.released(b(1)), Some(true));
        assert_eq!(p.released(b(2)), Some(true));
        assert_eq!(p.take(100).unwrap().0.id, 2); // smallest that fits
        assert_eq!(p.take(100).unwrap().0.id, 1);
        assert!(p.take(1).is_none());
        assert_eq!(capacity(0), None);
        assert_eq!(capacity(1), Some(4096));
        assert_eq!(capacity(4097), Some(8192));
        assert_eq!(capacity(65536), Some(65536));
        assert_eq!(capacity(65537), Some(131072));
    }
    #[test]
    fn a_full_free_list_retires_instead_of_growing() {
        let mut p = Pool::new(true);
        for id in 0..20 {
            p.adopt(b(id), 65536);
        }
        let kept = (0..20)
            .filter(|&id| p.released(b(id)) == Some(true))
            .count();
        assert_eq!(kept, 16);
        assert_eq!(p.stats().retired, 4);
        assert_eq!(p.drain().len(), 16);
        assert_eq!(p.stats().owned, 0);
    }
}
