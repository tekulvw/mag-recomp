//! RSX memory locations as guest reads (`group-zero-deformation.md`, "Guest addresses of the
//! instance rows and colour pool").
//!
//! A location word names RSX memory, not a guest address: bit 31 selects the location (0 local
//! video memory, 1 main memory) and bits 0–30 are the byte offset in it. Local offsets are added
//! to the base of the local-memory mapping. Main-memory offsets are RSX I/O offsets, translated
//! through the GCM I/O mapping in 1 MiB pages that need not be contiguous in guest memory, so a
//! range that crosses a page is read page by page.

use crate::guest::{GuestMemory, SourceError};

/// Size of one GCM I/O page.
pub const IO_PAGE: u32 = 1 << 20;

/// The GCM address map the caller observed.
pub trait RsxAddressMap {
    /// Guest address and size of the RSX local-memory mapping, if mapped.
    fn local_memory(&self) -> Option<(u32, u32)>;
    /// Guest address of I/O page `page` (offsets `page·2^20 ..`), if mapped.
    fn io_page(&self, page: u32) -> Option<u32>;
}

fn unmapped(location: u32, length: u32, reason: &str) -> SourceError {
    SourceError {
        address: location,
        length,
        reason: reason.into(),
    }
}

fn read_exact<M: GuestMemory + ?Sized>(
    memory: &M,
    address: u32,
    length: u32,
) -> Result<Vec<u8>, SourceError> {
    if address as u64 + length as u64 > 1 << 32 {
        return Err(unmapped(address, length, "guest range passes 2^32"));
    }
    let bytes = memory.read(address, length)?;
    if bytes.len() != length as usize {
        return Err(unmapped(address, length, "short read"));
    }
    Ok(bytes)
}

/// Reads `length` bytes at the RSX location `location`.
pub fn read_rsx_location<M: GuestMemory + ?Sized>(
    memory: &M,
    map: &dyn RsxAddressMap,
    location: u32,
    length: u32,
) -> Result<Vec<u8>, SourceError> {
    let offset = location & 0x7fff_ffff;
    if location >> 31 == 0 {
        let (base, size) = map
            .local_memory()
            .ok_or_else(|| unmapped(location, length, "local memory unmapped"))?;
        if offset as u64 + length as u64 > size as u64 {
            return Err(unmapped(location, length, "past the local-memory mapping"));
        }
        let address = base
            .checked_add(offset)
            .ok_or_else(|| unmapped(location, length, "local address overflow"))?;
        return read_exact(memory, address, length);
    }
    let mut out = Vec::with_capacity(length as usize);
    let mut at = offset as u64;
    let end = offset as u64 + length as u64;
    while at < end {
        let page = (at / IO_PAGE as u64) as u32;
        let within = (at % IO_PAGE as u64) as u32;
        let piece = (end - at).min((IO_PAGE - within) as u64) as u32;
        let guest = map
            .io_page(page)
            .ok_or_else(|| unmapped(location, length, "I/O page unmapped"))?;
        let address = guest
            .checked_add(within)
            .ok_or_else(|| unmapped(location, length, "I/O address overflow"))?;
        out.extend(read_exact(memory, address, piece)?);
        at += piece as u64;
    }
    Ok(out)
}

/// A fixed address map, for callers that hold a copy of the mapping table.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct FixedAddressMap {
    pub local: Option<(u32, u32)>,
    /// `(I/O page, guest address)` pairs.
    pub io_pages: Vec<(u32, u32)>,
}

impl RsxAddressMap for FixedAddressMap {
    fn local_memory(&self) -> Option<(u32, u32)> {
        self.local
    }
    fn io_page(&self, page: u32) -> Option<u32> {
        self.io_pages
            .iter()
            .find(|(p, _)| *p == page)
            .map(|(_, guest)| *guest)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::guest::SparseMemory;

    /// GZ-8, colour-pool part.
    #[test]
    fn gz8_colour_pool_locations() {
        let mut memory = SparseMemory::new();
        memory.put(0x3050_0010, &[1, 2, 3, 4]);
        memory.put(0x305f_fff8, &[10, 11, 12, 13, 14, 15, 16, 17]);
        memory.put(0x3090_0000, &[20, 21, 22, 23, 24, 25, 26, 27]);
        memory.put(0x4000_0400, &[9, 9, 9, 9]);
        let map = FixedAddressMap {
            local: Some((0x4000_0000, 0x1000)),
            io_pages: vec![(2, 0x3050_0000), (3, 0x3090_0000)],
        };
        assert_eq!(
            read_rsx_location(&memory, &map, 0x8020_0010, 4),
            Ok(vec![1, 2, 3, 4])
        );
        // Crossing from I/O page 2 into page 3.
        assert_eq!(
            read_rsx_location(&memory, &map, 0x802f_fff8, 16),
            Ok(vec![
                10, 11, 12, 13, 14, 15, 16, 17, 20, 21, 22, 23, 24, 25, 26, 27
            ])
        );
        assert_eq!(
            read_rsx_location(&memory, &map, 0x0000_0400, 4),
            Ok(vec![9; 4])
        );
        // An unmapped I/O page and a range past local memory.
        assert!(read_rsx_location(&memory, &map, 0x8040_0000, 4).is_err());
        assert!(read_rsx_location(&memory, &map, 0x0000_0ffe, 4).is_err());
        assert!(read_rsx_location(&memory, &FixedAddressMap::default(), 0x400, 4).is_err());
    }
}
