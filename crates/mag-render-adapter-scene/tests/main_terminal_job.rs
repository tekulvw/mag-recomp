use mag_render_adapter_scene::{
    main_terminal_job::*,
    main_terminal_writer::*,
    material_finish::{OwnedMainFinish, SourceFinishWords},
};
use std::collections::BTreeMap;
fn finish() -> OwnedMainFinish {
    let mut user = [0u8; 160];
    for (at, value) in [
        (0, 0x10000u32),
        (4, 0x10000),
        (8, 0x10000),
        (12, 0x30000),
        (16, 0x30004),
        (20, 0x30010),
        (24, 0x30020),
        (32, 0x10000),
        (40, 0x30040),
        (64, 0x30080),
        (68, 0x40000),
    ] {
        user[at..at + 4].copy_from_slice(&value.to_be_bytes());
    }
    for i in 0..9 {
        user[0x70 + i * 4..0x74 + i * 4]
            .copy_from_slice(&(0x80000u32 + i as u32 * 0x100).to_be_bytes());
    }
    user[0x90..0x94].copy_from_slice(&u32::MAX.to_be_bytes());
    user[38..40].copy_from_slice(&5u16.to_be_bytes());
    user[0x4c] = 1;
    OwnedMainFinish::decode(
        0x60000,
        0x61000,
        &user,
        SourceFinishWords {
            reference_increment_address: 0x31000,
            reference_exchange_address: 0x31004,
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
    conflict: bool,
    fail_exchange: bool,
}
impl TerminalIo for Memory {
    fn read_word(&mut self, a: u32) -> Result<u32, IoError> {
        self.events.push("read");
        Ok(*self.words.get(&a).unwrap_or(&0))
    }
    fn read_fences(&mut self, _: u32) -> Result<[u32; 16], IoError> {
        let mut a = [0; 16];
        a[5] = 1;
        Ok(a)
    }
    fn copy_bytes(&mut self, _: u32, b: &[u8]) -> Result<(), IoError> {
        self.events
            .push(if b.len() == 128 { "commands" } else { "copy" });
        Ok(())
    }
    fn write_word(&mut self, a: u32, v: u32) -> Result<(), IoError> {
        self.words.insert(a, v);
        Ok(())
    }
    fn compare_exchange(&mut self, a: u32, e: u32, v: u32) -> Result<CasResult, IoError> {
        self.events.push(if a == 0x31000 {
            "increment"
        } else if a == 0x31004 {
            "exchange"
        } else {
            "reserve"
        });
        if a == 0x31004 && self.fail_exchange {
            return Err(IoError::Failed);
        }
        let old = *self.words.get(&a).unwrap_or(&0);
        if self.conflict && a == 0x31000 {
            self.conflict = false;
            self.words.insert(a, old + 1);
            return Ok(CasResult {
                observed: old + 1,
                stored: false,
            });
        }
        if old == e {
            self.words.insert(a, v);
        }
        Ok(CasResult {
            observed: old,
            stored: old == e,
        })
    }
    fn arm_ring(&mut self, _: RingHeader) -> Result<u64, IoError> {
        self.events.push("arm");
        Ok(1)
    }
    fn publish_ring(&mut self, _: u64) -> Result<(), IoError> {
        self.events.push("published");
        Ok(())
    }
    fn publish_put(&mut self, a: u32, v: u32) -> Result<(), IoError> {
        self.events.push("put");
        self.words.insert(a, v);
        Ok(())
    }
}
#[test]
fn exact_cpu_reference_effects_precede_transport_and_never_replay() {
    let mut io = Memory {
        conflict: true,
        ..Memory::default()
    };
    io.words.insert(0x31000, 40);
    let mut job = TerminalJob::prepare(finish()).unwrap();
    let result = job.drive(&mut io, 96).unwrap().unwrap();
    assert_eq!(result.reference_value(), Some(42));
    assert_eq!(io.words[&0x31000], 42);
    assert_eq!(io.words[&0x31004], 42);
    let at = |s| io.events.iter().position(|x| *x == s).unwrap();
    assert!(
        at("increment") < at("exchange")
            && at("exchange") < at("arm")
            && at("published") < at("put")
    );
    let count = io.events.len();
    assert!(job.drive(&mut io, 96).unwrap().is_some());
    assert_eq!(io.events.len(), count);
}
#[test]
fn reuse_wait_precedes_even_the_irreversible_reference_increment() {
    let mut io = Memory::default();
    io.words.insert(0x30004, 1);
    let mut job = TerminalJob::prepare(finish()).unwrap();
    assert!(job.drive(&mut io, 96).unwrap().is_none());
    assert!(!job.effects_possible());
    assert!(!io.words.contains_key(&0x31000));
    io.words.insert(0x30004, 0);
    assert!(job.drive(&mut io, 96).unwrap().is_some());
}
#[test]
fn failure_after_atomic_increment_cannot_fall_back_or_publish_cpu_completion() {
    let mut io = Memory {
        fail_exchange: true,
        ..Memory::default()
    };
    let mut job = TerminalJob::prepare(finish()).unwrap();
    assert!(job.drive(&mut io, 96).is_err());
    assert!(job.effects_possible());
    assert!(!io.events.contains(&"arm"));
    let count = io.events.len();
    assert_eq!(job.drive(&mut io, 96), Err(JobError::Failed));
    assert_eq!(io.events.len(), count);
}
#[test]
fn overflowing_descriptor_or_fence_ranges_are_rejected_before_any_effect() {
    let mut f = finish();
    f.writer.descriptor_ring_address = 0xfffffff0;
    assert!(TerminalJob::prepare(f).is_err());
    let mut f = finish();
    f.writer.fence_table_address = 0xfffffff0;
    assert!(TerminalJob::prepare(f).is_err());
}
