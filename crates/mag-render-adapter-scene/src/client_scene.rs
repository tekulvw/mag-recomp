//! Owned geometry-to-client preparation. This is an explicitly incomplete
//! position-only scene; source omissions and the whole population survive.
use crate::{
    cohort::{CohortHeader, OwnedVisibilityCohort},
    cohort_geometry::*,
};

pub const OMIT_ATTRIBUTES_MATERIAL: u64 = 1;
pub const OMIT_RETAINED_PREPARATION: u64 = 2;
pub const OMIT_DEFORMATION: u64 = 4;
pub const OMIT_SOURCE: u64 = 8;
pub const OMIT_DEPTH_ADJUSTMENT: u64 = 16;
pub const OMIT_PROJECTION: u64 = 32;
pub struct SceneBudget {
    pub draws: usize,
    pub bytes: usize,
}
#[derive(Debug, PartialEq, Eq)]
pub enum ScenePrepareError {
    Population,
    Budget,
    AllocationFailed,
}
pub struct PreparedDraw {
    pub source_item: u64,
    pub source_partition: u64,
    pub positions: Vec<[f32; 4]>,
    pub indices: Vec<u16>,
    pub unresolved_obligations: u64,
}
pub struct PreparedItem {
    pub ordinal: usize,
    pub omissions: Vec<DiagnosticGeometryOmission>,
    pub projection_failures: Vec<(usize, DiagnosticProjectionError)>,
}
pub struct PreparedScene {
    pub source: CohortHeader,
    pub intended_items: u64,
    pub draws: Vec<PreparedDraw>,
    pub items: Vec<PreparedItem>,
    pub upload_bytes: usize,
}
pub(crate) fn obligations(omissions: &[DiagnosticGeometryOmission]) -> u64 {
    omissions.iter().fold(0, |bits, o| {
        bits | match o {
            DiagnosticGeometryOmission::OtherAttributesAndMaterial => OMIT_ATTRIBUTES_MATERIAL,
            DiagnosticGeometryOmission::RetainedProjectionPreparation
            | DiagnosticGeometryOmission::EnvironmentPreparationRetained => {
                OMIT_RETAINED_PREPARATION
            }
            DiagnosticGeometryOmission::UnappliedSkinning
            | DiagnosticGeometryOmission::UnappliedInstances(_) => OMIT_DEFORMATION,
            DiagnosticGeometryOmission::UnappliedDepthAxisAdjustment => OMIT_DEPTH_ADJUSTMENT,
            _ => OMIT_SOURCE,
        }
    })
}
/// The caller must supply viewport values associated with this exact Main
/// source and the qualified final target. A later HUD viewport is not valid.
/// Aggregate budgets are checked before projection. Unsupported draws remain
/// visible as omissions; no first-N selection or synthetic population is used.
pub fn prepare_metal_scene(
    cohort: &OwnedVisibilityCohort,
    geometry: &DiagnosticGeometry,
    viewport: MetalViewportDepth,
    budget: SceneBudget,
) -> Result<PreparedScene, ScenePrepareError> {
    if geometry.items.len() != cohort.items().len()
        || geometry
            .items
            .iter()
            .enumerate()
            .any(|(i, x)| i != x.ordinal)
    {
        return Err(ScenePrepareError::Population);
    }
    let mut count = 0usize;
    let mut upload_bytes = 0usize;
    for item in &geometry.items {
        for draw in &item.draws {
            count = count.checked_add(1).ok_or(ScenePrepareError::Budget)?;
            upload_bytes = draw
                .positions
                .len()
                .checked_mul(16)
                .and_then(|n| {
                    draw.indices
                        .len()
                        .checked_mul(4)
                        .and_then(|m| n.checked_add(m))
                })
                .and_then(|n| upload_bytes.checked_add(n))
                .ok_or(ScenePrepareError::Budget)?;
        }
    }
    if count > budget.draws || upload_bytes > budget.bytes {
        return Err(ScenePrepareError::Budget);
    }
    let mut out = PreparedScene {
        source: cohort.header().clone(),
        intended_items: geometry.items.len() as u64,
        draws: Vec::new(),
        items: Vec::new(),
        upload_bytes: 0,
    };
    out.draws
        .try_reserve_exact(count)
        .map_err(|_| ScenePrepareError::AllocationFailed)?;
    out.items
        .try_reserve_exact(geometry.items.len())
        .map_err(|_| ScenePrepareError::AllocationFailed)?;
    for item in &geometry.items {
        let mut record = PreparedItem {
            ordinal: item.ordinal,
            omissions: item.omissions.clone(),
            projection_failures: Vec::new(),
        };
        record
            .omissions
            .retain(|o| *o != DiagnosticGeometryOmission::NativeViewportMappingMissing);
        record
            .projection_failures
            .try_reserve_exact(item.draws.len())
            .map_err(|_| ScenePrepareError::AllocationFailed)?;
        let first = out.draws.len();
        for draw in &item.draws {
            match project_metal_diagnostic(draw, viewport, budget.bytes) {
                Ok(positions) => {
                    out.upload_bytes += positions.len() * 16 + draw.indices.len() * 4;
                    let mut indices = Vec::new();
                    indices
                        .try_reserve_exact(draw.indices.len())
                        .map_err(|_| ScenePrepareError::AllocationFailed)?;
                    indices.extend_from_slice(&draw.indices);
                    out.draws.push(PreparedDraw {
                        source_item: item.ordinal as u64,
                        source_partition: draw.partition_ordinal as u64,
                        positions,
                        indices,
                        unresolved_obligations: 0,
                    });
                }
                Err(error) => record
                    .projection_failures
                    .push((draw.partition_ordinal, error)),
            }
        }
        let unresolved = obligations(&record.omissions)
            | if record.projection_failures.is_empty() {
                0
            } else {
                OMIT_PROJECTION
            };
        for draw in &mut out.draws[first..] {
            draw.unresolved_obligations = unresolved;
        }
        out.items.push(record);
    }
    Ok(out)
}
