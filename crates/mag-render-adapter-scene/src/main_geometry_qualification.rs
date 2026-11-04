//! Main draw preflight and actual material-value verification. Retained source
//! visibility/software-cull work remains explicit; this is no removal proof.
use super::{DiagnosticGeometryOmission, DiagnosticItemGeometry};
use crate::{
    material_values::{KnownVertexBank, MaterialDrawSources, shader_math},
    vertex_inputs,
};
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum RetainedGeometryDependency {
    VisibilityProjection,
    SoftwareCullAndOutputPublication,
}
const RETAINED: [RetainedGeometryDependency; 2] = [
    RetainedGeometryDependency::VisibilityProjection,
    RetainedGeometryDependency::SoftwareCullAndOutputPublication,
];
#[derive(Debug, PartialEq, Eq)]
pub enum MainGeometryError {
    Ordinal,
    Empty,
    Preparation(usize),
    Group(usize),
    ProjectionAssociation(usize),
    MissingDepthAdjustment,
    NonfiniteProjection,
    Source(DiagnosticGeometryOmission),
    Bindings(vertex_inputs::VertexInputError),
    MaterialValue(u32),
}
/// A preflight result, not completed rendering: actual material composition must
/// pass verify_material_values, shader-reflected vertex inputs must interleave,
/// and the source-qualified viewport/target must still pass packet validation.
/// Original software visibility/output publication remains running separately.
pub struct MainGeometryQualification {
    ordinal: usize,
    draws: usize,
    projection: [[u32; 4]; 4],
}
impl MainGeometryQualification {
    pub fn ordinal(&self) -> usize {
        self.ordinal
    }
    pub fn draws(&self) -> usize {
        self.draws
    }
    pub fn retained_dependencies(&self) -> &'static [RetainedGeometryDependency] {
        &RETAINED
    }
}
/// Qualify source-prepared banks, never raw position presence. The owning reader
/// has already checked per-source readsets/controls and keeps failures typed.
/// In particular, this does not accept incomplete static/direct/optional paths.
pub fn qualify_main_geometry(
    ordinal: usize,
    source: &DiagnosticItemGeometry,
    material: &MaterialDrawSources,
) -> Result<MainGeometryQualification, MainGeometryError> {
    qualify_main_geometry_with_policy(
        ordinal,
        source,
        material,
        vertex_inputs::VertexInputPolicy::Exact,
    )
}
pub fn qualify_main_geometry_with_policy(
    ordinal: usize,
    source: &DiagnosticItemGeometry,
    material: &MaterialDrawSources,
    policy: vertex_inputs::VertexInputPolicy,
) -> Result<MainGeometryQualification, MainGeometryError> {
    use MainGeometryError as E;
    if ordinal != source.ordinal {
        return Err(E::Ordinal);
    }
    if source.draws.is_empty() {
        return Err(E::Empty);
    }
    let projection: [[u32; 4]; 4] = core::array::from_fn(|r| {
        core::array::from_fn(|c| {
            u32::from_be_bytes(
                material.transform[32 + 16 * r + 4 * c..36 + 16 * r + 4 * c]
                    .try_into()
                    .expect("fixed source transform"),
            )
        })
    });
    if projection
        .iter()
        .flatten()
        .any(|&v| !f32::from_bits(v).is_finite())
    {
        return Err(E::NonfiniteProjection);
    }
    let mut instances = true;
    let mut skinning = true;
    for d in &source.draws {
        if d.retained_projection_vectors != projection {
            return Err(E::ProjectionAssociation(d.partition_ordinal));
        }
        if let Some(direct) = &d.prepared_direct {
            if d.prepared_group_zero.is_some()
                || d.prepared_noninstanced.is_some()
                || direct.vertex_count == 0
                || direct.indices.is_empty()
                || direct
                    .indices
                    .iter()
                    .any(|&i| i as usize >= direct.vertex_count)
                || direct
                    .attributes
                    .attributes
                    .iter()
                    .any(|a| a.words.len() != direct.vertex_count)
            {
                return Err(E::Preparation(d.partition_ordinal));
            }
            vertex_inputs::effective_array_mask_with_policy(d, policy).map_err(E::Bindings)?;
            instances = false;
            skinning = false;
            continue;
        }
        match (&d.prepared_group_zero, &d.prepared_noninstanced) {
            (Some(Ok(p)), None) if p.geometry.instance_population != 0 && p.source.mode == 0x28 => {
                if material.mesh_group != 0 {
                    return Err(E::Group(d.partition_ordinal));
                }
                if p.geometry.instance_population != usize::from(material.transform[0x6c]) {
                    return Err(E::Preparation(d.partition_ordinal));
                }
                skinning = false;
            }
            (None, Some(Ok(p))) if matches!(p.mode, 8 | 10 | 0x28) => {
                if material.mesh_group != u16::from(p.geometry.group) {
                    return Err(E::Group(d.partition_ordinal));
                }
                if let Some(static_source) = &p.static_source
                    && (!matches!(p.mode, 8 | 10)
                        || static_source.config[6] & 15 != 0
                        || static_source.palette_controls != [0; 2])
                {
                    return Err(E::Preparation(d.partition_ordinal));
                }
                let skinned7 = p.geometry.group == 7
                    && crate::cohort_geometry::skinned_group7_enabled()
                    && p.skinning_source.is_some();
                if !matches!(p.geometry.group, 3 | 4 | 11) && p.static_source.is_none() && !skinned7
                {
                    return Err(E::Preparation(d.partition_ordinal));
                }
                if skinned7 && !matches!(p.mode, 8 | 10) {
                    return Err(E::Preparation(d.partition_ordinal));
                }
                if p.geometry.group == 11
                    && ((p.skinning_source.is_none() && p.static_source.is_none())
                        || !matches!(p.mode, 8 | 10))
                {
                    return Err(E::Preparation(d.partition_ordinal));
                }
                instances = false;
                skinning &= p.skinning_source.is_some();
            }
            _ => return Err(E::Preparation(d.partition_ordinal)),
        }
        vertex_inputs::effective_array_mask_with_policy(d, policy).map_err(E::Bindings)?;
    }
    for omission in &source.omissions {
        match omission {
            // These are fulfilled only by following actual material/shader and
            // viewport consumers. Source visibility/work accounting is retained.
            DiagnosticGeometryOmission::OtherAttributesAndMaterial
            | DiagnosticGeometryOmission::RetainedProjectionPreparation
            | DiagnosticGeometryOmission::NativeViewportMappingMissing
            | DiagnosticGeometryOmission::EnvironmentPreparationRetained => {}
            DiagnosticGeometryOmission::UnappliedDepthAxisAdjustment
                if material.mesh_group == 11 => {}
            DiagnosticGeometryOmission::UnappliedInstances(_) if instances => {}
            DiagnosticGeometryOmission::UnappliedSkinning if skinning => {}
            other => return Err(E::Source(other.clone())),
        }
    }
    let projection = if material.mesh_group == 11 {
        let [anchor, scale] = material.depth_adjustment.ok_or(E::MissingDepthAdjustment)?;
        if !f32::from_bits(anchor).is_finite() || !f32::from_bits(scale).is_finite() {
            return Err(E::NonfiniteProjection);
        }
        shader_math::depth_axis_projection(projection, anchor, scale)
    } else {
        projection
    };
    if projection
        .iter()
        .flatten()
        .any(|&v| !f32::from_bits(v).is_finite())
    {
        return Err(E::NonfiniteProjection);
    }
    Ok(MainGeometryQualification {
        ordinal,
        draws: source.draws.len(),
        projection,
    })
}
/// Verify the actual ordered material writer result before shader compilation.
/// Presence of the source depth values alone never marks axis work completed.
pub fn verify_material_values(
    qualification: &MainGeometryQualification,
    values: &KnownVertexBank,
) -> Result<(), MainGeometryError> {
    for (r, &expected) in qualification.projection.iter().enumerate() {
        let slot = 3 + r as u32;
        if values.row(3 + r) != Some(expected) {
            return Err(MainGeometryError::MaterialValue(slot));
        }
    }
    Ok(())
}
