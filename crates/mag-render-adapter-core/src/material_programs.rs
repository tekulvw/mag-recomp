//! Owned shader inputs from a selected technique resource, without RSX packets.
//! Source decoding does not establish a native shader, active inherited state,
//! resource generation, or eligibility to suppress a material producer.

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ProgramBudget {
    pub source_bytes: usize,
    pub vertex_instructions: usize,
    pub static_constant_rows: usize,
    pub fragment_bytes: u32,
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum ProgramError {
    Bounds,
    RelativeOffset,
    Overlap,
    Budget,
    Allocation,
    FragmentExtent,
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct StaticVertexConstant {
    pub slot: u32,
    pub words: [u32; 4],
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct VertexProgramSource {
    pub header_words: [u32; 6],
    pub load_slot: u16,
    pub input_mask: u32,
    pub output_mask: u32,
    /// Source header +12, formerly emitted at method 1EF8. Its semantic native
    /// consumer has not yet been qualified; preserve it, never assume zero.
    pub unresolved_transform_word: u32,
    pub instructions: Vec<[u32; 4]>,
    /// Source order and duplicates are significant; later writes replace lanes.
    pub static_constants: Vec<StaticVertexConstant>,
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum FragmentSourceReference {
    /// Only the offset is known here. The actual bank base and lease come from
    /// an independently owned asset publication, never a surviving worker LS.
    AssetBankOffset(u32),
    /// Preserve other source tags as an unresolved binding, not an empty shader.
    Unresolved { tag: u8, encoded: u32 },
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct FragmentProgramSource {
    pub header_words: [u32; 6],
    pub semantic_bytes: u32,
    pub transfer_bytes: u32,
    pub reference: FragmentSourceReference,
    pub shader_control: u32,
    pub texcoord_source_masks: [u16; 2],
    /// Exact per-input controls derived from the two declared source masks.
    pub texcoord_controls: [u32; 10],
    /// Immutable relocation directory rooted at the fragment header. Its row
    /// domain is supplied by actual source assignments, not a guessed field.
    pub relocation_directory: Vec<u8>,
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct TechniquePrograms {
    pub vertex: VertexProgramSource,
    pub fragment: FragmentProgramSource,
}

/// Extract direct typed shader/constant inputs, without a command stream,
/// program patch, original cache, mapped shader address or implicit prior state.
pub fn decode_technique_programs(
    chunk: &[u8],
    budget: ProgramBudget,
) -> Result<TechniquePrograms, ProgramError> {
    fn span(b: &[u8], at: usize, length: usize) -> Result<&[u8], ProgramError> {
        b.get(at..at.checked_add(length).ok_or(ProgramError::Bounds)?)
            .ok_or(ProgramError::Bounds)
    }
    fn word(b: &[u8], at: usize) -> Result<u32, ProgramError> {
        Ok(u32::from_be_bytes(
            span(b, at, 4)?
                .try_into()
                .map_err(|_| ProgramError::Bounds)?,
        ))
    }
    fn half(b: &[u8], at: usize) -> Result<u16, ProgramError> {
        Ok(u16::from_be_bytes(
            span(b, at, 2)?
                .try_into()
                .map_err(|_| ProgramError::Bounds)?,
        ))
    }
    fn object(b: &[u8], base: usize, field: usize, length: usize) -> Result<usize, ProgramError> {
        let relative = usize::try_from(word(
            b,
            base.checked_add(field).ok_or(ProgramError::Bounds)?,
        )?)
        .map_err(|_| ProgramError::Bounds)?;
        if relative == 0 || !relative.is_multiple_of(4) {
            return Err(ProgramError::RelativeOffset);
        }
        let at = base.checked_add(relative).ok_or(ProgramError::Bounds)?;
        span(b, at, length)?;
        Ok(at)
    }
    if chunk.len() > budget.source_bytes {
        return Err(ProgramError::Budget);
    }
    let a = object(chunk, 0, 0, 16)?;
    let b = object(chunk, a, 12, 20)?;
    let vd = object(chunk, b, 12, 20)?;
    let v = object(chunk, vd, 16, 24)?;
    let fd = object(chunk, b, 16, 20)?;
    let f = object(chunk, fd, 16, 24)?;
    let instruction_count = usize::from(half(chunk, v)?);
    let constant_count = usize::from(half(chunk, v + 22)?);
    if instruction_count > budget.vertex_instructions
        || constant_count > budget.static_constant_rows
    {
        return Err(ProgramError::Budget);
    }
    let registers = v + 24;
    let values = v
        .checked_add((24 + constant_count * 4 + 15) & !15)
        .ok_or(ProgramError::Bounds)?;
    span(chunk, registers, constant_count * 4)?;
    span(chunk, values, constant_count * 16)?;
    let code = if instruction_count == 0 {
        0
    } else {
        let relative = usize::from(half(chunk, v + 18)?);
        if relative == 0 || !relative.is_multiple_of(4) {
            return Err(ProgramError::RelativeOffset);
        }
        let code = v.checked_add(relative).ok_or(ProgramError::Bounds)?;
        span(chunk, code, instruction_count * 16)?;
        code
    };
    let mut regions = [
        (0, 4),
        (a, a + 16),
        (b, b + 20),
        (vd, vd + 20),
        (fd, fd + 20),
        (v, v + 24),
        (f, f + 24),
        (registers, registers + constant_count * 4),
        (values, values + constant_count * 16),
        (code, code + instruction_count * 16),
    ];
    regions.sort_unstable();
    let mut previous_end = 0;
    for (low, high) in regions {
        if low == high {
            continue;
        }
        if low < previous_end {
            return Err(ProgramError::Overlap);
        }
        previous_end = high;
    }
    let encoded = word(chunk, f + 12)?;
    let reference = match encoded & 3 {
        2 => FragmentSourceReference::AssetBankOffset(encoded & !3),
        tag => FragmentSourceReference::Unresolved {
            tag: tag as u8,
            encoded,
        },
    };
    Ok(TechniquePrograms {
        vertex: decode_vertex_program_source(&chunk[v..], budget)?,
        fragment: decode_fragment_program_source(&chunk[f..], budget, reference)?,
    })
}

fn object_word(b: &[u8], at: usize) -> Result<u32, ProgramError> {
    Ok(u32::from_be_bytes(
        b.get(at..at.checked_add(4).ok_or(ProgramError::Bounds)?)
            .ok_or(ProgramError::Bounds)?
            .try_into()
            .map_err(|_| ProgramError::Bounds)?,
    ))
}
fn object_half(b: &[u8], at: usize) -> Result<u16, ProgramError> {
    Ok(u16::from_be_bytes(
        b.get(at..at.checked_add(2).ok_or(ProgramError::Bounds)?)
            .ok_or(ProgramError::Bounds)?
            .try_into()
            .map_err(|_| ProgramError::Bounds)?,
    ))
}
fn object_header(b: &[u8]) -> Result<[u32; 6], ProgramError> {
    let mut h = [0; 6];
    for (i, w) in h.iter_mut().enumerate() {
        *w = object_word(b, i * 4)?;
    }
    Ok(h)
}
/// Same source program object used by a selected technique and standalone PPU
/// producer registry. This parses the bounded container, not another ISA.
pub fn decode_vertex_program_source(
    b: &[u8],
    budget: ProgramBudget,
) -> Result<VertexProgramSource, ProgramError> {
    if b.len() > budget.source_bytes {
        return Err(ProgramError::Budget);
    }
    let header_words = object_header(b)?;
    let count = usize::from(object_half(b, 0)?);
    let constants = usize::from(object_half(b, 22)?);
    if count > budget.vertex_instructions || constants > budget.static_constant_rows {
        return Err(ProgramError::Budget);
    }
    let values = (24 + constants * 4 + 15) & !15;
    let code = if count == 0 {
        0
    } else {
        let c = usize::from(object_half(b, 18)?);
        if c == 0 || !c.is_multiple_of(4) {
            return Err(ProgramError::RelativeOffset);
        }
        c
    };
    let mut regions = [
        (0, 24),
        (24, 24 + constants * 4),
        (values, values + constants * 16),
        (code, code + count * 16),
    ];
    regions.sort_unstable();
    let mut end = 0;
    for (low, high) in regions {
        if low == high {
            continue;
        }
        if low < end {
            return Err(ProgramError::Overlap);
        }
        if high > b.len() {
            return Err(ProgramError::Bounds);
        }
        end = high;
    }
    let mut instructions = Vec::new();
    instructions
        .try_reserve_exact(count)
        .map_err(|_| ProgramError::Allocation)?;
    for i in 0..count {
        let mut words = [0; 4];
        for (lane, w) in words.iter_mut().enumerate() {
            *w = object_word(b, code + i * 16 + lane * 4)?;
        }
        instructions.push(words);
    }
    let mut static_constants = Vec::new();
    static_constants
        .try_reserve_exact(constants)
        .map_err(|_| ProgramError::Allocation)?;
    for i in 0..constants {
        let mut words = [0; 4];
        for (lane, w) in words.iter_mut().enumerate() {
            *w = object_word(b, values + i * 16 + lane * 4)?;
        }
        static_constants.push(StaticVertexConstant {
            slot: object_word(b, 24 + i * 4)?,
            words,
        });
    }
    Ok(VertexProgramSource {
        header_words,
        load_slot: object_half(b, 2)?,
        input_mask: object_word(b, 4)?,
        output_mask: object_word(b, 8)?,
        unresolved_transform_word: object_word(b, 12)?,
        instructions,
        static_constants,
    })
}
/// Caller supplies the source-specific reference domain. A standalone PPU
/// header's live GPU location must not be relabelled an asset-bank offset.
pub fn decode_fragment_program_source(
    b: &[u8],
    budget: ProgramBudget,
    reference: FragmentSourceReference,
) -> Result<FragmentProgramSource, ProgramError> {
    if b.len() > budget.source_bytes {
        return Err(ProgramError::Budget);
    }
    let header_words = object_header(b)?;
    let semantic_bytes = object_word(b, 0)?;
    if semantic_bytes == 0 {
        return Err(ProgramError::FragmentExtent);
    }
    let transfer_bytes = semantic_bytes
        .checked_add(15)
        .ok_or(ProgramError::FragmentExtent)?
        & !15;
    if transfer_bytes > budget.fragment_bytes {
        return Err(ProgramError::Budget);
    }
    let masks = [object_half(b, 4)?, object_half(b, 6)?];
    let mut relocation_directory = Vec::new();
    relocation_directory
        .try_reserve_exact(b.len())
        .map_err(|_| ProgramError::Allocation)?;
    relocation_directory.extend_from_slice(b);
    Ok(FragmentProgramSource {
        header_words,
        semantic_bytes,
        transfer_bytes,
        reference,
        shader_control: object_word(b, 8)?,
        texcoord_source_masks: masks,
        texcoord_controls: std::array::from_fn(|i| {
            ((u32::from(masks[1]) >> i) & 1) | (((u32::from(masks[0]) >> i) & 1) << 4)
        }),
        relocation_directory,
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    fn word(bytes: &mut [u8], at: usize, value: u32) {
        bytes[at..at + 4].copy_from_slice(&value.to_be_bytes());
    }
    fn half(bytes: &mut [u8], at: usize, value: u16) {
        bytes[at..at + 2].copy_from_slice(&value.to_be_bytes());
    }
    #[test]
    fn standalone_producer_objects_share_the_technique_source_decoder() {
        let (b, budget) = fixture();
        let pair = decode_technique_programs(&b, budget).unwrap();
        assert_eq!(
            decode_vertex_program_source(&b[0xa0..], budget).unwrap(),
            pair.vertex
        );
        assert_eq!(
            decode_fragment_program_source(&b[0x150..], budget, pair.fragment.reference.clone())
                .unwrap(),
            pair.fragment
        );
        let mut bad = b[0xa0..].to_vec();
        bad[18..20].copy_from_slice(&4u16.to_be_bytes());
        assert!(decode_vertex_program_source(&bad, budget).is_err());
    }
    fn fixture() -> (Vec<u8>, ProgramBudget) {
        let mut b = vec![0; 512];
        for (at, value) in [
            (0, 0x20),
            (0x2c, 0x20),
            (0x4c, 0x20),
            (0x50, 0x40),
            (0x70, 0x40),
            (0x90, 0xd0),
        ] {
            word(&mut b, at, value);
        }
        half(&mut b, 0xa0, 2);
        half(&mut b, 0xa2, 3);
        word(&mut b, 0xa4, 5);
        word(&mut b, 0xa8, 0x80);
        word(&mut b, 0xac, 0xaabbccdd);
        half(&mut b, 0xb2, 0x40);
        half(&mut b, 0xb6, 2);
        word(&mut b, 0xb8, 7);
        word(&mut b, 0xbc, 7);
        for i in 0..16 {
            word(&mut b, 0xc0 + i * 4, 100 + i as u32);
        }
        word(&mut b, 0x150, 49);
        half(&mut b, 0x154, 2);
        half(&mut b, 0x156, 1);
        word(&mut b, 0x158, 0x40);
        word(&mut b, 0x15c, 0x1002);
        (
            b,
            ProgramBudget {
                source_bytes: 512,
                vertex_instructions: 2,
                static_constant_rows: 2,
                fragment_bytes: 64,
            },
        )
    }
    #[test]
    fn owned_stage_inputs_and_duplicate_constants_are_explicit() {
        let (mut bytes, budget) = fixture();
        let result = decode_technique_programs(&bytes, budget).unwrap();
        assert_eq!(result.vertex.load_slot, 3);
        assert_eq!(result.vertex.input_mask, 5);
        assert_eq!(result.vertex.output_mask, 0x80);
        assert_eq!(result.vertex.unresolved_transform_word, 0xaabbccdd);
        assert_eq!(
            result.vertex.instructions,
            [[108, 109, 110, 111], [112, 113, 114, 115]]
        );
        assert_eq!(
            result.vertex.static_constants,
            [
                StaticVertexConstant {
                    slot: 7,
                    words: [100, 101, 102, 103]
                },
                StaticVertexConstant {
                    slot: 7,
                    words: [104, 105, 106, 107]
                },
            ]
        );
        assert_eq!(result.fragment.semantic_bytes, 49);
        assert_eq!(result.fragment.transfer_bytes, 64);
        assert_eq!(
            result.fragment.reference,
            FragmentSourceReference::AssetBankOffset(0x1000)
        );
        assert_eq!(result.fragment.shader_control, 0x40);
        assert_eq!(result.fragment.texcoord_controls[..3], [1, 16, 0]);
        let directory = result.fragment.relocation_directory.clone();
        bytes.fill(0xff);
        assert_eq!(result.fragment.relocation_directory, directory);
    }
    #[test]
    fn unresolved_fragment_storage_is_preserved_as_an_obligation() {
        let (mut bytes, budget) = fixture();
        word(&mut bytes, 0x15c, 0x1231);
        assert_eq!(
            decode_technique_programs(&bytes, budget)
                .unwrap()
                .fragment
                .reference,
            FragmentSourceReference::Unresolved {
                tag: 1,
                encoded: 0x1231
            }
        );
    }
    #[test]
    fn every_relative_object_and_actual_range_is_checked() {
        let (bytes, budget) = fixture();
        for at in [0, 0x2c, 0x4c, 0x50, 0x70, 0x90] {
            for value in [0, 3, u32::MAX - 3] {
                let mut bad = bytes.clone();
                word(&mut bad, at, value);
                assert!(decode_technique_programs(&bad, budget).is_err());
            }
        }
        let mut bad = bytes.clone();
        half(&mut bad, 0xb2, 0x18);
        assert_eq!(
            decode_technique_programs(&bad, budget),
            Err(ProgramError::Overlap)
        );
        assert!(decode_technique_programs(&bytes[..0x160], budget).is_err());
    }
    #[test]
    fn source_budget_does_not_silently_drop_shader_or_constant_rows() {
        let (bytes, mut budget) = fixture();
        budget.vertex_instructions = 1;
        assert_eq!(
            decode_technique_programs(&bytes, budget),
            Err(ProgramError::Budget)
        );
        budget.vertex_instructions = 2;
        budget.static_constant_rows = 1;
        assert_eq!(
            decode_technique_programs(&bytes, budget),
            Err(ProgramError::Budget)
        );
        budget.static_constant_rows = 2;
        budget.fragment_bytes = 48;
        assert_eq!(
            decode_technique_programs(&bytes, budget),
            Err(ProgramError::Budget)
        );
    }
}
