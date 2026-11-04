//! `GuestSnapshot` as a behaviour [`GuestMemory`].

use mag_render_adapter_core::guest_memory::GuestSnapshot;
use mag_render_behavior::guest::{GuestMemory, SourceError};

/// A borrowed adapter-core snapshot read through the behaviour crate's reader interface.
pub struct SnapshotMemory<'a>(pub GuestSnapshot<'a>);

impl GuestMemory for SnapshotMemory<'_> {
    fn read(&self, address: u32, length: u32) -> Result<Vec<u8>, SourceError> {
        self.0
            .copy_bytes(address as u64, length as usize)
            .map_err(|error| SourceError {
                address,
                length,
                reason: format!("{error:?}"),
            })
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use mag_render_behavior::setup::copy_setup_bank;

    #[test]
    fn reads_inside_and_refuses_outside_the_snapshot() {
        let bytes = [0u8, 1, 2, 3, 4, 5, 6, 7];
        let memory = SnapshotMemory(GuestSnapshot::new(0x1000, &bytes).unwrap());
        assert_eq!(memory.read(0x1002, 4).unwrap(), vec![2, 3, 4, 5]);
        let error = memory.read(0x1006, 4).unwrap_err();
        assert_eq!((error.address, error.length), (0x1006, 4));
        assert!(error.reason.contains("OutsideSnapshot"));
        assert!(memory.read(0xffff_fffe, 4).is_err());
    }

    #[test]
    fn behaviour_readers_run_on_a_snapshot() {
        // A setup bank: one depth-test write, then return.
        let words: [u32; 3] = [0x0004_0a74, 1, 0x0002_0000];
        let bytes: Vec<u8> = words.iter().flat_map(|w| w.to_be_bytes()).collect();
        let memory = SnapshotMemory(GuestSnapshot::new(0x2000, &bytes).unwrap());
        let bank = copy_setup_bank(&memory, 0x2000, 16, 4).unwrap();
        assert_eq!(bank.methods.len(), 1);
        assert_eq!((bank.methods[0].method, bank.methods[0].value), (0xa74, 1));
    }
}
