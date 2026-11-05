//! Empty-pass ring transport for a side writer slot (1 to 15).
//!
//! When the frontend renders a pass natively, the pass job's ring segment can
//! shrink to an empty pass: no item, no child job. The segment still has to
//! leave everything a normally completed segment of that writer leaves:
//!
//! 1. the opening progress marker for the writer's slot;
//! 2. the pass's render state as the pass would leave it ([`EmptyPassProfile`]);
//! 3. the report pair, for passes that emit reports, when the input's gate word
//!    is nonzero (the input's first report index, then its gate index);
//! 4. the closing progress marker and the jump to the input's continuation;
//! 5. the shared fence, put, descriptor, epoch and next-writer publication.
//!
//! The framing, the ring-generation rule and the publication order are those
//! of [`crate::ring_writer`]. A profile names the writer slot and the pass's
//! own state block, so one writer serves every side pass:
//!
//! | Pass | Slot | Progress offset | Reports |
//! |---|---|---|---|
//! | Shadow cascade 0 | 1 | `0x490` | no |
//! | Shadow cascade 1 | 2 | `0x4A0` | no |
//! | Depth | 3 | `0x4B0` | yes |
//!
//! Only the Depth profile ([`DEPTH_EMPTY_PASS`]) is defined here; a Shadow
//! profile follows once its empty-run state is settled.
//!
//! This owns CPU transport effects only. No child job, original command
//! stream, GPU completion or presentation is produced or claimed here.
use crate::material_finish::SourceWriterState;
use crate::ring_writer::{
    POSITION, Publisher, STAGING, TerminalIo, WriterError, close_segment, progress_marker,
    ring_offset,
};

/// The job input as an empty-pass writer reads it: guest locations only,
/// never a lease.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct EmptyPassSource {
    pub writer: SourceWriterState,
    /// Report emitted first; the pair is present only when `gate_report` != 0.
    pub first_report: u16,
    pub gate_report: u16,
    /// Producer profile byte; an admitted empty pass requires zero.
    pub profile: u8,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum EmptyPassError {
    Extent,
    /// Not the profile's writer slot, or a producer profile other than zero;
    /// refused before any effect.
    Profile,
    Input,
    Capacity,
}

/// What a pass leaves in an empty segment besides its framing.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct EmptyPassProfile<'a> {
    /// The writer slot the input must name (1 to 15).
    pub writer_slot: u16,
    /// Render state left behind, as NV4097 method/value pairs in emission order.
    pub state: &'a [(u32, u32)],
    /// Whether the pass emits its report pair when the gate word is nonzero.
    pub reports: bool,
}

/// Render state a Depth pass leaves after its first group transition, as
/// public NV4097 method/value pairs: depth test and writes on with LEQUAL,
/// blending and alpha test off, all colour channels, method 0x370 = 0xFFF0
/// and the shader packer control cleared.
pub const DEPTH_GROUP_STATE: [(u32, u32); 8] = [
    (0x0a74, 1),
    (0x0a70, 1),
    (0x0a6c, 0x203),
    (0x0310, 0),
    (0x0304, 0),
    (0x0324, 0x0101_0101),
    (0x0370, 0xfff0),
    (0x1fec, 0),
];
/// The Depth pass: writer slot 3, its group state, reports when gated.
pub const DEPTH_EMPTY_PASS: EmptyPassProfile<'static> = EmptyPassProfile {
    writer_slot: 3,
    state: &DEPTH_GROUP_STATE,
    reports: true,
};

impl EmptyPassSource {
    /// Decode the 160-byte job input.
    pub fn decode(user: &[u8]) -> Result<Self, EmptyPassError> {
        if user.len() != 160 {
            return Err(EmptyPassError::Extent);
        }
        let w = |at: usize| u32::from_be_bytes(user[at..at + 4].try_into().unwrap());
        let h = |at: usize| u16::from_be_bytes(user[at..at + 2].try_into().unwrap());
        Ok(Self {
            writer: SourceWriterState {
                ring_start: w(0),
                cursor: w(4),
                ring_bytes: w(8),
                put_address: w(12),
                reference_address: w(16),
                state_address: w(20),
                epoch_address: w(24),
                epoch: w(28),
                address_base: w(32),
                writer_slot: h(36),
                fence_slot: h(38),
                fence_table_address: w(40),
                continuation_offset: w(44),
                reservation_index_address: w(64),
                descriptor_ring_address: w(68),
            },
            first_report: h(0x4e),
            gate_report: h(0x30),
            profile: user[0x49],
        })
    }
    /// Every guest span this transport may touch, with write permission.
    pub fn spans(&self) -> [(u32, u32, bool); 8] {
        let s = &self.writer;
        [
            (s.ring_start, s.ring_bytes, true),
            (s.put_address, 4, true),
            (s.reference_address, 4, false),
            (s.state_address, 16, true),
            (s.epoch_address, 4, true),
            (s.fence_table_address, 64, true),
            (s.reservation_index_address, 4, true),
            (s.descriptor_ring_address, 4096 * 32, true),
        ]
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PublishedEmptyPass {
    pub cursor: u32,
    pub epoch: u32,
    pub put: Option<u32>,
    pub descriptor_index: u32,
}

pub struct EmptyPassWriter {
    publisher: Publisher,
}

fn input(error: WriterError) -> EmptyPassError {
    match error {
        WriterError::Capacity => EmptyPassError::Capacity,
        _ => EmptyPassError::Input,
    }
}

impl EmptyPassWriter {
    /// Validates the whole segment and every consumer table before any effect.
    pub fn prepare(
        source: EmptyPassSource,
        profile: &EmptyPassProfile<'_>,
    ) -> Result<Self, EmptyPassError> {
        let s = source.writer;
        if !(1..16).contains(&profile.writer_slot)
            || s.writer_slot != profile.writer_slot
            || source.profile != 0
        {
            return Err(EmptyPassError::Profile);
        }
        let end = s
            .ring_start
            .checked_add(s.ring_bytes)
            .ok_or(EmptyPassError::Input)?;
        if s.fence_slot >= 16
            || s.epoch & POSITION != 0
            || !s.ring_start.is_multiple_of(128)
            || !s.cursor.is_multiple_of(128)
            || !s.ring_bytes.is_multiple_of(128)
            || s.ring_bytes < 2 * STAGING + 128
            || s.cursor < s.ring_start
            || s.cursor.checked_add(STAGING + 128).is_none_or(|p| p > end)
            || !s.continuation_offset.is_multiple_of(4)
            || s.continuation_offset > crate::ring_writer::DESCRIPTOR_POSITION
            || source
                .spans()
                .iter()
                .any(|(a, n, _)| *a == 0 || !a.is_multiple_of(4) || a.checked_add(*n).is_none())
        {
            return Err(EmptyPassError::Input);
        }
        ring_offset(&s, end).map_err(input)?;
        let start = ring_offset(&s, s.cursor).map_err(input)?;
        let mut words = progress_marker(s.writer_slot, (start + 12) | s.epoch).to_vec();
        for &(method, value) in profile.state {
            words.extend_from_slice(&[0x0004_0000 | method, value]);
        }
        if profile.reports && source.gate_report != 0 {
            for index in [source.first_report, source.gate_report] {
                words.extend_from_slice(&[0x0004_1800, 0x0100_0000 | (u32::from(index) << 4)]);
            }
        }
        let segment = close_segment(&s, words, end).map_err(input)?;
        Ok(Self {
            publisher: Publisher::new(s, segment, false),
        })
    }
    pub fn words(&self) -> &[u32] {
        self.publisher.segment().words()
    }
    pub fn wraps(&self) -> bool {
        self.publisher.segment().wraps()
    }
    /// True once a guest-visible effect may have happened; replay is forbidden.
    pub fn effects_possible(&self) -> bool {
        self.publisher.effects_possible()
    }
    /// Returns `Ok(None)` while waiting for ring reuse (no effect has happened
    /// while the stage is still the reuse wait), or after `steps` steps.
    pub fn drive(
        &mut self,
        io: &mut impl TerminalIo,
        steps: usize,
    ) -> Result<Option<PublishedEmptyPass>, WriterError> {
        Ok(self
            .publisher
            .drive(io, steps)?
            .map(|done| PublishedEmptyPass {
                cursor: done.cursor,
                epoch: done.epoch,
                put: done.put,
                descriptor_index: done.descriptor_index,
            }))
    }
}
