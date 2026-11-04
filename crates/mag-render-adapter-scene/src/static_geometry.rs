//! Canonical ordinary static banks after the source control-flow no-op gates.
//! Output precision is metadata; integer/rescaling variants stay unqualified.
use crate::{deformation::PreparedNoninstancedGeometry, geometry_bindings::VertexBinding};
#[derive(Debug, PartialEq, Eq)]
pub enum StaticGeometryError {
    DedicatedDeformation(u8),
    Mode,
    Optional,
    Palette,
    DirectionTransform,
    MissingSemantic(u8),
    Population(u8),
    OutputFormat { semantic: u8, format: u8 },
    OutputRange { semantic: u8, format: u8 },
    Nonfinite(u32),
    Budget,
    Allocation,
}
pub struct StaticGeometryInput<'a> {
    pub group: u8,
    pub mode: u32,
    pub workers: u32,
    pub optional_count: u8,
    pub palette_bytes: [usize; 2],
    pub skinning_control: u8,
    pub positions: &'a [[u32; 4]],
    pub normals: Option<&'a [[u32; 4]]>,
    pub tangents: Option<&'a [[u32; 4]]>,
    pub output: &'a [VertexBinding],
}
fn copy(bank: &[[u32; 4]]) -> Result<Vec<[u32; 4]>, StaticGeometryError> {
    let mut owned = Vec::new();
    owned
        .try_reserve_exact(bank.len())
        .map_err(|_| StaticGeometryError::Allocation)?;
    owned.extend_from_slice(bank);
    Ok(owned)
}
/// The source primitive0/3/4/6 branches perform other preparation. Ordinary
/// mode8/10 with no optional inputs, no palette and no direction-control nibble
/// leaves these decoded banks unchanged before output precision packing.
pub fn prepare_static_geometry(
    i: StaticGeometryInput<'_>,
    budget: usize,
) -> Result<PreparedNoninstancedGeometry, StaticGeometryError> {
    use StaticGeometryError as E;
    if i.group > 15 || matches!(i.group, 0 | 3 | 4 | 6) {
        return Err(E::DedicatedDeformation(i.group));
    }
    if !matches!(i.mode, 8 | 10) || i.workers != 2 {
        return Err(E::Mode);
    }
    if i.optional_count != 0 {
        return Err(E::Optional);
    }
    if i.palette_bytes != [0; 2] {
        return Err(E::Palette);
    }
    if i.skinning_control & 15 != 0 {
        return Err(E::DirectionTransform);
    }
    let count = i.positions.len();
    for (semantic, bank) in [(2, i.normals), (3, i.tangents)] {
        if bank.is_some_and(|b| b.len() != count) {
            return Err(E::Population(semantic));
        }
    }
    count
        .checked_mul(
            16 * (1 + usize::from(i.normals.is_some()) + usize::from(i.tangents.is_some())),
        )
        .filter(|&n| n <= budget)
        .ok_or(E::Budget)?;
    for &v in i.positions.iter().flatten() {
        if !f32::from_bits(v).is_finite() {
            return Err(E::Nonfinite(v));
        }
    }
    for binding in i.output.iter().filter(|b| matches!(b.semantic, 2 | 3)) {
        let bank = if binding.semantic == 2 {
            i.normals
        } else {
            i.tangents
        }
        .ok_or(E::MissingSemantic(binding.semantic))?;
        if !matches!(binding.format, 1 | 2 | 3 | 6) {
            return Err(E::OutputFormat {
                semantic: binding.semantic,
                format: binding.format,
            });
        }
        let components = if binding.format == 6 {
            3
        } else {
            usize::from(binding.components)
        };
        if !(1..=4).contains(&components) {
            return Err(E::OutputFormat {
                semantic: binding.semantic,
                format: binding.format,
            });
        }
        for row in bank {
            for &v in &row[..components] {
                if !f32::from_bits(v).is_finite() {
                    return Err(E::Nonfinite(v));
                }
                let v = f32::from_bits(v).abs();
                if (matches!(binding.format, 1 | 6) && v > 1.)
                    || (binding.format == 3 && v > 65504.)
                {
                    return Err(E::OutputRange {
                        semantic: binding.semantic,
                        format: binding.format,
                    });
                }
            }
        }
    }
    Ok(PreparedNoninstancedGeometry {
        group: i.group,
        positions: copy(i.positions)?,
        normals: i.normals.map(copy).transpose()?.unwrap_or_default(),
        weights: Vec::new(),
        tangents: i.tangents.map(copy).transpose()?,
    })
}
