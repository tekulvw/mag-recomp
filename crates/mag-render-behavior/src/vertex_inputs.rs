//! Main vertex-input packing (`vertex-inputs.md`).
//!
//! Packing is split into validation ([`plan_vertex_inputs`]), which raises every error, and
//! writing ([`VertexPlan::write`]), which cannot fail, so the bytes may be written later or on
//! another thread and still be identical.

/// Which source attribute feeds a vertex-array slot.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct VertexBinding {
    pub declaration: u32,
    pub semantic: u32,
    pub slot: u32,
    pub format: u32,
    pub components: u32,
}

/// Decoded rows, one per vertex, as binary32 bit patterns.
pub type Rows<'a> = &'a [[u32; 4]];

/// An auxiliary binding whose rows were decoded.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct AuxiliaryAttribute<'a> {
    pub binding: VertexBinding,
    pub rows: Rows<'a>,
}

/// Auxiliary attributes: successful ones with rows, and bindings whose decode failed.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct AuxiliarySet<'a> {
    pub decoded: Vec<AuxiliaryAttribute<'a>>,
    pub failed: Vec<VertexBinding>,
}

/// A decoded bank in a primary or secondary attribute list; `rows: None` is a decode failure.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct SemanticBank<'a> {
    pub semantic: u32,
    pub rows: Option<Rows<'a>>,
}

/// An attribute list: absent, failed as a whole, or a list of banks.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum AttributeList<'a> {
    Absent,
    Failed,
    Banks(Vec<SemanticBank<'a>>),
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct DirectPreparation<'a> {
    pub auxiliary: AuxiliarySet<'a>,
    pub vertex_count: usize,
    pub indices: &'a [u32],
}

/// The layout plan of GPU-produced group-zero vertices.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct GpuLayoutPlan {
    pub array_mask: u16,
    pub stride: u32,
    /// One field slot per required slot, in ascending order of required slots.
    pub fields: Vec<u32>,
    /// The number of vertices the GPU produces. With GPU-produced vertices the prepared
    /// positions need not be held on the CPU, so the index check uses this count.
    pub vertex_count: usize,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct GroupZeroPreparation<'a> {
    pub positions: Rows<'a>,
    pub indices: &'a [u32],
    pub colours: Rows<'a>,
    pub auxiliary: AuxiliarySet<'a>,
    /// Set when the vertices are produced on the GPU; `Err` is a layout failure.
    pub gpu: Option<Result<GpuLayoutPlan, String>>,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct NonInstancedPreparation<'a> {
    pub positions: Rows<'a>,
    pub normals: Rows<'a>,
    pub weights: Rows<'a>,
    pub tangents: Option<Rows<'a>>,
    pub mesh_group: u32,
    pub geometry_mode: u32,
    /// The static-source facts; `None` when the preparation has none (a skinning result).
    pub static_facts: Option<StaticSourceFacts>,
}

/// The facts that decide whether a non-instanced preparation is an undeformed static source.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct StaticSourceFacts {
    /// Configuration-stream byte 6.
    pub configuration_byte6: u8,
    /// Partition `+0x48` and `+0x50`.
    pub palette_controls: [u32; 2],
}

impl<'a> NonInstancedPreparation<'a> {
    /// A successful skinning result as a non-instanced preparation: skinned positions, normals
    /// and tangents, no weights (skinning consumes them) and no static-source facts.
    pub fn skinned(
        positions: Rows<'a>,
        normals: Rows<'a>,
        tangents: Rows<'a>,
        geometry_mode: u32,
    ) -> Self {
        Self {
            positions,
            normals,
            weights: &[],
            tangents: Some(tangents),
            mesh_group: 11,
            geometry_mode,
            static_facts: None,
        }
    }
}

/// A preparation slot: absent, a recorded refusal, or present.
#[derive(Clone, Debug, PartialEq, Eq, Default)]
pub enum Prepared<T> {
    #[default]
    Absent,
    Refused,
    Present(T),
}

/// One prepared geometry draw.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct VertexDraw<'a> {
    /// Primary output bindings; `None` when the declaration could not be resolved.
    pub output_bindings: Option<Vec<VertexBinding>>,
    /// The draw's own auxiliary attributes; `None` when unavailable.
    pub auxiliary: Option<AuxiliarySet<'a>>,
    pub primary: AttributeList<'a>,
    pub secondary: AttributeList<'a>,
    pub positions: Rows<'a>,
    pub raw_indices: &'a [u16],
    /// A direct preparation is present or absent; a refused one never reaches packing.
    pub direct: Option<DirectPreparation<'a>>,
    pub group_zero: Prepared<GroupZeroPreparation<'a>>,
    pub non_instanced: Prepared<NonInstancedPreparation<'a>>,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum VertexInputError {
    SourceBindings,
    GroupZeroPreparation(String),
    NonInstancedPreparation,
    ConflictingPreparation,
    Slot(u32),
    MissingArray(u32),
    UnpreparedPrimarySemantic {
        slot: u32,
        semantic: u32,
    },
    MissingSemantic {
        slot: u32,
        semantic: u32,
    },
    FailedSemantic {
        slot: u32,
        semantic: u32,
    },
    FailedAuxiliary(u32),
    Components(u32),
    Format(u32),
    VertexCount {
        slot: u32,
        expected: usize,
        actual: usize,
    },
    Index {
        index: u32,
        count: usize,
    },
    Budget,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum SourceKind {
    Auxiliary,
    FailedAuxiliary,
    Colour,
    Output,
}

#[derive(Clone, Copy, Debug)]
struct Source<'a> {
    binding: VertexBinding,
    kind: SourceKind,
    rows: Option<Rows<'a>>,
}

/// One required slot's validated row source.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct Column<'a> {
    rows: Rows<'a>,
    lanes: usize,
}

/// A validated packing; [`VertexPlan::write`] produces the bytes.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct VertexPlan<'a> {
    pub array_mask: u16,
    pub stride: u32,
    /// `(location, offset)` per required slot.
    pub attributes: Vec<(u32, u32)>,
    pub indices: Vec<u32>,
    pub count: usize,
    /// GPU-produced group-zero layout; vertex bytes are then empty.
    pub produced: Option<GpuLayoutPlan>,
    columns: Vec<Column<'a>>,
}

/// The packed native vertex inputs.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct NativeVertexInputs {
    pub array_mask: u16,
    pub stride: u32,
    pub attributes: Vec<(u32, u32)>,
    pub vertices: Vec<u8>,
    pub indices: Vec<u32>,
    pub produced: Option<GpuLayoutPlan>,
}

const LANE_DEFAULTS: [u32; 4] = [0, 0, 0, 0x3f80_0000];

impl VertexPlan<'_> {
    /// Writes the interleaved bytes. Cannot fail: every error was raised during validation.
    pub fn write(&self) -> NativeVertexInputs {
        let mut vertices = Vec::new();
        if self.produced.is_none() {
            vertices.reserve(self.count * self.stride as usize);
            for vertex in 0..self.count {
                for column in &self.columns {
                    let row = column.rows[vertex];
                    for lane in 0..4 {
                        let word = if lane < column.lanes {
                            row[lane]
                        } else {
                            LANE_DEFAULTS[lane]
                        };
                        vertices.extend_from_slice(&word.to_le_bytes());
                    }
                }
            }
        }
        NativeVertexInputs {
            array_mask: self.array_mask,
            stride: self.stride,
            attributes: self.attributes.clone(),
            vertices,
            indices: self.indices.clone(),
            produced: self.produced.clone(),
        }
    }
}

fn place<'a>(
    table: &mut [Option<Source<'a>>; 16],
    source: Source<'a>,
) -> Result<(), VertexInputError> {
    let slot = source.binding.slot;
    if slot >= 16 {
        return Err(VertexInputError::Slot(slot));
    }
    let entry = &mut table[slot as usize];
    if entry.is_none_or(|occupant| occupant.binding.declaration <= source.binding.declaration) {
        *entry = Some(source);
    }
    Ok(())
}

/// The effective vertex-array mask of a draw.
pub fn effective_array_mask(draw: &VertexDraw<'_>) -> Result<u16, VertexInputError> {
    Ok(build_table(draw)?.mask)
}

struct Table<'a> {
    sources: [Option<Source<'a>>; 16],
    mask: u16,
    gpu: Option<GpuLayoutPlan>,
}

fn build_table<'a>(draw: &VertexDraw<'a>) -> Result<Table<'a>, VertexInputError> {
    let present = |refused: bool, present: bool| refused || present;
    let direct = draw.direct.is_some();
    let zero = present(
        matches!(draw.group_zero, Prepared::Refused),
        matches!(draw.group_zero, Prepared::Present(_)),
    );
    let non = present(
        matches!(draw.non_instanced, Prepared::Refused),
        matches!(draw.non_instanced, Prepared::Present(_)),
    );
    if (direct && (zero || non)) || (zero && non) {
        return Err(VertexInputError::ConflictingPreparation);
    }
    if matches!(draw.group_zero, Prepared::Refused) {
        return Err(VertexInputError::GroupZeroPreparation("refused".into()));
    }
    if matches!(draw.non_instanced, Prepared::Refused) {
        return Err(VertexInputError::NonInstancedPreparation);
    }
    if draw.output_bindings.is_none() && !direct {
        return Err(VertexInputError::SourceBindings);
    }
    if let Prepared::Present(prep) = &draw.group_zero
        && let Some(gpu) = &prep.gpu
    {
        let plan = gpu
            .clone()
            .map_err(VertexInputError::GroupZeroPreparation)?;
        return Ok(Table {
            sources: [None; 16],
            mask: plan.array_mask,
            gpu: Some(plan),
        });
    }

    let mut table: [Option<Source<'a>>; 16] = [None; 16];
    if let Prepared::Present(prep) = &draw.group_zero {
        table[12] = Some(Source {
            binding: VertexBinding {
                declaration: 0,
                semantic: 0,
                slot: 12,
                format: 2,
                components: 4,
            },
            kind: SourceKind::Colour,
            rows: Some(prep.colours),
        });
    }
    let auxiliary = match (&draw.direct, &draw.group_zero) {
        (Some(prep), _) => &prep.auxiliary,
        (_, Prepared::Present(prep)) => &prep.auxiliary,
        _ => draw
            .auxiliary
            .as_ref()
            .ok_or(VertexInputError::SourceBindings)?,
    };
    for attribute in &auxiliary.decoded {
        place(
            &mut table,
            Source {
                binding: attribute.binding,
                kind: SourceKind::Auxiliary,
                rows: Some(attribute.rows),
            },
        )?;
    }
    for binding in &auxiliary.failed {
        place(
            &mut table,
            Source {
                binding: *binding,
                kind: SourceKind::FailedAuxiliary,
                rows: None,
            },
        )?;
    }
    if !direct {
        for binding in draw.output_bindings.as_deref().unwrap_or_default() {
            if binding.slot >= 16 {
                return Err(VertexInputError::Slot(binding.slot));
            }
            table[binding.slot as usize] = Some(Source {
                binding: *binding,
                kind: SourceKind::Output,
                rows: None,
            });
        }
    }
    let mask = (0..16)
        .filter(|slot| table[*slot].is_some_and(|s| s.binding.components != 0))
        .fold(0u16, |mask, slot| mask | (1 << slot));
    Ok(Table {
        sources: table,
        mask,
        gpu: None,
    })
}

fn first_bank<'a>(list: &AttributeList<'a>, semantic: u32) -> Option<SemanticBank<'a>> {
    match list {
        AttributeList::Banks(banks) => banks.iter().find(|bank| bank.semantic == semantic).copied(),
        _ => None,
    }
}

fn output_rows<'a>(
    draw: &VertexDraw<'a>,
    binding: &VertexBinding,
) -> Result<Rows<'a>, VertexInputError> {
    let (slot, semantic) = (binding.slot, binding.semantic);
    let missing = VertexInputError::MissingSemantic { slot, semantic };
    let failed = VertexInputError::FailedSemantic { slot, semantic };
    let unprepared = VertexInputError::UnpreparedPrimarySemantic { slot, semantic };
    if let Prepared::Present(prep) = &draw.non_instanced {
        return match semantic {
            1 => Ok(prep.positions),
            2 if !prep.normals.is_empty() => Ok(prep.normals),
            9 if !prep.weights.is_empty() => Ok(prep.weights),
            3 => prep.tangents.ok_or(missing),
            2 | 9 => Err(missing),
            4 => {
                let static_source = matches!(prep.geometry_mode, 8 | 10)
                    && !matches!(prep.mesh_group, 0 | 3 | 4 | 6)
                    && prep.static_facts.is_some_and(|facts| {
                        facts.configuration_byte6 & 0xf == 0 && facts.palette_controls == [0, 0]
                    });
                if !static_source {
                    return Err(unprepared);
                }
                // The primary list first, then the secondary list; a secondary list that failed
                // as a whole fails the lookup (`geometry-streams.md`).
                let bank = match first_bank(&draw.primary, 4) {
                    Some(bank) => bank,
                    None if draw.secondary == AttributeList::Failed => return Err(failed),
                    None => first_bank(&draw.secondary, 4).ok_or(missing)?,
                };
                let rows = bank.rows.ok_or(failed.clone())?;
                let in_domain = rows.iter().all(|row| {
                    row[..3]
                        .iter()
                        .all(|w| f32::from_bits(*w).is_finite() && f32::from_bits(*w).abs() <= 1.0)
                });
                if !in_domain {
                    return Err(failed);
                }
                Ok(rows)
            }
            _ => Err(unprepared),
        };
    }
    if semantic != 1 {
        return Err(unprepared);
    }
    if let Prepared::Present(prep) = &draw.group_zero {
        return Ok(prep.positions);
    }
    match &draw.primary {
        AttributeList::Absent => Ok(draw.positions),
        AttributeList::Failed => Err(failed),
        AttributeList::Banks(_) => first_bank(&draw.primary, 1)
            .ok_or(missing)?
            .rows
            .ok_or(failed),
    }
}

/// Validates one draw's vertex inputs for the `required` slots within `budget` bytes.
pub fn plan_vertex_inputs<'a>(
    draw: &VertexDraw<'a>,
    required: u16,
    budget: u64,
) -> Result<VertexPlan<'a>, VertexInputError> {
    let table = build_table(draw)?;
    if let Some(slot) =
        (0..16).find(|slot| required & (1 << slot) != 0 && table.mask & (1 << slot) == 0)
    {
        return Err(VertexInputError::MissingArray(slot));
    }
    let required_slots: Vec<u32> = (0..16).filter(|slot| required & (1 << slot) != 0).collect();

    if let Some(plan) = table.gpu {
        let Prepared::Present(prep) = &draw.group_zero else {
            unreachable!("GPU plans come from group zero")
        };
        if plan.fields.len() != required_slots.len() {
            return Err(VertexInputError::GroupZeroPreparation(
                "layout fields do not match required slots".into(),
            ));
        }
        let count = plan.vertex_count;
        if let Some(&index) = prep.indices.iter().find(|i| **i as usize >= count) {
            return Err(VertexInputError::Index { index, count });
        }
        return Ok(VertexPlan {
            array_mask: plan.array_mask,
            stride: plan.stride,
            attributes: plan
                .fields
                .iter()
                .enumerate()
                .map(|(k, slot)| (*slot, 16 * k as u32))
                .collect(),
            indices: prep.indices.to_vec(),
            count,
            produced: Some(plan),
            columns: Vec::new(),
        });
    }

    let count = match (&draw.direct, &draw.non_instanced, &draw.group_zero) {
        (Some(direct), _, _) => direct.vertex_count,
        (_, Prepared::Present(non), _) => non.positions.len(),
        (_, _, Prepared::Present(zero)) => zero.positions.len(),
        _ => draw.positions.len(),
    };
    let stride = 16 * required_slots.len() as u32;
    let mut columns = Vec::with_capacity(required_slots.len());
    for &slot in &required_slots {
        let source = table.sources[slot as usize].expect("required slots are enabled");
        let binding = source.binding;
        if !(1..=4).contains(&binding.components) {
            return Err(VertexInputError::Components(binding.components));
        }
        if !(1..=7).contains(&binding.format) {
            return Err(VertexInputError::Format(binding.format));
        }
        let rows = match source.kind {
            SourceKind::Auxiliary | SourceKind::Colour => source.rows.expect("decoded rows"),
            SourceKind::FailedAuxiliary => return Err(VertexInputError::FailedAuxiliary(slot)),
            SourceKind::Output => output_rows(draw, &binding)?,
        };
        if rows.len() != count {
            return Err(VertexInputError::VertexCount {
                slot,
                expected: count,
                actual: rows.len(),
            });
        }
        let lanes = if binding.format == 6 {
            3
        } else {
            binding.components as usize
        };
        columns.push(Column { rows, lanes });
    }

    let indices: Vec<u32> = match (&draw.direct, &draw.group_zero) {
        (Some(direct), _) => direct.indices.to_vec(),
        (_, Prepared::Present(zero)) => zero.indices.to_vec(),
        _ => draw.raw_indices.iter().map(|i| *i as u32).collect(),
    };
    if let Some(&index) = indices.iter().find(|i| **i as usize >= count) {
        return Err(VertexInputError::Index { index, count });
    }
    let bytes = (count as u64)
        .checked_mul(stride as u64)
        .and_then(|v| v.checked_add(4u64.checked_mul(indices.len() as u64)?));
    if bytes.is_none_or(|bytes| bytes > budget) {
        return Err(VertexInputError::Budget);
    }
    Ok(VertexPlan {
        array_mask: table.mask,
        stride,
        attributes: required_slots
            .iter()
            .enumerate()
            .map(|(k, slot)| (*slot, 16 * k as u32))
            .collect(),
        indices,
        count,
        produced: None,
        columns,
    })
}

/// Validates and writes in one step.
pub fn pack_vertex_inputs(
    draw: &VertexDraw<'_>,
    required: u16,
    budget: u64,
) -> Result<NativeVertexInputs, VertexInputError> {
    Ok(plan_vertex_inputs(draw, required, budget)?.write())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn row(values: [f32; 4]) -> [u32; 4] {
        values.map(f32::to_bits)
    }

    fn binding(
        declaration: u32,
        semantic: u32,
        slot: u32,
        format: u32,
        components: u32,
    ) -> VertexBinding {
        VertexBinding {
            declaration,
            semantic,
            slot,
            format,
            components,
        }
    }

    const POSITIONS: [[u32; 4]; 3] = [
        [0xbf80_0000, 0xbf80_0000, 0, 0x1234_5678],
        [0x3f80_0000, 0xbf80_0000, 0, 0x1234_5678],
        [0xbf80_0000, 0x3f80_0000, 0, 0x1234_5678],
    ];

    fn texcoords() -> [[u32; 4]; 3] {
        [
            row([0.0, 1.0, 0.0, 0.0]),
            row([1.0, 1.0, 0.0, 0.0]),
            row([0.0, 0.0, 0.0, 0.0]),
        ]
    }

    fn base<'a>(texcoords: Rows<'a>) -> VertexDraw<'a> {
        VertexDraw {
            output_bindings: Some(vec![binding(0, 1, 0, 2, 3)]),
            auxiliary: Some(AuxiliarySet {
                decoded: vec![AuxiliaryAttribute {
                    binding: binding(4, 7, 8, 2, 2),
                    rows: texcoords,
                }],
                failed: Vec::new(),
            }),
            primary: AttributeList::Absent,
            secondary: AttributeList::Absent,
            positions: &POSITIONS,
            raw_indices: &[0, 1, 2],
            direct: None,
            group_zero: Prepared::Absent,
            non_instanced: Prepared::Absent,
        }
    }

    fn words(bytes: &[u8]) -> Vec<u32> {
        bytes
            .chunks(4)
            .map(|c| u32::from_le_bytes([c[0], c[1], c[2], c[3]]))
            .collect()
    }

    #[test]
    fn v1_position_and_texcoord() {
        let t = texcoords();
        let out = pack_vertex_inputs(&base(&t), 0x0101, 1 << 20).unwrap();
        assert_eq!((out.array_mask, out.stride), (0x0101, 32));
        assert_eq!(out.attributes, vec![(0, 0), (8, 16)]);
        assert_eq!(
            words(&out.vertices[32..64]),
            vec![
                0x3f80_0000,
                0xbf80_0000,
                0,
                0x3f80_0000,
                0x3f80_0000,
                0x3f80_0000,
                0,
                0x3f80_0000
            ]
        );
        assert_eq!(out.indices, vec![0, 1, 2]);
    }

    #[test]
    fn v2_default_lanes() {
        let mut t = texcoords();
        t[0][2] = 0x1234_5678;
        t[0][3] = 0x1234_5678;
        let out = pack_vertex_inputs(&base(&t), 0x0101, 1 << 20).unwrap();
        assert_eq!(words(&out.vertices[24..32]), vec![0, 0x3f80_0000]);
    }

    #[test]
    fn v3_declaration_order() {
        let t = texcoords();
        let other = [row([9.0; 4]); 3];
        let mut draw = base(&t);
        let aux = draw.auxiliary.as_mut().unwrap();
        aux.decoded.push(AuxiliaryAttribute {
            binding: binding(2, 7, 5, 2, 4),
            rows: &other,
        });
        aux.decoded.push(AuxiliaryAttribute {
            binding: binding(6, 7, 5, 2, 4),
            rows: &t,
        });
        let out = pack_vertex_inputs(&draw, 0x0020, 1 << 20).unwrap();
        assert_eq!(words(&out.vertices[..16]), t[0].to_vec());
        draw.auxiliary
            .as_mut()
            .unwrap()
            .failed
            .push(binding(6, 7, 5, 2, 4));
        assert_eq!(
            pack_vertex_inputs(&draw, 0x0020, 1 << 20),
            Err(VertexInputError::FailedAuxiliary(5))
        );
        let out = pack_vertex_inputs(&draw, 0x0001, 1 << 20).unwrap();
        assert_ne!(out.array_mask & (1 << 5), 0);
    }

    #[test]
    fn v4_output_overrides_auxiliary() {
        let t = texcoords();
        let mut draw = base(&t);
        draw.auxiliary
            .as_mut()
            .unwrap()
            .decoded
            .push(AuxiliaryAttribute {
                binding: binding(9, 7, 0, 2, 4),
                rows: &t,
            });
        let out = pack_vertex_inputs(&draw, 0x0001, 1 << 20).unwrap();
        assert_eq!(words(&out.vertices[..12]), POSITIONS[0][..3].to_vec());
    }

    #[test]
    fn v5_disabled_component_count() {
        let t = texcoords();
        let mut draw = base(&t);
        draw.auxiliary
            .as_mut()
            .unwrap()
            .decoded
            .push(AuxiliaryAttribute {
                binding: binding(1, 7, 3, 2, 0),
                rows: &t,
            });
        assert_eq!(effective_array_mask(&draw).unwrap() & (1 << 3), 0);
        assert_eq!(
            pack_vertex_inputs(&draw, 0x0009, 1 << 20),
            Err(VertexInputError::MissingArray(3))
        );
    }

    #[test]
    fn v6_cmp_format() {
        let t = texcoords();
        let cmp = [row([0.5, -0.5, 0.25, 9.0]); 3];
        let mut draw = base(&t);
        draw.auxiliary
            .as_mut()
            .unwrap()
            .decoded
            .push(AuxiliaryAttribute {
                binding: binding(1, 7, 2, 6, 1),
                rows: &cmp,
            });
        let out = pack_vertex_inputs(&draw, 0x0004, 1 << 20).unwrap();
        assert_eq!(
            words(&out.vertices[..16]),
            row([0.5, -0.5, 0.25, 1.0]).to_vec()
        );
    }

    #[test]
    fn v7_index_range() {
        let t = texcoords();
        let mut draw = base(&t);
        draw.raw_indices = &[0, 3, 1];
        assert_eq!(
            pack_vertex_inputs(&draw, 0x0001, 1 << 20),
            Err(VertexInputError::Index { index: 3, count: 3 })
        );
    }

    #[test]
    fn v8_budget() {
        let positions = vec![row([0.0; 4]); 100];
        let texcoords = vec![row([0.0; 4]); 100];
        let indices = vec![0u16; 300];
        let mut draw = base(&texcoords);
        draw.positions = &positions;
        draw.raw_indices = &indices;
        assert_eq!(
            pack_vertex_inputs(&draw, 0x0101, 4399),
            Err(VertexInputError::Budget)
        );
        assert!(pack_vertex_inputs(&draw, 0x0101, 4400).is_ok());
    }

    fn non_instanced<'a>(positions: Rows<'a>, normals: Rows<'a>) -> NonInstancedPreparation<'a> {
        NonInstancedPreparation {
            positions,
            normals,
            weights: &[],
            tangents: None,
            mesh_group: 5,
            geometry_mode: 8,
            static_facts: Some(StaticSourceFacts {
                configuration_byte6: 0,
                palette_controls: [0, 0],
            }),
        }
    }

    #[test]
    fn v9_v10_normals() {
        let t = texcoords();
        let mut draw = base(&t);
        draw.output_bindings
            .as_mut()
            .unwrap()
            .push(binding(1, 2, 1, 2, 3));
        assert_eq!(
            pack_vertex_inputs(&draw, 0x0003, 1 << 20),
            Err(VertexInputError::UnpreparedPrimarySemantic {
                slot: 1,
                semantic: 2
            })
        );
        let normals = [row([0.0, 0.0, 1.0, 0.0]); 3];
        draw.non_instanced = Prepared::Present(non_instanced(&POSITIONS, &normals));
        let out = pack_vertex_inputs(&draw, 0x0003, 1 << 20).unwrap();
        assert_eq!(words(&out.vertices[16..28]), normals[0][..3].to_vec());
        // Weights are missing in this preparation.
        draw.output_bindings
            .as_mut()
            .unwrap()
            .push(binding(2, 9, 4, 2, 4));
        assert_eq!(
            pack_vertex_inputs(&draw, 0x0011, 1 << 20),
            Err(VertexInputError::MissingSemantic {
                slot: 4,
                semantic: 9
            })
        );
    }

    #[test]
    fn v11_secondary_direction_domain() {
        let t = texcoords();
        let good = [[0.6f32.to_bits(), 0.8f32.to_bits(), 0, f32::NAN.to_bits()]; 3];
        let bad = [row([1.5, 0.0, 0.0, 0.0]); 3];
        for (rows, ok) in [(&good, true), (&bad, false)] {
            let mut draw = base(&t);
            draw.output_bindings
                .as_mut()
                .unwrap()
                .push(binding(1, 4, 6, 2, 4));
            draw.primary = AttributeList::Banks(vec![SemanticBank {
                semantic: 4,
                rows: Some(rows),
            }]);
            draw.non_instanced = Prepared::Present(non_instanced(&POSITIONS, &[]));
            let result = pack_vertex_inputs(&draw, 0x0041, 1 << 20);
            if ok {
                assert!(result.is_ok());
            } else {
                assert_eq!(
                    result,
                    Err(VertexInputError::FailedSemantic {
                        slot: 6,
                        semantic: 4
                    })
                );
            }
        }
    }

    #[test]
    fn v14_skinned_preparation() {
        let normals = [row([0.0, 1.0, 0.0, 0.0]); 3];
        let tangents = [row([1.0, 0.0, 0.0, -1.0]); 3];
        let mut draw = base(&[]);
        draw.auxiliary = Some(AuxiliarySet::default());
        draw.output_bindings = Some(vec![
            binding(0, 1, 0, 2, 3),
            binding(1, 2, 2, 2, 3),
            binding(2, 3, 6, 2, 4),
        ]);
        draw.non_instanced = Prepared::Present(NonInstancedPreparation::skinned(
            &POSITIONS, &normals, &tangents, 8,
        ));
        assert_eq!(effective_array_mask(&draw), Ok(0x45));
        let out = pack_vertex_inputs(&draw, 0x45, 1 << 20).unwrap();
        assert_eq!(out.attributes, vec![(0, 0), (2, 16), (6, 32)]);
        assert_eq!(words(&out.vertices[16..28]), normals[0][..3].to_vec());
        assert_eq!(words(&out.vertices[32..48]), tangents[0].to_vec());
        // Weights are consumed by skinning; semantic 4 has no static-source facts.
        for (semantic, error) in [
            (
                9,
                VertexInputError::MissingSemantic {
                    slot: 4,
                    semantic: 9,
                },
            ),
            (
                4,
                VertexInputError::UnpreparedPrimarySemantic {
                    slot: 4,
                    semantic: 4,
                },
            ),
        ] {
            let mut d = draw.clone();
            d.output_bindings
                .as_mut()
                .unwrap()
                .push(binding(3, semantic, 4, 2, 4));
            assert_eq!(pack_vertex_inputs(&d, 0x55, 1 << 20), Err(error));
        }
    }

    #[test]
    fn semantic_four_lookup_fails_on_a_failed_secondary_list() {
        let t = texcoords();
        let mut draw = base(&t);
        draw.output_bindings
            .as_mut()
            .unwrap()
            .push(binding(1, 4, 6, 2, 4));
        draw.non_instanced = Prepared::Present(non_instanced(&POSITIONS, &[]));
        draw.secondary = AttributeList::Failed;
        assert_eq!(
            pack_vertex_inputs(&draw, 0x0041, 1 << 20),
            Err(VertexInputError::FailedSemantic {
                slot: 6,
                semantic: 4
            })
        );
        // A primary bank still wins over the failed secondary list.
        let good = [[0.6f32.to_bits(), 0.8f32.to_bits(), 0, 0]; 3];
        draw.primary = AttributeList::Banks(vec![SemanticBank {
            semantic: 4,
            rows: Some(&good),
        }]);
        assert!(pack_vertex_inputs(&draw, 0x0041, 1 << 20).is_ok());
    }

    #[test]
    fn semantic_four_missing_or_unprepared_and_slot_twelve_replacement() {
        let t = texcoords();
        let mut draw = base(&t);
        draw.output_bindings
            .as_mut()
            .unwrap()
            .push(binding(1, 4, 6, 2, 4));
        draw.non_instanced = Prepared::Present(non_instanced(&POSITIONS, &[]));
        // Static-source conditions hold but no list has a semantic-4 bank.
        assert_eq!(
            pack_vertex_inputs(&draw, 0x0041, 1 << 20),
            Err(VertexInputError::MissingSemantic {
                slot: 6,
                semantic: 4
            })
        );
        // Conditions unmet (mesh group 3).
        if let Prepared::Present(prep) = &mut draw.non_instanced {
            prep.mesh_group = 3;
        }
        assert_eq!(
            pack_vertex_inputs(&draw, 0x0041, 1 << 20),
            Err(VertexInputError::UnpreparedPrimarySemantic {
                slot: 6,
                semantic: 4
            })
        );
        // Any auxiliary binding to slot 12 replaces the prepared group-zero colours.
        let colours = [row([0.5; 4]); 3];
        let other = [row([0.25; 4]); 3];
        let mut draw = base(&t);
        draw.auxiliary
            .as_mut()
            .unwrap()
            .decoded
            .push(AuxiliaryAttribute {
                binding: binding(0, 7, 12, 2, 4),
                rows: &other,
            });
        draw.group_zero = Prepared::Present(GroupZeroPreparation {
            positions: &POSITIONS,
            indices: &[0, 1, 2],
            colours: &colours,
            auxiliary: draw.auxiliary.clone().unwrap(),
            gpu: None,
        });
        let out = pack_vertex_inputs(&draw, 0x1001, 1 << 20).unwrap();
        assert_eq!(words(&out.vertices[16..32]), other[0].to_vec());
    }

    #[test]
    fn v12_conflict_and_refusals() {
        let t = texcoords();
        let mut draw = base(&t);
        draw.direct = Some(DirectPreparation {
            auxiliary: AuxiliarySet::default(),
            vertex_count: 3,
            indices: &[0],
        });
        draw.non_instanced = Prepared::Present(non_instanced(&POSITIONS, &[]));
        assert_eq!(
            pack_vertex_inputs(&draw, 0, 1 << 20),
            Err(VertexInputError::ConflictingPreparation)
        );
        let mut draw = base(&t);
        draw.non_instanced = Prepared::Refused;
        assert_eq!(
            pack_vertex_inputs(&draw, 0, 1 << 20),
            Err(VertexInputError::NonInstancedPreparation)
        );
        let mut draw = base(&t);
        draw.output_bindings = None;
        assert_eq!(
            pack_vertex_inputs(&draw, 0, 1 << 20),
            Err(VertexInputError::SourceBindings)
        );
        let mut draw = base(&t);
        draw.auxiliary = None;
        assert_eq!(
            pack_vertex_inputs(&draw, 0, 1 << 20),
            Err(VertexInputError::SourceBindings)
        );
        let mut draw = base(&t);
        draw.auxiliary
            .as_mut()
            .unwrap()
            .failed
            .push(binding(1, 7, 16, 2, 4));
        assert_eq!(
            pack_vertex_inputs(&draw, 0, 1 << 20),
            Err(VertexInputError::Slot(16))
        );
    }

    #[test]
    fn v13_group_zero_colour_and_gpu() {
        let t = texcoords();
        let colours = [row([0.5; 4]); 3];
        let mut draw = base(&t);
        draw.group_zero = Prepared::Present(GroupZeroPreparation {
            positions: &POSITIONS,
            indices: &[2, 1, 0],
            colours: &colours,
            auxiliary: AuxiliarySet::default(),
            gpu: None,
        });
        let out = pack_vertex_inputs(&draw, 0x1001, 1 << 20).unwrap();
        assert_ne!(out.array_mask & (1 << 12), 0);
        assert_eq!(words(&out.vertices[16..32]), colours[0].to_vec());
        assert_eq!(out.indices, vec![2, 1, 0]);
        if let Prepared::Present(prep) = &mut draw.group_zero {
            prep.gpu = Some(Ok(GpuLayoutPlan {
                array_mask: 0x1001,
                stride: 48,
                fields: vec![0, 12],
                vertex_count: 3,
            }));
        }
        let out = pack_vertex_inputs(&draw, 0x1001, 1 << 20).unwrap();
        assert!(out.vertices.is_empty());
        assert_eq!(out.attributes, vec![(0, 0), (12, 16)]);
        assert_eq!(out.stride, 48);
        // The index check uses the GPU vertex count, not the CPU positions.
        if let Prepared::Present(prep) = &mut draw.group_zero {
            prep.positions = &[];
            prep.indices = &[0, 1, 2];
        }
        assert!(pack_vertex_inputs(&draw, 0x1001, 1 << 20).is_ok());
        if let Prepared::Present(prep) = &mut draw.group_zero {
            prep.indices = &[0, 1, 3];
        }
        assert_eq!(
            pack_vertex_inputs(&draw, 0x1001, 1 << 20),
            Err(VertexInputError::Index { index: 3, count: 3 })
        );
        if let Prepared::Present(prep) = &mut draw.group_zero {
            prep.gpu = Some(Err("layout".into()));
        }
        assert_eq!(
            pack_vertex_inputs(&draw, 0x1001, 1 << 20),
            Err(VertexInputError::GroupZeroPreparation("layout".into()))
        );
    }

    #[test]
    fn zero_required_and_counts() {
        let t = texcoords();
        let out = pack_vertex_inputs(&base(&t), 0, 1 << 20).unwrap();
        assert_eq!(
            (out.stride, out.attributes.len(), out.vertices.len()),
            (0, 0, 0)
        );
        assert_eq!(out.indices, vec![0, 1, 2]);
        let short = [row([0.0; 4]); 2];
        let draw = base(&short);
        assert_eq!(
            pack_vertex_inputs(&draw, 0x0100, 1 << 20),
            Err(VertexInputError::VertexCount {
                slot: 8,
                expected: 3,
                actual: 2
            })
        );
        let mut draw = base(&t);
        draw.output_bindings = Some(vec![binding(0, 1, 0, 8, 3)]);
        assert_eq!(
            pack_vertex_inputs(&draw, 0x0001, 1 << 20),
            Err(VertexInputError::Format(8))
        );
        let mut draw = base(&t);
        draw.primary = AttributeList::Failed;
        assert_eq!(
            pack_vertex_inputs(&draw, 0x0001, 1 << 20),
            Err(VertexInputError::FailedSemantic {
                slot: 0,
                semantic: 1
            })
        );
        draw.primary = AttributeList::Banks(vec![]);
        assert_eq!(
            pack_vertex_inputs(&draw, 0x0001, 1 << 20),
            Err(VertexInputError::MissingSemantic {
                slot: 0,
                semantic: 1
            })
        );
    }

    /// Property: deferred writing is byte-identical to immediate writing, and every packed lane
    /// is a source word or a stated default.
    #[test]
    fn property_deferred_equals_immediate() {
        let mut rng = crate::testutil::Rng::new(71);
        for _ in 0..500 {
            let count = 1 + rng.below(20) as usize;
            let banks: Vec<Vec<[u32; 4]>> = (0..4)
                .map(|_| {
                    (0..count)
                        .map(|_| std::array::from_fn(|_| rng.next_u32()))
                        .collect()
                })
                .collect();
            let mut draw = base(&banks[0]);
            draw.positions = &banks[1];
            draw.raw_indices = &[];
            let aux = draw.auxiliary.as_mut().unwrap();
            aux.decoded.push(AuxiliaryAttribute {
                binding: binding(5, 7, 3, 1 + rng.below(7), 1 + rng.below(4)),
                rows: &banks[2],
            });
            aux.decoded.push(AuxiliaryAttribute {
                binding: binding(6, 7, 9, 2, 4),
                rows: &banks[3],
            });
            let required = 0x0209 | if rng.below(2) == 0 { 0x0100 } else { 0 };
            let plan = plan_vertex_inputs(&draw, required, 1 << 30).unwrap();
            let deferred = std::thread::scope(|scope| scope.spawn(|| plan.write()).join().unwrap());
            assert_eq!(
                deferred,
                pack_vertex_inputs(&draw, required, 1 << 30).unwrap()
            );
            let w = words(&deferred.vertices);
            assert_eq!(w.len(), count * deferred.stride as usize / 4);
        }
    }
}
