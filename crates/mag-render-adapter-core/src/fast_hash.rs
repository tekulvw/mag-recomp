//! Fast deterministic hasher for planner-internal tables whose keys are
//! adapter-built values, not attacker-chosen input. Every user compares the
//! full key (or exact contents) after lookup, so hash quality only affects
//! speed, never which entry is returned.
use std::hash::{BuildHasherDefault, Hasher};

#[derive(Clone, Copy)]
pub struct FastHasher(u64);
impl Default for FastHasher {
    fn default() -> Self {
        Self(0x243f_6a88_85a3_08d3)
    }
}
const K: u64 = 0x9e37_79b9_7f4a_7c15;
impl FastHasher {
    #[inline]
    fn mix(&mut self, w: u64) {
        self.0 = (self.0.rotate_left(23) ^ w).wrapping_mul(K);
    }
}
impl Hasher for FastHasher {
    #[inline]
    fn finish(&self) -> u64 {
        let mut h = self.0;
        h ^= h >> 31;
        h = h.wrapping_mul(0xff51_afd7_ed55_8ccd);
        h ^ (h >> 29)
    }
    #[inline]
    fn write(&mut self, bytes: &[u8]) {
        let (words, tail) = bytes.as_chunks::<8>();
        for w in words {
            self.mix(u64::from_le_bytes(*w));
        }
        let mut last = [0u8; 8];
        last[..tail.len()].copy_from_slice(tail);
        self.mix(u64::from_le_bytes(last) ^ ((tail.len() as u64) << 56));
    }
    #[inline]
    fn write_u8(&mut self, v: u8) {
        self.mix(u64::from(v));
    }
    #[inline]
    fn write_u16(&mut self, v: u16) {
        self.mix(u64::from(v));
    }
    #[inline]
    fn write_u32(&mut self, v: u32) {
        self.mix(u64::from(v));
    }
    #[inline]
    fn write_u64(&mut self, v: u64) {
        self.mix(v);
    }
    #[inline]
    fn write_usize(&mut self, v: usize) {
        self.mix(v as u64);
    }
}
pub type FastBuildHasher = BuildHasherDefault<FastHasher>;
pub type FastHashMap<K, V> = std::collections::HashMap<K, V, FastBuildHasher>;
pub type FastHashSet<K> = std::collections::HashSet<K, FastBuildHasher>;

#[cfg(test)]
mod tests {
    use super::*;
    use std::hash::Hash;
    fn h(v: impl Hash) -> u64 {
        let mut s = FastHasher::default();
        v.hash(&mut s);
        s.finish()
    }
    #[test]
    fn distinct_small_keys_spread_and_maps_behave_like_std() {
        let mut seen = std::collections::HashSet::new();
        for a in 0..64u32 {
            for b in 0..64u32 {
                assert!(seen.insert(h((a, b))));
            }
        }
        assert_ne!(h(&[1u8, 2, 3][..]), h(&[1u8, 2, 3, 0][..]));
        let mut m: FastHashMap<Vec<u64>, u32> = FastHashMap::default();
        for i in 0..1000u64 {
            m.insert(vec![i, i * 3], i as u32);
        }
        for i in 0..1000u64 {
            assert_eq!(m[&vec![i, i * 3]], i as u32);
        }
    }
}
