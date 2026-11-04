//! Command-ring segment transport shared by the native pass writers.
//!
//! A pass that the frontend renders natively still owns one segment of the
//! game's shared command ring. That segment must look exactly like a normally
//! completed one to later writers, to the command consumer and to the next pass
//! builder. This module holds the parts that every such writer shares:
//!
//! - the guest service interface ([`TerminalIo`]);
//! - the ring-reuse test of the two-bit ring generation ([`reusable`]);
//! - the segment framing rules ([`close_segment`]);
//! - the ordered publication after the command copy ([`Publisher`]).
//!
//! ## Writer slots, progress markers and the ring generation
//!
//! Each writer has a slot number. Its progress semaphore sits at offset
//! `(72 + slot) * 16` of the semaphore area ([`progress_offset`]). A segment
//! opens and closes with a progress marker: the semaphore offset, then a
//! texture-read semaphore release whose value is the ring offset of the word
//! after the marker, combined with the ring generation in bits 30–31.
//!
//! The ring generation is a two-bit counter. It is the writer's epoch word, and
//! it advances by one (3 wraps to 0) whenever the writer's next segment
//! restarts at the ring start ([`advance_epoch`]). Every release a segment
//! writes carries the generation in force when the segment was written. The
//! published epoch word carries the generation of the next segment.
//!
//! ## Where a segment continues
//!
//! The next segment restarts at the ring start when less than one staging
//! span plus 128 bytes would remain after this segment. Slot 0 is the main
//! stream's own ring writer: it simply runs on into the next segment, and it
//! jumps back to the ring start only when the ring wraps. Every other slot is a
//! side writer entered from the main command stream. It always ends with a
//! jump to the continuation offset its input names, even when the next
//! segment restarts at the ring start.
//!
//! Nothing here runs unless a host calls it.
use crate::material_finish::SourceWriterState;

/// Bytes of one staging span of the ring.
pub const STAGING: u32 = 8192;
/// Usable payload of a staging span, published as the next writer's limit.
pub const PAYLOAD: u32 = STAGING - 20;
/// Ring-position bits of a release value; bits 30–31 are the generation.
pub const POSITION: u32 = 0x3fff_ffff;
/// Largest ring offset a segment descriptor can name.
pub const DESCRIPTOR_POSITION: u32 = 0x0fff_ffff;
/// Entries of the descriptor ring (32 bytes each).
pub const DESCRIPTOR_ENTRIES: u32 = 4096;

const SEMAPHORE_OFFSET: u32 = 0x0004_1d6c;
const TEXTURE_READ_RELEASE: u32 = 0x0004_1d74;
const JUMP: u32 = 0x2000_0000;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum IoError {
    Stale,
    Bounds,
    Busy,
    Failed,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum WriterError {
    Input,
    Capacity,
    Reference,
    Fence,
    Reservation,
    Failed,
    Io(IoError),
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct CasResult {
    pub observed: u32,
    pub stored: bool,
}
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct RingHeader {
    pub writer_state_address: u32,
    pub ring_begin: u32,
    pub ring_bytes: u32,
    pub header_address: u32,
    pub header: u32,
    pub argument: u32,
}
/// Implementations must hold the real VM, job and output lifetime and use the
/// host's guest reservation coherence for compare-exchange and copies. The put
/// word must go through the host's notifying store. Arm and publish call the
/// host's real ring handoff, never a fabricated success token; writers that do
/// not hand their segment off never call them.
pub trait TerminalIo {
    fn read_word(&mut self, address: u32) -> Result<u32, IoError>;
    fn read_fences(&mut self, address: u32) -> Result<[u32; 16], IoError>;
    fn copy_bytes(&mut self, address: u32, bytes: &[u8]) -> Result<(), IoError>;
    fn write_word(&mut self, address: u32, value: u32) -> Result<(), IoError>;
    fn compare_exchange(
        &mut self,
        address: u32,
        expected: u32,
        value: u32,
    ) -> Result<CasResult, IoError>;
    fn arm_ring(&mut self, header: RingHeader) -> Result<u64, IoError>;
    fn publish_ring(&mut self, cookie: u64) -> Result<(), IoError>;
    fn publish_put(&mut self, address: u32, value: u32) -> Result<(), IoError>;
}

/// Offset of a writer slot's progress semaphore.
pub fn progress_offset(slot: u16) -> u32 {
    (72 + u32::from(slot)) << 4
}
/// A progress marker: semaphore offset, then the texture-read release.
pub fn progress_marker(slot: u16, release: u32) -> [u32; 4] {
    [
        SEMAPHORE_OFFSET,
        progress_offset(slot),
        TEXTURE_READ_RELEASE,
        release,
    ]
}
/// The generation after a ring wrap (two bits, 3 wraps to 0).
pub fn advance_epoch(epoch: u32) -> u32 {
    ((epoch >> 30).wrapping_add(1) & 3) << 30
}
/// Ring offset of a guest address (relative to the input's address base).
pub fn ring_offset(source: &SourceWriterState, address: u32) -> Result<u32, WriterError> {
    address
        .checked_sub(source.address_base)
        .filter(|v| *v <= DESCRIPTOR_POSITION)
        .ok_or(WriterError::Input)
}
/// Whether the consumer's reference value proves the span `start..end` (ring
/// offsets) reusable for a segment of generation `epoch`. The same generation
/// must have passed no further than `start`; the previous generation must
/// already have passed `end`. Any other generation proves nothing.
pub fn reusable(reference: u32, epoch: u32, start: u32, end: u32) -> bool {
    let generation = epoch >> 30;
    let observed = reference >> 30;
    if observed == generation {
        reference & POSITION <= start
    } else if observed == generation.wrapping_sub(1) & 3 {
        reference & POSITION >= end
    } else {
        false
    }
}
pub(crate) fn be_bytes(words: &[u32]) -> Vec<u8> {
    words.iter().flat_map(|w| w.to_be_bytes()).collect()
}

/// One prepared segment: its padded words and where the writer continues.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Segment {
    words: Vec<u32>,
    command_end: u32,
    padded_end: u32,
    next_cursor: u32,
    next_epoch: u32,
    wrap: bool,
}
impl Segment {
    pub fn words(&self) -> &[u32] {
        &self.words
    }
    /// Ring offset just after the closing marker (the published put value).
    pub fn command_end(&self) -> u32 {
        self.command_end
    }
    /// Guest address just after the padded segment.
    pub fn padded_end(&self) -> u32 {
        self.padded_end
    }
    pub fn next_cursor(&self) -> u32 {
        self.next_cursor
    }
    pub fn next_epoch(&self) -> u32 {
        self.next_epoch
    }
    pub fn wraps(&self) -> bool {
        self.wrap
    }
}

/// Close a segment whose body is `words`. The body must already begin with
/// the opening progress marker. `end` is the ring end address. Appends the
/// closing marker and the continuation (see the module documentation), pads
/// to a multiple of 128 bytes and refuses a segment that does not fit one
/// staging span or the ring.
pub fn close_segment(
    source: &SourceWriterState,
    mut words: Vec<u32>,
    end: u32,
) -> Result<Segment, WriterError> {
    let start = ring_offset(source, source.cursor)?;
    let used = u32::try_from(words.len() * 4).map_err(|_| WriterError::Capacity)?;
    let wrap = source
        .cursor
        .checked_add((used + 127) & !127)
        .and_then(|v| v.checked_add(STAGING + 128))
        .is_none_or(|v| v > end);
    words.extend_from_slice(&progress_marker(
        source.writer_slot,
        (start + used + 12) | source.epoch,
    ));
    let command_end = start + u32::try_from(words.len() * 4).map_err(|_| WriterError::Capacity)?;
    if source.writer_slot != 0 {
        words.push(source.continuation_offset | JUMP);
    } else if wrap {
        words.push(ring_offset(source, source.ring_start)? | JUMP);
    }
    let count = words.len().next_multiple_of(32);
    if count * 4 > STAGING as usize {
        return Err(WriterError::Capacity);
    }
    words.resize(count, 0);
    let padded_end = source
        .cursor
        .checked_add((words.len() * 4) as u32)
        .filter(|v| *v <= end)
        .ok_or(WriterError::Capacity)?;
    let next_cursor = if wrap { source.ring_start } else { padded_end };
    let next_epoch = if wrap {
        advance_epoch(source.epoch)
    } else {
        source.epoch
    };
    Ok(Segment {
        words,
        command_end,
        padded_end,
        next_cursor,
        next_epoch,
        wrap,
    })
}

/// What a completed publication left behind.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Published {
    /// Where the next segment starts.
    pub cursor: u32,
    /// The published epoch word.
    pub epoch: u32,
    /// The ring handoff cookie (zero when the segment was not handed off).
    pub ring_cookie: u64,
    /// The put value published, if this writer was the oldest in progress.
    pub put: Option<u32>,
    /// The descriptor ring entry written.
    pub descriptor_index: u32,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Stage {
    Reference,
    Arm,
    Copy,
    Publish,
    Fences,
    FenceWrite,
    Put,
    FinalFence,
    Reservation,
    Cas,
    Descriptor,
    Epoch,
    State,
    Done,
    Failed,
}

/// Publishes one prepared segment, in this order:
///
/// 1. wait until the consumer's reference value proves the span reusable;
/// 2. arm the ring handoff (handed-off writers only);
/// 3. copy the segment;
/// 4. publish the handoff (handed-off writers only);
/// 5. mark the writer's own fence done, publishing the put value when no
///    older writer is still in progress (an older one keeps the put);
/// 6. reserve a descriptor ring entry by compare-exchange, then write the
///    segment descriptor;
/// 7. write the epoch word and the 16-byte next-writer state.
///
/// No guest effect happens while the reuse wait is pending. Once any effect
/// may have happened, a failure is final: the publisher never restarts.
pub struct Publisher {
    source: SourceWriterState,
    segment: Segment,
    bytes: Vec<u8>,
    handoff: bool,
    stage: Stage,
    reference: u32,
    candidate: Option<u32>,
    fence_write: Option<u32>,
    reservation: u32,
    ring_cookie: u64,
    effects_possible: bool,
}
impl Publisher {
    pub fn new(source: SourceWriterState, segment: Segment, handoff: bool) -> Self {
        Self {
            bytes: be_bytes(&segment.words),
            source,
            segment,
            handoff,
            stage: Stage::Reference,
            reference: 0,
            candidate: None,
            fence_write: None,
            reservation: 0,
            ring_cookie: 0,
            effects_possible: false,
        }
    }
    pub fn segment(&self) -> &Segment {
        &self.segment
    }
    /// True once a guest-visible effect may have happened; replay is forbidden.
    pub fn effects_possible(&self) -> bool {
        self.effects_possible
    }
    /// Whether `reference` proves this segment's span reusable.
    pub fn reuse_ready(&self, reference: u32) -> bool {
        match (
            ring_offset(&self.source, self.source.cursor),
            ring_offset(&self.source, self.segment.padded_end),
        ) {
            (Ok(start), Ok(end)) => reusable(reference, self.source.epoch, start, end),
            _ => false,
        }
    }
    /// Returns `Ok(None)` while waiting for ring reuse, or after `steps` steps.
    pub fn drive(
        &mut self,
        io: &mut impl TerminalIo,
        steps: usize,
    ) -> Result<Option<Published>, WriterError> {
        for _ in 0..steps {
            if self.stage == Stage::Failed {
                return Err(WriterError::Failed);
            }
            match self.step(io) {
                Ok(Some(done)) => return Ok(Some(done)),
                Ok(None) if self.stage == Stage::Reference => return Ok(None),
                Ok(None) => {}
                Err(error) => {
                    self.stage = Stage::Failed;
                    return Err(error);
                }
            }
        }
        Ok(None)
    }
    fn step(&mut self, io: &mut impl TerminalIo) -> Result<Option<Published>, WriterError> {
        use Stage as S;
        let s = self.source;
        let effect = |result: Result<(), IoError>| result.map_err(WriterError::Io);
        let fence_address = s.fence_table_address + u32::from(s.fence_slot) * 4;
        match self.stage {
            S::Reference => {
                let value = io.read_word(s.reference_address).map_err(WriterError::Io)?;
                if !reusable(
                    value,
                    s.epoch,
                    ring_offset(&s, s.cursor)?,
                    ring_offset(&s, self.segment.padded_end)?,
                ) {
                    return Ok(None);
                }
                self.reference = value;
                self.stage = if self.handoff { S::Arm } else { S::Copy };
            }
            S::Arm => {
                self.ring_cookie = io
                    .arm_ring(RingHeader {
                        writer_state_address: s.state_address,
                        ring_begin: s.ring_start,
                        ring_bytes: s.ring_bytes,
                        header_address: s.cursor,
                        header: self.segment.words[0],
                        argument: self.segment.words[1],
                    })
                    .map_err(WriterError::Io)?;
                if self.ring_cookie == 0 {
                    return Err(WriterError::Io(IoError::Failed));
                }
                self.effects_possible = true;
                self.stage = S::Copy;
            }
            S::Copy => {
                self.effects_possible = true;
                effect(io.copy_bytes(s.cursor, &self.bytes))?;
                self.stage = if self.handoff { S::Publish } else { S::Fences };
            }
            S::Publish => {
                effect(io.publish_ring(self.ring_cookie))?;
                self.stage = S::Fences;
            }
            S::Fences => {
                let mut table = io
                    .read_fences(s.fence_table_address)
                    .map_err(WriterError::Io)?;
                let own = usize::from(s.fence_slot);
                if table[own] & 3 != 1 {
                    return Err(WriterError::Fence);
                }
                let done = self.segment.command_end | 2;
                table[own] = done;
                // The oldest writer still in progress keeps the put value; we
                // publish it only when that is us (or nobody before us).
                let mut candidate = self.segment.command_end;
                let mut available = true;
                for (i, word) in table.into_iter().enumerate() {
                    match word & 3 {
                        0 => break,
                        2 => candidate = word & !3,
                        _ => {
                            candidate = word & !3;
                            available = i == own;
                            break;
                        }
                    }
                }
                if available {
                    self.candidate = Some(candidate);
                } else {
                    self.fence_write = Some(done);
                }
                self.stage = S::FenceWrite;
            }
            S::FenceWrite => {
                if let Some(value) = self.fence_write {
                    effect(io.write_word(fence_address, value))?;
                }
                self.stage = S::Put;
            }
            S::Put => {
                if let Some(value) = self.candidate {
                    effect(io.publish_put(s.put_address, value))?;
                }
                self.stage = S::FinalFence;
            }
            S::FinalFence => {
                effect(io.write_word(fence_address, self.segment.command_end | 2))?;
                self.stage = S::Reservation;
            }
            S::Reservation => {
                self.reservation = io
                    .read_word(s.reservation_index_address)
                    .map_err(WriterError::Io)?;
                self.stage = S::Cas;
            }
            S::Cas => {
                if self.reservation >= DESCRIPTOR_ENTRIES {
                    return Err(WriterError::Reservation);
                }
                let result = io
                    .compare_exchange(
                        s.reservation_index_address,
                        self.reservation,
                        (self.reservation + 1) % DESCRIPTOR_ENTRIES,
                    )
                    .map_err(WriterError::Io)?;
                if result.stored {
                    if result.observed != self.reservation {
                        return Err(WriterError::Reservation);
                    }
                    self.stage = S::Descriptor;
                } else {
                    self.reservation = result.observed;
                }
            }
            S::Descriptor => {
                let start = ring_offset(&s, s.cursor)?;
                let end = ring_offset(&s, self.segment.padded_end)?;
                let next = ring_offset(&s, self.segment.next_cursor)?;
                let descriptor = [
                    start | 0x4000_0000 | (s.epoch >> 2),
                    end | (u32::from(s.writer_slot) << 29) | (u32::from(self.segment.wrap) << 28),
                    end | (u32::from(s.fence_slot) << 28),
                    next | 0xb000_0000 | (u32::from(self.candidate.is_some()) << 30),
                    self.reference,
                    u32::MAX,
                    self.candidate.unwrap_or(u32::MAX),
                    self.segment.command_end,
                ];
                let address = s
                    .descriptor_ring_address
                    .checked_add(self.reservation * 32)
                    .ok_or(WriterError::Input)?;
                effect(io.copy_bytes(address, &be_bytes(&descriptor)))?;
                self.stage = S::Epoch;
            }
            S::Epoch => {
                effect(io.write_word(s.epoch_address, self.segment.next_epoch))?;
                self.stage = S::State;
            }
            S::State => {
                let next = self.segment.next_cursor;
                let limit = next.checked_add(PAYLOAD).ok_or(WriterError::Input)?;
                effect(io.copy_bytes(s.state_address, &be_bytes(&[next, limit, next, u32::MAX])))?;
                self.stage = S::Done;
            }
            S::Done => {
                return Ok(Some(Published {
                    cursor: self.segment.next_cursor,
                    epoch: self.segment.next_epoch,
                    ring_cookie: self.ring_cookie,
                    put: self.candidate,
                    descriptor_index: self.reservation,
                }));
            }
            S::Failed => return Err(WriterError::Failed),
        }
        Ok(None)
    }
}
