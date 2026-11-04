//! Owned terminal Main inputs and unapplied obligations. No guest effect,
//! original queue completion, native submission or completion proof is produced.
use crate::material_capture::MaterialCaptureError;
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum FinishCaptureError {
    MissingSource,
    Capture(MaterialCaptureError),
    Decode(FinishError),
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum FinishError {
    UserExtent,
    ReferenceAddress(u32),
    AuxiliaryTable,
    Continuation { user_offset: u16, target: u32 },
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct SourceFinishWords {
    pub reference_increment_address: u32,
    pub reference_exchange_address: u32,
    pub auxiliary_table_address: u32,
    pub auxiliary_table_count: u32,
    pub pass_flags: u32,
}
/// Guest locations, never allocation generations or a lease. In particular the
/// current reference value must be acquired under the actual writer service.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct SourceWriterState {
    pub ring_start: u32,
    pub cursor: u32,
    pub ring_bytes: u32,
    pub put_address: u32,
    pub reference_address: u32,
    pub state_address: u32,
    pub epoch_address: u32,
    pub epoch: u32,
    pub address_base: u32,
    pub writer_slot: u16,
    pub fence_slot: u16,
    pub fence_table_address: u32,
    pub continuation_offset: u32,
    pub reservation_index_address: u32,
    pub descriptor_ring_address: u32,
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct OwnedMainFinish {
    pub source_user_address: u32,
    pub source_pass_address: u32,
    pub lists_enabled: bool,
    pub first_report: u16,
    pub second_report: u16,
    pub restore_target: u32,
    /// Actual user70..90 words, including optional90 sentinel. Original order
    /// is described by requirements, not ascending source field order.
    pub terminal_targets: [u32; 9],
    pub source_words: SourceFinishWords,
    pub writer: SourceWriterState,
    /// User 0x68: the auxiliary state set selected by a record's bit 0 clear
    /// (`restore_target`, user 0x6C, is the set for bit 0 set).
    pub auxiliary_state_target: u32,
    /// The pass's depth-sorted auxiliary records (threshold bits, call
    /// target), captured only under `PS3_RENDER_MAIN_AUX_FLUSH=1`. `None`
    /// leaves the terminal transport exactly as without the option.
    pub auxiliary_records: Option<std::sync::Arc<[[u32; 2]]>>,
}
/// Default off. `PS3_RENDER_MAIN_AUX_FLUSH=1` captures the auxiliary table and
/// emits its end-of-pass records into the Main terminal transport.
pub fn auxiliary_flush_enabled() -> bool {
    static F: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    *F.get_or_init(|| std::env::var_os("PS3_RENDER_MAIN_AUX_FLUSH").is_some_and(|v| v == "1"))
}
/// Upper bound on captured auxiliary records (8 KiB of table).
pub const AUXILIARY_RECORD_LIMIT: u32 = 1024;
/// Decode a captured auxiliary table: `count` 8-byte big-endian records.
pub fn decode_auxiliary_records(bytes: &[u8], count: u32) -> Result<Vec<[u32; 2]>, FinishError> {
    if count > AUXILIARY_RECORD_LIMIT || bytes.len() < count as usize * 8 {
        return Err(FinishError::AuxiliaryTable);
    }
    Ok(bytes[..count as usize * 8]
        .as_chunks::<8>()
        .0
        .iter()
        .map(|r| {
            [
                u32::from_be_bytes(r[..4].try_into().unwrap()),
                u32::from_be_bytes(r[4..].try_into().unwrap()),
            ]
        })
        .collect())
}
/// Required effects only. Constructing this list never applies an effect. The
/// reference requirement preserves increment -> ordered SET_REFERENCE(value)
/// -> exchange(value); it does not claim GPU completion of that reference.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum FinishRequirement {
    AuxiliaryDepthZero {
        restore_target: u32,
    },
    ReferencePublication {
        increment_address: u32,
        exchange_address: u32,
    },
    Report {
        report_type: u32,
        offset: u32,
    },
    TerminalRenderState,
    Continuation {
        user_offset: u16,
        target: u32,
    },
    ResetVertexArrays,
    ResetMaterialState,
    /// Preserve or eliminate every original writer consumer, publish the real
    /// remaining CPU outputs, then discharge the producer frontier separately
    /// from later GPU completion and guest job allocation retirement.
    FinalProducerPublication,
}
impl OwnedMainFinish {
    pub fn decode(
        source_user_address: u32,
        source_pass_address: u32,
        user: &[u8],
        source_words: SourceFinishWords,
    ) -> Result<Self, FinishError> {
        if user.len() != 160 {
            return Err(FinishError::UserExtent);
        }
        let w = |at: usize| u32::from_be_bytes(user[at..at + 4].try_into().unwrap());
        let h = |at: usize| u16::from_be_bytes(user[at..at + 2].try_into().unwrap());
        Ok(Self {
            source_user_address,
            source_pass_address,
            lists_enabled: user[0x4c] != 0,
            first_report: h(0x30),
            second_report: h(0x4e),
            restore_target: w(0x6c),
            terminal_targets: core::array::from_fn(|i| w(0x70 + 4 * i)),
            source_words,
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
            auxiliary_state_target: w(0x68),
            auxiliary_records: None,
        })
    }
    /// Threshold16 obligations only. The caller must first handle every
    /// earlier threshold crossed from the actual previous processed group;
    /// absent group items do not establish absent transition effects.
    pub fn terminal_requirements(&self) -> Result<Vec<FinishRequirement>, FinishError> {
        use FinishRequirement as R;
        let mut result = Vec::new();
        let call = |result: &mut Vec<R>, user_offset: u16, target: u32| {
            if target == 0 || !target.is_multiple_of(4) {
                return Err(FinishError::Continuation {
                    user_offset,
                    target,
                });
            }
            result.push(R::Continuation {
                user_offset,
                target,
            });
            Ok(())
        };
        if self.lists_enabled {
            if self.restore_target == 0 || !self.restore_target.is_multiple_of(4) {
                return Err(FinishError::Continuation {
                    user_offset: 0x6c,
                    target: self.restore_target,
                });
            }
            for address in [
                self.source_words.reference_increment_address,
                self.source_words.reference_exchange_address,
            ] {
                if address == 0 || !address.is_multiple_of(4) {
                    return Err(FinishError::ReferenceAddress(address));
                }
            }
            result.push(R::AuxiliaryDepthZero {
                restore_target: self.restore_target,
            });
            result.push(R::ReferencePublication {
                increment_address: self.source_words.reference_increment_address,
                exchange_address: self.source_words.reference_exchange_address,
            });
        }
        if self.first_report != 0 {
            for index in [self.second_report, self.first_report] {
                result.push(R::Report {
                    report_type: 1,
                    offset: u32::from(index) * 16,
                });
            }
        }
        if self.lists_enabled {
            result.push(R::TerminalRenderState);
            if self.source_words.pass_flags & 2 != 0 {
                call(&mut result, 0x6c, self.restore_target)?;
            }
            for i in 0..4 {
                call(&mut result, 0x70 + i as u16 * 4, self.terminal_targets[i])?;
            }
        }
        result.push(R::ResetVertexArrays);
        for offset in [0x80u16, 0x8c] {
            call(
                &mut result,
                offset,
                self.terminal_targets[usize::from(offset - 0x70) / 4],
            )?;
        }
        let optional = self.terminal_targets[8];
        if self.source_words.pass_flags & 1 != 0 && optional != u32::MAX {
            call(&mut result, 0x90, optional)?;
        }
        call(&mut result, 0x84, self.terminal_targets[5])?;
        if self.source_words.pass_flags & 1 == 0 && optional != u32::MAX {
            call(&mut result, 0x90, optional)?;
        }
        call(&mut result, 0x88, self.terminal_targets[6])?;
        result.push(R::ResetMaterialState);
        result.push(R::FinalProducerPublication);
        Ok(result)
    }
}
