use mag_render_adapter_scene::{
    main_terminal_writer::*,
    material_finish::{OwnedMainFinish, SourceFinishWords},
};
use std::collections::BTreeMap;
fn finish(cursor: u32, epoch: u32) -> OwnedMainFinish {
    let mut u = [0u8; 160];
    for (at, value) in [
        (0, 0x10000),
        (4, cursor),
        (8, 0x10000),
        (12, 0x30000),
        (16, 0x30004),
        (20, 0x30010),
        (24, 0x30020),
        (28, epoch),
        (32, 0x10000),
        (40, 0x30040),
        (44, 3),
        (64, 0x30080),
        (68, 0x40000),
    ] {
        u[at..at + 4].copy_from_slice(&value.to_be_bytes());
    }
    for i in 0..9 {
        u[0x70 + i * 4..0x74 + i * 4]
            .copy_from_slice(&(0x80000u32 + i as u32 * 0x100).to_be_bytes());
    }
    u[0x90..0x94].copy_from_slice(&u32::MAX.to_be_bytes());
    u[38..40].copy_from_slice(&5u16.to_be_bytes());
    OwnedMainFinish::decode(
        0x60000,
        0x61000,
        &u,
        SourceFinishWords {
            reference_increment_address: 0,
            reference_exchange_address: 0,
            auxiliary_table_address: 0,
            auxiliary_table_count: 0,
            pass_flags: 0,
        },
    )
    .unwrap()
}
#[derive(Default)]
struct Memory {
    words: BTreeMap<u32, u32>,
    events: Vec<&'static str>,
    copies: Vec<(u32, Vec<u8>)>,
    reference: u32,
    fences: [u32; 16],
    conflict: bool,
    fail_put: bool,
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
            Ok(CasResult {
                observed: 7,
                stored: false,
            })
        } else {
            let old = *self.words.get(&address).unwrap_or(&0);
            if old == expected {
                self.words.insert(address, value);
            }
            Ok(CasResult {
                observed: old,
                stored: old == expected,
            })
        }
    }
    fn arm_ring(&mut self, header: RingHeader) -> Result<u64, IoError> {
        self.events.push("arm");
        assert_eq!((header.header, header.argument), (0x41d6c, 72 << 4));
        Ok(91)
    }
    fn publish_ring(&mut self, cookie: u64) -> Result<(), IoError> {
        self.events.push("publish");
        assert_eq!(cookie, 91);
        Ok(())
    }
    fn publish_put(&mut self, address: u32, value: u32) -> Result<(), IoError> {
        self.events.push("put");
        if self.fail_put {
            Err(IoError::Failed)
        } else {
            self.words.insert(address, value);
            Ok(())
        }
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
fn actual_terminal_transport_arms_after_reuse_and_publishes_before_put() {
    let mut writer = TerminalWriter::prepare(&finish(0x10000, 0), None).unwrap();
    let mut m = memory();
    let out = writer.drive(&mut m, 64).unwrap().unwrap();
    assert_eq!(out.cursor(), 0x10100);
    assert_eq!(out.epoch(), 0);
    let at = |e| m.events.iter().position(|v| *v == e).unwrap();
    assert!(
        at("read") < at("arm")
            && at("arm") < at("copy")
            && at("copy") < at("publish")
            && at("publish") < at("put")
    );
    assert_eq!(m.words[&0x30000], 176);
    assert_eq!(m.words[&0x30054], 178);
    assert_eq!(
        words(&m.copies[0].1)[..8],
        [0x41d6c, 0x480, 0x41d74, 12, 0x41740, 2, 0x41744, 2]
    );
    assert_eq!(
        words(&m.copies[1].1),
        [
            0x40000000,
            256,
            0x50000100,
            0xf0000100,
            0,
            u32::MAX,
            176,
            176
        ]
    );
    assert_eq!(words(&m.copies[2].1), [0x10100, 0x120ec, 0x10100, u32::MAX]);
    assert!(writer.drive(&mut m, 64).unwrap().is_some()); /* no replay */
    assert_eq!(m.events.iter().filter(|e| **e == "put").count(), 1);
}
#[test]
fn real_reuse_wait_does_not_arm_or_write_an_old_ring_generation() {
    let mut writer = TerminalWriter::prepare(&finish(0x10000, 0), None).unwrap();
    let mut m = memory();
    m.reference = 1;
    assert!(writer.drive(&mut m, 64).unwrap().is_none());
    assert_eq!(m.events, ["read"]);
    m.reference = 0;
    assert!(writer.drive(&mut m, 1).unwrap().is_none());
    assert!(writer.drive(&mut m, 64).unwrap().is_some());
}
#[test]
fn reservation_conflict_does_not_replay_commands_and_wrap_preserves_slot_zero_rule() {
    let mut writer = TerminalWriter::prepare(&finish(0x1df80, 0xc0000000), None).unwrap();
    let mut m = memory();
    m.reference = 0xc0000000;
    m.conflict = true;
    let out = writer.drive(&mut m, 64).unwrap().unwrap();
    assert_eq!((out.cursor(), out.epoch()), (0x10000, 0));
    assert_eq!(m.events.iter().filter(|e| **e == "copy").count(), 3);
    let copy = words(&m.copies[0].1);
    assert_eq!(copy[44], 0x20000000); /* ring base, never user2C */
    assert_eq!(m.copies[1].0, 0x400e0);
    assert_eq!(m.words[&0x30080], 8);
}
#[test]
fn failed_irreversible_put_cannot_restart_or_claim_completion() {
    let mut writer = TerminalWriter::prepare(&finish(0x10000, 0), None).unwrap();
    let mut m = memory();
    m.fail_put = true;
    assert_eq!(
        writer.drive(&mut m, 64),
        Err(WriterError::Io(IoError::Failed))
    );
    assert!(writer.effects_possible());
    let events = m.events.len();
    assert_eq!(writer.drive(&mut m, 64), Err(WriterError::Failed));
    assert_eq!(m.events.len(), events);
}
#[test]
fn retained_terminal_calls_preserve_source_order_and_array_reset_frontiers() {
    for flag in [0, 1] {
        let mut f = finish(0x10000, 0);
        f.lists_enabled = true;
        f.source_words.pass_flags = flag;
        f.terminal_targets = core::array::from_fn(|i| 0x80000 + i as u32 * 0x100);
        let mut writer = TerminalWriter::prepare(&f, Some(29)).unwrap();
        let mut m = memory();
        writer.drive(&mut m, 64).unwrap().unwrap();
        let command = words(&m.copies[0].1);
        let calls: Vec<_> = command
            .iter()
            .copied()
            .filter(|w| *w >= 0x80000 && w & 3 == 2)
            .collect();
        let mut order = vec![0, 1, 2, 3, 4, 7];
        if flag != 0 {
            order.push(8);
        }
        order.push(5);
        if flag == 0 {
            order.push(8);
        }
        order.push(6);
        assert_eq!(
            calls,
            order
                .iter()
                .map(|i| f.terminal_targets[*i] | 2)
                .collect::<Vec<_>>()
        );
        let reset: Vec<_> = (0..16).flat_map(|i| [0x41740 + 4 * i, 2]).collect();
        let first = command
            .windows(reset.len())
            .position(|w| w == reset)
            .unwrap();
        assert_eq!(
            &command[first + reset.len()..first + reset.len() + 4],
            &[0x80002, 0x80102, 0x80202, 0x80302]
        );
        let second = first + reset.len() + 4;
        assert_eq!(&command[second..second + reset.len()], reset);
        let reference = command.windows(2).position(|w| w == [0x40050, 29]).unwrap();
        assert!(reference < first);
        let final_semaphore = command
            .windows(2)
            .rposition(|w| w == [0x41d6c, 0x480])
            .unwrap();
        assert!(final_semaphore > second + reset.len() + 4);
    }
}
#[test]
fn terminal_optional_sentinel_and_malformed_required_call_are_distinct() {
    let mut f = finish(0x10000, 0);
    f.terminal_targets = core::array::from_fn(|i| 0x80000 + i as u32 * 0x100);
    f.terminal_targets[8] = u32::MAX;
    let mut writer = TerminalWriter::prepare(&f, None).unwrap();
    let mut m = memory();
    writer.drive(&mut m, 64).unwrap().unwrap();
    let command = words(&m.copies[0].1);
    assert!(!command.contains(&u32::MAX));
    assert_eq!(
        command
            .iter()
            .filter(|w| **w >= 0x80000 && **w & 3 == 2)
            .count(),
        4
    );
    f.terminal_targets[4] = 3;
    assert!(matches!(
        TerminalWriter::prepare(&f, None),
        Err(WriterError::Input)
    ));
}
