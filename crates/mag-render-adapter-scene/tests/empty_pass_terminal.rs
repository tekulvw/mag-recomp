use mag_render_adapter_scene::{
    empty_pass_terminal::*,
    ring_writer::{CasResult, IoError, RingHeader, TerminalIo, WriterError},
};
use std::collections::BTreeMap;

const RING: u32 = 0x10000;
const RING_BYTES: u32 = 0x10000;
const BASE: u32 = 0x10000;

fn user(cursor: u32, epoch: u32) -> [u8; 160] {
    let mut u = [0u8; 160];
    for (at, value) in [
        (0, RING),
        (4, cursor),
        (8, RING_BYTES),
        (12, 0x30000),
        (16, 0x30004),
        (20, 0x30010),
        (24, 0x30020),
        (28, epoch),
        (32, BASE),
        (40, 0x30040),
        (44, 0x5_4320),
        (64, 0x30080),
        (68, 0x40000),
    ] {
        u[at..at + 4].copy_from_slice(&value.to_be_bytes());
    }
    u[36..38].copy_from_slice(&3u16.to_be_bytes());
    u[38..40].copy_from_slice(&5u16.to_be_bytes());
    u[0x30..0x32].copy_from_slice(&7u16.to_be_bytes());
    u[0x4e..0x50].copy_from_slice(&9u16.to_be_bytes());
    u
}
fn source(cursor: u32, epoch: u32) -> EmptyPassSource {
    EmptyPassSource::decode(&user(cursor, epoch)).unwrap()
}

#[derive(Default)]
struct Memory {
    words: BTreeMap<u32, u32>,
    events: Vec<&'static str>,
    copies: Vec<(u32, Vec<u8>)>,
    reference: u32,
    fences: [u32; 16],
    conflict: bool,
}
impl TerminalIo for Memory {
    fn read_word(&mut self, address: u32) -> Result<u32, IoError> {
        self.events.push("read");
        Ok(if address == 0x30004 {
            self.reference
        } else {
            *self.words.get(&address).unwrap_or(&0)
        })
    }
    fn read_fences(&mut self, _: u32) -> Result<[u32; 16], IoError> {
        self.events.push("fences");
        Ok(self.fences)
    }
    fn copy_bytes(&mut self, address: u32, bytes: &[u8]) -> Result<(), IoError> {
        self.events.push("copy");
        self.copies.push((address, bytes.to_vec()));
        Ok(())
    }
    fn write_word(&mut self, address: u32, value: u32) -> Result<(), IoError> {
        self.events.push("write");
        self.words.insert(address, value);
        Ok(())
    }
    fn compare_exchange(
        &mut self,
        address: u32,
        expected: u32,
        value: u32,
    ) -> Result<CasResult, IoError> {
        self.events.push("cas");
        if self.conflict {
            self.conflict = false;
            self.words.insert(address, 7);
            return Ok(CasResult {
                observed: 7,
                stored: false,
            });
        }
        let old = *self.words.get(&address).unwrap_or(&0);
        if old == expected {
            self.words.insert(address, value);
        }
        Ok(CasResult {
            observed: old,
            stored: old == expected,
        })
    }
    fn arm_ring(&mut self, _: RingHeader) -> Result<u64, IoError> {
        panic!("an empty pass never arms a ring handoff")
    }
    fn publish_ring(&mut self, _: u64) -> Result<(), IoError> {
        panic!("an empty pass never publishes a ring handoff")
    }
    fn publish_put(&mut self, address: u32, value: u32) -> Result<(), IoError> {
        self.events.push("put");
        self.words.insert(address, value);
        Ok(())
    }
}
fn memory() -> Memory {
    let mut m = Memory::default();
    m.fences[5] = 1;
    m
}
fn words(bytes: &[u8]) -> Vec<u32> {
    bytes
        .as_chunks::<4>()
        .0
        .iter()
        .map(|b| u32::from_be_bytes(*b))
        .collect()
}

#[test]
fn empty_pass_segment_carries_progress_state_reports_and_continuation() {
    let writer = EmptyPassWriter::prepare(source(0x10080, 0x4000_0000), &DEPTH_EMPTY_PASS).unwrap();
    let w = writer.words();
    // Opening progress semaphore at the slot-three offset 0x4B0.
    assert_eq!(
        &w[..4],
        &[0x41d6c, 0x4b0, 0x41d74, (0x80 + 12) | 0x4000_0000]
    );
    let state: Vec<u32> = DEPTH_GROUP_STATE
        .iter()
        .flat_map(|(m, v)| [0x40000 | m, *v])
        .collect();
    assert_eq!(&w[4..20], state.as_slice());
    // Reports: gated by user+0x30, index user+0x4E first.
    assert_eq!(&w[20..24], &[0x41800, 0x0100_0090, 0x41800, 0x0100_0070]);
    let used = 24 * 4;
    assert_eq!(
        &w[24..28],
        &[0x41d6c, 0x4b0, 0x41d74, (0x80 + used + 12) | 0x4000_0000]
    );
    assert_eq!(w[28], 0x5_4320 | 0x2000_0000);
    assert_eq!(w.len(), 32);
    assert!(w[29..].iter().all(|v| *v == 0));
    assert!(!writer.wraps());
}

#[test]
fn reports_are_absent_without_the_gate_word() {
    let mut u = user(0x10000, 0);
    u[0x30..0x32].fill(0);
    let writer =
        EmptyPassWriter::prepare(EmptyPassSource::decode(&u).unwrap(), &DEPTH_EMPTY_PASS).unwrap();
    let w = writer.words();
    assert_eq!(&w[20..24], &[0x41d6c, 0x4b0, 0x41d74, (20 * 4 + 12)]);
    assert_eq!(w[24], 0x5_4320 | 0x2000_0000);
}

#[test]
fn waits_for_ring_reuse_without_any_effect() {
    let mut writer =
        EmptyPassWriter::prepare(source(0x10080, 0x4000_0000), &DEPTH_EMPTY_PASS).unwrap();
    let mut m = memory();
    // Previous-lap progress that has not yet passed the protected span end.
    m.reference = 0x0000_00c0;
    assert_eq!(writer.drive(&mut m, 64).unwrap(), None);
    assert!(!writer.effects_possible());
    assert_eq!(m.events, ["read"]);
    m.reference = 0x4000_0000 | 0x40;
    let out = writer.drive(&mut m, 64).unwrap().unwrap();
    assert!(writer.effects_possible());
    assert_eq!(out.cursor, 0x10100);
    assert_eq!(out.epoch, 0x4000_0000);
}

#[test]
fn publishes_fence_put_descriptor_epoch_and_state_in_order() {
    let mut writer = EmptyPassWriter::prepare(source(0x10080, 0), &DEPTH_EMPTY_PASS).unwrap();
    let mut m = memory();
    let out = writer.drive(&mut m, 64).unwrap().unwrap();
    assert_eq!(
        m.events,
        [
            "read", "copy", "fences", "put", "write", "read", "cas", "copy", "write", "copy"
        ]
    );
    let command_end = 0x80 + 28 * 4;
    assert_eq!(out.put, Some(command_end));
    assert_eq!(m.words[&0x30000], command_end);
    assert_eq!(m.words[&(0x30040 + 5 * 4)], command_end | 2);
    assert_eq!(m.words[&0x30080], 1);
    assert_eq!(m.copies[0].0, 0x10080);
    assert_eq!(m.copies[0].1.len(), 128);
    let descriptor = words(&m.copies[1].1);
    assert_eq!(m.copies[1].0, 0x40000);
    assert_eq!(
        descriptor,
        [
            0x80 | 0x4000_0000,
            0x100 | (3 << 29),
            0x100 | (5 << 28),
            0x100 | 0xb000_0000 | (1 << 30),
            0,
            u32::MAX,
            command_end,
            command_end,
        ]
    );
    assert_eq!(m.words[&0x30020], 0);
    assert_eq!(
        words(&m.copies[2].1),
        [0x10100, 0x10100 + 8172, 0x10100, u32::MAX]
    );
}

#[test]
fn an_earlier_in_progress_writer_keeps_put_and_receives_our_done_fence() {
    let mut writer = EmptyPassWriter::prepare(source(0x10000, 0), &DEPTH_EMPTY_PASS).unwrap();
    let mut m = memory();
    m.fences[0] = 0x200 | 1;
    m.conflict = true;
    let out = writer.drive(&mut m, 64).unwrap().unwrap();
    assert_eq!(out.put, None);
    assert!(!m.events.contains(&"put"));
    assert_eq!(
        m.events,
        [
            "read", "copy", "fences", "write", "write", "read", "cas", "cas", "copy", "write",
            "copy"
        ]
    );
    assert_eq!(out.descriptor_index, 7);
    let descriptor = words(&m.copies[1].1);
    assert_eq!(m.copies[1].0, 0x40000 + 7 * 32);
    assert_eq!(descriptor[3] & (1 << 30), 0);
    assert_eq!(descriptor[6], u32::MAX);
}

#[test]
fn a_segment_near_the_ring_end_wraps_but_still_returns_through_its_continuation() {
    let cursor = RING + RING_BYTES - 8192 - 128;
    let mut writer =
        EmptyPassWriter::prepare(source(cursor, 0xc000_0000), &DEPTH_EMPTY_PASS).unwrap();
    assert!(writer.wraps());
    assert_eq!(writer.words()[28], 0x5_4320 | 0x2000_0000);
    let mut m = memory();
    m.reference = 0xc000_0000;
    let out = writer.drive(&mut m, 64).unwrap().unwrap();
    assert_eq!(out.cursor, RING);
    // Two-bit generation 3 wraps to 0.
    assert_eq!(out.epoch, 0);
    let descriptor = words(&m.copies[1].1);
    assert_eq!(descriptor[1] >> 28, (3 << 1) | 1);
    assert_eq!(descriptor[3] & 0x0fff_ffff, 0);
}

#[test]
fn unqualified_profiles_refuse_before_any_effect() {
    let mut u = user(0x10000, 0);
    u[36..38].copy_from_slice(&0u16.to_be_bytes());
    assert_eq!(
        EmptyPassWriter::prepare(EmptyPassSource::decode(&u).unwrap(), &DEPTH_EMPTY_PASS).err(),
        Some(EmptyPassError::Profile)
    );
    let mut u = user(0x10000, 0);
    u[0x49] = 1;
    assert_eq!(
        EmptyPassWriter::prepare(EmptyPassSource::decode(&u).unwrap(), &DEPTH_EMPTY_PASS).err(),
        Some(EmptyPassError::Profile)
    );
    let mut u = user(0x10000, 0);
    u[44..48].copy_from_slice(&0x1000_0000u32.to_be_bytes());
    assert_eq!(
        EmptyPassWriter::prepare(EmptyPassSource::decode(&u).unwrap(), &DEPTH_EMPTY_PASS).err(),
        Some(EmptyPassError::Input)
    );
    // Cursor without a full staging span before the ring end.
    let s = source(RING + RING_BYTES - 128, 0);
    assert_eq!(
        EmptyPassWriter::prepare(s, &DEPTH_EMPTY_PASS).err(),
        Some(EmptyPassError::Input)
    );
    assert_eq!(
        EmptyPassSource::decode(&[0; 159]).err(),
        Some(EmptyPassError::Extent)
    );
}

#[test]
fn a_fence_that_is_not_in_progress_fails_closed_after_the_command_copy() {
    let mut writer = EmptyPassWriter::prepare(source(0x10000, 0), &DEPTH_EMPTY_PASS).unwrap();
    let mut m = memory();
    m.fences[5] = 2;
    assert_eq!(writer.drive(&mut m, 64), Err(WriterError::Fence));
    assert!(writer.effects_possible());
    assert_eq!(writer.drive(&mut m, 64), Err(WriterError::Failed));
}

// ---- Side writer slots other than Depth's ----

const SIDE_STATE: [(u32, u32); 3] = [(0x0a74, 1), (0x0a70, 1), (0x0324, 0)];

fn side_user(slot: u16, cursor: u32, epoch: u32) -> [u8; 160] {
    let mut u = user(cursor, epoch);
    u[36..38].copy_from_slice(&slot.to_be_bytes());
    u
}
fn side(slot: u16) -> EmptyPassProfile<'static> {
    EmptyPassProfile {
        writer_slot: slot,
        state: &SIDE_STATE,
        reports: false,
    }
}

#[test]
fn side_slots_use_their_own_progress_offset_and_state_without_reports() {
    for (slot, offset) in [(1u16, 0x490u32), (2, 0x4a0)] {
        // The gate word is nonzero, but this profile emits no reports.
        let source = EmptyPassSource::decode(&side_user(slot, 0x10080, 0x8000_0000)).unwrap();
        let writer = EmptyPassWriter::prepare(source, &side(slot)).unwrap();
        let w = writer.words();
        assert_eq!(
            &w[..4],
            &[0x41d6c, offset, 0x41d74, (0x80 + 12) | 0x8000_0000]
        );
        assert_eq!(&w[4..10], &[0x40a74, 1, 0x40a70, 1, 0x40324, 0]);
        let used = 10 * 4;
        assert_eq!(
            &w[10..14],
            &[0x41d6c, offset, 0x41d74, (0x80 + used + 12) | 0x8000_0000]
        );
        assert_eq!(w[14], 0x5_4320 | 0x2000_0000);
        assert!(w[15..].iter().all(|v| *v == 0));
        assert_eq!(w.len(), 32);
    }
}

#[test]
fn a_side_slot_descriptor_names_its_slot() {
    let source = EmptyPassSource::decode(&side_user(2, 0x10080, 0)).unwrap();
    let mut writer = EmptyPassWriter::prepare(source, &side(2)).unwrap();
    let mut m = memory();
    let out = writer.drive(&mut m, 64).unwrap().unwrap();
    let command_end = 0x80 + 14 * 4;
    assert_eq!(out.put, Some(command_end));
    let descriptor = words(&m.copies[1].1);
    assert_eq!(descriptor[1], 0x100 | (2 << 29));
    assert_eq!(descriptor[2], 0x100 | (5 << 28));
    assert_eq!(descriptor[7], command_end);
}

#[test]
fn the_slot_must_match_the_profile_and_be_a_side_slot() {
    let source = EmptyPassSource::decode(&side_user(1, 0x10000, 0)).unwrap();
    assert_eq!(
        EmptyPassWriter::prepare(source, &side(2)).err(),
        Some(EmptyPassError::Profile)
    );
    for slot in [0u16, 16] {
        let source = EmptyPassSource::decode(&side_user(slot, 0x10000, 0)).unwrap();
        assert_eq!(
            EmptyPassWriter::prepare(source, &side(slot)).err(),
            Some(EmptyPassError::Profile)
        );
    }
}

#[test]
fn every_generation_advances_by_one_on_a_wrap_and_releases_carry_the_current_one() {
    let cursor = RING + RING_BYTES - 8192 - 128;
    for generation in 0..4u32 {
        let epoch = generation << 30;
        let source = EmptyPassSource::decode(&side_user(1, cursor, epoch)).unwrap();
        let mut writer = EmptyPassWriter::prepare(source, &side(1)).unwrap();
        assert!(writer.wraps());
        let w = writer.words().to_vec();
        assert_eq!(w[3] >> 30, generation);
        assert_eq!(w[13] >> 30, generation);
        // A side slot still returns through its continuation on a wrap.
        assert_eq!(w[14], 0x5_4320 | 0x2000_0000);
        let mut m = memory();
        m.reference = epoch;
        let out = writer.drive(&mut m, 64).unwrap().unwrap();
        assert_eq!(out.cursor, RING);
        assert_eq!(out.epoch, ((generation + 1) & 3) << 30);
        assert_eq!(m.words[&0x30020], out.epoch);
    }
}

#[test]
fn reuse_needs_the_same_generation_behind_the_start_or_the_previous_past_the_end() {
    use mag_render_adapter_scene::ring_writer::reusable;
    let epoch = 2 << 30;
    assert!(reusable(epoch | 0x100, epoch, 0x100, 0x200));
    assert!(!reusable(epoch | 0x101, epoch, 0x100, 0x200));
    assert!(reusable((1 << 30) | 0x200, epoch, 0x100, 0x200));
    assert!(!reusable((1 << 30) | 0x1ff, epoch, 0x100, 0x200));
    assert!(!reusable(0x300, epoch, 0x100, 0x200));
    // Generation 0 follows 3.
    assert!(reusable((3 << 30) | 0x200, 0, 0x100, 0x200));
}

#[test]
fn progress_offsets_and_generation_steps() {
    use mag_render_adapter_scene::ring_writer::{advance_epoch, progress_offset};
    assert_eq!(
        [0u16, 1, 2, 3].map(progress_offset),
        [0x480, 0x490, 0x4a0, 0x4b0]
    );
    assert_eq!(
        [0u32, 1, 2, 3].map(|g| advance_epoch(g << 30) >> 30),
        [1, 2, 3, 0]
    );
}
