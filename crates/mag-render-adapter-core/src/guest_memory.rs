//! Checked reads from an immutable caller-owned byte snapshot.
//!
//! Immutability proves consistency of these supplied bytes only. It does not
//! prove they were captured at a real retail writer frontier or scene epoch.
use crate::error::Error;
use core::ops::Range;

/// A synchronous borrow of an immutable snapshot, never a live guest VM pointer.
pub struct GuestSnapshot<'a> {
    base: u32,
    bytes: &'a [u8],
}
impl<'a> GuestSnapshot<'a> {
    pub fn new(base: u32, bytes: &'a [u8]) -> Result<Self, Error> {
        let end = u64::from(base)
            .checked_add(u64::try_from(bytes.len()).map_err(|_| Error::InvalidRange)?)
            .ok_or(Error::InvalidRange)?;
        if end > 1u64 << 32 {
            return Err(Error::InvalidRange);
        }
        Ok(Self { base, bytes })
    }
    /// Checked index range of `length` bytes at guest `address` in this snapshot.
    pub fn range(&self, address: u64, length: usize) -> Result<Range<usize>, Error> {
        let end = address
            .checked_add(u64::try_from(length).map_err(|_| Error::InvalidRange)?)
            .ok_or(Error::InvalidRange)?;
        if address >= 1u64 << 32 || end > 1u64 << 32 {
            return Err(Error::InvalidRange);
        }
        let offset = address
            .checked_sub(u64::from(self.base))
            .ok_or(Error::OutsideSnapshot)?;
        let start = usize::try_from(offset).map_err(|_| Error::OutsideSnapshot)?;
        let end = start.checked_add(length).ok_or(Error::OutsideSnapshot)?;
        if end > self.bytes.len() {
            return Err(Error::OutsideSnapshot);
        }
        Ok(start..end)
    }
    /// Own exactly the checked supplied bytes. This does not acquire a live
    /// resource lease or establish the source writer's completion.
    pub fn copy_bytes(&self, address: u64, length: usize) -> Result<Vec<u8>, Error> {
        let range = self.range(address, length)?;
        let mut result = Vec::new();
        result
            .try_reserve_exact(length)
            .map_err(|_| Error::AllocationFailed)?;
        result.extend_from_slice(&self.bytes[range]);
        Ok(result)
    }
    /// Preserve every guest word, including NaNs and signed zero float patterns.
    pub fn words<const N: usize>(&self, address: u64) -> Result<[u32; N], Error> {
        let length = N.checked_mul(4).ok_or(Error::InvalidRange)?;
        let source = &self.bytes[self.range(address, length)?];
        Ok(core::array::from_fn(|index| {
            let offset = index * 4;
            u32::from_be_bytes([
                source[offset],
                source[offset + 1],
                source[offset + 2],
                source[offset + 3],
            ])
        }))
    }
}
