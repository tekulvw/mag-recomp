//! Material parameter values and fragment constant layout.
//!
//! Fragment structure and inline values are separate immutable/instance data.
//! No patched shader bytes, command stream or original worker is produced.
//! Fragment builtin requests are decoded in the `builtins` submodule and
//! re-exported here; their rows are prepared above this crate.

#[path = "material_builtins.rs"]
mod builtins;
pub use builtins::{
    BuiltinError, BuiltinPassValues, FragmentBuiltinRequest, RoleValues,
    decode_fragment_builtin_requests,
};

pub type ConstantWords = [u32; 4];
use crate::material_float as source_float;
pub use source_float::{
    add as source_add, fused as source_fused, multiply as source_multiply, ratio as source_ratio,
    unsigned as source_unsigned,
};

/// Direct source alpha-transfer values. The implementation is safe scalar Rust
/// numeric arithmetic and requires no original material child or SDK header.
pub fn alpha_transfer_values(alpha: u8, scalar: u32) -> ConstantWords {
    source_float::alpha_transfer(alpha, scalar)
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum ParameterError {
    Bounds,
    Mapping,
    ValueType(u8),
    Allocation,
    AttributeMatrixUnqualified(u8),
    MissingCurrentAttribute(u8),
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ParameterTarget {
    /// Source kind1 writes the RSX current-vertex-attribute register file;
    /// enabled vertex arrays take precedence, without deleting this value.
    ConstantVertexAttribute(u16),
    VertexConstants(u16),
    FragmentConstants(u16),
    IgnoredKind(u8),
    NoOp,
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ParameterAssignment {
    pub record_index: usize,
    pub source_type: u8,
    pub target: ParameterTarget,
    /// Host-order float bit patterns after source-defined scalar conversion.
    pub words: Vec<u32>,
}
/// Signed integer to binary32, rounding toward zero and with no scaling, as the
/// SPU number format converts. This is an integer magnitude/exponent
/// conversion, with no host FP mode or SDK primitive dependency.
pub fn signed_scalar_word(bits: u32) -> u32 {
    let value = bits as i32;
    if value == 0 {
        return 0;
    }
    let sign = bits & 0x80000000;
    let magnitude = value.unsigned_abs();
    let exponent = 31 - magnitude.leading_zeros();
    let significand = if exponent > 23 {
        magnitude >> (exponent - 23)
    } else {
        magnitude << (23 - exponent)
    };
    sign | ((exponent + 127) << 23) | (significand & 0x7fffff)
}
/// Decode the actual contiguous active ordinary-parameter run. Type1/2 use the
/// signed scalar rule; type3 is a source no-op; types4..10 retain scalar/vector/
/// matrix values. Primary/selected-role matching and builtins are separate
/// source operations, never inferred from an equal pointer or missing record.
pub fn decode_ordinary_parameters(
    material: &[u8],
    selector: u32,
    chunk: &[u8],
) -> Result<Vec<ParameterAssignment>, ParameterError> {
    fn half(b: &[u8], at: usize) -> Result<u16, ParameterError> {
        Ok(u16::from_be_bytes(
            b.get(at..at.checked_add(2).ok_or(ParameterError::Bounds)?)
                .ok_or(ParameterError::Bounds)?
                .try_into()
                .map_err(|_| ParameterError::Bounds)?,
        ))
    }
    fn word(b: &[u8], at: usize) -> Result<u32, ParameterError> {
        Ok(u32::from_be_bytes(
            b.get(at..at.checked_add(4).ok_or(ParameterError::Bounds)?)
                .ok_or(ParameterError::Bounds)?
                .try_into()
                .map_err(|_| ParameterError::Bounds)?,
        ))
    }
    let technique = usize::from(half(material, 0x48)?)
        .checked_add(
            (selector as usize)
                .checked_mul(32)
                .ok_or(ParameterError::Bounds)?,
        )
        .ok_or(ParameterError::Bounds)?;
    let map = *material.get(technique + 1).ok_or(ParameterError::Bounds)?;
    let first = usize::from(*material.get(technique + 3).ok_or(ParameterError::Bounds)?);
    if map >= 32 {
        return Err(ParameterError::Mapping);
    }
    let count = usize::from(half(material, 0x4c)?);
    if first >= count {
        return Ok(Vec::new());
    }
    let records = usize::from(half(material, 0x44)?);
    let values = usize::from(half(material, 0x46)?);
    material
        .get(records..records + count * 16)
        .ok_or(ParameterError::Bounds)?;
    let mut assignments = Vec::new();
    assignments
        .try_reserve_exact(count - first)
        .map_err(|_| ParameterError::Allocation)?;
    for index in first..count {
        let record = records + index * 16;
        if word(material, record + 4)? & (1u32 << map) == 0 {
            break;
        }
        let source_type = material[record + 10];
        if source_type == 3 {
            assignments.push(ParameterAssignment {
                record_index: index,
                source_type,
                target: ParameterTarget::NoOp,
                words: Vec::new(),
            });
            continue;
        }
        let count = match source_type {
            1 | 2 => 1,
            4..=7 => usize::from(source_type - 3),
            8..=10 => usize::from(source_type - 6) * 4,
            other => return Err(ParameterError::ValueType(other)),
        };
        let at = values
            .checked_add(usize::from(half(material, record + 8)?) * 4)
            .ok_or(ParameterError::Bounds)?;
        let source = material
            .get(at..at + count * 4)
            .ok_or(ParameterError::Bounds)?;
        let descriptor = word(chunk, usize::from(half(material, record + 12)?))?;
        let slot = ((descriptor >> 14) & 1023) as u16;
        let target = match ((descriptor >> 24) & 15) as u8 {
            1 => ParameterTarget::ConstantVertexAttribute(slot),
            2 => ParameterTarget::VertexConstants(slot),
            3 => ParameterTarget::FragmentConstants(slot),
            kind => ParameterTarget::IgnoredKind(kind),
        };
        let mut words = Vec::new();
        words
            .try_reserve_exact(count)
            .map_err(|_| ParameterError::Allocation)?;
        for bytes in source.as_chunks::<4>().0 {
            words.push(u32::from_be_bytes(*bytes));
        }
        if source_type <= 2 {
            words[0] = signed_scalar_word(words[0]);
        }
        assignments.push(ParameterAssignment {
            record_index: index,
            source_type,
            target,
            words,
        });
    }
    Ok(assignments)
}
/// Direct fragment row writes from the decoded source assignments. Other
/// destination domains remain explicit in the complete assignment list.
pub fn ordinary_fragment_rows(
    assignments: &[ParameterAssignment],
) -> Result<Vec<ConstantRowWrite>, ParameterError> {
    ordinary_rows(assignments, false)
}
/// Actual kind2 writes target the512-row vertex bank, independently of FP rows.
pub fn ordinary_vertex_rows(
    assignments: &[ParameterAssignment],
) -> Result<Vec<ConstantRowWrite>, ParameterError> {
    ordinary_rows(assignments, true)
}
fn ordinary_rows(
    assignments: &[ParameterAssignment],
    vertex: bool,
) -> Result<Vec<ConstantRowWrite>, ParameterError> {
    let mut output = Vec::new();
    for assignment in assignments {
        let expected = match assignment.source_type {
            1 | 2 => 1,
            3 => 0,
            4..=7 => usize::from(assignment.source_type - 3),
            8..=10 => usize::from(assignment.source_type - 6) * 4,
            other => return Err(ParameterError::ValueType(other)),
        };
        if assignment.words.len() != expected {
            return Err(ParameterError::Bounds);
        }
        let first = match assignment.target {
            ParameterTarget::FragmentConstants(first) if !vertex => first,
            ParameterTarget::VertexConstants(first) if vertex => first,
            _ => continue,
        };
        let rows = assignment.words.len().div_ceil(4);
        if usize::from(first) + rows > if vertex { 512 } else { 1024 } {
            return Err(ParameterError::Mapping);
        }
        output
            .try_reserve(rows)
            .map_err(|_| ParameterError::Allocation)?;
        for (row, words) in assignment.words.chunks(4).enumerate() {
            let mut lanes = [0, 0, 0, 0x3f800000];
            lanes[..words.len()].copy_from_slice(words);
            output.push(ConstantRowWrite {
                logical_slot: u32::from(first) + row as u32,
                words: lanes,
            });
        }
    }
    Ok(output)
}

pub type CurrentVertexAttributes = [Option<ConstantWords>; 16];
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct CurrentAttributeUpdate {
    pub location: u8,
    pub words: ConstantWords,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum VertexInputSource {
    Stream { location: u8 },
    Current { location: u8, words: ConstantWords },
}
/// Source1F/2F/3F/4F update the complete current attribute with component
/// defaults(0,0,0,1), not only the supplied lanes. Matrix-kind1 addressing is
/// still being source-qualified and fails explicitly instead of guessing rows.
pub fn current_attribute_updates(
    assignments: &[ParameterAssignment],
) -> Result<Vec<CurrentAttributeUpdate>, ParameterError> {
    let mut updates = Vec::new();
    for assignment in assignments {
        let ParameterTarget::ConstantVertexAttribute(slot) = assignment.target else {
            continue;
        };
        if slot >= 16 {
            return Err(ParameterError::Mapping);
        }
        let width = match assignment.source_type {
            1 | 2 | 4 => 1,
            5 => 2,
            6 => 3,
            7 => 4,
            ty @ 8..=10 => return Err(ParameterError::AttributeMatrixUnqualified(ty)),
            ty => return Err(ParameterError::ValueType(ty)),
        };
        if assignment.words.len() != width {
            return Err(ParameterError::Bounds);
        }
        let mut words = [0, 0, 0, 0x3f800000];
        words[..width].copy_from_slice(&assignment.words);
        updates
            .try_reserve(1)
            .map_err(|_| ParameterError::Allocation)?;
        updates.push(CurrentAttributeUpdate {
            location: slot as u8,
            words,
        });
    }
    Ok(updates)
}
/// Actual ordered native current state. The caller supplies qualified prior
/// state; missing values are not initialized from an original renderer or zero.
pub fn apply_current_attributes(
    mut prior: CurrentVertexAttributes,
    updates: &[CurrentAttributeUpdate],
) -> Result<CurrentVertexAttributes, ParameterError> {
    if updates.iter().any(|u| u.location >= 16) {
        return Err(ParameterError::Mapping);
    }
    for update in updates {
        prior[usize::from(update.location)] = Some(update.words);
    }
    Ok(prior)
}
/// A requested enabled array wins over a current value. Disabling the array
/// exposes the retained current value; an unknown current value remains error.
pub fn resolve_vertex_input_sources(
    used_mask: u16,
    array_enabled_mask: u16,
    current: &CurrentVertexAttributes,
) -> Result<Vec<VertexInputSource>, ParameterError> {
    let mut inputs = Vec::new();
    inputs
        .try_reserve_exact(used_mask.count_ones() as usize)
        .map_err(|_| ParameterError::Allocation)?;
    for location in 0..16u8 {
        let bit = 1u16 << location;
        if used_mask & bit == 0 {
            continue;
        }
        if array_enabled_mask & bit != 0 {
            inputs.push(VertexInputSource::Stream { location });
        } else {
            let words = current[usize::from(location)]
                .ok_or(ParameterError::MissingCurrentAttribute(location))?;
            inputs.push(VertexInputSource::Current { location, words });
        }
    }
    Ok(inputs)
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ConstantBudget {
    pub program_bytes: usize,
    pub inline_rows: usize,
    pub logical_slots: usize,
    pub relocations: usize,
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum ConstantError {
    Bounds,
    ProgramExtent,
    Unterminated,
    TrailingBytes,
    UnqualifiedFlow,
    Mapping,
    Budget,
    Allocation,
}
#[derive(Clone, Debug, PartialEq, Eq, Hash)]
pub struct FragmentInstruction {
    pub byte_offset: u32,
    pub words: ConstantWords,
    pub inline_slot: Option<u32>,
}
#[derive(Clone, Debug, PartialEq, Eq, Hash)]
pub struct FragmentStructure {
    /// Body structure only. Native pipeline identity also includes actual
    /// shader controls, vertex interface, pass formats and fixed-function state.
    pub instructions: Vec<FragmentInstruction>,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ConstantRowWrite {
    pub logical_slot: u32,
    pub words: ConstantWords,
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct FragmentConstantValues {
    pub inline_rows: Vec<ConstantWords>,
    /// Actual pass alpha/window-position words. Not derived from a late payload
    /// or inferred from a material's alpha byte. No lane is silently replaced.
    pub alpha_window: ConstantWords,
    pub sanitized_words: usize,
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct FragmentConstantLayout {
    pub structure: FragmentStructure,
    seeds: Vec<ConstantWords>,
    targets: Vec<Vec<usize>>,
    seed_sanitized_words: usize,
}
fn span(bytes: &[u8], at: usize, length: usize) -> Result<&[u8], ConstantError> {
    bytes
        .get(at..at.checked_add(length).ok_or(ConstantError::Bounds)?)
        .ok_or(ConstantError::Bounds)
}
fn word(bytes: &[u8], at: usize) -> Result<u32, ConstantError> {
    Ok(u32::from_be_bytes(
        span(bytes, at, 4)?
            .try_into()
            .map_err(|_| ConstantError::Bounds)?,
    ))
}
fn half(bytes: &[u8], at: usize) -> Result<u16, ConstantError> {
    Ok(u16::from_be_bytes(
        span(bytes, at, 2)?
            .try_into()
            .map_err(|_| ConstantError::Bounds)?,
    ))
}
fn sanitize(words: ConstantWords) -> (ConstantWords, usize) {
    let mut changed = 0;
    let clean = words.map(|bits| {
        let value = f32::from_bits(bits);
        if (-3.0e38_f32..=3.0e38_f32).contains(&value) {
            bits
        } else {
            changed += 1;
            0
        }
    });
    (clean, changed)
}
impl FragmentConstantLayout {
    /// Decode once for an immutable program/directory version. The caller's
    /// declared row domain comes from actual source bindings, not a guessed
    /// directory-header length. Control-flow programs remain explicitly
    /// unqualified until their constant addressing contract is closed.
    pub fn decode(
        program: &[u8],
        directory: &[u8],
        logical_slots: usize,
        budget: ConstantBudget,
    ) -> Result<Self, ConstantError> {
        if program.is_empty() || !program.len().is_multiple_of(16) {
            return Err(ConstantError::ProgramExtent);
        }
        if program.len() > budget.program_bytes || logical_slots > budget.logical_slots {
            return Err(ConstantError::Budget);
        }
        span(
            directory,
            24,
            logical_slots.checked_mul(4).ok_or(ConstantError::Bounds)?,
        )?;
        let mut instructions = Vec::new();
        instructions
            .try_reserve_exact(program.len() / 16)
            .map_err(|_| ConstantError::Allocation)?;
        let mut seeds = Vec::new();
        let mut inline_offsets = Vec::new();
        let mut seed_sanitized_words = 0;
        let mut offset = 0;
        let mut ended = false;
        while offset < program.len() {
            let words = [
                word(program, offset)?.rotate_left(16),
                word(program, offset + 4)?.rotate_left(16),
                word(program, offset + 8)?.rotate_left(16),
                word(program, offset + 12)?.rotate_left(16),
            ];
            if words[2] & 0x80000000 != 0 {
                return Err(ConstantError::UnqualifiedFlow);
            }
            let instruction_offset = u32::try_from(offset).map_err(|_| ConstantError::Budget)?;
            offset += 16;
            let inline_slot = if words[1..].iter().any(|word| word & 3 == 2) {
                if seeds.len() >= budget.inline_rows {
                    return Err(ConstantError::Budget);
                }
                span(program, offset, 16)?;
                let ordinal = u32::try_from(seeds.len()).map_err(|_| ConstantError::Budget)?;
                seeds
                    .try_reserve(1)
                    .map_err(|_| ConstantError::Allocation)?;
                inline_offsets
                    .try_reserve(1)
                    .map_err(|_| ConstantError::Allocation)?;
                let (seed, changed) = sanitize([
                    word(program, offset)?.rotate_left(16),
                    word(program, offset + 4)?.rotate_left(16),
                    word(program, offset + 8)?.rotate_left(16),
                    word(program, offset + 12)?.rotate_left(16),
                ]);
                seed_sanitized_words += changed;
                seeds.push(seed);
                inline_offsets.push(offset);
                offset += 16;
                Some(ordinal)
            } else {
                None
            };
            instructions.push(FragmentInstruction {
                byte_offset: instruction_offset,
                words,
                inline_slot,
            });
            if words[0] & 1 != 0 {
                ended = true;
                break;
            }
        }
        if !ended {
            return Err(ConstantError::Unterminated);
        }
        if offset != program.len() {
            return Err(ConstantError::TrailingBytes);
        }
        let mut targets = Vec::new();
        targets
            .try_reserve_exact(logical_slots)
            .map_err(|_| ConstantError::Allocation)?;
        let mut relocations = 0usize;
        for slot in 0..logical_slots {
            let list = usize::try_from(word(directory, 24 + slot * 4)?)
                .map_err(|_| ConstantError::Bounds)?;
            let count = usize::from(half(directory, list)?);
            span(directory, list + 2, count * 2)?;
            relocations = relocations
                .checked_add(count)
                .ok_or(ConstantError::Budget)?;
            if relocations > budget.relocations {
                return Err(ConstantError::Budget);
            }
            let mut ordinals = Vec::new();
            ordinals
                .try_reserve_exact(count)
                .map_err(|_| ConstantError::Allocation)?;
            for index in 0..count {
                // The authored source format explicitly truncates this product
                // to16bits; it is not unchecked host address arithmetic.
                let at = usize::from(half(directory, list + 2 + index * 2)?.wrapping_mul(16));
                ordinals.push(
                    inline_offsets
                        .binary_search(&at)
                        .map_err(|_| ConstantError::Mapping)?,
                );
            }
            targets.push(ordinals);
        }
        Ok(Self {
            structure: FragmentStructure { instructions },
            seeds,
            targets,
            seed_sanitized_words,
        })
    }
    /// Actual source order is retained; later overlapping writes win. Reject
    /// every bad mapping before returning any usable value block.
    pub fn evaluate(
        &self,
        writes: &[ConstantRowWrite],
        alpha_window: ConstantWords,
    ) -> Result<FragmentConstantValues, ConstantError> {
        if writes
            .iter()
            .any(|write| write.logical_slot as usize >= self.targets.len())
        {
            return Err(ConstantError::Mapping);
        }
        let mut inline_rows = Vec::new();
        inline_rows
            .try_reserve_exact(self.seeds.len())
            .map_err(|_| ConstantError::Allocation)?;
        inline_rows.extend_from_slice(&self.seeds);
        let mut sanitized_words = self.seed_sanitized_words;
        for write in writes {
            let (words, changed) = sanitize(write.words);
            sanitized_words = sanitized_words
                .checked_add(changed)
                .ok_or(ConstantError::Budget)?;
            for ordinal in &self.targets[write.logical_slot as usize] {
                inline_rows[*ordinal] = words;
            }
        }
        Ok(FragmentConstantValues {
            inline_rows,
            alpha_window,
            sanitized_words,
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn encoded(words: &[ConstantWords]) -> Vec<u8> {
        words
            .iter()
            .flat_map(|row| {
                row.iter()
                    .flat_map(|word| word.rotate_left(16).to_be_bytes())
            })
            .collect()
    }
    fn fixture() -> (Vec<u8>, Vec<u8>, ConstantBudget) {
        let program = encoded(&[[0, 2, 2, 0], [1, 2, 3, 4], [1, 2, 0, 0], [5, 6, 7, 8]]);
        let mut directory = vec![0; 42];
        directory[24..28].copy_from_slice(&32u32.to_be_bytes());
        directory[28..32].copy_from_slice(&38u32.to_be_bytes());
        directory[32..34].copy_from_slice(&2u16.to_be_bytes());
        directory[34..36].copy_from_slice(&1u16.to_be_bytes());
        directory[36..38].copy_from_slice(&3u16.to_be_bytes());
        directory[38..40].copy_from_slice(&1u16.to_be_bytes());
        directory[40..42].copy_from_slice(&3u16.to_be_bytes());
        (
            program,
            directory,
            ConstantBudget {
                program_bytes: 64,
                inline_rows: 2,
                logical_slots: 2,
                relocations: 3,
            },
        )
    }
    #[test]
    fn duplicate_operands_use_one_inline_slot_and_source_order_wins() {
        let (program, directory, budget) = fixture();
        let plan = FragmentConstantLayout::decode(&program, &directory, 2, budget).unwrap();
        assert_eq!(plan.structure.instructions.len(), 2);
        assert_eq!(plan.structure.instructions[0].inline_slot, Some(0));
        assert_eq!(plan.structure.instructions[1].inline_slot, Some(1));
        let tail = [9, 10, 11, 12];
        let values = plan
            .evaluate(
                &[
                    ConstantRowWrite {
                        logical_slot: 0,
                        words: [20, 21, 22, 23],
                    },
                    ConstantRowWrite {
                        logical_slot: 1,
                        words: [30, 31, 32, 33],
                    },
                ],
                tail,
            )
            .unwrap();
        assert_eq!(values.inline_rows, [[20, 21, 22, 23], [30, 31, 32, 33]]);
        assert_eq!(values.alpha_window, tail);
        assert_eq!(
            plan.evaluate(&[], tail).unwrap().inline_rows,
            [[1, 2, 3, 4], [5, 6, 7, 8]]
        );
    }
    #[test]
    fn values_do_not_change_shader_structure_and_seed_versions_stay_immutable() {
        let (program, directory, budget) = fixture();
        let mut changed = program.clone();
        changed[16..32].copy_from_slice(&encoded(&[[100, 101, 102, 103]]));
        let a = FragmentConstantLayout::decode(&program, &directory, 2, budget).unwrap();
        let b = FragmentConstantLayout::decode(&changed, &directory, 2, budget).unwrap();
        assert_eq!(a.structure, b.structure);
        assert_ne!(
            a.evaluate(&[], [0; 4]).unwrap(),
            b.evaluate(&[], [0; 4]).unwrap()
        );
    }
    #[test]
    fn dynamic_sanitization_preserves_signed_zero_and_every_tail_lane() {
        let (program, directory, budget) = fixture();
        let plan = FragmentConstantLayout::decode(&program, &directory, 2, budget).unwrap();
        let tail = [0x7fc01234, 0x80000000, 0x3f800000, 0xffffffff];
        let values = plan
            .evaluate(
                &[ConstantRowWrite {
                    logical_slot: 0,
                    words: [0x7fc01234, 0x7f800000, 0x80000000, 0xff800000],
                }],
                tail,
            )
            .unwrap();
        assert_eq!(values.inline_rows, [[0, 0, 0x80000000, 0]; 2]);
        assert_eq!(values.sanitized_words, 3);
        assert_eq!(values.alpha_window, tail);
    }
    #[test]
    fn no_inline_constants_does_not_invent_a_seed_or_drop_the_pass_tail() {
        let program = encoded(&[[1, 0, 0, 0]]);
        let budget = ConstantBudget {
            program_bytes: 16,
            inline_rows: 0,
            logical_slots: 0,
            relocations: 0,
        };
        let layout = FragmentConstantLayout::decode(&program, &[0; 24], 0, budget).unwrap();
        let values = layout.evaluate(&[], [1, 2, 3, 4]).unwrap();
        assert!(values.inline_rows.is_empty());
        assert_eq!(values.alpha_window, [1, 2, 3, 4]);
    }
    #[test]
    fn invalid_mapping_flow_and_limits_never_create_usable_values() {
        let (program, directory, budget) = fixture();
        let mut invalid = directory.clone();
        invalid[40..42].copy_from_slice(&2u16.to_be_bytes());
        assert_eq!(
            FragmentConstantLayout::decode(&program, &invalid, 2, budget),
            Err(ConstantError::Mapping)
        );
        let layout = FragmentConstantLayout::decode(&program, &directory, 2, budget).unwrap();
        assert_eq!(
            layout.evaluate(
                &[ConstantRowWrite {
                    logical_slot: 2,
                    words: [0; 4]
                }],
                [0; 4]
            ),
            Err(ConstantError::Mapping)
        );
        let mut flow = program.clone();
        flow[8..12].copy_from_slice(&0x80000000u32.rotate_left(16).to_be_bytes());
        assert_eq!(
            FragmentConstantLayout::decode(&flow, &directory, 2, budget),
            Err(ConstantError::UnqualifiedFlow)
        );
        let mut small = budget;
        small.relocations = 2;
        assert_eq!(
            FragmentConstantLayout::decode(&program, &directory, 2, small),
            Err(ConstantError::Budget)
        );
    }
    #[test]
    fn termination_and_explicit_sixteen_bit_directory_arithmetic_are_checked() {
        let (program, mut directory, budget) = fixture();
        directory[34..36].copy_from_slice(&0x1001u16.to_be_bytes());
        let plan = FragmentConstantLayout::decode(&program, &directory, 2, budget).unwrap();
        assert_eq!(
            plan.evaluate(
                &[ConstantRowWrite {
                    logical_slot: 0,
                    words: [9; 4]
                }],
                [0; 4]
            )
            .unwrap()
            .inline_rows,
            [[9; 4]; 2]
        );
        let mut unterminated = program.clone();
        unterminated[32..36].copy_from_slice(&0u32.to_be_bytes());
        assert_eq!(
            FragmentConstantLayout::decode(&unterminated, &directory, 2, budget),
            Err(ConstantError::Unterminated)
        );
        let mut trailing = program;
        trailing.extend_from_slice(&[0; 16]);
        let mut larger = budget;
        larger.program_bytes = trailing.len();
        assert_eq!(
            FragmentConstantLayout::decode(&trailing, &directory, 2, larger),
            Err(ConstantError::TrailingBytes)
        );
    }
    #[test]
    fn signed_scalar_conversion_uses_source_truncation_not_host_round_to_nearest() {
        for (input, expected) in [
            (0i32, 0),
            (1, 0x3f800000),
            (-1, 0xbf800000),
            (16_777_215, 0x4b7fffff),
            (16_777_217, 0x4b800000),
            (-16_777_217, 0xcb800000),
            (i32::MAX, 0x4effffff),
            (i32::MIN, 0xcf000000),
        ] {
            assert_eq!(signed_scalar_word(input as u32), expected);
        }
        assert_ne!(
            signed_scalar_word(i32::MAX as u32),
            (i32::MAX as f32).to_bits()
        );
    }
    #[test]
    fn source_parameter_types_and_every_target_domain_survive_ordered_decode() {
        let mut material = vec![0; 1024];
        let mut chunk = vec![0; 40];
        material[0x44..0x46].copy_from_slice(&0x100u16.to_be_bytes());
        material[0x46..0x48].copy_from_slice(&0x200u16.to_be_bytes());
        material[0x48..0x4a].copy_from_slice(&0x80u16.to_be_bytes());
        material[0x4c..0x4e].copy_from_slice(&10u16.to_be_bytes());
        let counts = [1, 1, 0, 1, 2, 3, 4, 8, 12, 16];
        let mut cursor = 0;
        for (i, count) in counts.iter().copied().enumerate() {
            let record = 0x100 + i * 16;
            material[record + 4..record + 8].copy_from_slice(&1u32.to_be_bytes());
            material[record + 8..record + 10].copy_from_slice(&(cursor as u16).to_be_bytes());
            material[record + 10] = i as u8 + 1;
            material[record + 12..record + 14].copy_from_slice(&(i as u16 * 4).to_be_bytes());
            let kind = if i == 0 {
                1
            } else if i == 1 {
                2
            } else {
                3
            };
            chunk[i * 4..i * 4 + 4]
                .copy_from_slice(&((kind << 24) | ((i as u32 * 4) << 14)).to_be_bytes());
            for lane in 0..count {
                let value = if i == 0 {
                    (-16_777_217i32) as u32
                } else if i == 1 {
                    i32::MAX as u32
                } else {
                    0x3f800000 + cursor as u32 + lane as u32
                };
                material[0x200 + (cursor + lane) * 4..0x204 + (cursor + lane) * 4]
                    .copy_from_slice(&value.to_be_bytes());
            }
            cursor += count;
        }
        // Source type3 is an actual no-op. These unused values/offsets must
        // never be read merely to manufacture a complete-looking assignment.
        material[0x128..0x12a].fill(0xff);
        material[0x12c..0x12e].fill(0xff);
        let assignments = decode_ordinary_parameters(&material, 0, &chunk).unwrap();
        assert_eq!(assignments.len(), 10);
        assert_eq!(
            assignments[0].target,
            ParameterTarget::ConstantVertexAttribute(0)
        );
        assert_eq!(assignments[0].words, [0xcb800000]);
        assert_eq!(assignments[1].target, ParameterTarget::VertexConstants(4));
        assert_eq!(assignments[1].words, [0x4effffff]);
        assert_eq!(assignments[2].target, ParameterTarget::NoOp);
        assert_eq!(
            assignments
                .iter()
                .map(|a| a.words.len())
                .collect::<Vec<_>>(),
            counts
        );
        let rows = ordinary_fragment_rows(&assignments).unwrap();
        assert_eq!(rows.len(), 13);
        assert_eq!(rows[0].logical_slot, 12);
        assert_eq!(rows[0].words, [0x3f800002, 0, 0, 0x3f800000]);
        material[0x114..0x118].fill(0); // First inactive mask terminates run.
        assert_eq!(
            decode_ordinary_parameters(&material, 0, &chunk)
                .unwrap()
                .len(),
            1
        );
        material[0x4c..0x4e].fill(0);
        material[0x83] = 255;
        material[0x44..0x48].fill(0xff);
        assert!(
            decode_ordinary_parameters(&material, 0, &[])
                .unwrap()
                .is_empty()
        );
    }
    #[test]
    fn current_attribute_updates_reset_unsupplied_lanes_and_preserve_actual_order() {
        let mut assignments = Vec::new();
        for (ty, words) in [
            (7, vec![10, 11, 12, 13]),
            (4, vec![99]),
            (5, vec![20, 21]),
            (6, vec![30, 31, 32]),
        ] {
            assignments.push(ParameterAssignment {
                record_index: assignments.len(),
                source_type: ty,
                target: ParameterTarget::ConstantVertexAttribute(3),
                words,
            });
        }
        let updates = current_attribute_updates(&assignments).unwrap();
        assert_eq!(
            updates.iter().map(|u| u.words).collect::<Vec<_>>(),
            [
                [10, 11, 12, 13],
                [99, 0, 0, 0x3f800000],
                [20, 21, 0, 0x3f800000],
                [30, 31, 32, 0x3f800000]
            ]
        );
        let state = apply_current_attributes([None; 16], &updates).unwrap();
        assert_eq!(state[3], Some([30, 31, 32, 0x3f800000]));
        assert!(state[2].is_none());
    }
    #[test]
    fn enabled_array_has_precedence_then_disabled_array_exposes_the_same_current_value() {
        let mut current = [None; 16];
        current[3] = Some([10, 11, 12, 13]);
        assert_eq!(
            resolve_vertex_input_sources(1 << 3, 1 << 3, &current).unwrap(),
            [VertexInputSource::Stream { location: 3 }]
        );
        assert_eq!(
            resolve_vertex_input_sources(1 << 3, 0, &current).unwrap(),
            [VertexInputSource::Current {
                location: 3,
                words: [10, 11, 12, 13]
            }]
        );
        assert_eq!(
            resolve_vertex_input_sources(1 << 2, 0, &current),
            Err(ParameterError::MissingCurrentAttribute(2))
        );
        assert!(
            resolve_vertex_input_sources(0, 0, &current)
                .unwrap()
                .is_empty()
        );
    }
}
