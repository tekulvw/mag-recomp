//! Per-capture memo of pure decodes, keyed by every input byte and parameter.
//! A fingerprint selects candidates only; a stored result is returned only
//! when all input bytes and parameters are exactly equal, so the result is
//! the one the decoder would compute. Nothing is retained across captures.
use std::collections::HashMap;

/// (concatenated input bytes, first-input length, parameter, result)
type Entry<V> = (Box<[u8]>, usize, u64, V);
pub struct ExactMemo<V> {
    buckets: HashMap<(usize, u64, u64), Vec<Entry<V>>>,
    entries: usize,
    bytes: usize,
    pub hits: u64,
    pub misses: u64,
}
const ENTRIES: usize = 8192;
const BYTES: usize = 32 * 1024 * 1024;
impl<V> Default for ExactMemo<V> {
    fn default() -> Self {
        Self {
            buckets: HashMap::new(),
            entries: 0,
            bytes: 0,
            hits: 0,
            misses: 0,
        }
    }
}
impl<V: Clone> ExactMemo<V> {
    /// `decode(first, second, parameter)` must be a pure function of exactly
    /// these inputs. Returns a clone of an earlier identical call's result.
    pub fn get_or_decode(
        &mut self,
        first: &[u8],
        second: &[u8],
        parameter: u64,
        decode: impl FnOnce() -> V,
    ) -> V {
        let (n, a) = crate::shared_scene_sources::fingerprint(first);
        let (_, b) = crate::shared_scene_sources::fingerprint(second);
        let key = (
            n,
            a ^ b.rotate_left(17) ^ (second.len() as u64).wrapping_mul(0x9e37_79b9_7f4a_7c15),
            parameter,
        );
        if let Some(bucket) = self.buckets.get(&key) {
            for (bytes, split, p, value) in bucket {
                if *p == parameter
                    && *split == first.len()
                    && bytes.len() == first.len() + second.len()
                    && &bytes[..*split] == first
                    && &bytes[*split..] == second
                {
                    self.hits += 1;
                    return value.clone();
                }
            }
        }
        self.misses += 1;
        let value = decode();
        let size = first.len() + second.len();
        if self.entries < ENTRIES && self.bytes + size <= BYTES {
            let mut bytes = Vec::with_capacity(size);
            bytes.extend_from_slice(first);
            bytes.extend_from_slice(second);
            self.buckets.entry(key).or_default().push((
                bytes.into_boxed_slice(),
                first.len(),
                parameter,
                value.clone(),
            ));
            self.entries += 1;
            self.bytes += size;
        }
        value
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn reuse_requires_equal_bytes_split_and_parameter() {
        let mut memo = ExactMemo::default();
        let mut calls = 0;
        let mut run = |memo: &mut ExactMemo<Vec<u8>>, a: &[u8], b: &[u8], p| {
            memo.get_or_decode(a, b, p, || {
                calls += 1;
                [a, b, &p.to_le_bytes()].concat()
            })
        };
        let x = run(&mut memo, b"abc", b"de", 1);
        assert_eq!(run(&mut memo, b"abc", b"de", 1), x); // hit
        run(&mut memo, b"abcd", b"e", 1); // same concatenation, other split
        run(&mut memo, b"abc", b"de", 2); // other parameter
        run(&mut memo, b"abc", b"dE", 1); // one byte differs
        run(&mut memo, b"abc", b"", 1);
        assert_eq!(
            run(&mut memo, b"abcd", b"e", 1),
            [&b"abcde"[..], &1u64.to_le_bytes()].concat()
        );
        drop(run);
        assert_eq!(calls, 5);
        assert_eq!((memo.hits, memo.misses), (2, 5));
    }
}
