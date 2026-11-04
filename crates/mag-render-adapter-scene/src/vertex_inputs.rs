//! Canonical shader inputs from owned semantic streams and source register writes.
//! Auxiliary offsets precede child output bindings. Only shader-read arrays are
//! packed, but the full effective array mask survives for current-value selection.
use crate::{
    cohort_geometry::{
        DecodedAuxiliaryAttribute, DiagnosticMeshDraw, OwnedAuxiliaryAttributes,
        OwnedPreparedGroupZero,
    },
    geometry_bindings::VertexBinding,
};
/// Exact remains the default. This POC policy deliberately binds real owned
/// static positions where the source custom descriptor leaves prior arrays
/// unresolved; it does not reconstruct the original GPU array offset.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum VertexInputPolicy {
    #[default]
    Exact,
    PocStaticPosition,
}
const POC_STATIC_POSITION: VertexBinding = VertexBinding {
    declaration: 0,
    semantic: 1,
    slot: 0,
    format: 2,
    components: 3,
    stride: 12,
    byte_offset: 0,
    encoded_bytes: 12,
};
pub fn position_binding_approximated(draw: &DiagnosticMeshDraw, policy: VertexInputPolicy) -> bool {
    policy == VertexInputPolicy::PocStaticPosition
        && crate::cohort_geometry::poc_static_position_binding(draw).is_some()
}
/// `position_binding_approximated(draw, policy)` for a draw whose
/// `effective_array_mask_with_policy(draw, policy)` already succeeded, without
/// repeating the binding qualification: that success means the source output
/// bindings resolved, which for a non-direct draw with failed bindings happens
/// only through the approximated position binding.
pub fn position_binding_approximated_after_mask(
    draw: &DiagnosticMeshDraw,
    policy: VertexInputPolicy,
) -> bool {
    let approximated = draw.prepared_direct.is_none() && draw.output_bindings.is_err();
    debug_assert_eq!(approximated, position_binding_approximated(draw, policy));
    approximated
}
fn output_bindings(
    draw: &DiagnosticMeshDraw,
    policy: VertexInputPolicy,
) -> Result<&[VertexBinding], VertexInputError> {
    if draw.prepared_direct.is_some() {
        return Ok(&[]);
    }
    match &draw.output_bindings {
        Ok(output) => Ok(output),
        Err(_) if position_binding_approximated(draw, policy) => {
            Ok(std::slice::from_ref(&POC_STATIC_POSITION))
        }
        Err(_) => Err(VertexInputError::SourceBindings),
    }
}
#[derive(Debug, PartialEq, Eq)]
pub enum VertexInputError {
    SourceBindings,
    /// The owning source result retains the exact preparation refusal.
    GroupZeroPreparation,
    NoninstancedPreparation,
    ConflictingPreparation,
    Slot(u8),
    MissingArray(u8),
    UnpreparedPrimarySemantic {
        slot: u8,
        semantic: u8,
    },
    MissingSemantic {
        slot: u8,
        semantic: u8,
    },
    FailedSemantic {
        slot: u8,
        semantic: u8,
    },
    FailedAuxiliary(u8),
    Components(u8),
    Format(u8),
    VertexCount {
        slot: u8,
        expected: usize,
        actual: usize,
    },
    Index {
        index: u32,
        vertices: usize,
    },
    Budget,
    Allocation,
}
#[derive(Debug, PartialEq, Eq)]
pub struct VertexAttribute {
    pub location: u32,
    pub offset: u32,
}
pub struct NativeVertexInputs {
    pub array_mask: u16,
    pub stride: u32,
    /// All native attributes use Float4; source component defaults are explicit.
    pub attributes: Vec<VertexAttribute>,
    pub vertices: Vec<u8>,
    pub indices: Vec<u32>,
}
#[derive(Clone, Copy)]
enum Source<'a> {
    Primary(&'a VertexBinding),
    Auxiliary(&'a DecodedAuxiliaryAttribute),
    FailedAuxiliary(&'a VertexBinding),
    PreparedColor(&'a [[u32; 4]]),
}
impl<'a> Source<'a> {
    fn binding(self) -> &'a VertexBinding {
        match self {
            Self::Primary(b) | Self::FailedAuxiliary(b) => b,
            Self::Auxiliary(a) => &a.source.binding,
            Self::PreparedColor(_) => &PREPARED_COLOR,
        }
    }
}
// Actual Group0 parent enables array12 before auxiliary/output assignments.
// Canonical values are Float4 after the source UNORM8 fetch conversion.
const PREPARED_COLOR: VertexBinding = VertexBinding {
    declaration: 0,
    semantic: 0,
    slot: 12,
    format: 2,
    components: 4,
    stride: 16,
    byte_offset: 0,
    encoded_bytes: 16,
};
fn prepared(
    draw: &DiagnosticMeshDraw,
) -> Result<Option<&OwnedPreparedGroupZero>, VertexInputError> {
    draw.prepared_group_zero
        .as_ref()
        .map(|r| {
            r.as_ref()
                .map_err(|_| VertexInputError::GroupZeroPreparation)
        })
        .transpose()
}
fn static_direction_four(
    draw: &DiagnosticMeshDraw,
    slot: u8,
) -> Result<&[[u32; 4]], VertexInputError> {
    let failure = || VertexInputError::FailedSemantic { slot, semantic: 4 };
    let bank = if let Some(primary) = &draw.attributes
        && let Some(bank) = primary.as_ref().map_err(|_| failure())?.first_semantic(4)
    {
        bank.map_err(|_| failure())?.words.as_slice()
    } else if let Some(secondary) = &draw.secondary_attributes
        && let Some(bank) = secondary.as_ref().map_err(|_| failure())?.first_semantic(4)
    {
        bank.map_err(|_| failure())?.words.as_slice()
    } else {
        return Err(VertexInputError::MissingSemantic { slot, semantic: 4 });
    };
    // Builtin3 packs this source semantic as CMP XYZ. Preserve the same
    // admitted direction domain as the qualified static normal/tangent path;
    // unused W is not an arithmetic operand or a finite-value requirement.
    if bank.iter().any(|row| {
        row[..3].iter().any(|&v| {
            let value = f32::from_bits(v);
            !value.is_finite() || value.abs() > 1.
        })
    }) {
        return Err(failure());
    }
    Ok(bank)
}
fn selected<'a>(
    output: &'a [VertexBinding],
    auxiliary: &'a OwnedAuxiliaryAttributes,
    color: Option<&'a [[u32; 4]]>,
) -> Result<[Option<Source<'a>>; 16], VertexInputError> {
    let mut slots: [Option<Source<'a>>; 16] = [None; 16];
    slots[12] = color.map(Source::PreparedColor);
    // Success/failure arrays retain declaration ordinals separately. Resolve
    // both in source order so an earlier failure never replaces a later write.
    for source in auxiliary.attributes.iter().map(Source::Auxiliary).chain(
        auxiliary
            .failures
            .iter()
            .map(|f| Source::FailedAuxiliary(&f.source.binding)),
    ) {
        let b = source.binding();
        let slot = slots
            .get_mut(usize::from(b.slot))
            .ok_or(VertexInputError::Slot(b.slot))?;
        if slot.is_none_or(|old| old.binding().declaration <= b.declaration) {
            *slot = Some(source);
        }
    }
    for b in output {
        *slots
            .get_mut(usize::from(b.slot))
            .ok_or(VertexInputError::Slot(b.slot))? = Some(Source::Primary(b));
    }
    Ok(slots)
}
/// Array enablement is resolved before compiling current-attribute fallback.
/// It requires source binding metadata, but not unused attribute payloads.
pub fn effective_array_mask(draw: &DiagnosticMeshDraw) -> Result<u16, VertexInputError> {
    effective_array_mask_with_policy(draw, VertexInputPolicy::Exact)
}
pub fn effective_array_mask_with_policy(
    draw: &DiagnosticMeshDraw,
    policy: VertexInputPolicy,
) -> Result<u16, VertexInputError> {
    let direct = draw.prepared_direct.as_ref();
    if direct.is_some()
        && (draw.prepared_group_zero.is_some() || draw.prepared_noninstanced.is_some())
    {
        return Err(VertexInputError::ConflictingPreparation);
    }
    let output = output_bindings(draw, policy)?;
    let prepared = prepared(draw)?;
    let noninstanced = draw
        .prepared_noninstanced
        .as_ref()
        .map(|p| {
            p.as_ref()
                .map_err(|_| VertexInputError::NoninstancedPreparation)
        })
        .transpose()?;
    if prepared.is_some() && noninstanced.is_some() {
        return Err(VertexInputError::ConflictingPreparation);
    }
    let auxiliary = match (direct, prepared) {
        (Some(d), _) => &d.attributes,
        (None, Some(p)) => &p.auxiliary,
        (None, None) => draw
            .auxiliary_attributes
            .as_ref()
            .map_err(|_| VertexInputError::SourceBindings)?,
    };
    let slots = selected(
        output,
        auxiliary,
        prepared.map(|p| p.geometry.attribute12.as_slice()),
    )?;
    Ok(slots.iter().enumerate().fold(0u16, |mask, (i, s)| {
        mask | if s.is_some_and(|s| s.binding().components != 0) {
            1 << i
        } else {
            0
        }
    }))
}
/// No source pointer or original packed command/vertex output is consumed.
/// Float32 canonical values retain source interpretation while avoiding an
/// unnecessary SPU output quantize/dequantize round trip. This alone is not
/// deformation, complete material selection or visual-parity acceptance.
pub fn prepare_vertex_inputs(
    draw: &DiagnosticMeshDraw,
    required_arrays: u16,
    byte_budget: usize,
) -> Result<NativeVertexInputs, VertexInputError> {
    prepare_vertex_inputs_with_policy(draw, required_arrays, byte_budget, VertexInputPolicy::Exact)
}
pub fn prepare_vertex_inputs_with_policy(
    draw: &DiagnosticMeshDraw,
    required_arrays: u16,
    byte_budget: usize,
    policy: VertexInputPolicy,
) -> Result<NativeVertexInputs, VertexInputError> {
    let plan = plan_vertex_inputs(draw, required_arrays, byte_budget, policy)?;
    let mut vertices = Vec::new();
    vertices
        .try_reserve_exact(plan.vertex_bytes())
        .map_err(|_| VertexInputError::Allocation)?;
    vertices.resize(plan.vertex_bytes(), 0);
    let (mut inputs, rows) = plan.split(Vec::new());
    if let Some(rows) = rows {
        rows.write(&mut vertices);
    }
    inputs.vertices = vertices;
    Ok(inputs)
}
/// Every validation of `prepare_vertex_inputs_with_policy`, in the same order
/// with the same errors, without building the interleaved vertex bytes. The
/// bytes depend only on `rows`, so they can be written later (possibly on
/// another thread) with `VertexRows::write`, identical to the eager result.
pub struct VertexInputPlan<'a> {
    array_mask: u16,
    attributes: Vec<VertexAttribute>,
    indices: Vec<u32>,
    rows: VertexRows<'a>,
}
/// The selected source rows of one draw and how they interleave.
pub struct VertexRows<'a> {
    rows: Vec<(&'a VertexBinding, &'a [[u32; 4]])>,
    count: usize,
    stride: u32,
}
impl VertexRows<'_> {
    pub fn bytes(&self) -> usize {
        self.count * self.stride as usize
    }
    /// Write exactly `bytes()` interleaved bytes: per vertex, each selected
    /// row as four little-endian words with missing components defaulted to
    /// (0, 0, 0, 1.0). Panics if `out.len() != bytes()`.
    pub fn write(&self, out: &mut [u8]) {
        assert_eq!(out.len(), self.bytes());
        if self.rows.is_empty() || self.count == 0 {
            return;
        }
        let stride = self.stride as usize;
        for (row, (binding, source)) in self.rows.iter().enumerate() {
            // Packed11/11/10 has one encoded component but produces XYZ at
            // the actual fetch boundary; its fourth lane still defaults to one.
            let n = if binding.format == 6 {
                3
            } else {
                usize::from(binding.components)
            };
            let reverse = binding.format == 4
                && binding.components == 4
                && binding.slot < 16
                && diag_reverse_byte4_lanes() & (1 << binding.slot) != 0;
            for (vertex, words) in out.chunks_exact_mut(stride).zip(source.iter()) {
                let mut value = [0, 0, 0, 0x3f800000];
                value[..n].copy_from_slice(&words[..n]);
                if reverse {
                    value.reverse();
                }
                let lane = &mut vertex[row * 16..row * 16 + 16];
                for (bytes, word) in lane.as_chunks_mut::<4>().0.iter_mut().zip(value) {
                    *bytes = word.to_le_bytes();
                }
            }
        }
    }
}
/// Diagnostic A/B only (`PS3_RENDER_DIAG_REVERSE_BYTE4_LANES=<inputs>`, a
/// comma list such as `6` or `3,6`): present four-component unsigned-
/// normalized byte arrays at those inputs with their lanes reversed
/// (w,z,y,x). The default keeps memory order, which the weapon's input3
/// colour (opaque alpha in the last byte) supports.
fn diag_reverse_byte4_lanes() -> u16 {
    static MASK: std::sync::OnceLock<u16> = std::sync::OnceLock::new();
    *MASK.get_or_init(|| {
        let mask = std::env::var("PS3_RENDER_DIAG_REVERSE_BYTE4_LANES")
            .ok()
            .map(|v| {
                v.split(',')
                    .filter_map(|n| n.trim().parse::<u16>().ok())
                    .filter(|&n| n < 16)
                    .fold(0u16, |m, n| m | (1 << n))
            })
            .unwrap_or(0);
        if mask != 0 {
            eprintln!("[mag main visual] diag_reverse_byte4_lanes={mask:04x}");
        }
        mask
    })
}
impl<'a> VertexInputPlan<'a> {
    /// Interleaved CPU vertex bytes.
    pub fn vertex_bytes(&self) -> usize {
        self.rows.bytes()
    }
    /// The native inputs with `vertices` as given, plus the rows that produce
    /// them. Vertices are always CPU rows here, so the rows are always `Some`.
    pub fn split(self, vertices: Vec<u8>) -> (NativeVertexInputs, Option<VertexRows<'a>>) {
        let stride = self.rows.stride;
        let rows = Some(self.rows);
        (
            NativeVertexInputs {
                array_mask: self.array_mask,
                stride,
                attributes: self.attributes,
                vertices,
                indices: self.indices,
            },
            rows,
        )
    }
}
pub fn plan_vertex_inputs<'a>(
    draw: &'a DiagnosticMeshDraw,
    required_arrays: u16,
    byte_budget: usize,
    policy: VertexInputPolicy,
) -> Result<VertexInputPlan<'a>, VertexInputError> {
    let direct = draw.prepared_direct.as_ref();
    if direct.is_some()
        && (draw.prepared_group_zero.is_some() || draw.prepared_noninstanced.is_some())
    {
        return Err(VertexInputError::ConflictingPreparation);
    }
    let output = output_bindings(draw, policy)?;
    let prepared = prepared(draw)?;
    let noninstanced = draw
        .prepared_noninstanced
        .as_ref()
        .map(|p| {
            p.as_ref()
                .map_err(|_| VertexInputError::NoninstancedPreparation)
        })
        .transpose()?;
    if prepared.is_some() && noninstanced.is_some() {
        return Err(VertexInputError::ConflictingPreparation);
    }
    let auxiliary = match (direct, prepared) {
        (Some(d), _) => &d.attributes,
        (None, Some(p)) => &p.auxiliary,
        (None, None) => draw
            .auxiliary_attributes
            .as_ref()
            .map_err(|_| VertexInputError::SourceBindings)?,
    };
    let slots = selected(
        output,
        auxiliary,
        prepared.map(|p| p.geometry.attribute12.as_slice()),
    )?;
    let array_mask = slots.iter().enumerate().fold(0u16, |mask, (i, s)| {
        mask | if s.is_some_and(|s| s.binding().components != 0) {
            1 << i
        } else {
            0
        }
    });
    if required_arrays & !array_mask != 0 {
        return Err(VertexInputError::MissingArray(
            (required_arrays & !array_mask).trailing_zeros() as u8,
        ));
    }
    let positions = noninstanced
        .map(|p| p.geometry.positions.as_slice())
        .or_else(|| prepared.map(|p| p.geometry.positions.as_slice()))
        .unwrap_or(draw.positions.as_slice());
    let count = direct.map_or(positions.len(), |d| d.vertex_count);
    let index_count = direct.map_or_else(
        || prepared.map_or(draw.indices.len(), |p| p.geometry.indices.len()),
        |d| d.indices.len(),
    );
    let source_indices = || match (direct, prepared) {
        (Some(d), _) => SourceIndices::Prepared(d.indices.iter()),
        (None, Some(p)) => SourceIndices::Prepared(p.geometry.indices.iter()),
        (None, None) => SourceIndices::Raw(draw.indices.iter()),
    };
    for index in source_indices() {
        if usize::try_from(index).map_or(true, |i| i >= count) {
            return Err(VertexInputError::Index {
                index,
                vertices: count,
            });
        }
    }
    let stride = required_arrays.count_ones() * 16;
    let vertex_bytes = count
        .checked_mul(stride as usize)
        .ok_or(VertexInputError::Budget)?;
    if index_count
        .checked_mul(4)
        .and_then(|n| n.checked_add(vertex_bytes))
        .is_none_or(|n| n > byte_budget)
    {
        return Err(VertexInputError::Budget);
    }
    let mut rows: Vec<(&VertexBinding, &[[u32; 4]])> = Vec::new();
    rows.try_reserve_exact(required_arrays.count_ones() as usize)
        .map_err(|_| VertexInputError::Allocation)?;
    for (slot, source) in slots.into_iter().enumerate() {
        if required_arrays & (1 << slot) == 0 {
            continue;
        }
        let source = source.ok_or(VertexInputError::MissingArray(slot as u8))?;
        let b = source.binding();
        if !(1..=4).contains(&b.components) {
            return Err(VertexInputError::Components(b.components));
        }
        if !(1..=7).contains(&b.format) {
            return Err(VertexInputError::Format(b.format));
        }
        let values = match source {
            Source::Auxiliary(a) => a.words.as_slice(),
            Source::PreparedColor(words) => words,
            Source::FailedAuxiliary(_) => {
                return Err(VertexInputError::FailedAuxiliary(slot as u8));
            }
            Source::Primary(_) if noninstanced.is_some() => {
                let prepared = noninstanced.expect("selected owning preparation");
                let p = &prepared.geometry;
                match b.semantic {
                    1 => p.positions.as_slice(),
                    2 if !p.normals.is_empty() => p.normals.as_slice(),
                    2 => {
                        return Err(VertexInputError::MissingSemantic {
                            slot: b.slot,
                            semantic: 2,
                        });
                    }
                    9 if !p.weights.is_empty() => p.weights.as_slice(),
                    9 => {
                        return Err(VertexInputError::MissingSemantic {
                            slot: b.slot,
                            semantic: 9,
                        });
                    }
                    3 => p
                        .tangents
                        .as_deref()
                        .ok_or(VertexInputError::MissingSemantic {
                            slot: b.slot,
                            semantic: b.semantic,
                        })?,
                    // Built-in layout 3 reads semantic 4 unchanged only on the
                    // ordinary, undeformed static path.
                    4 if prepared.static_source.as_ref().is_some_and(|s| {
                        matches!(prepared.mode, 8 | 10)
                            && !matches!(p.group, 0 | 3 | 4 | 6)
                            && s.config[6] & 15 == 0
                            && s.palette_controls == [0; 2]
                    }) =>
                    {
                        static_direction_four(draw, b.slot)?
                    }
                    _ => {
                        return Err(VertexInputError::UnpreparedPrimarySemantic {
                            slot: b.slot,
                            semantic: b.semantic,
                        });
                    }
                }
            }
            Source::Primary(_) if b.semantic != 1 => {
                // Raw input normals/tangents/weights are not yet the final
                // post-deformation semantic bank. Output packing alone cannot
                // certify that they have the value read by the actual shader.
                return Err(VertexInputError::UnpreparedPrimarySemantic {
                    slot: b.slot,
                    semantic: b.semantic,
                });
            }
            Source::Primary(_) if prepared.is_some() => positions,
            Source::Primary(_) => match &draw.attributes {
                Some(Ok(attributes)) => match attributes.first_semantic(b.semantic) {
                    Some(Ok(a)) => a.words.as_slice(),
                    Some(Err(_)) => {
                        return Err(VertexInputError::FailedSemantic {
                            slot: b.slot,
                            semantic: b.semantic,
                        });
                    }
                    None => {
                        return Err(VertexInputError::MissingSemantic {
                            slot: b.slot,
                            semantic: b.semantic,
                        });
                    }
                },
                Some(Err(_)) => {
                    return Err(VertexInputError::FailedSemantic {
                        slot: b.slot,
                        semantic: b.semantic,
                    });
                }
                // The separately qualified position decoder owns builtin
                // positions; it cannot stand in for another missing semantic.
                None if b.semantic == 1 => draw.positions.as_slice(),
                None => {
                    return Err(VertexInputError::MissingSemantic {
                        slot: b.slot,
                        semantic: b.semantic,
                    });
                }
            },
        };
        if values.len() != count {
            return Err(VertexInputError::VertexCount {
                slot: b.slot,
                expected: count,
                actual: values.len(),
            });
        }
        rows.push((b, values));
    }
    let mut attributes = Vec::new();
    attributes
        .try_reserve_exact(rows.len())
        .map_err(|_| VertexInputError::Allocation)?;
    for (i, (b, _)) in rows.iter().enumerate() {
        attributes.push(VertexAttribute {
            location: u32::from(b.slot),
            offset: (i * 16) as u32,
        });
    }
    debug_assert_eq!(vertex_bytes, count * stride as usize);
    let mut indices = Vec::new();
    indices
        .try_reserve_exact(index_count)
        .map_err(|_| VertexInputError::Allocation)?;
    indices.extend(source_indices());
    Ok(VertexInputPlan {
        array_mask,
        attributes,
        indices,
        rows: VertexRows {
            rows,
            count,
            stride,
        },
    })
}

// Keep original u16 and prepared u32 populations borrowed until the one native
// allocation; no narrowing or temporary index-copy budget escapes this boundary.
enum SourceIndices<'a> {
    Raw(std::slice::Iter<'a, u16>),
    Prepared(std::slice::Iter<'a, u32>),
}
impl Iterator for SourceIndices<'_> {
    type Item = u32;
    fn next(&mut self) -> Option<u32> {
        match self {
            Self::Raw(i) => i.next().copied().map(u32::from),
            Self::Prepared(i) => i.next().copied(),
        }
    }
}
