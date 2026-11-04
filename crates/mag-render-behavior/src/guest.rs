//! Guest memory access shared by the specifications that define their own reads.
//!
//! All guest data is big-endian. A reader copies bytes out of an immutable window of guest memory
//! that the caller owns; this crate never holds guest pointers.

use std::fmt;

/// A failed guest read reported by the caller's reader.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct SourceError {
    /// Guest address of the failed request.
    pub address: u32,
    /// Requested length in bytes.
    pub length: u32,
    /// Reader-specific description.
    pub reason: String,
}

impl fmt::Display for SourceError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "guest read of {} bytes at {:#x} failed: {}",
            self.length, self.address, self.reason
        )
    }
}

impl std::error::Error for SourceError {}

/// Read access to one immutable window of guest memory.
///
/// A request returns the bytes it could copy. Returning fewer than `length` bytes is a short read
/// that the caller of this trait reports as an extent error.
pub trait GuestMemory {
    /// Copies `length` bytes starting at guest address `address`.
    fn read(&self, address: u32, length: u32) -> Result<Vec<u8>, SourceError>;
}

/// A sparse in-memory guest image, used by tests and by callers that already hold copies.
#[derive(Clone, Debug, Default)]
pub struct SparseMemory {
    regions: Vec<(u32, Vec<u8>)>,
}

impl SparseMemory {
    /// An empty image: every read fails.
    pub fn new() -> Self {
        Self::default()
    }

    /// Adds (or overwrites) bytes at `address`.
    pub fn put(&mut self, address: u32, bytes: &[u8]) {
        for (offset, byte) in bytes.iter().enumerate() {
            let at = address.wrapping_add(offset as u32);
            self.put_byte(at, *byte);
        }
    }

    /// Stores big-endian words starting at `address`.
    pub fn put_words(&mut self, address: u32, words: &[u32]) {
        for (index, word) in words.iter().enumerate() {
            self.put(address + 4 * index as u32, &word.to_be_bytes());
        }
    }

    /// Stores one big-endian word.
    pub fn put_u32(&mut self, address: u32, word: u32) {
        self.put(address, &word.to_be_bytes());
    }

    /// Stores one big-endian `f32`.
    pub fn put_f32(&mut self, address: u32, value: f32) {
        self.put_u32(address, value.to_bits());
    }

    fn put_byte(&mut self, address: u32, byte: u8) {
        for (start, bytes) in &mut self.regions {
            let end = *start as u64 + bytes.len() as u64;
            if (address as u64) >= *start as u64 && (address as u64) < end {
                bytes[(address - *start) as usize] = byte;
                return;
            }
            if address as u64 == end {
                bytes.push(byte);
                return;
            }
        }
        self.regions.push((address, vec![byte]));
    }

    fn byte(&self, address: u32) -> Option<u8> {
        self.regions.iter().find_map(|(start, bytes)| {
            let offset = address.checked_sub(*start)? as usize;
            bytes.get(offset).copied()
        })
    }
}

impl GuestMemory for SparseMemory {
    fn read(&self, address: u32, length: u32) -> Result<Vec<u8>, SourceError> {
        let mut out = Vec::with_capacity(length as usize);
        for offset in 0..length {
            let Some(at) = address.checked_add(offset) else {
                break;
            };
            match self.byte(at) {
                Some(byte) => out.push(byte),
                None if offset == 0 => {
                    return Err(SourceError {
                        address,
                        length,
                        reason: "unmapped".into(),
                    });
                }
                None => break,
            }
        }
        Ok(out)
    }
}

/// Big-endian `u32` at byte offset `at`. Panics if out of range; callers check extents first.
pub(crate) fn be_u32(bytes: &[u8], at: usize) -> u32 {
    u32::from_be_bytes([bytes[at], bytes[at + 1], bytes[at + 2], bytes[at + 3]])
}

/// Big-endian `u16` at byte offset `at`.
pub(crate) fn be_u16(bytes: &[u8], at: usize) -> u16 {
    u16::from_be_bytes([bytes[at], bytes[at + 1]])
}

/// `count` big-endian words starting at byte offset `at`.
pub(crate) fn be_words<const N: usize>(bytes: &[u8], at: usize) -> [u32; N] {
    std::array::from_fn(|index| be_u32(bytes, at + 4 * index))
}
