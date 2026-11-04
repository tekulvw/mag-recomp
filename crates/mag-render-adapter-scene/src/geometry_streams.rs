//! Owned logical geometry components before deformation and complete draw assembly.
//!
//! These functions consume immutable owned copies of guest stream bytes, never
//! a borrowed guest pointer or another stage's output.
//! A decoded component is not a complete geometry operation or source lease.
use crate::partition_inputs::{OwnedGroupZeroInputs, OwnedPartitionInputs};
use mag_render_behavior::normals;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum GeometryStreamError {
    SourceExtent,
    MissingPositionAttribute,
    MissingAttribute(u8),
    UnclassifiedSemantic(u8),
    UnsupportedAttributeEncoding {
        semantic: u8,
        codec: u8,
    },
    BudgetExceeded,
    AllocationFailed,
    UnsupportedPositionEncoding(u8),
    UnqualifiedNormalSelector(u8),
    /// Kept for callers that match on it. The packed-normal decoder clamps a
    /// negative remainder to zero, so this is no longer produced.
    NormalMagnitude(u32),
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PositionEncoding {
    Float32x3,
    Rebased16x3,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct InstanceRecord {
    pub position_scale: [u32; 4],
    pub quaternion: [u32; 4],
}
/// Source-specific signed exponent rebias, not IEEE binary16 conversion.
pub fn rebased_component_bits(encoded: u16) -> u32 {
    let sign = u32::from(encoded & 0x8000) << 16;
    let magnitude = u32::from(encoded & 0x7fff);
    sign | if magnitude == 0 {
        0
    } else {
        0x3800_0000 + (magnitude << 13)
    }
}
fn extent(source: &[u8], count: usize, stride: usize) -> Result<&[u8], GeometryStreamError> {
    let bytes = count
        .checked_mul(stride)
        .ok_or(GeometryStreamError::SourceExtent)?;
    source.get(..bytes).ok_or(GeometryStreamError::SourceExtent)
}
fn allocate<T>(count: usize, byte_budget: usize) -> Result<Vec<T>, GeometryStreamError> {
    count
        .checked_mul(core::mem::size_of::<T>())
        .filter(|&bytes| bytes <= byte_budget)
        .ok_or(GeometryStreamError::BudgetExceeded)?;
    let mut result = Vec::new();
    result
        .try_reserve_exact(count)
        .map_err(|_| GeometryStreamError::AllocationFailed)?;
    Ok(result)
}
/// Decode exactly the logical population; additional source bytes are ignored.
pub fn decode_instance_rows(
    source: &[u8],
    count: usize,
    byte_budget: usize,
) -> Result<Vec<InstanceRecord>, GeometryStreamError> {
    let bytes = extent(source, count, 16)?;
    let mut result = allocate(count, byte_budget)?;
    for row in bytes.as_chunks::<16>().0 {
        let components: [u32; 8] = core::array::from_fn(|i| {
            rebased_component_bits(u16::from_be_bytes([row[2 * i], row[2 * i + 1]]))
        });
        result.push(InstanceRecord {
            position_scale: components[..4].try_into().expect("four components"),
            quaternion: components[4..].try_into().expect("four components"),
        });
    }
    Ok(result)
}
/// Static source operation only. Generic descriptor-driven packed positions
/// require their own format decoder and must not be passed as static triples.
pub fn decode_positions(
    source: &[u8],
    count: usize,
    encoding: PositionEncoding,
    byte_budget: usize,
) -> Result<Vec<[u32; 4]>, GeometryStreamError> {
    let stride = match encoding {
        PositionEncoding::Float32x3 => 12,
        PositionEncoding::Rebased16x3 => 6,
    };
    let bytes = extent(source, count, stride)?;
    let mut result = allocate(count, byte_budget)?;
    for row in bytes.chunks_exact(stride) {
        let mut position = [0, 0, 0, 0x3f80_0000];
        for (axis, lane) in position[..3].iter_mut().enumerate() {
            *lane = match encoding {
                PositionEncoding::Float32x3 => {
                    u32::from_be_bytes(row[axis * 4..axis * 4 + 4].try_into().expect("four bytes"))
                }
                PositionEncoding::Rebased16x3 => {
                    rebased_component_bits(u16::from_be_bytes([row[axis * 2], row[axis * 2 + 1]]))
                }
            };
        }
        result.push(position);
    }
    Ok(result)
}
/// Preserve logical source order and duplicates without reading SIMD padding.
pub fn decode_indices(
    source: &[u8],
    count: usize,
    byte_budget: usize,
) -> Result<Vec<u16>, GeometryStreamError> {
    let bytes = extent(source, count, 2)?;
    let mut result = allocate(count, byte_budget)?;
    result.extend(
        bytes
            .as_chunks::<2>()
            .0
            .iter()
            .map(|&x| u16::from_be_bytes(x)),
    );
    Ok(result)
}
#[derive(Debug, PartialEq, Eq)]
pub struct PositionIndexInputs {
    pub positions: Vec<[u32; 4]>,
    pub indices: Vec<u16>,
}
/// Connect owned raw input19/7/1 to the static position operation. This does
/// not claim descriptor-driven geometry, deformation, attributes or draw state.
pub fn decode_static_position_inputs(
    inputs: &OwnedPartitionInputs,
    byte_budget: usize,
) -> Result<PositionIndexInputs, GeometryStreamError> {
    let sources = inputs.inputs();
    let config = sources[19]
        .bytes()
        .get(..16)
        .ok_or(GeometryStreamError::SourceExtent)?;
    let encoding = match config[2] {
        0 => PositionEncoding::Float32x3,
        6 => PositionEncoding::Rebased16x3,
        other => return Err(GeometryStreamError::UnsupportedPositionEncoding(other)),
    };
    let vertices = usize::from(u16::from_be_bytes([config[8], config[9]]));
    let indices = usize::from(u16::from_be_bytes([config[10], config[11]]));
    let vertex_bytes = vertices * 16;
    vertex_bytes
        .checked_add(indices * 2)
        .filter(|&n| n <= byte_budget)
        .ok_or(GeometryStreamError::BudgetExceeded)?;
    // Validate both extents before allocating either output.
    extent(
        sources[7].bytes(),
        vertices,
        match encoding {
            PositionEncoding::Float32x3 => 12,
            PositionEncoding::Rebased16x3 => 6,
        },
    )?;
    extent(sources[1].bytes(), indices, 2)?;
    Ok(PositionIndexInputs {
        positions: decode_positions(sources[7].bytes(), vertices, encoding, vertex_bytes)?,
        indices: decode_indices(sources[1].bytes(), indices, indices * 2)?,
    })
}
/// Decode actual planned Main rows. None retains the distinct source operation
/// with one instance and no row stream; it is not a fabricated identity pose.
pub fn decode_group_zero_instance_rows(
    inputs: &OwnedGroupZeroInputs,
    byte_budget: usize,
) -> Result<Option<Vec<InstanceRecord>>, GeometryStreamError> {
    let source = inputs.inputs.inputs()[13].bytes();
    if source.is_empty() {
        return if inputs.instances == 1 {
            Ok(None)
        } else {
            Err(GeometryStreamError::SourceExtent)
        };
    }
    decode_instance_rows(source, usize::from(inputs.instances), byte_budget).map(Some)
}

/// Generic descriptor-driven position only; other attribute declarations remain
/// required consumers of the full geometry operation. Fresh operation declaration order is
/// preserved: a position semantic already decoded is not replaced by a later
/// duplicate declaration. No state cached by an earlier decode is an input here.
pub fn decode_descriptor_positions(
    format: &[u8],
    source: &[u8],
    count: usize,
    byte_budget: usize,
) -> Result<Vec<[u32; 4]>, GeometryStreamError> {
    decode_descriptor_positions_at(format, source, 0, count, byte_budget)
}
/// Decode an owned source stream with its genuine DMA byte phase. This is a
/// source address only, never a generation or a pointer into guest memory.
pub fn decode_descriptor_positions_at(
    format: &[u8],
    source: &[u8],
    source_address: u32,
    count: usize,
    byte_budget: usize,
) -> Result<Vec<[u32; 4]>, GeometryStreamError> {
    decode_descriptor_attribute_at(format, source, source_address, 1, count, byte_budget)
}
/// Decode the first declaration for one source semantic. Numeric IDs remain
/// source interface data; this does not assign native shader locations/names.
pub fn decode_descriptor_attribute_at(
    format: &[u8],
    source: &[u8],
    source_address: u32,
    semantic: u8,
    count: usize,
    byte_budget: usize,
) -> Result<Vec<[u32; 4]>, GeometryStreamError> {
    let header = format.get(..8).ok_or(GeometryStreamError::SourceExtent)?;
    let declarations = format
        .get(8..8 + usize::from(header[0]) * 8)
        .ok_or(GeometryStreamError::SourceExtent)?;
    let ordinal = declarations
        .as_chunks::<8>()
        .0
        .iter()
        .position(|entry| entry[3] == semantic)
        .ok_or(if semantic == 1 {
            GeometryStreamError::MissingPositionAttribute
        } else {
            GeometryStreamError::MissingAttribute(semantic)
        })?;
    decode_descriptor_declaration_at(format, source, source_address, ordinal, count, byte_budget)
}
/// Decode one actual declaration, including duplicate semantic declarations.
/// Consumers choosing by semantic may separately use the first-lookup API.
pub fn decode_descriptor_declaration_at(
    format: &[u8],
    source: &[u8],
    source_address: u32,
    ordinal: usize,
    count: usize,
    byte_budget: usize,
) -> Result<Vec<[u32; 4]>, GeometryStreamError> {
    let header = format.get(..8).ok_or(GeometryStreamError::SourceExtent)?;
    let declarations = format
        .get(8..8 + usize::from(header[0]) * 8)
        .ok_or(GeometryStreamError::SourceExtent)?;
    let selected = declarations
        .as_chunks::<8>()
        .0
        .get(ordinal)
        .ok_or(GeometryStreamError::SourceExtent)?;
    let semantic = selected[3];
    if !matches!(selected[1], 1..=7 | 9) {
        return Err(if semantic == 1 {
            GeometryStreamError::UnsupportedPositionEncoding(selected[1])
        } else {
            GeometryStreamError::UnsupportedAttributeEncoding {
                semantic,
                codec: selected[1],
            }
        });
    }
    let stride = usize::from(header[1]);
    let offset = usize::from(selected[0]);
    if selected[1] != 6 {
        let phase = (source_address & 15) as usize;
        if count != 0 {
            (count - 1)
                .checked_mul(stride)
                .and_then(|n| n.checked_add(offset))
                .and_then(|n| n.checked_add(phase + 16))
                .ok_or(GeometryStreamError::SourceExtent)?;
        }
        let mut positions = allocate(count, byte_budget)?;
        for index in 0..count {
            let logical_at = index * stride + offset;
            let block = (phase + logical_at) & !15;
            let within = logical_at & 15;
            let at = (block + within)
                .checked_sub(phase)
                .ok_or(GeometryStreamError::SourceExtent)?;
            if selected[1] == 9 {
                let encoded = source
                    .get(at..at + 3)
                    .ok_or(GeometryStreamError::SourceExtent)?;
                // Codec 9 is the packed spherical normal of `geometry-normals.md`,
                // decoded by the clean-room behaviour crate in native arithmetic
                // (within that specification's stated tolerance).
                let mut normal =
                    normals::decode_packed_normal(encoded.try_into().expect("three source bytes"))
                        .map(|lanes| lanes.map(f32::to_bits))
                        .map_err(|normals::NormalError::UnqualifiedSelector(v)| {
                            GeometryStreamError::UnqualifiedNormalSelector(v as u8)
                        })?;
                if semantic == 1 {
                    for (lane, value) in normal
                        .iter_mut()
                        .enumerate()
                        .skip(usize::from(selected[2]).min(4))
                    {
                        *value = if lane == 3 { 0x3f800000 } else { 0 };
                    }
                }
                positions.push(normal);
                continue;
            }
            let mut position = [0, 0, 0, 0x3f80_0000];
            for (lane, output) in position.iter_mut().enumerate().take(if semantic == 1 {
                usize::from(selected[2]).min(4)
            } else {
                4
            }) {
                *output = if matches!(selected[1], 1 | 3 | 5) {
                    let part = source
                        .get(at + lane * 2..at + lane * 2 + 2)
                        .ok_or(GeometryStreamError::SourceExtent)?;
                    let encoded = u16::from_be_bytes([part[0], part[1]]);
                    match selected[1] {
                        1 => quantized_normal_bits(
                            (2 * i64::from(encoded as i16) + 1) * 0x800080,
                            39,
                        ),
                        3 => rebased_component_bits(encoded),
                        5 => quantized_normal_bits(i64::from(encoded as i16), 0),
                        _ => unreachable!("selected two-byte field codec"),
                    }
                } else if matches!(selected[1], 4 | 7) {
                    let encoded = i64::from(
                        *source
                            .get(at + lane)
                            .ok_or(GeometryStreamError::SourceExtent)?,
                    );
                    if selected[1] == 4 {
                        quantized_normal_bits(encoded * 0x808080, 31)
                    } else {
                        quantized_normal_bits(encoded, 0)
                    }
                } else {
                    let virtual_at = block + within;
                    let cross = within + usize::from(selected[4]) > 16;
                    let mut component = [0u8; 4];
                    for (byte, output) in component.iter_mut().enumerate() {
                        let relative = lane * 4 + byte;
                        let selected = if cross {
                            virtual_at + relative
                        } else {
                            (virtual_at & !15) + ((within + relative) & 15)
                        };
                        let selected = selected
                            .checked_sub(phase)
                            .ok_or(GeometryStreamError::SourceExtent)?;
                        *output = *source
                            .get(selected)
                            .ok_or(GeometryStreamError::SourceExtent)?;
                    }
                    u32::from_be_bytes(component)
                };
            }
            positions.push(position);
        }
        return Ok(positions);
    }
    let bytes = if count == 0 {
        0
    } else {
        (count - 1)
            .checked_mul(stride)
            .and_then(|n| n.checked_add(offset))
            .and_then(|n| n.checked_add(4))
            .ok_or(GeometryStreamError::SourceExtent)?
    };
    if source_address & 15 == 0 {
        source
            .get(..bytes)
            .ok_or(GeometryStreamError::SourceExtent)?;
    }
    let mut positions = allocate(count, byte_budget)?;
    let phase = (source_address & 15) as usize;
    for index in 0..count {
        let logical_at = index * stride + offset;
        let at = phase
            .checked_add(logical_at)
            .map(|n| (n & !15) + (logical_at & 15))
            .and_then(|n| n.checked_sub(phase))
            .ok_or(GeometryStreamError::SourceExtent)?;
        let packed = u32::from_be_bytes(
            source
                .get(at..at.checked_add(4).ok_or(GeometryStreamError::SourceExtent)?)
                .ok_or(GeometryStreamError::SourceExtent)?
                .try_into()
                .expect("four bytes"),
        );
        // The authored affine rational law admits exact 64-bit integer
        // evaluation, independently of host/SPU/GPU floating arithmetic.
        let mut position = [0, 0, 0, 0x3f80_0000];
        for (axis, (shift, width, coefficient, scale)) in [
            (0, 11, 0x801002, 34),
            (11, 11, 0x801002, 34),
            (22, 10, 0x802008, 33),
        ]
        .into_iter()
        .enumerate()
        {
            let raw = (packed >> shift) & ((1 << width) - 1);
            let signed = if raw & (1 << (width - 1)) == 0 {
                i64::from(raw)
            } else {
                i64::from(raw) - (1 << width)
            };
            position[axis] = quantized_normal_bits((2 * signed + 1) * coefficient, scale);
        }
        // The generic position postprocessor fills unspecified axes with +0.
        // Metadata >=3 retains all three decoded axes; W is always point one.
        if semantic == 1 {
            position[..3]
                .iter_mut()
                .skip(usize::from(selected[2]))
                .for_each(|lane| *lane = 0);
        }
        positions.push(position);
    }
    Ok(positions)
}
/// Pack the bounded format's exact signed rational numerator * 2^-scale as
/// binary32 truncated toward zero. Zero maps to positive zero; supported
/// nonzero inputs produce normal values. No generic float emulation is exposed.
fn quantized_normal_bits(numerator: i64, scale: u32) -> u32 {
    if numerator == 0 {
        return 0;
    }
    let magnitude = numerator.unsigned_abs();
    let highest = 63 - magnitude.leading_zeros();
    let significand = if highest >= 23 {
        magnitude >> (highest - 23)
    } else {
        magnitude << (23 - highest)
    };
    let exponent = highest + 127 - scale;
    (u32::from(numerator < 0) << 31) | (exponent << 23) | (significand as u32 & 0x7f_ffff)
}
/// Connect owned source19/20/7/1 to descriptor position and base-index outputs.
/// This is still a component operation: normals/weights, deformation, mapped
/// attributes, material bindings and draw/resource lifetimes remain explicit.
pub fn decode_descriptor_position_inputs(
    inputs: &OwnedPartitionInputs,
    byte_budget: usize,
) -> Result<PositionIndexInputs, GeometryStreamError> {
    let sources = inputs.inputs();
    let config = sources[19]
        .bytes()
        .get(..16)
        .ok_or(GeometryStreamError::SourceExtent)?;
    if config[2] != 255 {
        return Err(GeometryStreamError::UnsupportedPositionEncoding(config[2]));
    }
    let vertices = usize::from(u16::from_be_bytes([config[8], config[9]]));
    let count = usize::from(u16::from_be_bytes([config[10], config[11]]));
    let vertex_bytes = vertices * 16;
    vertex_bytes
        .checked_add(count * 2)
        .filter(|&n| n <= byte_budget)
        .ok_or(GeometryStreamError::BudgetExceeded)?;
    extent(sources[1].bytes(), count, 2)?;
    Ok(PositionIndexInputs {
        positions: decode_descriptor_positions_at(
            sources[20].bytes(),
            sources[7].bytes(),
            sources[7].source_address(),
            vertices,
            vertex_bytes,
        )?,
        indices: decode_indices(sources[1].bytes(), count, count * 2)?,
    })
}
