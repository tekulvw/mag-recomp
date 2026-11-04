//! Source-qualified vertex bindings, independent of RSX packet transport.
//! Output declaration byte5 selects a register; byte3 remains a semantic ID.
//! Canonical Float32 values need not reproduce the discarded SPU scratch pack.

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum BindingError {
    Extent,
    Range,
    Budget,
    Allocation,
    Slot(u8),
    Format(u8),
    Components(u8),
    StateHeader(u32),
    MissingTechnique,
    UnsupportedBuiltin(u8),
    Source(crate::error::Error),
    SourceControl {
        slot: usize,
        control: u32,
    },
    OutputSourceExtent {
        selector: u8,
        control: u32,
        address: u32,
    },
    /// No new assignments; previously bound vertex arrays may remain in effect.
    /// This is unresolved native array state, not a malformed descriptor.
    UnresolvedEmptyOutput {
        selector: u8,
        control: u32,
        address: u32,
        bytes: usize,
        header: Vec<u8>,
        config: [u8; 16],
        auxiliary_header: [u8; 8],
        primary_mask: Option<u32>,
        vertex_layout_address: u32,
        /// [descriptor presence by ordinal, nonzero format by slot,
        /// enabled components by slot, expected format header by slot].
        vf_masks: Option<[u16; 4]>,
    },
    OutputDescriptorExtent {
        selector: u8,
        control: u32,
        address: u32,
        bytes: usize,
        header: Option<[u8; 8]>,
        required_bytes: usize,
    },
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct VertexBinding {
    pub declaration: usize,
    pub semantic: u8,
    pub slot: u8,
    pub format: u8,
    pub components: u8,
    pub stride: u8,
    pub byte_offset: u8,
    /// Source output precision/packing metadata, not a native scratch limit.
    pub encoded_bytes: u8,
}
fn declaration(row: &[u8], stride: u8, ordinal: usize) -> Result<VertexBinding, BindingError> {
    if row[5] >= 16 {
        return Err(BindingError::Slot(row[5]));
    }
    Ok(VertexBinding {
        declaration: ordinal,
        semantic: row[3],
        slot: row[5],
        format: row[1],
        components: row[2],
        stride,
        byte_offset: row[0],
        encoded_bytes: row[4],
    })
}
/// Custom output bindings: header byte 0 is the entry count (not byte 2) and
/// byte 1 the stride; each 8-byte entry declares one output register. Entries
/// and repeated register assignments stay in source order.
pub fn decode_output_bindings(format: &[u8]) -> Result<Vec<VertexBinding>, BindingError> {
    let header = format.get(..8).ok_or(BindingError::Extent)?;
    let count = usize::from(header[0]);
    let rows = format.get(8..8 + 8 * count).ok_or(BindingError::Extent)?;
    let mut bindings = Vec::new();
    bindings
        .try_reserve_exact(count)
        .map_err(|_| BindingError::Allocation)?;
    for (ordinal, row) in rows.as_chunks::<8>().0.iter().enumerate() {
        bindings.push(declaration(row, header[1], ordinal)?);
    }
    Ok(bindings)
}
/// Built-in output layouts by selector: every layout starts with a 3-float
/// position (semantic 1); selectors 1-3 add semantic 2 and 3 entries, and
/// selector 3 also semantic 4. Strides are 12, 20, 24 and 24 bytes. Keep the
/// exact source packing metadata even though native rows remain Float4.
pub fn builtin_output_bindings(selector: u8) -> Result<Vec<VertexBinding>, BindingError> {
    let stride = match selector {
        0 => 12,
        1 => 20,
        2 | 3 => 24,
        other => return Err(BindingError::UnsupportedBuiltin(other)),
    };
    let mut bindings = vec![VertexBinding {
        declaration: 0,
        semantic: 1,
        slot: 0,
        format: 2,
        components: 3,
        stride,
        byte_offset: 0,
        encoded_bytes: 12,
    }];
    if selector != 0 {
        bindings.push(VertexBinding {
            declaration: 1,
            semantic: 2,
            slot: 2,
            format: 6,
            components: 1,
            stride,
            byte_offset: 12,
            encoded_bytes: 4,
        });
        bindings.push(VertexBinding {
            declaration: 2,
            semantic: 3,
            slot: 14,
            format: if selector == 2 { 1 } else { 6 },
            components: if selector == 2 { 4 } else { 1 },
            stride,
            byte_offset: 16,
            encoded_bytes: if selector == 2 { 8 } else { 4 },
        });
    }
    if selector == 3 {
        bindings.push(VertexBinding {
            declaration: 3,
            semantic: 4,
            slot: 15,
            format: 6,
            components: 1,
            stride,
            byte_offset: 20,
            encoded_bytes: 4,
        });
    }
    Ok(bindings)
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct AuxiliaryBinding {
    pub binding: VertexBinding,
    /// Raw source declaration retained separately from the effective state.
    pub source_declaration: [u8; 8],
    pub source_format_word: u32,
    pub frequency: u16,
    pub modulo: bool,
    pub location: u32,
    pub offset: u32,
}
fn word(bytes: &[u8], at: usize) -> u32 {
    u32::from_be_bytes(bytes[at..at + 4].try_into().expect("checked source record"))
}
/// The auxiliary table at partition +0x110 uses header byte 2 for its
/// population. Only array offsets come from the partition: the actual GPU
/// format comes from the selected raw vertex-state record, not the partition
/// declaration. `vertex_byte_offset` sums the earlier partitions' word +0x10
/// sizes, each rounded up to 16.
pub fn plan_auxiliary_bindings(
    partition: &[u8; 544],
    vertex_state: &[u8; 656],
    mesh_stream: u32,
    vertex_byte_offset: u32,
    technique_mask: u32,
) -> Result<Vec<AuxiliaryBinding>, BindingError> {
    let count = usize::from(partition[0x112]);
    let rows = partition
        .get(0x118..0x118 + 8 * count)
        .ok_or(BindingError::Extent)?;
    let mut bindings = Vec::new();
    bindings
        .try_reserve_exact(count)
        .map_err(|_| BindingError::Allocation)?;
    for (ordinal, row) in rows.as_chunks::<8>().0.iter().enumerate() {
        let mut binding = declaration(row, partition[0x111], ordinal)?;
        let slot = usize::from(binding.slot);
        if technique_mask & (1 << (2 * (15 - slot))) == 0
            || word(vertex_state, 0x120 + 20 * slot) == 0
        {
            continue;
        }
        let header = word(vertex_state, 8 * slot);
        if header != 0x0004_1740 + 4 * slot as u32 {
            return Err(BindingError::StateHeader(header));
        }
        let state = word(vertex_state, 8 * slot + 4);
        let frequency = state >> 16;
        let modulo = if frequency > 1 {
            let header = word(vertex_state, 0x80);
            if header != 0x0004_1fc0 {
                return Err(BindingError::StateHeader(header));
            }
            word(vertex_state, 0x84) & (1 << slot) != 0
        } else {
            false
        };
        binding.stride = (state >> 8) as u8;
        binding.components = ((state >> 4) & 15) as u8;
        binding.format = (state & 15) as u8;
        let offset = (mesh_stream & 0x7fff_ffff)
            .checked_add(vertex_byte_offset)
            .and_then(|n| n.checked_add(u32::from(row[0])))
            .filter(|&n| n < 0x8000_0000)
            .ok_or(BindingError::Range)?;
        bindings.push(AuxiliaryBinding {
            binding,
            source_declaration: *row,
            source_format_word: state,
            frequency: frequency as u16,
            modulo,
            location: mesh_stream >> 31,
            offset,
        });
    }
    Ok(bindings)
}
pub fn gpu_attribute_source_bytes(
    binding: &VertexBinding,
    count: usize,
) -> Result<usize, BindingError> {
    if !(1..=4).contains(&binding.components) {
        return Err(BindingError::Components(binding.components));
    }
    let element = match binding.format {
        1 | 3 | 5 => 2 * usize::from(binding.components),
        2 => 4 * usize::from(binding.components),
        4 | 7 => usize::from(binding.components),
        6 => 4,
        other => return Err(BindingError::Format(other)),
    };
    if count == 0 {
        return Ok(0);
    }
    (count - 1)
        .checked_mul(usize::from(binding.stride))
        .and_then(|n| n.checked_add(element))
        .ok_or(BindingError::Range)
}
fn half_bits(h: u16) -> u32 {
    let sign = u32::from(h & 0x8000) << 16;
    let exp = u32::from((h >> 10) & 31);
    let fraction = u32::from(h & 1023) << 13;
    match exp {
        0 => sign,
        31 => sign | 0x7f80_0000 | fraction,
        _ => sign | ((exp + 112) << 23) | fraction,
    }
}
fn auxiliary_element(binding: &AuxiliaryBinding, row: usize) -> usize {
    match usize::from(binding.frequency) {
        0 | 1 => row,
        n if binding.modulo => row % n,
        n => row / n,
    }
}
pub fn auxiliary_source_bytes(
    binding: &AuxiliaryBinding,
    count: usize,
) -> Result<usize, BindingError> {
    let rows = if count == 0 {
        0
    } else if binding.frequency > 1 && binding.modulo {
        count.min(usize::from(binding.frequency))
    } else {
        auxiliary_element(binding, count - 1) + 1
    };
    gpu_attribute_source_bytes(&binding.binding, rows)
}
/// Expand source-qualified modulo/division frequency to logical source rows.
/// These are canonical per-partition rows; no inherited base index is guessed.
pub fn decode_auxiliary_attribute(
    binding: &AuxiliaryBinding,
    source: &[u8],
    count: usize,
    byte_budget: usize,
) -> Result<Vec<[u32; 4]>, BindingError> {
    decode_rows(
        &binding.binding,
        source,
        count,
        byte_budget,
        auxiliary_source_bytes(binding, count)?,
        |row| auxiliary_element(binding, row),
    )
}
/// Retained SDK GPU fetch semantics, separate from SPU source codecs. `source`
/// starts at the already resolved attribute offset; byte_offset is metadata.
/// Half subnormals flush to signedzero; normal floats and special bits survive.
pub fn decode_gpu_attribute(
    binding: &VertexBinding,
    source: &[u8],
    count: usize,
    byte_budget: usize,
) -> Result<Vec<[u32; 4]>, BindingError> {
    decode_rows(
        binding,
        source,
        count,
        byte_budget,
        gpu_attribute_source_bytes(binding, count)?,
        |row| row,
    )
}
fn decode_rows(
    binding: &VertexBinding,
    source: &[u8],
    count: usize,
    byte_budget: usize,
    extent: usize,
    element: impl Fn(usize) -> usize,
) -> Result<Vec<[u32; 4]>, BindingError> {
    let bytes = count
        .checked_mul(16)
        .filter(|&n| n <= byte_budget)
        .ok_or(BindingError::Budget)?;
    source.get(..extent).ok_or(BindingError::Extent)?;
    let mut result = Vec::new();
    result
        .try_reserve_exact(bytes / 16)
        .map_err(|_| BindingError::Allocation)?;
    for row in 0..count {
        let at = element(row) * usize::from(binding.stride);
        let mut out = [0, 0, 0, 0x3f80_0000];
        if binding.format == 6 {
            let w = word(source, at);
            let signed = |v: u32, n: u32| ((v << (32 - n)) as i32) >> (32 - n);
            out = [
                (signed(w & 2047, 11) as f32 / 1023.).to_bits(),
                (signed((w >> 11) & 2047, 11) as f32 / 1023.).to_bits(),
                (signed(w >> 22, 10) as f32 / 511.).to_bits(),
                0x3f80_0000,
            ];
        } else {
            for (lane, value) in out
                .iter_mut()
                .enumerate()
                .take(usize::from(binding.components))
            {
                *value = match binding.format {
                    2 => word(source, at + 4 * lane),
                    4 => (f32::from(source[at + lane]) / 255.).to_bits(),
                    7 => f32::from(source[at + lane]).to_bits(),
                    kind => {
                        let at = at + 2 * lane;
                        let raw = u16::from_be_bytes([source[at], source[at + 1]]);
                        match kind {
                            3 => half_bits(raw),
                            1 => (f32::from(raw as i16) / 32767.).to_bits(),
                            5 => f32::from(raw as i16).to_bits(),
                            _ => unreachable!("validated format"),
                        }
                    }
                };
            }
        }
        result.push(out);
    }
    Ok(result)
}

/// Main's static-partition slot6 source, selected before any draw-specific
/// work (shared by the material capture and the geometry capture).
///
/// Behaviour: Group0 and non-static meshes never touch slot6. A static mesh
/// whose selected material lacks flag 0x40 leaves slot6 unchanged. With the
/// flag, an MBI whose word24 bit0 is clear and whose word16 (16-byte aligned)
/// is nonzero names a 64-byte optional-array descriptor; otherwise Main writes
/// its static current value.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SlotSixSource {
    Unchanged,
    Current,
    Descriptor(u32),
}
pub fn slot_six_source(
    mesh_group: u16,
    mesh_static_word: u32,
    selected_flags: u32,
    mbi: &[u8; 32],
) -> SlotSixSource {
    if mesh_group == 0 || mesh_static_word & 0x1000 == 0 || selected_flags & 0x40 == 0 {
        return SlotSixSource::Unchanged;
    }
    let control = word(mbi, 24);
    let descriptor = word(mbi, 16) & !15;
    if control & 1 == 0 && descriptor != 0 {
        SlotSixSource::Descriptor(descriptor)
    } else {
        SlotSixSource::Current
    }
}
/// The optional slot6 array described by an actual 64-byte descriptor.
///
/// Behaviour: descriptor byte16 equal to 2 selects an array (any other value
/// means Main writes its current constant instead). Byte52 nonzero selects
/// four big-endian floats per vertex (16-byte stride); zero selects four
/// unsigned-normalized bytes (4-byte stride). Word44 holds the GPU location
/// in bit31 (set = main memory) and the base offset in the remaining bits.
/// The partition's first source vertex (header word 0xF8) advances the base by
/// whole elements, so partition row `i` reads element `first + i`. The array is
/// per-vertex: no frequency divider or modulo applies.
pub fn slot_six_stream_binding(
    descriptor: &[u8; 64],
    first_vertex: u32,
) -> Result<Option<AuxiliaryBinding>, BindingError> {
    if descriptor[16] != 2 {
        return Ok(None);
    }
    let float4 = descriptor[52] != 0;
    let (format, stride) = if float4 { (2u8, 16u8) } else { (4, 4) };
    let placement = word(descriptor, 44);
    let offset = (placement & 0x7fff_ffff)
        .checked_add(
            first_vertex
                .checked_mul(u32::from(stride))
                .ok_or(BindingError::Range)?,
        )
        .filter(|&o| o < 0x8000_0000)
        .ok_or(BindingError::Range)?;
    let raw: [u8; 8] = descriptor[44..52].try_into().expect("descriptor extent");
    Ok(Some(AuxiliaryBinding {
        binding: VertexBinding {
            // Main binds this array after the auxiliary declarations, so it
            // takes precedence over any of them that name slot6.
            declaration: usize::MAX,
            semantic: 0,
            slot: 6,
            format,
            components: 4,
            stride,
            byte_offset: 0,
            encoded_bytes: stride,
        },
        source_declaration: raw,
        source_format_word: (u32::from(stride) << 8) | 0x40 | u32::from(format),
        frequency: 0,
        modulo: false,
        location: placement >> 31,
        offset,
    }))
}
#[cfg(test)]
mod slot_six_tests {
    use super::*;
    #[test]
    fn slot_six_source_follows_static_flag_and_descriptor_predicates() {
        let mut mbi = [0u8; 32];
        mbi[16..20].copy_from_slice(&0x1234_5678u32.to_be_bytes());
        assert_eq!(
            slot_six_source(0, 0x1000, 0x40, &mbi),
            SlotSixSource::Unchanged
        );
        assert_eq!(slot_six_source(3, 0, 0x40, &mbi), SlotSixSource::Unchanged);
        assert_eq!(
            slot_six_source(3, 0x1000, 0, &mbi),
            SlotSixSource::Unchanged
        );
        assert_eq!(
            slot_six_source(3, 0x1000, 0x40, &mbi),
            SlotSixSource::Descriptor(0x1234_5670)
        );
        mbi[27] = 1;
        assert_eq!(
            slot_six_source(3, 0x1000, 0x40, &mbi),
            SlotSixSource::Current
        );
        mbi[27] = 0;
        mbi[16..20].copy_from_slice(&8u32.to_be_bytes());
        assert_eq!(
            slot_six_source(3, 0x1000, 0x40, &mbi),
            SlotSixSource::Current
        );
    }
    #[test]
    fn slot_six_descriptor_selects_layout_location_and_first_vertex_offset() {
        let mut d = [0u8; 64];
        d[44..48].copy_from_slice(&0x0000_0100u32.to_be_bytes());
        assert_eq!(slot_six_stream_binding(&d, 3).unwrap(), None);
        d[16] = 2;
        let b = slot_six_stream_binding(&d, 3).unwrap().unwrap();
        assert_eq!(
            (
                b.binding.slot,
                b.binding.format,
                b.binding.stride,
                b.location,
                b.offset
            ),
            (6, 4, 4, 0, 0x100 + 12)
        );
        d[52] = 1;
        d[44..48].copy_from_slice(&0x8000_0100u32.to_be_bytes());
        let b = slot_six_stream_binding(&d, 3).unwrap().unwrap();
        assert_eq!(
            (b.binding.format, b.binding.stride, b.location, b.offset),
            (2, 16, 1, 0x100 + 48)
        );
        d[44..48].copy_from_slice(&0xffff_fff0u32.to_be_bytes());
        assert_eq!(slot_six_stream_binding(&d, 3), Err(BindingError::Range));
    }
}
