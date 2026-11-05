//! Main's ring transport (writer slot 0) with no child descriptors or holes.
//!
//! When the frontend renders the Main pass natively, the Main job's own ring
//! segment shrinks to its terminal obligations: the progress markers, the
//! reference value operand, the optional report pair, the retained terminal
//! state calls the job input names, and (opt-in) the end-of-pass auxiliary
//! records. The segment is handed off to the host's ring handoff so the native
//! packet runs when the consumer reaches it, then published like any other
//! segment ([`crate::ring_writer`]).
//!
//! The host supplies the real VM and coherent services. Completion here means
//! these transport effects only: never the pass's other finish effects, the
//! original job's completion, a GPU terminal or presentation.
use crate::material_finish::OwnedMainFinish;
pub use crate::ring_writer::{CasResult, IoError, RingHeader, TerminalIo, WriterError, reusable};
use crate::ring_writer::{POSITION, Publisher, STAGING, close_segment, progress_marker};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PublishedTerminal {
    cursor: u32,
    epoch: u32,
    ring_cookie: u64,
}
impl PublishedTerminal {
    pub fn cursor(self) -> u32 {
        self.cursor
    }
    pub fn epoch(self) -> u32 {
        self.epoch
    }
    pub fn ring_cookie(self) -> u64 {
        self.ring_cookie
    }
}
pub struct TerminalWriter {
    publisher: Publisher,
    auxiliary: AuxiliaryFlush,
}
/// What the end-of-pass auxiliary flush emitted into this transport.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct AuxiliaryFlush {
    pub records: u32,
    pub state_calls: u32,
}
fn method(words: &mut Vec<u32>, method: u32, values: &[u32]) {
    words.push(((values.len() as u32) << 18) | method);
    words.extend_from_slice(values);
}
fn call_target(target: u32) -> Result<u32, WriterError> {
    if target == 0 || !target.is_multiple_of(4) || target & 0xe000_0000 != 0 {
        return Err(WriterError::Input);
    }
    Ok(target | 2)
}
/// End-of-pass auxiliary records, emitted after the native Main callback and
/// before the reference value. First the fixed-function state these records
/// inherit from the transparent group: RGB colour mask, source-alpha blending,
/// depth test on, depth writes off, alpha test off. Then every record whose
/// threshold is above zero, in table order up to the first that is not. Each
/// record first switches to its state set when the pass flags enable state
/// switching (bit 1) and the set differs, disables the sixteen vertex arrays,
/// then calls its list. A final switch restores set B. The transparent group's
/// shader inputs are not re-established here.
fn auxiliary_flush(
    finish: &OwnedMainFinish,
    records: &[[u32; 2]],
    words: &mut Vec<u32>,
) -> Result<AuxiliaryFlush, WriterError> {
    let positive = |bits: u32| bits & 0x7f80_0000 != 0 && f32::from_bits(bits) > 0.0;
    let mut selected = Vec::new();
    for &[threshold, target] in records {
        if threshold & 0x7f80_0000 == 0x7f80_0000 || target & 2 != 0 {
            return Err(WriterError::Input);
        }
        if !positive(threshold) {
            break;
        }
        call_target(target & !1)?;
        selected.push(target);
    }
    let mut flush = AuxiliaryFlush::default();
    if selected.is_empty() {
        return Ok(flush);
    }
    let switching = finish.source_words.pass_flags & 2 != 0;
    let sets = [finish.auxiliary_state_target, finish.restore_target];
    if switching {
        for set in sets {
            call_target(set)?;
        }
    }
    for (m, v) in [
        (0x1b8c, 0),
        (0x1bac, 0),
        (0x324, 0x0001_0101),
        (0x370, 0xeee0),
    ] {
        method(words, m, &[v]);
    }
    method(words, 0x314, &[0x0302_0302, 0x0303_0303]);
    for (m, v) in [
        (0xa74, 1),
        (0xa70, 0),
        (0x310, 1),
        (0x304, 0),
        (0xa70, 0),
        (0x310, 1),
    ] {
        method(words, m, &[v]);
    }
    let mut state_b = true;
    for target in selected {
        let wants_b = target & 1 != 0;
        if switching && wants_b != state_b {
            words.push(call_target(sets[usize::from(wants_b)])?);
            flush.state_calls += 1;
            state_b = wants_b;
        }
        for index in 0..16 {
            method(words, 0x1740 + index * 4, &[2]);
        }
        words.push(call_target(target & !1)?);
        flush.records += 1;
    }
    if !state_b {
        words.push(call_target(finish.restore_target)?);
        flush.state_calls += 1;
    }
    Ok(flush)
}
impl TerminalWriter {
    /// `reference_value` is a command operand only. The caller must separately
    /// perform the real reference increment and exchange and every other
    /// finish effect.
    pub fn prepare(
        finish: &OwnedMainFinish,
        reference_value: Option<u32>,
    ) -> Result<Self, WriterError> {
        let source = finish.writer;
        let end = source
            .ring_start
            .checked_add(source.ring_bytes)
            .ok_or(WriterError::Input)?;
        if source.writer_slot != 0
            || source.fence_slot >= 16
            || source.epoch & POSITION != 0
            || !source.ring_start.is_multiple_of(128)
            || !source.cursor.is_multiple_of(128)
            || !source.ring_bytes.is_multiple_of(128)
            || source.ring_bytes < 2 * STAGING + 128
            || source.cursor < source.ring_start
            || source
                .cursor
                .checked_add(STAGING + 128)
                .is_none_or(|p| p > end)
            || [
                source.put_address,
                source.reference_address,
                source.state_address,
                source.epoch_address,
                source.fence_table_address,
                source.reservation_index_address,
                source.descriptor_ring_address,
            ]
            .iter()
            .any(|p| *p == 0 || !p.is_multiple_of(4))
            || finish.lists_enabled != reference_value.is_some()
        {
            return Err(WriterError::Input);
        }
        // Validate the entire bounded consumer tables before any ring arm,
        // reference atomic or publication can make fallback unsafe.
        source
            .fence_table_address
            .checked_add(64)
            .ok_or(WriterError::Input)?;
        source
            .descriptor_ring_address
            .checked_add(4096 * 32)
            .ok_or(WriterError::Input)?;
        source
            .state_address
            .checked_add(16)
            .ok_or(WriterError::Input)?;
        crate::ring_writer::ring_offset(&source, end)?;
        let start = crate::ring_writer::ring_offset(&source, source.cursor)?;
        let mut words = progress_marker(0, (start + 12) | source.epoch).to_vec();
        let auxiliary = match (&finish.auxiliary_records, finish.lists_enabled) {
            (Some(records), true) => auxiliary_flush(finish, records, &mut words)?,
            _ => AuxiliaryFlush::default(),
        };
        if let Some(value) = reference_value {
            words.extend_from_slice(&[0x40050, value]);
        }
        if finish.first_report != 0 {
            for index in [finish.second_report, finish.first_report] {
                words.extend_from_slice(&[0x41800, 0x0100_0000 | (u32::from(index) << 4)]);
            }
        }
        // Only the terminal state calls named by the job input are kept. They
        // run after the native callback, in the input's order; no skipped item
        // or child command stream is reintroduced. Each group of calls is
        // preceded by resetting the sixteen vertex array formats to float,
        // size zero.
        let reset_arrays = |words: &mut Vec<u32>| {
            for index in 0..16 {
                words.extend_from_slice(&[0x41740 + index * 4, 2]);
            }
        };
        let call = |words: &mut Vec<u32>, index: usize| -> Result<(), WriterError> {
            words.push(call_target(finish.terminal_targets[index])?);
            Ok(())
        };
        if finish.lists_enabled {
            reset_arrays(&mut words);
            for index in 0..4 {
                call(&mut words, index)?;
            }
        }
        reset_arrays(&mut words);
        for index in [4, 7] {
            call(&mut words, index)?;
        }
        let optional = finish.terminal_targets[8] != u32::MAX;
        if finish.source_words.pass_flags & 1 != 0 && optional {
            call(&mut words, 8)?;
        }
        call(&mut words, 5)?;
        if finish.source_words.pass_flags & 1 == 0 && optional {
            call(&mut words, 8)?;
        }
        call(&mut words, 6)?;
        let segment = close_segment(&source, words, end)?;
        Ok(Self {
            publisher: Publisher::new(source, segment, true),
            auxiliary,
        })
    }
    pub fn reuse_ready(&self, reference: u32) -> bool {
        self.publisher.reuse_ready(reference)
    }
    /// The prepared transport command words (read-only).
    pub fn words(&self) -> &[u32] {
        self.publisher.segment().words()
    }
    pub fn auxiliary_flush(&self) -> AuxiliaryFlush {
        self.auxiliary
    }
    pub fn effects_possible(&self) -> bool {
        self.publisher.effects_possible()
    }
    pub fn drive(
        &mut self,
        io: &mut impl TerminalIo,
        steps: usize,
    ) -> Result<Option<PublishedTerminal>, WriterError> {
        Ok(self
            .publisher
            .drive(io, steps)?
            .map(|done| PublishedTerminal {
                cursor: done.cursor,
                epoch: done.epoch,
                ring_cookie: done.ring_cookie,
            }))
    }
}

/// Process totals of emitted auxiliary flushes (opt-in), reported rate-limited.
pub fn count_auxiliary_flush(flush: AuxiliaryFlush) {
    use std::sync::atomic::{AtomicU64, Ordering};
    static FLUSHES: AtomicU64 = AtomicU64::new(0);
    static RECORDS: AtomicU64 = AtomicU64::new(0);
    static CALLS: AtomicU64 = AtomicU64::new(0);
    if flush.records == 0 {
        return;
    }
    let n = FLUSHES.fetch_add(1, Ordering::Relaxed) + 1;
    let records =
        RECORDS.fetch_add(u64::from(flush.records), Ordering::Relaxed) + u64::from(flush.records);
    let calls = CALLS.fetch_add(u64::from(flush.state_calls), Ordering::Relaxed)
        + u64::from(flush.state_calls);
    if n <= 4 || n.is_multiple_of(300) {
        eprintln!(
            "[mag main auxiliary flush] flushes={n} records_total={records} state_calls_total={calls} last_records={} last_state_calls={}",
            flush.records, flush.state_calls
        );
    }
}
