//! Deterministic generators for property tests. No external dependencies, so the crate builds
//! offline with the locked toolchain alone.

pub(crate) struct Rng(u64);

impl Rng {
    pub(crate) fn new(seed: u64) -> Self {
        Self(seed ^ 0x9e37_79b9_7f4a_7c15)
    }

    pub(crate) fn next_u64(&mut self) -> u64 {
        // splitmix64
        self.0 = self.0.wrapping_add(0x9e37_79b9_7f4a_7c15);
        let mut z = self.0;
        z = (z ^ (z >> 30)).wrapping_mul(0xbf58_476d_1ce4_e5b9);
        z = (z ^ (z >> 27)).wrapping_mul(0x94d0_49bb_1331_11eb);
        z ^ (z >> 31)
    }

    pub(crate) fn next_u32(&mut self) -> u32 {
        (self.next_u64() >> 32) as u32
    }

    /// Uniform in `[lo, hi)`.
    pub(crate) fn range_f64(&mut self, lo: f64, hi: f64) -> f64 {
        let unit = (self.next_u64() >> 11) as f64 / (1u64 << 53) as f64;
        lo + (hi - lo) * unit
    }

    /// The binary32 value nearest a uniform sample in `[lo, hi)`.
    pub(crate) fn range_f32(&mut self, lo: f32, hi: f32) -> f32 {
        self.range_f64(lo as f64, hi as f64) as f32
    }

    pub(crate) fn below(&mut self, bound: u32) -> u32 {
        (self.next_u64() % bound as u64) as u32
    }
}

/// Distance in units in the last place between two finite binary32 values.
pub(crate) fn ulps(a: f32, b: f32) -> u64 {
    fn key(x: f32) -> i64 {
        let bits = x.to_bits() as i32 as i64;
        if bits < 0 {
            i32::MIN as i64 - bits
        } else {
            bits
        }
    }
    (key(a) - key(b)).unsigned_abs()
}
