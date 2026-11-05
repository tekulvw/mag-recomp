//! Per-item vertex deformation for the cohort geometry capture.
//!
//! The capture owns the guest reads, the admission checks and the byte budget.
//! The deformation itself is never computed here: every group goes through the
//! single shared entry point in `mag_render_behavior_glue::deformation`, which
//! calls the clean-room behaviour crate (`noninstanced-deformation.md`,
//! `skinning-deformation.md`, `group-zero-deformation.md`). Main and the depth
//! pre-pass must see identical positions for every deformed item, so neither
//! may keep its own copy.
//!
//! Results are native arithmetic within the tolerances those specifications
//! state, and groups 0 and 3 use the behaviour crate's own wind tuning values.
use mag_render_behavior::{group_zero, noninstanced, skinning};
use mag_render_behavior_glue::deformation::{self as shared, PreparationError, Prepared};
use mag_render_behavior_glue::gpu_group_zero;

/// Deformed (or, for the static path, unchanged) banks of one partition.
#[derive(Debug)]
pub struct PreparedNoninstancedGeometry {
    pub group: u8,
    pub positions: Vec<[u32; 4]>,
    pub normals: Vec<[u32; 4]>,
    pub weights: Vec<[u32; 4]>,
    pub tangents: Option<Vec<[u32; 4]>>,
}

/// Group 3 (wind) and group 4 (ripple) inputs, as guest words.
pub struct NoninstancedGeometryInput<'a> {
    pub group: u8,
    pub positions: &'a [[u32; 4]],
    pub normals: &'a [[u32; 4]],
    pub weights: &'a [[u32; 4]],
    pub tangents: Option<&'a [[u32; 4]]>,
    /// Primary material rows 0..=2.
    pub material_rows: [[u32; 4]; 3],
    /// Pass record `+0x260..+0x274`.
    pub wind: [u32; 5],
    /// Object transform `+0x10..+0x1C`.
    pub object_origin: [u32; 3],
}

#[derive(Debug, PartialEq, Eq)]
pub enum NoninstancedGeometryError {
    Group(u8),
    Population(u8),
    MissingTangent,
    Budget,
    Allocation,
    /// A refusal of the behaviour deformation (for example a non-finite word).
    Deform(noninstanced::DeformError),
}

/// Bytes the retained banks of `vertices` vertices take.
fn noninstanced_bytes(vertices: usize, tangents: bool) -> Option<usize> {
    vertices.checked_mul(if tangents { 64 } else { 48 })
}

/// The refusals that need no arithmetic: group, bank populations, a missing
/// group-4 tangent bank and the byte budget. Success means only the
/// deformation itself can still refuse.
pub fn validate_noninstanced_geometry(
    i: &NoninstancedGeometryInput<'_>,
    budget: usize,
) -> Result<(), NoninstancedGeometryError> {
    if !matches!(i.group, 3 | 4) {
        return Err(NoninstancedGeometryError::Group(i.group));
    }
    let count = i.positions.len();
    for (semantic, values) in [(2, i.normals), (9, i.weights)] {
        if values.len() != count {
            return Err(NoninstancedGeometryError::Population(semantic));
        }
    }
    if i.group == 4 && i.tangents.is_none() {
        return Err(NoninstancedGeometryError::MissingTangent);
    }
    if let Some(t) = i.tangents
        && t.len() != count
    {
        return Err(NoninstancedGeometryError::Population(3));
    }
    noninstanced_bytes(count, i.tangents.is_some())
        .filter(|&n| n <= budget)
        .ok_or(NoninstancedGeometryError::Budget)?;
    Ok(())
}

/// The deformation of an input that [`validate_noninstanced_geometry`]
/// accepted.
pub fn compute_validated_noninstanced_geometry(
    i: NoninstancedGeometryInput<'_>,
) -> Result<PreparedNoninstancedGeometry, NoninstancedGeometryError> {
    let inputs = noninstanced::DeformationInputs {
        group: u32::from(i.group),
        wind: i.wind,
        material: i.material_rows,
        origin: i.object_origin,
        positions: i.positions,
        normals: i.normals,
        weights: i.weights,
        tangents: i.tangents,
    };
    match shared::noninstanced(&inputs, i.positions.len()) {
        Ok(Prepared::Ripple(banks)) => Ok(PreparedNoninstancedGeometry {
            group: i.group,
            positions: banks.positions,
            normals: banks.normals,
            weights: banks.weights,
            tangents: banks.tangents,
        }),
        Err(PreparationError::Noninstanced(error)) => Err(match error {
            noninstanced::DeformError::Budget => NoninstancedGeometryError::Budget,
            error => NoninstancedGeometryError::Deform(error),
        }),
        Ok(_) | Err(_) => unreachable!("the group 3/4 entry point returns its own result"),
    }
}

pub fn prepare_noninstanced_geometry(
    i: NoninstancedGeometryInput<'_>,
    budget: usize,
) -> Result<PreparedNoninstancedGeometry, NoninstancedGeometryError> {
    validate_noninstanced_geometry(&i, budget)?;
    compute_validated_noninstanced_geometry(i)
}

/// Palette-skinning inputs (groups 7 and 11), as guest words and bytes.
pub struct SkinningInput<'a> {
    pub positions: &'a [[u32; 4]],
    pub normals: &'a [[u32; 4]],
    pub tangents: &'a [[u32; 4]],
    /// The two palette segments, as copied; a 3x4 matrix may cross from the
    /// first into the second.
    pub segments: [&'a [u8]; 2],
    /// One 8-byte (weight, matrix index) x 4 record per vertex.
    pub influences: &'a [u8],
    /// Object transform `+0x00..+0x1C`: quaternion, translation and scale.
    pub object_pose: [u32; 8],
}

pub struct PreparedSkinning {
    pub positions: Vec<[u32; 4]>,
    pub normals: Vec<[u32; 4]>,
    pub tangents: Vec<[u32; 4]>,
}

#[derive(Debug, PartialEq, Eq)]
pub enum SkinningError {
    Budget,
    Allocation,
    /// A refusal of the behaviour skinning.
    Skin(skinning::SkinningError),
}

fn floats(bank: &[[u32; 4]]) -> Vec<[f32; 4]> {
    bank.iter().map(|row| row.map(f32::from_bits)).collect()
}
fn words(bank: Vec<[f32; 4]>) -> Vec<[u32; 4]> {
    bank.into_iter().map(|row| row.map(f32::to_bits)).collect()
}

pub fn prepare_skinning(
    i: SkinningInput<'_>,
    budget: usize,
) -> Result<PreparedSkinning, SkinningError> {
    i.positions
        .len()
        .checked_mul(48)
        .filter(|&n| n <= budget)
        .ok_or(SkinningError::Budget)?;
    let (positions, normals, tangents) =
        (floats(i.positions), floats(i.normals), floats(i.tangents));
    let inputs = skinning::SkinningInputs {
        segments: i.segments,
        influences: i.influences,
        positions: &positions,
        normals: &normals,
        tangents: &tangents,
        object: i.object_pose,
    };
    match shared::skinned(&inputs, positions.len()) {
        Ok(Prepared::Skinned(banks)) => Ok(PreparedSkinning {
            positions: words(banks.positions),
            normals: words(banks.normals),
            tangents: words(banks.tangents),
        }),
        Err(PreparationError::Skinning(error)) => Err(match error {
            skinning::SkinningError::Budget => SkinningError::Budget,
            error => SkinningError::Skin(error),
        }),
        Ok(_) | Err(_) => unreachable!("the skinning entry point returns its own result"),
    }
}

/// Group-0 inputs (instanced wind), as guest words, halves and bytes.
pub struct GroupZeroGeometryInput<'a> {
    pub positions: &'a [[u32; 4]],
    pub normals: &'a [[u32; 4]],
    pub weights: &'a [[u32; 4]],
    pub indices: &'a [u16],
    /// One row of eight halves per instance.
    pub instances: &'a [[u16; 8]],
    /// Colour pool entries, R, G, B, A.
    pub colour_pool: &'a [[u8; 4]],
    pub color_frequency: u32,
    pub batch_maximum: usize,
    /// Object transform `+0x00..+0x1C`: quaternion, translation and scale.
    pub object_pose: [u32; 8],
    /// View environment rows at `+0x10..+0x50`.
    pub source_view_rows: [[u32; 4]; 4],
    /// Object transform `+0x68` and `+0x6A`: fade multiplier and start halves.
    pub fade: [u16; 2],
    /// Primary material rows 0 and 1.
    pub material_rows: [[u32; 4]; 2],
    /// Pass record `+0x260..+0x274`.
    pub wind: [u32; 5],
}

/// Expanded instances, instance-major.
#[derive(Debug)]
pub struct PreparedGroupZeroGeometry {
    pub positions: Vec<[u32; 4]>,
    pub indices: Vec<u32>,
    /// Slot-12 colour rows in the same (instance, vertex) order.
    pub attribute12: Vec<[u32; 4]>,
    pub instance_population: usize,
    pub base_vertices: usize,
}

#[derive(Debug, PartialEq, Eq)]
pub enum GroupZeroGeometryError {
    ColorFrequency(u32),
    BatchMaximum,
    Budget,
    /// A refusal of the behaviour deformation.
    Deform(group_zero::GroupZeroError),
}

/// The opt-in GPU group-zero path (`MAG_RENDER_GPU_GROUP_ZERO=1`): the same inputs as
/// [`prepare_group_zero_geometry`], prepared as compute work whose produced vertex rows the draw
/// reads (`mag_render_behavior_glue::gpu_group_zero`). Off by default. On a refusal the caller
/// keeps the CPU path for every pass of the item.
pub fn prepare_group_zero_gpu_work(
    i: &GroupZeroGeometryInput<'_>,
    byte_budget: usize,
) -> Result<gpu_group_zero::GpuGroupZeroWork, gpu_group_zero::GpuRefusal> {
    let count = i.positions.len().saturating_mul(i.instances.len());
    let inputs = group_zero::GroupZeroInputs {
        wind: i.wind,
        material: i.material_rows,
        object: i.object_pose,
        fade: i.fade,
        environment: i.source_view_rows,
        instances: i.instances,
        colour_frequency: i.color_frequency,
        batch_max: u32::try_from(i.batch_maximum).unwrap_or(u32::MAX),
        colour_pool: i.colour_pool,
        positions: i.positions,
        normals: i.normals,
        weights: i.weights,
        indices: i.indices,
    };
    let work = gpu_group_zero::prepare(&inputs, count as u64)?;
    let bytes = work.output_bytes() as usize
        + work.parameters.len()
        + work.instances.len()
        + work.vertices.len()
        + work.pool.len()
        + 4 * work.indices.len();
    if bytes > byte_budget {
        return Err(gpu_group_zero::GpuRefusal::Budget);
    }
    Ok(work)
}

/// Whether the GPU group-zero path is enabled in this process.
pub fn group_zero_gpu_enabled() -> bool {
    gpu_group_zero::enabled_by_environment()
}

/// Colour pool entries the expansion of `instances` instances reads.
pub fn required_color_population(
    vertices: usize,
    instances: usize,
    batch_maximum: usize,
    frequency: u32,
) -> Result<usize, GroupZeroGeometryError> {
    if frequency <= 1 {
        return Err(GroupZeroGeometryError::ColorFrequency(frequency));
    }
    if batch_maximum == 0 {
        return Err(GroupZeroGeometryError::BatchMaximum);
    }
    usize::try_from(group_zero::required_pool_length(
        instances as u64,
        vertices as u64,
        u64::from(frequency),
        batch_maximum as u64,
    ))
    .map_err(|_| GroupZeroGeometryError::Budget)
}

pub fn prepare_group_zero_geometry(
    i: GroupZeroGeometryInput<'_>,
    byte_budget: usize,
) -> Result<PreparedGroupZeroGeometry, GroupZeroGeometryError> {
    let vertices = i.positions.len();
    let instances = i.instances.len();
    let count = vertices
        .checked_mul(instances)
        .filter(|&n| n <= u32::MAX as usize)
        .ok_or(GroupZeroGeometryError::Budget)?;
    let index_count = i
        .indices
        .len()
        .checked_mul(instances)
        .ok_or(GroupZeroGeometryError::Budget)?;
    count
        .checked_mul(32)
        .and_then(|n| index_count.checked_mul(4).and_then(|x| n.checked_add(x)))
        .filter(|&n| n <= byte_budget)
        .ok_or(GroupZeroGeometryError::Budget)?;
    let inputs = group_zero::GroupZeroInputs {
        wind: i.wind,
        material: i.material_rows,
        object: i.object_pose,
        fade: i.fade,
        environment: i.source_view_rows,
        instances: i.instances,
        colour_frequency: i.color_frequency,
        batch_max: u32::try_from(i.batch_maximum).unwrap_or(u32::MAX),
        colour_pool: i.colour_pool,
        positions: i.positions,
        normals: i.normals,
        weights: i.weights,
        indices: i.indices,
    };
    match shared::group_zero(&inputs, count as u64) {
        Ok(Prepared::GroupZero(output)) => Ok(PreparedGroupZeroGeometry {
            positions: output.positions,
            indices: output.indices,
            attribute12: output.colours,
            instance_population: instances,
            base_vertices: vertices,
        }),
        Err(PreparationError::GroupZero(error)) => Err(match error {
            group_zero::GroupZeroError::Budget => GroupZeroGeometryError::Budget,
            error => GroupZeroGeometryError::Deform(error),
        }),
        Ok(_) | Err(_) => unreachable!("the group 0 entry point returns its own result"),
    }
}
