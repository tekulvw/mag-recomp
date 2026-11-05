//! Adapter from one captured Shadow run to the native depth-only cascade pass.
//!
//! A Shadow run takes the same input format as Depth, so its sources are captured with
//! [`crate::depth_sources::copy_depth_sources`] for the run's cohort camera slot. This module turns
//! that capture into draws:
//!
//! - [`mag_render_behavior::shadow::describe_cascade_pass`] gives the pass itself: region,
//!   viewport, clear, state and the caster list in cohort order;
//! - each caster is routed onto Main's preparation with
//!   [`mag_render_behavior::shadow::caster_preparation`];
//! - its geometry is the run's retained immutable partitions (positions and indices), the same
//!   records Main consumes;
//! - its transform follows Main's rule.
//!
//! ## Decisions this adapter makes locally
//!
//! The behaviour crate leaves some questions open. A depth-only pass does not need them answered,
//! so this adapter decides them the way the Depth capture does:
//!
//! - **Transform rows (open question 2, the light view).** Main's rule applies unchanged: a
//!   draw's object-to-clip transform is the four vectors at bytes 32–95 of the item's transform
//!   record. The run's own cohort carries its own records, so this yields the run's light-space
//!   clip transform. No light-view maths is involved.
//! - **Row 42 (open question 5).** Not used: a native depth-only draw writes positions only.
//! - **Per-item child work and per-material culling (open question 7).** Child technique work is
//!   not needed for depth only. Culling follows [`CullPolicy`], chosen by the caller (back faces
//!   culled by default, as most captured casters are).
//! - **Groups 3 and 4 deformation.** As Main does, through the shared
//!   [`crate::deformation::prepare_noninstanced_geometry`] entry point: the partition's normal,
//!   weight and (group 4) tangent banks, the material's first three rows, the run's pass wind words
//!   and the transform record's object origin, under Main's mode and worker checks. A caster whose
//!   inputs do not qualify refuses, unless [`ShadowRunPolicy::allow_undeformed`] lets it draw
//!   undeformed (counted).
//!
//! An unprepared caster refuses the whole run unless [`ShadowRunPolicy::omit_unprepared`] is set,
//! in which case it is left out and listed. Nothing here runs unless a host calls it.
use crate::depth_sources::{DepthImmutablePartition, DepthItemRoute, OwnedDepthSources};
use crate::mesh_sources::GeometrySource as CapturedGeometry;
use mag_render_behavior::{
    material_sources::GeometrySource,
    setup::PipelineState,
    shadow::{self, CascadePass, LayoutAssumptions, ShadowError},
};
use std::sync::Arc;

/// How the adapter culls casters while per-material culling is unresolved.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum CullPolicy {
    /// Back faces culled, counter-clockwise front faces, for every caster.
    #[default]
    BackFaces,
    /// No culling for any caster.
    Disabled,
}

/// Choices the caller makes for the questions the behaviour crate leaves open.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct ShadowRunPolicy {
    /// Leave unprepared casters out (listed in [`ShadowRunPlan::omitted`]) instead of refusing.
    pub omit_unprepared: bool,
    /// Draw group 3 and 4 casters whose deformation inputs do not qualify without their
    /// deformation (counted) instead of refusing.
    pub allow_undeformed: bool,
    pub cull: CullPolicy,
}

/// Why a caster could not be prepared, or why the run was refused.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum ShadowRunRefusal {
    /// The behaviour crate refused the pass or a caster.
    Behaviour(ShadowError),
    /// The item has no transform record.
    MissingTransform,
    /// The item has no geometry source or mesh.
    MissingGeometry,
    /// A direct-draw source, which this capture does not prepare.
    DirectDraw,
    /// The immutable partitions were not retained or a stream failed.
    Partitions,
    /// A group 3 or 4 caster's deformation inputs do not qualify, or it refused.
    Deformation,
}

/// One caster's view of a captured item, as [`plan_shadow_casters`] needs it.
#[derive(Clone, Copy, Debug)]
pub struct CasterSource<'a> {
    /// The item's census group (`mbi[0] >> 4`), which orders the cohort.
    pub census_group: u32,
    pub route: DepthItemRoute,
    /// The mesh group (`u16` at mesh+0xBC), which routes the preparation.
    pub mesh_group: Option<u16>,
    pub geometry: Option<CapturedGeometry>,
    pub transform: Option<&'a [u8; 112]>,
    /// `None` when the capture did not retain them or a stream failed.
    pub partitions: Option<&'a [DepthImmutablePartition]>,
    /// The item's draw-batch record (its optional-stream bytes 24 and 27).
    pub mbi: &'a [u8; 32],
    /// The item's material record, when captured.
    pub material: Option<&'a [u8]>,
}

/// One partition of a caster: its positions (four words per vertex) and 16-bit indices.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ShadowPartition {
    pub positions: Arc<Vec<[u32; 4]>>,
    pub indices: Arc<Vec<u16>>,
}

/// One native depth-only draw.
#[derive(Clone, Debug, PartialEq)]
pub struct ShadowDraw {
    /// The item's index in the run's cohort.
    pub item: usize,
    /// The draw's state, from the pass with this caster's culling.
    pub state: PipelineState,
    /// Column vectors of the object-to-clip transform, as the item's record stores them
    /// (`x*c0 + y*c1 + z*c2 + c3`), raw binary32 words.
    pub object_to_clip: [[u32; 4]; 4],
    /// Drawn without the deformation its group would apply.
    pub undeformed: bool,
    /// Deformed through the shared group 3/4 entry point.
    pub deformed: bool,
    pub partitions: Vec<ShadowPartition>,
}

/// The native pass of one run.
#[derive(Clone, Debug, PartialEq)]
pub struct ShadowRunPlan {
    pub pass: CascadePass,
    pub draws: Vec<ShadowDraw>,
    /// Casters left out under [`ShadowRunPolicy::omit_unprepared`], with why.
    pub omitted: Vec<(usize, ShadowRunRefusal)>,
    /// Casters skipped because the run repeats the previous item.
    pub repeats: usize,
    /// Draws drawn undeformed under [`ShadowRunPolicy::allow_undeformed`].
    pub undeformed: usize,
    /// Draws deformed through the shared group 3/4 entry point.
    pub deformed: usize,
}

fn behaviour_geometry(source: CapturedGeometry) -> Result<GeometrySource, ShadowRunRefusal> {
    match source {
        CapturedGeometry::StaticPartitions { first, .. } => Ok(GeometrySource::Static { first }),
        CapturedGeometry::SkinnedPartitions {
            first,
            palette_address,
            ..
        } => Ok(GeometrySource::Skinned {
            first,
            palette: palette_address,
        }),
        CapturedGeometry::DirectDraw => Err(ShadowRunRefusal::DirectDraw),
    }
}

/// Main's transform rule: the four column vectors at bytes 32–95 of the transform record.
pub fn object_to_clip(transform: &[u8; 112]) -> [[u32; 4]; 4] {
    core::array::from_fn(|r| {
        core::array::from_fn(|c| {
            let at = 32 + 16 * r + 4 * c;
            u32::from_be_bytes(transform[at..at + 4].try_into().unwrap())
        })
    })
}

/// A prepared caster: whether it draws undeformed, its partitions and its transform.
struct Prepared {
    undeformed: bool,
    deformed: bool,
    partitions: Vec<ShadowPartition>,
    object_to_clip: [[u32; 4]; 4],
}
fn word(bytes: &[u8], at: usize) -> Option<u32> {
    bytes
        .get(at..at + 4)
        .map(|b| u32::from_be_bytes(b.try_into().unwrap()))
}
fn bank(partition: &DepthImmutablePartition, semantic: u8) -> Option<&[[u32; 4]]> {
    match partition
        .attributes
        .as_ref()?
        .as_ref()
        .ok()?
        .first_semantic(semantic)?
    {
        Ok(bank) => Some(bank.words.as_slice()),
        Err(_) => None,
    }
}
/// The group 3/4 deformation of one partition, as Main prepares it.
fn deform(
    group: u16,
    partition: &DepthImmutablePartition,
    positions: &[[u32; 4]],
    source: &CasterSource<'_>,
    transform: &[u8; 112],
    pass: &[u8],
    budget: usize,
) -> Option<Vec<[u32; 4]>> {
    let material = source.material.filter(|m| m.len() >= 96)?;
    // Optional streams (MBI bytes 24 and 27) are not part of this preparation.
    let optional = if source.mbi[27] & 1 == 0 {
        source.mbi[24] >> 2
    } else {
        0
    };
    if optional != 0 {
        return None;
    }
    let flag = word(material, 0x58)? & 1 != 0;
    let mode = (word(pass, 0x230)? | if flag { 32 } else { 0 }) & !2;
    if !matches!(mode, 8 | 10 | 0x28) {
        return None;
    }
    let workers = word(pass, 0x22c)?.min(if flag { 1 } else { 2 });
    if workers != if mode == 0x28 { 1 } else { 2 } {
        return None;
    }
    let tangents = if group == 4 {
        Some(bank(partition, 3)?)
    } else {
        None
    };
    let input = crate::deformation::NoninstancedGeometryInput {
        group: group as u8,
        positions,
        normals: bank(partition, 2)?,
        weights: bank(partition, 9)?,
        tangents,
        material_rows: core::array::from_fn(|r| {
            core::array::from_fn(|c| word(material, r * 16 + c * 4).unwrap_or(0))
        }),
        wind: core::array::from_fn(|i| word(pass, 0x260 + i * 4).unwrap_or(0)),
        object_origin: core::array::from_fn(|i| word(transform, 16 + i * 4).unwrap_or(0)),
    };
    pass.get(0x260..0x274)?;
    crate::deformation::prepare_noninstanced_geometry(input, budget)
        .ok()
        .map(|g| g.positions)
}
fn prepare(
    source: &CasterSource<'_>,
    pass: &[u8],
    policy: &ShadowRunPolicy,
) -> Result<Prepared, ShadowRunRefusal> {
    let mesh_group = source.mesh_group.ok_or(ShadowRunRefusal::MissingGeometry)?;
    let geometry = behaviour_geometry(source.geometry.ok_or(ShadowRunRefusal::MissingGeometry)?)?;
    let preparation =
        shadow::caster_preparation(mesh_group, geometry).map_err(ShadowRunRefusal::Behaviour)?;
    let transform = source.transform.ok_or(ShadowRunRefusal::MissingTransform)?;
    let mut undeformed = false;
    let mut partitions = Vec::new();
    for p in source.partitions.ok_or(ShadowRunRefusal::Partitions)? {
        let (Ok(positions), Ok(indices)) = (&p.positions, &p.indices) else {
            return Err(ShadowRunRefusal::Partitions);
        };
        let positions = if preparation.deform {
            match deform(
                mesh_group,
                p,
                positions,
                source,
                transform,
                pass,
                DEFORMATION_BUDGET,
            ) {
                Some(deformed) => Arc::new(deformed),
                None if policy.allow_undeformed => {
                    undeformed = true;
                    Arc::clone(positions)
                }
                None => return Err(ShadowRunRefusal::Deformation),
            }
        } else {
            Arc::clone(positions)
        };
        partitions.push(ShadowPartition {
            positions,
            indices: Arc::clone(indices),
        });
    }
    Ok(Prepared {
        undeformed,
        deformed: preparation.deform && !undeformed,
        partitions,
        object_to_clip: object_to_clip(transform),
    })
}
/// Byte budget of one caster's deformation outputs.
pub const DEFORMATION_BUDGET: usize = 64 << 20;

/// Plans cascade `cascade`'s pass from its casters' sources, in cohort order.
///
/// Items routed as terminating, or after the termination, are not cohort members of the pass.
/// A repeated item is a cohort member but draws nothing, as the job skips it.
#[allow(clippy::too_many_arguments)]
pub fn plan_shadow_casters(
    cascade: usize,
    group_mask: u16,
    sources: &[CasterSource<'_>],
    pass_record: &[u8],
    inherited: &PipelineState,
    assumptions: &LayoutAssumptions,
    policy: &ShadowRunPolicy,
) -> Result<ShadowRunPlan, ShadowRunRefusal> {
    let members: Vec<usize> = (0..sources.len())
        .filter(|&i| {
            matches!(
                sources[i].route,
                DepthItemRoute::Prepare | DepthItemRoute::Repeated
            )
        })
        .collect();
    let groups: Vec<u32> = members.iter().map(|&i| sources[i].census_group).collect();
    let pass = shadow::describe_cascade_pass(cascade, group_mask, &groups, inherited, assumptions)
        .map_err(ShadowRunRefusal::Behaviour)?;
    let culling_disabled = policy.cull == CullPolicy::Disabled;
    let mut plan = ShadowRunPlan {
        draws: Vec::with_capacity(pass.casters.len()),
        pass,
        omitted: Vec::new(),
        repeats: 0,
        undeformed: 0,
        deformed: 0,
    };
    for &member in &plan.pass.casters {
        let item = members[member];
        let source = &sources[item];
        if source.route == DepthItemRoute::Repeated {
            plan.repeats += 1;
            continue;
        }
        match prepare(source, pass_record, policy) {
            Ok(Prepared {
                undeformed,
                deformed,
                partitions,
                object_to_clip,
            }) => {
                plan.undeformed += usize::from(undeformed);
                plan.deformed += usize::from(deformed);
                plan.draws.push(ShadowDraw {
                    item,
                    state: plan.pass.caster_state(culling_disabled),
                    object_to_clip,
                    undeformed,
                    deformed,
                    partitions,
                });
            }
            Err(refusal) if policy.omit_unprepared => plan.omitted.push((item, refusal)),
            Err(refusal) => return Err(refusal),
        }
    }
    Ok(plan)
}

/// The mesh group of a captured mesh record (`u16` at +0xBC).
pub fn mesh_group(mesh: &[u8]) -> Option<u16> {
    mesh.get(0xbc..0xbe)
        .map(|b| u16::from_be_bytes([b[0], b[1]]))
}

/// The inputs of one captured run.
#[derive(Clone, Copy, Debug)]
pub struct ShadowRunInput<'a> {
    /// 0 for the update's first Shadow run, 1 for its second.
    pub cascade: usize,
    /// The run's own enabled-group mask (low half of its input's group-enable word).
    pub group_mask: u16,
    pub sources: &'a OwnedDepthSources,
    /// The pipeline state in force when the run starts.
    pub inherited: &'a PipelineState,
    pub assumptions: LayoutAssumptions,
    pub policy: ShadowRunPolicy,
}

/// Plans one captured run.
pub fn plan_shadow_run(input: &ShadowRunInput<'_>) -> Result<ShadowRunPlan, ShadowRunRefusal> {
    let sources: Vec<CasterSource<'_>> = input
        .sources
        .items
        .iter()
        .map(|item| CasterSource {
            census_group: u32::from(item.mbi[0] >> 4),
            route: item.route,
            mesh_group: item.mesh.as_ref().and_then(|m| mesh_group(m.bytes())),
            geometry: item.geometry,
            transform: item.transform.as_ref(),
            partitions: match &item.immutable_partitions {
                Some(Ok(parts)) => Some(parts.as_slice()),
                _ => None,
            },
            mbi: &item.mbi,
            material: item.material.as_ref().map(|m| m.bytes()),
        })
        .collect();
    plan_shadow_casters(
        input.cascade,
        input.group_mask,
        &sources,
        input.sources.pass.bytes(),
        input.inherited,
        &input.assumptions,
        &input.policy,
    )
}
