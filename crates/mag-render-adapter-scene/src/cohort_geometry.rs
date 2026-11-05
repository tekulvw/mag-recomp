//! Actual raw mesh position/index acquisition for an explicitly incomplete
//! diagnostic scene. Every cohort item remains in the result, including failed
//! formats, direct draws, missing transforms and undeformed/instanced variants.
//!
//! Deformation (groups 0, 3, 4, 7 and 11) goes through [`crate::deformation`],
//! which calls the one shared behaviour-crate implementation.
#[path = "direct_draw_geometry.rs"]
pub mod direct;
#[path = "main_geometry_qualification.rs"]
pub mod qualification;
#[path = "static_geometry.rs"]
pub mod static_geometry;
use crate::{
    cohort::OwnedVisibilityCohort,
    cohort_resources::{MainPassSnapshot, OwnedCohortResources, ResourceCopyBudget},
    decoded_geometry_cache::{self, RowCodec},
    deformation::{
        self as noninstanced, GroupZeroGeometryError, GroupZeroGeometryInput,
        PreparedGroupZeroGeometry,
    },
    error::Error,
    geometry_attributes::{OwnedPrimaryAttributes, decode_primary_attributes},
    geometry_bindings::{self, AuxiliaryBinding, BindingError, VertexBinding},
    geometry_streams::GeometryStreamError,
    guest_memory::GuestSnapshot,
    material_sources::MaterialSource,
    material_textures::{LocatedTextureRequest, TextureError, TextureLocation},
    mesh_sources::{MainGeometrySource, MeshSource, PartitionChain},
    partition_inputs::StreamRequest,
};
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum DiagnosticGeometryOmission {
    MissingTransform,
    MissingMesh,
    UnresolvedDispatch,
    DirectDraw,
    DirectDrawSource(direct::DirectDrawError),
    UnappliedSkinning,
    UnappliedInstances(u32),
    OtherAttributesAndMaterial,
    RetainedProjectionPreparation,
    NativeViewportMappingMissing,
    UnappliedDepthAxisAdjustment,
    EnvironmentPreparationRetained,
    Source(Error),
    Budget,
    SourceExtent,
    UnsupportedControl { slot: usize, control: u32 },
    Codec(GeometryStreamError),
    PartitionChain,
}
#[derive(Debug)]
pub struct DiagnosticMeshDraw {
    pub partition_ordinal: usize,
    pub positions: std::sync::Arc<Vec<[u32; 4]>>,
    pub indices: std::sync::Arc<Vec<u16>>,
    /// Primary source declarations, retained for full material/layout binding.
    /// None means builtin format mapping is still unresolved, not no attributes.
    pub attributes: Option<Result<OwnedPrimaryAttributes, GeometryStreamError>>,
    /// Distinct raw secondary10/21 banks, never implicitly substituted for primary.
    pub secondary_attributes: Option<Result<OwnedPrimaryAttributes, SecondaryAttributeError>>,
    pub prepared_group_zero: Option<Result<OwnedPreparedGroupZero, GroupZeroSourceError>>,
    pub prepared_direct: Option<direct::OwnedDirectDraw>,
    pub prepared_noninstanced: Option<Result<OwnedPreparedNoninstanced, NoninstancedSourceError>>,
    pub output_bindings: Result<Vec<VertexBinding>, BindingError>,
    pub auxiliary_attributes: Result<OwnedAuxiliaryAttributes, AuxiliaryError>,
    /// Source transform20..5F, preserved without choosing a convention here.
    pub retained_projection_vectors: [[u32; 4]; 4],
}
#[derive(Debug)]
pub struct OwnedPreparedGroupZero {
    pub geometry: PreparedGroupZeroGeometry,
    pub auxiliary: OwnedAuxiliaryAttributes,
    pub source: GroupZeroSourceReadset,
}
#[derive(Debug)]
pub struct OwnedPreparedNoninstanced {
    pub geometry: noninstanced::PreparedNoninstancedGeometry,
    pub pass_address: u32,
    pub mode: u32,
    pub skinning_source: Option<SkinningSourceReadset>,
    pub static_source: Option<StaticSourceReadset>,
}
/// Explicit static position binding candidate, never proof of an array offset. Only
/// actual empty-custom static VF state is admitted when owned positions plus
/// owned auxiliary sources cover every source-selected active array.
/// The real prepared rows and full index sequence remain owned by the draw.
pub fn poc_static_position_binding(draw: &DiagnosticMeshDraw) -> Option<&[[u32; 4]]> {
    let BindingError::UnresolvedEmptyOutput {
        selector,
        control,
        bytes,
        header,
        config,
        primary_mask,
        vertex_layout_address,
        vf_masks,
        ..
    } = draw.output_bindings.as_ref().err()?
    else {
        return None;
    };
    if *selector != 255
        || *control as usize != *bytes
        || *bytes < 16
        || header.len() != 16
        || header.iter().any(|&b| b != 0)
        || config[0] != 0x81
        || !matches!(config[2], 0 | 255)
        || config[3..8] != [255, 255, 255, 16, 0]
        || config[12..] != [255; 4]
        || *vertex_layout_address == 0
        || draw.prepared_group_zero.is_some()
        || draw.prepared_direct.is_some()
    {
        return None;
    }
    let prepared = draw.prepared_noninstanced.as_ref()?.as_ref().ok()?;
    let source = prepared.static_source.as_ref()?;
    let auxiliary = draw.auxiliary_attributes.as_ref().ok()?;
    // Command-hole allocation is discarded transport, not a native row input.
    if !matches!(prepared.mode, 8 | 10)
        || prepared.skinning_source.is_some()
        || prepared.geometry.group > 15
        || matches!(prepared.geometry.group, 0 | 3 | 4 | 6)
        || source.config != *config
        || source.palette_controls != [0; 2]
        || prepared.geometry.positions.is_empty()
        || prepared.geometry.positions.as_slice() != draw.positions.as_slice()
        || usize::from(u16::from_be_bytes([config[8], config[9]]))
            != prepared.geometry.positions.len()
        || usize::from(u16::from_be_bytes([config[10], config[11]])) != draw.indices.len()
    {
        return None;
    }
    let technique = (*primary_mask)?;
    let [present, _, enabled, headers] = (*vf_masks)?;
    let selected = (0..16).fold(0u16, |mask, slot| {
        mask | if technique & (1 << (2 * (15 - slot))) != 0 {
            1 << slot
        } else {
            0
        }
    }) & enabled;
    if selected & 1 == 0 || selected & !present != 0 || selected & !headers != 0 {
        return None;
    }
    let mut covered = 1u16;
    for binding in auxiliary
        .attributes
        .iter()
        .map(|a| &a.source.binding)
        .chain(auxiliary.failures.iter().map(|a| &a.source.binding))
    {
        if binding.slot >= 16 {
            return None;
        }
        if binding.components != 0 {
            covered |= 1 << binding.slot;
        }
    }
    // Failed owned auxiliary sources still preserve array enablement. The
    // consumer rejects them if the actual VP reads that slot; never reinterpret
    // an unowned active array as a current attribute.
    if selected & !covered != 0 {
        return None;
    }
    Some(&prepared.geometry.positions)
}
#[derive(Debug)]
pub struct StaticSourceReadset {
    pub config_address: u32,
    pub config: [u8; 16],
    pub palette_controls: [u32; 2],
}
#[derive(Debug)]
pub struct SkinningSourceReadset {
    pub palette_addresses: [u32; 2],
    pub palette_bytes: [usize; 2],
    pub matrix_count: usize,
    pub influence_address: u32,
    pub influence_bytes: usize,
}
#[derive(Debug, PartialEq, Eq)]
pub enum NoninstancedSourceError {
    Group,
    MissingPass,
    MissingMaterial,
    OptionalMode(u8),
    PaletteInputs,
    SkinningConfig,
    Skinning(noninstanced::SkinningError),
    Static(static_geometry::StaticGeometryError),
    OutputBindings(BindingError),
    Mode(u32),
    Workers(u32),
    Bank(GroupZeroSourceError),
    Source(DiagnosticGeometryOmission),
    Native(noninstanced::NoninstancedGeometryError),
}
#[derive(Debug)]
pub struct GroupZeroSourceReadset {
    pub pass_address: u32,
    pub environment_address: u32,
    pub instance_address: u32,
    pub color_location: u32,
    pub color_offset: u32,
    pub color_frequency: u32,
    pub batch_maximum: u32,
    pub mode: u32,
}
#[derive(Debug, PartialEq, Eq)]
pub enum GroupZeroSourceError {
    UpdateOnly,
    MissingPass,
    MissingMaterial,
    MissingInstances,
    MissingSemantic(u8),
    GroupMismatch,
    Source(DiagnosticGeometryOmission),
    Located(AuxiliaryError),
    Codec(GeometryStreamError),
    Native(GroupZeroGeometryError),
    Mode(u32),
    Workers(u32),
    Output(u8),
    Range,
    Auxiliary12Override,
}
struct GroupZeroCaptureInput<'a> {
    source_pass_address: Option<u32>,
    resource_base: u32,
    mesh: &'a MeshSource,
    partition: &'a [u8; 544],
    transform: &'a [u8; 112],
    mbi: &'a [u8; 32],
    material: Option<&'a MaterialSource>,
    draw: &'a DiagnosticMeshDraw,
}
fn source_attribute(
    draw: &DiagnosticMeshDraw,
    semantic: u8,
) -> Result<&crate::geometry_attributes::DecodedPrimaryAttribute, GroupZeroSourceError> {
    if let Some(primary) = &draw.attributes {
        let primary = primary
            .as_ref()
            .map_err(|e| GroupZeroSourceError::Codec(*e))?;
        if let Some(bank) = primary.first_semantic(semantic) {
            return bank.map_err(|failure| GroupZeroSourceError::Codec(failure.error));
        }
    }
    if let Some(secondary) = &draw.secondary_attributes {
        let secondary = secondary.as_ref().map_err(|e| match e {
            SecondaryAttributeError::Source(e) => GroupZeroSourceError::Source(e.clone()),
            SecondaryAttributeError::Codec(e) => GroupZeroSourceError::Codec(*e),
            _ => GroupZeroSourceError::MissingSemantic(semantic),
        })?;
        if let Some(bank) = secondary.first_semantic(semantic) {
            return bank.map_err(|failure| GroupZeroSourceError::Codec(failure.error));
        }
    }
    Err(GroupZeroSourceError::MissingSemantic(semantic))
}
fn source_bank(
    draw: &DiagnosticMeshDraw,
    semantic: u8,
) -> Result<&[[u32; 4]], GroupZeroSourceError> {
    source_attribute(draw, semantic).map(|b| b.words.as_slice())
}
/// Owned, already validated arithmetic inputs of one group3/4 preparation.
/// Running it gives exactly the result the inline call would have produced;
/// its byte charge was taken from the capture budget when it was validated.
pub(crate) struct PendingNoninstanced {
    ordinal: usize,
    draw: usize,
    group: u8,
    positions: std::sync::Arc<Vec<[u32; 4]>>,
    normals: std::sync::Arc<Vec<[u32; 4]>>,
    weights: std::sync::Arc<Vec<[u32; 4]>>,
    tangents: Option<std::sync::Arc<Vec<[u32; 4]>>>,
    material_rows: [[u32; 4]; 3],
    wind: [u32; 5],
    object_origin: [u32; 3],
    pass_address: u32,
    mode: u32,
    charge: usize,
}
impl PendingNoninstanced {
    fn run(&self) -> Result<OwnedPreparedNoninstanced, NoninstancedSourceError> {
        let geometry = noninstanced::compute_validated_noninstanced_geometry(
            noninstanced::NoninstancedGeometryInput {
                group: self.group,
                positions: &self.positions,
                normals: &self.normals,
                weights: &self.weights,
                tangents: self.tangents.as_deref().map(Vec::as_slice),
                material_rows: self.material_rows,
                wind: self.wind,
                object_origin: self.object_origin,
            },
        )
        .map_err(NoninstancedSourceError::Native)?;
        Ok(OwnedPreparedNoninstanced {
            geometry,
            pass_address: self.pass_address,
            mode: self.mode,
            skinning_source: None,
            static_source: None,
        })
    }
}
/// Group3/4 wind/ripple arithmetic is pure per-vertex work over owned inputs;
/// it needs no source callback. `PS3_RENDER_SERIAL_GEOMETRY_PREP=1` keeps it
/// inline on the capture thread (the previous behaviour).
fn defer_noninstanced() -> bool {
    static F: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    *F.get_or_init(|| std::env::var_os("PS3_RENDER_SERIAL_GEOMETRY_PREP").is_none_or(|v| v != "1"))
}
/// Run every pending preparation, on scoped helper threads when the total
/// vertex work is large enough to pay for them. Result order is job order.
fn run_pending_noninstanced(
    jobs: &[PendingNoninstanced],
) -> Vec<Result<OwnedPreparedNoninstanced, NoninstancedSourceError>> {
    use std::sync::{
        OnceLock,
        atomic::{AtomicUsize, Ordering},
    };
    let work: usize = jobs.iter().map(|j| j.positions.len()).sum();
    let threads = if work < 4096 {
        1
    } else {
        std::thread::available_parallelism()
            .map_or(1, |n| n.get())
            .clamp(1, 8)
            .min(jobs.len())
    };
    if threads <= 1 {
        return jobs.iter().map(PendingNoninstanced::run).collect();
    }
    let next = AtomicUsize::new(0);
    let slots: Vec<OnceLock<Result<OwnedPreparedNoninstanced, NoninstancedSourceError>>> =
        (0..jobs.len()).map(|_| OnceLock::new()).collect();
    let worker = || {
        loop {
            let at = next.fetch_add(1, Ordering::Relaxed);
            let Some(job) = jobs.get(at) else {
                break;
            };
            let _ = slots[at].set(job.run());
        }
    };
    if crate::capture_pool::enabled() {
        // Persistent helpers instead of a spawn per capture; same job order.
        let helpers = (threads - 1).min(crate::capture_pool::helpers());
        crate::capture_pool::join(
            (0..helpers)
                .map(|_| Box::new(&worker) as Box<dyn FnOnce() + Send + '_>)
                .collect(),
            worker,
        );
    } else {
        std::thread::scope(|scope| {
            for _ in 1..threads {
                // A refused helper only means less parallelism; the calling
                // thread below still drains every remaining job.
                let _ = std::thread::Builder::new().spawn_scoped(scope, worker);
            }
            worker();
        });
    }
    slots
        .into_iter()
        .map(|slot| slot.into_inner().expect("every pending job ran"))
        .collect()
}
enum NoninstancedOutcome {
    Ready(OwnedPreparedNoninstanced),
    Deferred(PendingNoninstanced),
}
/// Default on; `PS3_RENDER_SKINNED_GROUP7=0` opts out. Prepares material
/// group 7's palette-skinned partitions with the same palette skinning as
/// group 11: both are ordinary matrix-palette skinned partitions;
/// only group 11 adds Main's depth-axis projection, which group 7 does not use.
pub fn skinned_group7_enabled() -> bool {
    static F: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    *F.get_or_init(|| !std::env::var_os("PS3_RENDER_SKINNED_GROUP7").is_some_and(|v| v == "0"))
}
/// Material groups whose skinned partitions take the palette skinning path.
fn palette_skinned_group(group: u16) -> bool {
    group == 11 || (group == 7 && skinned_group7_enabled())
}
fn capture_noninstanced<F>(
    copy: &mut Copy<'_, F>,
    input: GroupZeroCaptureInput<'_>,
    defer: Option<usize>,
) -> Result<NoninstancedOutcome, NoninstancedSourceError>
where
    F: FnMut(StreamRequest) -> Result<Vec<u8>, Error>,
{
    use NoninstancedSourceError as E;
    let _timer = crate::source_acquisition::section(
        crate::source_acquisition::Section::NoninstancedPreparation,
    );
    let group = input.mesh.material_group();
    if !(matches!(group, 3 | 4) || palette_skinned_group(group))
        || u16::from(input.mbi[0] >> 4) != group
    {
        return Err(E::Group);
    }
    let pass_address = input
        .source_pass_address
        .filter(|&a| a != 0)
        .ok_or(E::MissingPass)?;
    let pass = copy.read_pass(pass_address).map_err(E::Source)?;
    let fallback;
    let material = if let Some(m) = input.material {
        m.bytes()
    } else {
        if (word(input.mbi, 28) & 511) * 16 < 96 {
            return Err(E::MissingMaterial);
        }
        fallback = copy
            .read(
                u32::from_be_bytes([input.mbi[6], input.mbi[7], input.mbi[8], 0]),
                96,
            )
            .map_err(E::Source)?;
        &fallback
    };
    if material.len() < 96 {
        return Err(E::MissingMaterial);
    }
    // The optional-stream count comes from MBI bytes 24 and 27. The matrix
    // count comes from the two palette segment lengths; zero bypasses skinning.
    let optional = if input.mbi[27] & 1 == 0 {
        input.mbi[24] >> 2
    } else {
        0
    };
    if optional != 0 {
        return Err(E::OptionalMode(optional));
    }
    if !palette_skinned_group(group)
        && (word(input.partition, 0x48) != 0 || word(input.partition, 0x50) != 0)
    {
        return Err(E::PaletteInputs);
    }
    let flag = word(material, 0x58) & 1 != 0;
    let mode = (word(&pass, 0x230) | if flag { 32 } else { 0 }) & !2;
    if !matches!(mode, 8 | 10 | 0x28) || (palette_skinned_group(group) && !matches!(mode, 8 | 10)) {
        return Err(E::Mode(mode));
    }
    let workers = word(&pass, 0x22c).min(if flag { 1 } else { 2 });
    if workers != if mode == 0x28 { 1 } else { 2 } {
        return Err(E::Workers(workers));
    }
    if palette_skinned_group(group) {
        return capture_skinning(copy, input, pass_address, mode, word(material, 0x58))
            .map(NoninstancedOutcome::Ready);
    }
    if let Some(ordinal) = defer {
        let normals = source_attribute(input.draw, 2).map_err(E::Bank)?;
        let weights = source_attribute(input.draw, 9).map_err(E::Bank)?;
        let tangents = if group == 4 {
            Some(source_attribute(input.draw, 3).map_err(E::Bank)?)
        } else {
            None
        };
        let job = PendingNoninstanced {
            ordinal,
            draw: usize::MAX,
            group: group as u8,
            positions: input.draw.positions.clone(),
            normals: normals.words.clone(),
            weights: weights.words.clone(),
            tangents: tangents.map(|t| t.words.clone()),
            material_rows: core::array::from_fn(|r| {
                core::array::from_fn(|c| word(material, r * 16 + c * 4))
            }),
            wind: core::array::from_fn(|i| word(&pass, 0x260 + i * 4)),
            object_origin: core::array::from_fn(|i| word(input.transform, 16 + i * 4)),
            pass_address,
            mode,
            charge: 0,
        };
        noninstanced::validate_noninstanced_geometry(
            &noninstanced::NoninstancedGeometryInput {
                group: job.group,
                positions: &job.positions,
                normals: &job.normals,
                weights: &job.weights,
                tangents: job.tangents.as_deref().map(Vec::as_slice),
                material_rows: job.material_rows,
                wind: job.wind,
                object_origin: job.object_origin,
            },
            copy.budget.bytes,
        )
        .inspect_err(budget_refusal)
        .map_err(E::Native)?;
        // The same charge the inline path takes on success, taken now; a late
        // arithmetic refusal is refunded when the job is joined.
        let charge = job.positions.len() * if job.tangents.is_some() { 64 } else { 48 };
        copy.budget.bytes -= charge;
        return Ok(NoninstancedOutcome::Deferred(PendingNoninstanced {
            charge,
            ..job
        }));
    }
    let geometry = noninstanced::prepare_noninstanced_geometry(
        noninstanced::NoninstancedGeometryInput {
            group: group as u8,
            positions: &input.draw.positions,
            normals: source_bank(input.draw, 2).map_err(E::Bank)?,
            weights: source_bank(input.draw, 9).map_err(E::Bank)?,
            tangents: if group == 4 {
                Some(source_bank(input.draw, 3).map_err(E::Bank)?)
            } else {
                None
            },
            material_rows: core::array::from_fn(|r| {
                core::array::from_fn(|c| word(material, r * 16 + c * 4))
            }),
            wind: core::array::from_fn(|i| word(&pass, 0x260 + i * 4)),
            object_origin: core::array::from_fn(|i| word(input.transform, 16 + i * 4)),
        },
        copy.budget.bytes,
    )
    .inspect_err(budget_refusal)
    .map_err(E::Native)?;
    copy.budget.bytes -=
        geometry.positions.len() * if geometry.tangents.is_some() { 64 } else { 48 };
    Ok(NoninstancedOutcome::Ready(OwnedPreparedNoninstanced {
        geometry,
        pass_address,
        mode,
        skinning_source: None,
        static_source: None,
    }))
}
fn capture_skinning<F>(
    copy: &mut Copy<'_, F>,
    input: GroupZeroCaptureInput<'_>,
    pass_address: u32,
    mode: u32,
    primary_flags: u32,
) -> Result<OwnedPreparedNoninstanced, NoninstancedSourceError>
where
    F: FnMut(StreamRequest) -> Result<Vec<u8>, Error>,
{
    use NoninstancedSourceError as E;
    let _timer =
        crate::source_acquisition::section(crate::source_acquisition::Section::SkinningPreparation);
    let config = copy.stream(input.partition, 19).map_err(E::Source)?;
    // Source control17 selects four byte-weight/index influences and the
    // standard direction path; palette control0 supplies packed float3x4.
    // Alternate palette, inverse-direction and optional paths remain explicit.
    if config.len() < 16
        || config[0] != 0x83
        || config[6] != 17
        || config[7] != 0
        || config[12..16] != [255; 4]
    {
        return Err(E::SkinningConfig);
    }
    let count = input.draw.positions.len();
    let palette_base = match input.mesh.main_geometry_source(
        primary_flags,
        input.mbi,
        input.transform[0x6f],
        input.resource_base,
    ) {
        MainGeometrySource::SkinnedPartitions {
            palette_address, ..
        } => palette_address,
        _ => return Err(E::PaletteInputs),
    };
    let palette_addresses = core::array::from_fn(|i| {
        let at = 0x14 + 2 * i;
        palette_base.wrapping_add(u32::from(u16::from_be_bytes([
            input.partition[at],
            input.partition[at + 1],
        ])))
    });
    let mut palette_bytes = [0usize; 2];
    let mut raw = [Vec::new(), Vec::new()];
    for i in 0..2 {
        let slot = 3 + i;
        let control = word(input.partition, 0x30 + 8 * slot);
        if control >> 16 != 0 {
            return Err(E::Source(DiagnosticGeometryOmission::UnsupportedControl {
                slot,
                control,
            }));
        }
        palette_bytes[i] = control as usize;
        if control != 0 {
            raw[i] = copy
                .read(palette_addresses[i], control as usize)
                .map_err(E::Source)?;
        }
    }
    let matrix_count = (palette_bytes[0] + palette_bytes[1]) / 48;
    if matrix_count == 0 {
        return Err(E::PaletteInputs);
    }
    // The palette is charged as its decoded matrices; the skinning reads the
    // two copied segments directly (a matrix may cross from one to the next).
    let bytes = matrix_count
        .checked_mul(48)
        .filter(|&n| n <= copy.budget.bytes || budget_refused())
        .ok_or(E::Source(DiagnosticGeometryOmission::Budget))?;
    copy.budget.bytes -= bytes;
    let influence_address = word(input.partition, 0x34 + 8 * 5);
    let influence = copy.stream(input.partition, 5).map_err(E::Source)?;
    let influence_bytes = count
        .checked_mul(8)
        .ok_or(E::Source(DiagnosticGeometryOmission::Budget))?;
    let influences = influence
        .get(..influence_bytes)
        .ok_or(E::Source(DiagnosticGeometryOmission::SourceExtent))?;
    let prepared = noninstanced::prepare_skinning(
        noninstanced::SkinningInput {
            positions: &input.draw.positions,
            normals: source_bank(input.draw, 2).map_err(E::Bank)?,
            tangents: source_bank(input.draw, 3).map_err(E::Bank)?,
            segments: [&raw[0], &raw[1]],
            influences,
            object_pose: core::array::from_fn(|i| word(input.transform, 4 * i)),
        },
        copy.budget.bytes,
    )
    .inspect_err(budget_refusal)
    .map_err(E::Skinning)?;
    copy.budget.bytes -= count * 48;
    Ok(OwnedPreparedNoninstanced {
        geometry: noninstanced::PreparedNoninstancedGeometry {
            group: input.mesh.material_group() as u8,
            positions: prepared.positions,
            normals: prepared.normals,
            // This caller has no source weight semantic9 bank. Do not invent it.
            weights: Vec::new(),
            tangents: Some(prepared.tangents),
        },
        pass_address,
        mode,
        static_source: None,
        skinning_source: Some(SkinningSourceReadset {
            palette_addresses,
            palette_bytes,
            matrix_count,
            influence_address,
            influence_bytes,
        }),
    })
}
fn capture_static<F>(
    copy: &mut Copy<'_, F>,
    input: GroupZeroCaptureInput<'_>,
) -> Result<OwnedPreparedNoninstanced, NoninstancedSourceError>
where
    F: FnMut(StreamRequest) -> Result<Vec<u8>, Error>,
{
    use NoninstancedSourceError as E;
    let _timer =
        crate::source_acquisition::section(crate::source_acquisition::Section::StaticPreparation);
    let group = u8::try_from(input.mesh.material_group()).map_err(|_| E::Group)?;
    if u16::from(input.mbi[0] >> 4) != u16::from(group) {
        return Err(E::Group);
    }
    let pass_address = input
        .source_pass_address
        .filter(|&a| a != 0)
        .ok_or(E::MissingPass)?;
    let pass = copy.read_pass(pass_address).map_err(E::Source)?;
    let fallback;
    let material = if let Some(m) = input.material {
        m.bytes()
    } else {
        if (word(input.mbi, 28) & 511) * 16 < 96 {
            return Err(E::MissingMaterial);
        }
        fallback = copy
            .read(
                u32::from_be_bytes([input.mbi[6], input.mbi[7], input.mbi[8], 0]),
                96,
            )
            .map_err(E::Source)?;
        &fallback
    };
    if material.len() < 96 {
        return Err(E::MissingMaterial);
    }
    let flags = word(material, 0x58);
    if !matches!(
        input.mesh.main_geometry_source(
            flags,
            input.mbi,
            input.transform[0x6f],
            input.resource_base
        ),
        MainGeometrySource::StaticPartitions { .. }
    ) {
        return Err(E::Group);
    }
    let flag = flags & 1 != 0;
    let mode = (word(&pass, 0x230) | if flag { 32 } else { 0 }) & !2;
    let workers = word(&pass, 0x22c).min(if flag { 1 } else { 2 });
    let config = copy.stream(input.partition, 19).map_err(E::Source)?;
    let config: [u8; 16] = config
        .get(..16)
        .ok_or(E::Source(DiagnosticGeometryOmission::SourceExtent))?
        .try_into()
        .expect("checked source config");
    let palette_controls = [word(input.partition, 0x48), word(input.partition, 0x50)];
    // A source-empty descriptor does not make decoded positions disappear.
    // Keep the unresolved binding error on the draw; exact admission still
    // refuses it. This only retains the undeformed preparation for the
    // explicitly selected static position binding policy downstream.
    let output = match &input.draw.output_bindings {
        Ok(output) => output.as_slice(),
        Err(BindingError::UnresolvedEmptyOutput { .. }) => &[],
        Err(error) => return Err(E::OutputBindings(error.clone())),
    };
    let normals = if output.iter().any(|b| b.semantic == 2) {
        Some(source_bank(input.draw, 2).map_err(E::Bank)?)
    } else {
        None
    };
    let tangents = if output.iter().any(|b| b.semantic == 3) {
        Some(source_bank(input.draw, 3).map_err(E::Bank)?)
    } else {
        None
    };
    let geometry = static_geometry::prepare_static_geometry(
        static_geometry::StaticGeometryInput {
            group,
            mode,
            workers,
            optional_count: if input.mbi[27] & 1 == 0 {
                input.mbi[24] >> 2
            } else {
                0
            },
            palette_bytes: palette_controls.map(|v| v as usize),
            skinning_control: config[6],
            positions: &input.draw.positions,
            normals,
            tangents,
            output,
        },
        copy.budget.bytes,
    )
    .inspect_err(budget_refusal)
    .map_err(E::Static)?;
    copy.budget.bytes -= geometry.positions.len()
        * 16
        * (1 + usize::from(normals.is_some()) + usize::from(tangents.is_some()));
    Ok(OwnedPreparedNoninstanced {
        geometry,
        pass_address,
        mode,
        skinning_source: None,
        static_source: Some(StaticSourceReadset {
            config_address: word(input.partition, 0x34 + 8 * 19),
            config,
            palette_controls,
        }),
    })
}
/// Own the exact raw common Group0 source readset under this invocation.
fn capture_group_zero<F, L>(
    copy: &mut Copy<'_, F>,
    located: &mut L,
    input: GroupZeroCaptureInput<'_>,
) -> Result<OwnedPreparedGroupZero, GroupZeroSourceError>
where
    F: FnMut(StreamRequest) -> Result<Vec<u8>, Error>,
    L: FnMut(LocatedTextureRequest) -> Result<Vec<u8>, TextureError>,
{
    let _timer = crate::source_acquisition::section(
        crate::source_acquisition::Section::GroupZeroPreparation,
    );
    let GroupZeroCaptureInput {
        source_pass_address,
        resource_base,
        mesh,
        partition,
        transform,
        mbi,
        material,
        draw,
    } = input;
    if mbi[0] >> 4 != 0 {
        return Err(GroupZeroSourceError::GroupMismatch);
    }
    let pass_address = source_pass_address
        .filter(|&address| address != 0)
        .ok_or(GroupZeroSourceError::MissingPass)?;
    let pass = copy
        .read_pass(pass_address)
        .map_err(GroupZeroSourceError::Source)?;
    if matches!(
        crate::main_flow::main_item_route(0, word(&pass, 0x144)),
        Ok(crate::main_flow::MainItemRoute::UpdateOnly)
    ) {
        return Err(GroupZeroSourceError::UpdateOnly);
    }
    let fallback;
    let material = if let Some(material) = material {
        material.bytes()
    } else {
        if ((word(mbi, 28) & 511) * 16) < 96 {
            return Err(GroupZeroSourceError::MissingMaterial);
        }
        fallback = copy
            .read(u32::from_be_bytes([mbi[6], mbi[7], mbi[8], 0]), 96)
            .map_err(GroupZeroSourceError::Source)?;
        &fallback
    };
    if material.len() < 96 {
        return Err(GroupZeroSourceError::MissingMaterial);
    }
    let material_flag = word(material, 0x58) & 1 != 0;
    let mode = (word(&pass, 0x230) | if material_flag { 32 } else { 0 }) & !2;
    if mode != 0x28 {
        return Err(GroupZeroSourceError::Mode(mode));
    }
    let workers = word(&pass, 0x22c).min(if material_flag { 1 } else { 2 });
    if workers != 1 {
        return Err(GroupZeroSourceError::Workers(workers));
    }
    let config = copy
        .stream(partition, 19)
        .map_err(GroupZeroSourceError::Source)?;
    let output = *config.get(4).ok_or(GroupZeroSourceError::Range)?;
    if output != 0 {
        return Err(GroupZeroSourceError::Output(output));
    }
    let instances = usize::from(transform[0x6c]);
    if instances == 0 {
        return Err(GroupZeroSourceError::MissingInstances);
    }
    let extra = word(transform, 0x60);
    if extra & 0x80000000 == 0 {
        return Err(GroupZeroSourceError::Range);
    }
    let instance_address = resource_base.wrapping_add(extra & 0x7fffffff);
    let compact = copy
        .read(instance_address, instances * 16)
        .map_err(GroupZeroSourceError::Source)?;
    // The decoded rows are checked and charged as before; the deformation
    // reads the same rows as raw halves.
    crate::geometry_streams::decode_instance_rows(&compact, instances, copy.budget.bytes)
        .inspect_err(budget_refusal)
        .map_err(GroupZeroSourceError::Codec)?;
    copy.budget.bytes -= instances * 32;
    let instance_rows: Vec<[u16; 8]> = compact
        .as_chunks::<16>()
        .0
        .iter()
        .take(instances)
        .map(|row| core::array::from_fn(|i| u16::from_be_bytes([row[2 * i], row[2 * i + 1]])))
        .collect();
    let batch_maximum = word(partition, 0xfc);
    let color_frequency = word(mesh.bytes(), 0x34) & 0xffff;
    let colors = noninstanced::required_color_population(
        draw.positions.len(),
        instances,
        batch_maximum as usize,
        color_frequency,
    )
    .map_err(GroupZeroSourceError::Native)?;
    let source_aux = draw.auxiliary_attributes.as_ref().map_err(|e| {
        GroupZeroSourceError::Located(match e {
            AuxiliaryError::Binding(e) => AuxiliaryError::Binding(e.clone()),
            AuxiliaryError::Source(e) => AuxiliaryError::Source(e.clone()),
            AuxiliaryError::Located(e) => AuxiliaryError::Located(e.clone()),
            AuxiliaryError::Budget => AuxiliaryError::Budget,
            AuxiliaryError::Extent => AuxiliaryError::Extent,
        })
    })?;
    if source_aux
        .attributes
        .iter()
        .any(|a| a.source.binding.slot == 12)
        || source_aux
            .failures
            .iter()
            .any(|a| a.source.binding.slot == 12)
    {
        return Err(GroupZeroSourceError::Auxiliary12Override);
    }
    let color_source = word(transform, 0x64);
    let color_location = color_source >> 31;
    let color_offset = color_source & 0x7fffffff;
    let raw_colors = copy
        .located(
            LocatedTextureRequest {
                location: if color_location == 0 {
                    TextureLocation::Local
                } else {
                    TextureLocation::Main
                },
                offset: color_offset,
                bytes: colors.checked_mul(4).ok_or(GroupZeroSourceError::Range)?,
            },
            located,
        )
        .map_err(GroupZeroSourceError::Located)?;
    let color_binding = VertexBinding {
        declaration: 0,
        semantic: 0,
        slot: 12,
        format: 4,
        components: 4,
        stride: 4,
        byte_offset: 0,
        encoded_bytes: 4,
    };
    // As for the instance rows: the decoded colours are checked and charged
    // as before, and the deformation reads the raw colour pool entries.
    geometry_bindings::decode_gpu_attribute(&color_binding, &raw_colors, colors, copy.budget.bytes)
        .inspect_err(budget_refusal)
        .map_err(|e| GroupZeroSourceError::Located(AuxiliaryError::Binding(e)))?;
    copy.budget.bytes -= colors * 16;
    let colour_pool = &raw_colors.as_chunks::<4>().0[..colors];
    let environment_address = word(&pass, 0x238);
    let environment = copy
        .read(environment_address, 128)
        .map_err(GroupZeroSourceError::Source)?;
    let normals = source_bank(draw, 2)?;
    let weights = source_bank(draw, 9)?;
    let fade = core::array::from_fn(|i| {
        u16::from_be_bytes([transform[0x68 + 2 * i], transform[0x69 + 2 * i]])
    });
    let geometry = noninstanced::prepare_group_zero_geometry(
        GroupZeroGeometryInput {
            positions: &draw.positions,
            normals,
            weights,
            indices: &draw.indices,
            instances: &instance_rows,
            colour_pool,
            color_frequency,
            batch_maximum: batch_maximum as usize,
            object_pose: core::array::from_fn(|i| word(transform, i * 4)),
            source_view_rows: core::array::from_fn(|r| {
                core::array::from_fn(|c| word(&environment, 16 + r * 16 + c * 4))
            }),
            fade,
            material_rows: core::array::from_fn(|r| {
                core::array::from_fn(|c| word(material, r * 16 + c * 4))
            }),
            wind: core::array::from_fn(|i| word(&pass, 0x260 + i * 4)),
        },
        copy.budget.bytes,
    )
    .inspect_err(budget_refusal)
    .map_err(GroupZeroSourceError::Native)?;
    copy.budget.bytes -= geometry.positions.len() * 32 + geometry.indices.len() * 4;
    let count = geometry.positions.len();
    let maximum_vertices = (batch_maximum as usize)
        .min(instances)
        .checked_mul(draw.positions.len())
        .ok_or(GroupZeroSourceError::Range)?;
    let mut auxiliary = OwnedAuxiliaryAttributes {
        primary_technique: source_aux.primary_technique.clone(),
        ..Default::default()
    };
    let mut plans = source_aux
        .attributes
        .iter()
        .map(|a| a.source.clone())
        .chain(source_aux.failures.iter().map(|a| a.source.clone()))
        .collect::<Vec<_>>();
    plans.sort_by_key(|a| a.binding.declaration);
    for binding in plans {
        let decoded = (|| {
            let bytes = geometry_bindings::auxiliary_source_bytes(&binding, maximum_vertices)
                .map_err(AuxiliaryError::Binding)?;
            let source = copy.located(
                LocatedTextureRequest {
                    location: if binding.location == 0 {
                        TextureLocation::Local
                    } else {
                        TextureLocation::Main
                    },
                    offset: binding.offset,
                    bytes,
                },
                located,
            )?;
            let rows = decoded_geometry_cache::auxiliary(
                &binding,
                &source,
                maximum_vertices,
                copy.budget.bytes,
            )
            .inspect_err(budget_refusal)
            .map_err(AuxiliaryError::Binding)?;
            copy.budget.bytes -= rows.len() * 16;
            let output = count
                .checked_mul(16)
                .filter(|&n| n <= copy.budget.bytes || budget_refused())
                .ok_or(AuxiliaryError::Budget)?;
            let mut words = Vec::new();
            words
                .try_reserve_exact(count)
                .map_err(|_| AuxiliaryError::Source(Error::AllocationFailed))?;
            let mut first = 0;
            while first < instances {
                let batch = (instances - first).min(batch_maximum as usize);
                words.extend_from_slice(&rows[..batch * draw.positions.len()]);
                first += batch;
            }
            copy.budget.bytes -= output;
            Ok(words)
        })();
        match decoded {
            Ok(words) => auxiliary.attributes.push(DecodedAuxiliaryAttribute {
                source: binding,
                words,
            }),
            Err(error) => auxiliary.failures.push(AuxiliaryFailure {
                source: binding,
                error,
            }),
        }
    }
    Ok(OwnedPreparedGroupZero {
        geometry,
        auxiliary,
        source: GroupZeroSourceReadset {
            pass_address,
            environment_address,
            instance_address,
            color_location,
            color_offset,
            color_frequency,
            batch_maximum,
            mode,
        },
    })
}
#[derive(Debug, PartialEq, Eq)]
pub enum SecondaryAttributeError {
    Source(DiagnosticGeometryOmission),
    Codec(GeometryStreamError),
    SplitInputChunks([u16; 3]),
    Builtin(u8),
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum AuxiliaryError {
    Binding(BindingError),
    Source(Error),
    Located(TextureError),
    Budget,
    Extent,
}
#[derive(Debug)]
pub struct DecodedAuxiliaryAttribute {
    pub source: AuxiliaryBinding,
    pub words: Vec<[u32; 4]>,
}
#[derive(Debug)]
pub struct AuxiliaryFailure {
    pub source: AuxiliaryBinding,
    pub error: AuxiliaryError,
}
#[derive(Debug, Default)]
pub struct OwnedAuxiliaryAttributes {
    /// Primary source role only; effective overrides require explicit composition.
    pub primary_technique: Option<PrimaryTechniqueBinding>,
    pub attributes: Vec<DecodedAuxiliaryAttribute>,
    pub failures: Vec<AuxiliaryFailure>,
}
#[derive(Debug, Clone)]
pub struct PrimaryTechniqueBinding {
    pub selector: u32,
    pub record: [u8; 32],
}
impl DiagnosticMeshDraw {
    /// Canonical column-vector model→vertex-program clip transform. The four
    /// source vectors are the columns: x*c3+y*c4+z*c5+c6. This is BEFORE
    /// actual RSX viewport→native clip conversion, which must be supplied.
    pub fn source_clip_columns(&self) -> [u32; 16] {
        core::array::from_fn(|i| self.retained_projection_vectors[i / 4][i % 4])
    }
}
pub struct DiagnosticItemGeometry {
    pub ordinal: usize,
    pub draws: Vec<DiagnosticMeshDraw>,
    pub omissions: Vec<DiagnosticGeometryOmission>,
}
pub struct DiagnosticGeometry {
    pub items: Vec<DiagnosticItemGeometry>,
    pub source_bytes: usize,
    pub source_reads: usize,
}
fn word(bytes: &[u8], at: usize) -> u32 {
    u32::from_be_bytes(bytes[at..at + 4].try_into().expect("checked source record"))
}
/// The largest single stream transfer a partition slot can name.
const MAX_TRANSFER: usize = 16 * 1024;
/// Upper bound for a stream extended past `MAX_TRANSFER` from its counts.
const LARGE_STREAM_LIMIT: usize = 4 * 1024 * 1024;
static LARGE_STREAMS: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);
/// Streams read past a 16 KiB slot control from their element counts.
pub fn large_streams() -> u64 {
    LARGE_STREAMS.load(std::sync::atomic::Ordering::Relaxed)
}
/// Stream bytes: a fresh copy, or retained catalog content that was
/// byte-identical to the guest span when compared in place.
enum StreamBytes {
    Owned(Vec<u8>),
    Shared(std::sync::Arc<crate::shared_scene_sources::SourceContent>),
}
impl std::ops::Deref for StreamBytes {
    type Target = [u8];
    fn deref(&self) -> &[u8] {
        match self {
            Self::Owned(v) => v,
            Self::Shared(c) => c.bytes(),
        }
    }
}
impl StreamBytes {
    fn content(&self) -> Option<&std::sync::Arc<crate::shared_scene_sources::SourceContent>> {
        match self {
            Self::Owned(_) => None,
            Self::Shared(c) => Some(c),
        }
    }
}
struct Copy<'a, F> {
    read: &'a mut F,
    pass_snapshot: Option<MainPassSnapshot<'a>>,
    budget: ResourceCopyBudget,
    bytes: usize,
    reads: usize,
    /// Byte charges returned inside the item loop (failed draws' deferred jobs).
    refunded: usize,
}
thread_local! {
    // Set by every budget refusal of the copy budget. A geometry chunk is
    // synchronous on one thread.
    static BUDGET_PRESSURE: std::cell::Cell<bool> = const { std::cell::Cell::new(false) };
}
fn budget_pressure() {
    BUDGET_PRESSURE.with(|p| p.set(true));
}
/// A decoder given the remaining budget as its limit reports a refusal
/// through a budget variant of its error (`Budget`, nested or not). Matching
/// the name can only over-report, which makes the chunked guard re-run serial.
fn budget_refusal<E: std::fmt::Debug>(error: &E) {
    if format!("{error:?}").contains("Budget") {
        budget_pressure();
    }
}
/// `budget_pressure` for a failing `filter` predicate.
fn budget_refused() -> bool {
    budget_pressure();
    false
}
fn take_budget_pressure() -> bool {
    BUDGET_PRESSURE.with(|p| p.replace(false))
}
impl<'a, F: FnMut(StreamRequest) -> Result<Vec<u8>, Error>> Copy<'a, F> {
    fn read_pass(
        &mut self,
        address: u32,
    ) -> Result<std::borrow::Cow<'a, [u8]>, DiagnosticGeometryOmission> {
        if let Some(pass) = self.pass_snapshot {
            if pass.address() != address {
                return Err(DiagnosticGeometryOmission::SourceExtent);
            }
            return Ok(std::borrow::Cow::Borrowed(pass.bytes()));
        }
        self.read(address, 688).map(std::borrow::Cow::Owned)
    }
    fn read(&mut self, address: u32, bytes: usize) -> Result<Vec<u8>, DiagnosticGeometryOmission> {
        self.read_counted(address, bytes)
    }
    fn read_counted(
        &mut self,
        address: u32,
        bytes: usize,
    ) -> Result<Vec<u8>, DiagnosticGeometryOmission> {
        if self.budget.reads == 0 || bytes > self.budget.bytes {
            budget_pressure();
            return Err(DiagnosticGeometryOmission::Budget);
        }
        if address == 0
            || u64::from(address)
                .checked_add(bytes as u64)
                .is_none_or(|end| end > 1u64 << 32)
        {
            return Err(DiagnosticGeometryOmission::SourceExtent);
        }
        let owned = (self.read)(StreamRequest { address, bytes })
            .map_err(DiagnosticGeometryOmission::Source)?;
        if owned.len() != bytes {
            return Err(DiagnosticGeometryOmission::SourceExtent);
        }
        self.budget.bytes -= bytes;
        self.budget.reads -= 1;
        self.bytes += bytes;
        self.reads += 1;
        Ok(owned)
    }
    fn stream(
        &mut self,
        header: &[u8; 544],
        slot: usize,
    ) -> Result<Vec<u8>, DiagnosticGeometryOmission> {
        let control = word(header, 0x30 + 8 * slot);
        if control >> 16 != 0 {
            return Err(DiagnosticGeometryOmission::UnsupportedControl { slot, control });
        }
        if control == 0 {
            return Ok(Vec::new());
        }
        self.read(word(header, 0x34 + 8 * slot), control as usize)
    }
    fn located(
        &mut self,
        request: LocatedTextureRequest,
        reader: &mut impl FnMut(LocatedTextureRequest) -> Result<Vec<u8>, TextureError>,
    ) -> Result<Vec<u8>, AuxiliaryError> {
        self.located_counted(request, reader)
    }
    /// `stream`, but with `PS3_RENDER_CAPTURE_VIEW_VALIDATION=1` a span whose
    /// current guest bytes equal retained catalog content is compared in
    /// place and returned as that content, charged and counted exactly as the
    /// copied read. Any other outcome is the ordinary copied `stream`.
    fn stream_shared(
        &mut self,
        header: &[u8; 544],
        slot: usize,
    ) -> Result<StreamBytes, DiagnosticGeometryOmission> {
        self.stream_at_least(header, slot, 0)
    }
    /// A stream slot whose control is exactly `MAX_TRANSFER` (16 KiB) names
    /// the largest single transfer, not the stream's size: a larger stream
    /// still starts at the slot's address. When the consumer's own counts
    /// need `required` bytes beyond that, read exactly `required` bytes from
    /// the same address (bounded by `LARGE_STREAM_LIMIT`, counted). Every
    /// other control word is the stream size, as before.
    fn stream_at_least(
        &mut self,
        header: &[u8; 544],
        slot: usize,
        required: usize,
    ) -> Result<StreamBytes, DiagnosticGeometryOmission> {
        let control = word(header, 0x30 + 8 * slot);
        let large = control as usize == MAX_TRANSFER
            && required > MAX_TRANSFER
            && required <= LARGE_STREAM_LIMIT;
        if large || crate::source_acquisition::view_validation_enabled() {
            if control >> 16 != 0 {
                return Err(DiagnosticGeometryOmission::UnsupportedControl { slot, control });
            }
            if control == 0 {
                return Ok(StreamBytes::Owned(Vec::new()));
            }
            let bytes = if large {
                LARGE_STREAMS.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
                required
            } else {
                control as usize
            };
            let address = word(header, 0x34 + 8 * slot);
            return if crate::source_acquisition::view_validation_enabled() {
                self.read_shared(address, bytes)
            } else {
                self.read(address, bytes).map(StreamBytes::Owned)
            };
        }
        self.stream(header, slot).map(StreamBytes::Owned)
    }
    fn read_shared(
        &mut self,
        address: u32,
        bytes: usize,
    ) -> Result<StreamBytes, DiagnosticGeometryOmission> {
        // The same refusals, in the same order, as `read`; then the view.
        if self.budget.reads == 0 || bytes > self.budget.bytes {
            budget_pressure();
            return Err(DiagnosticGeometryOmission::Budget);
        }
        if address == 0
            || u64::from(address)
                .checked_add(bytes as u64)
                .is_none_or(|end| end > 1u64 << 32)
        {
            return Err(DiagnosticGeometryOmission::SourceExtent);
        }
        if let Some(content) = crate::capture_view::guest_view_retained(address, bytes) {
            self.budget.bytes -= bytes;
            self.budget.reads -= 1;
            self.bytes += bytes;
            self.reads += 1;
            return Ok(StreamBytes::Shared(content));
        }
        self.read(address, bytes).map(StreamBytes::Owned)
    }
    fn located_counted(
        &mut self,
        request: LocatedTextureRequest,
        reader: &mut impl FnMut(LocatedTextureRequest) -> Result<Vec<u8>, TextureError>,
    ) -> Result<Vec<u8>, AuxiliaryError> {
        if self.budget.reads == 0 || request.bytes > self.budget.bytes {
            budget_pressure();
            return Err(AuxiliaryError::Budget);
        }
        if u64::from(request.offset) + request.bytes as u64 > 0x8000_0000 {
            return Err(AuxiliaryError::Extent);
        }
        let bytes = reader(request).map_err(AuxiliaryError::Located)?;
        if bytes.len() != request.bytes {
            return Err(AuxiliaryError::Extent);
        }
        self.budget.bytes -= request.bytes;
        self.budget.reads -= 1;
        self.bytes += request.bytes;
        self.reads += 1;
        Ok(bytes)
    }
}
/// Placeholder identity for a slot6 failure whose descriptor itself could not
/// be owned; the failure keeps slot6 explicitly unsatisfied.
fn slot_six_failure_binding() -> AuxiliaryBinding {
    AuxiliaryBinding {
        binding: VertexBinding {
            declaration: usize::MAX,
            semantic: 0,
            slot: 6,
            format: 4,
            components: 4,
            stride: 4,
            byte_offset: 0,
            encoded_bytes: 4,
        },
        source_declaration: [0; 8],
        source_format_word: 0,
        frequency: 0,
        modulo: false,
        location: 0,
        offset: 0,
    }
}
/// Own Main's optional per-vertex slot6 array for one static partition, with
/// the same budgeted LOCAL/MAIN located reader and GPU fetch decoding as the
/// other auxiliary arrays. A non-array descriptor adds nothing.
fn attach_slot_six<F: FnMut(StreamRequest) -> Result<Vec<u8>, Error>>(
    descriptor: &Result<[u8; 64], AuxiliaryError>,
    first_vertex: u32,
    count: usize,
    copy: &mut Copy<'_, F>,
    located_reader: &mut impl FnMut(LocatedTextureRequest) -> Result<Vec<u8>, TextureError>,
    auxiliary: &mut OwnedAuxiliaryAttributes,
) {
    let descriptor = match descriptor {
        Ok(d) => d,
        Err(error) => {
            auxiliary.failures.push(AuxiliaryFailure {
                source: slot_six_failure_binding(),
                error: clone_auxiliary_error(error),
            });
            return;
        }
    };
    let binding = match geometry_bindings::slot_six_stream_binding(descriptor, first_vertex) {
        Ok(Some(b)) => b,
        Ok(None) => return,
        Err(error) => {
            auxiliary.failures.push(AuxiliaryFailure {
                source: slot_six_failure_binding(),
                error: AuxiliaryError::Binding(error),
            });
            return;
        }
    };
    let decoded = (|| {
        let bytes = geometry_bindings::auxiliary_source_bytes(&binding, count)
            .map_err(AuxiliaryError::Binding)?;
        let output = count.checked_mul(16).ok_or(AuxiliaryError::Budget)?;
        if output > copy.budget.bytes.saturating_sub(bytes) {
            budget_pressure();
            return Err(AuxiliaryError::Budget);
        }
        let source = copy.located(
            LocatedTextureRequest {
                location: if binding.location == 0 {
                    TextureLocation::Local
                } else {
                    TextureLocation::Main
                },
                offset: binding.offset,
                bytes,
            },
            located_reader,
        )?;
        copy.budget.bytes -= output;
        geometry_bindings::decode_auxiliary_attribute(&binding, &source, count, output)
            .map_err(AuxiliaryError::Binding)
    })();
    match decoded {
        Ok(words) => auxiliary.attributes.push(DecodedAuxiliaryAttribute {
            source: binding,
            words,
        }),
        Err(error) => auxiliary.failures.push(AuxiliaryFailure {
            source: binding,
            error,
        }),
    }
}
fn clone_auxiliary_error(e: &AuxiliaryError) -> AuxiliaryError {
    match e {
        AuxiliaryError::Binding(e) => AuxiliaryError::Binding(e.clone()),
        AuxiliaryError::Source(e) => AuxiliaryError::Source(e.clone()),
        AuxiliaryError::Located(e) => AuxiliaryError::Located(e.clone()),
        AuxiliaryError::Budget => AuxiliaryError::Budget,
        AuxiliaryError::Extent => AuxiliaryError::Extent,
    }
}
/// This diagnostic deliberately consumes only source positions/indices and
/// retained projection inputs. It does not fabricate a complete24-input bundle
/// or silently substitute zero for the environment, deformation or attributes.
pub fn copy_diagnostic_geometry(
    cohort: &OwnedVisibilityCohort,
    resources: Option<&OwnedCohortResources>,
    resource_base: u32,
    budget: ResourceCopyBudget,
    reader: &mut impl FnMut(StreamRequest) -> Result<Vec<u8>, Error>,
) -> Result<DiagnosticGeometry, Error> {
    copy_diagnostic_geometry_with_located(
        cohort,
        resources,
        resource_base,
        budget,
        reader,
        &mut |_| Err(TextureError::MissingSource),
    )
}
/// Live source path with the same checked LOCAL/MAIN located reader as owned
/// textures. The callbacks copy synchronously inside the source invocation;
/// retained owned data does not imply a guest allocation generation.
pub fn copy_diagnostic_geometry_with_located(
    cohort: &OwnedVisibilityCohort,
    resources: Option<&OwnedCohortResources>,
    resource_base: u32,
    budget: ResourceCopyBudget,
    reader: &mut impl FnMut(StreamRequest) -> Result<Vec<u8>, Error>,
    located_reader: &mut impl FnMut(LocatedTextureRequest) -> Result<Vec<u8>, TextureError>,
) -> Result<DiagnosticGeometry, Error> {
    copy_diagnostic_geometry_with_pass(
        cohort,
        resources,
        resource_base,
        None,
        budget,
        reader,
        located_reader,
    )
}
pub fn copy_diagnostic_geometry_with_pass(
    cohort: &OwnedVisibilityCohort,
    resources: Option<&OwnedCohortResources>,
    resource_base: u32,
    source_pass_address: Option<u32>,
    budget: ResourceCopyBudget,
    reader: &mut impl FnMut(StreamRequest) -> Result<Vec<u8>, Error>,
    located_reader: &mut impl FnMut(LocatedTextureRequest) -> Result<Vec<u8>, TextureError>,
) -> Result<DiagnosticGeometry, Error> {
    copy_geometry_with_pass_source(
        cohort,
        resources,
        resource_base,
        GeometryPassSource {
            address: source_pass_address,
            snapshot: None,
        },
        budget,
        reader,
        located_reader,
    )
}
/// Use the one actual source-owned pass snapshot throughout partition capture.
/// No per-partition pass callback/allocation is required; other reads preserve
/// their original source ownership and byte/read budgets.
pub fn copy_diagnostic_geometry_with_snapshot(
    cohort: &OwnedVisibilityCohort,
    resources: Option<&OwnedCohortResources>,
    resource_base: u32,
    snapshot: MainPassSnapshot<'_>,
    budget: ResourceCopyBudget,
    reader: &mut impl FnMut(StreamRequest) -> Result<Vec<u8>, Error>,
    located_reader: &mut impl FnMut(LocatedTextureRequest) -> Result<Vec<u8>, TextureError>,
) -> Result<DiagnosticGeometry, Error> {
    copy_geometry_with_pass_source(
        cohort,
        resources,
        resource_base,
        GeometryPassSource {
            address: Some(snapshot.address()),
            snapshot: Some(snapshot),
        },
        budget,
        reader,
        located_reader,
    )
}
struct GeometryPassSource<'a> {
    address: Option<u32>,
    snapshot: Option<MainPassSnapshot<'a>>,
}
fn copy_geometry_with_pass_source(
    cohort: &OwnedVisibilityCohort,
    resources: Option<&OwnedCohortResources>,
    resource_base: u32,
    source_pass: GeometryPassSource<'_>,
    budget: ResourceCopyBudget,
    reader: &mut impl FnMut(StreamRequest) -> Result<Vec<u8>, Error>,
    located_reader: &mut impl FnMut(LocatedTextureRequest) -> Result<Vec<u8>, TextureError>,
) -> Result<DiagnosticGeometry, Error> {
    let source_pass_address = source_pass.address;
    let mut copy = Copy {
        read: reader,
        pass_snapshot: source_pass.snapshot,
        budget,
        bytes: 0,
        reads: 0,
        refunded: 0,
    };
    let mut items = Vec::new();
    items
        .try_reserve_exact(cohort.items().len())
        .map_err(|_| Error::AllocationFailed)?;
    let defer = defer_noninstanced();
    let mut pending: Vec<PendingNoninstanced> = Vec::new();
    for (ordinal, item) in cohort.items().iter().enumerate() {
        items.push(capture_item(
            ItemContext {
                cohort,
                resources,
                resource_base,
                source_pass_address,
                defer,
            },
            ordinal,
            item,
            &mut copy,
            located_reader,
            &mut pending,
        ));
    }
    if !pending.is_empty() {
        let _timer = crate::source_acquisition::section(
            crate::source_acquisition::Section::NoninstancedPreparation,
        );
        for (job, result) in pending.iter().zip(run_pending_noninstanced(&pending)) {
            if result.is_err() {
                copy.budget.bytes += job.charge;
            }
            items[job.ordinal].draws[job.draw].prepared_noninstanced = Some(result);
        }
    }
    Ok(DiagnosticGeometry {
        items,
        source_bytes: copy.bytes,
        source_reads: copy.reads,
    })
}
/// Readers a helper thread may call for its own chunk of items. They perform
/// the same counted source copies as the calling thread's readers.
pub trait ChunkReaders: Sync {
    fn guest(&self, request: StreamRequest) -> Result<Vec<u8>, Error>;
    fn located(&self, request: LocatedTextureRequest) -> Result<Vec<u8>, TextureError>;
}
/// Default-off `PS3_RENDER_CAPTURE_GEOMETRY_CHUNKS=N` (2..=8): split the item
/// loop into N contiguous ranges run concurrently. It needs helper reads
/// (`PS3_RENDER_CAPTURE_HELPER_READS=1`); otherwise the capture stays serial.
pub fn geometry_chunks() -> usize {
    static F: std::sync::OnceLock<usize> = std::sync::OnceLock::new();
    *F.get_or_init(|| {
        std::env::var("PS3_RENDER_CAPTURE_GEOMETRY_CHUNKS")
            .ok()
            .and_then(|v| v.parse::<usize>().ok())
            .filter(|n| (2..=8).contains(n))
            .unwrap_or(0)
    })
}
/// What the chunked schedule did for one capture.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct ChunkOutcome {
    /// Ranges run concurrently (0: the serial schedule ran directly).
    pub chunks: u32,
    /// 1 when the exactness guard refused the chunked result and the serial
    /// schedule ran instead.
    pub reruns: u32,
    /// Helper threads' acquisition counters and profile sections.
    pub helper_stats: crate::source_acquisition::AcquisitionStats,
    pub helper_profile: [u64; crate::source_acquisition::SECTIONS],
}
struct ChunkResult {
    items: Vec<DiagnosticItemGeometry>,
    pending: Vec<PendingNoninstanced>,
    bytes: usize,
    reads: usize,
    end: ResourceCopyBudget,
    refunded: usize,
    pressure: bool,
    stats: crate::source_acquisition::AcquisitionStats,
    profile: [u64; crate::source_acquisition::SECTIONS],
}
#[allow(clippy::too_many_arguments)]
fn capture_chunk<F, L>(
    context: ItemContext<'_>,
    range: std::ops::Range<usize>,
    pass_snapshot: Option<MainPassSnapshot<'_>>,
    budget: ResourceCopyBudget,
    reader: &mut F,
    located_reader: &mut L,
) -> ChunkResult
where
    F: FnMut(StreamRequest) -> Result<Vec<u8>, Error>,
    L: FnMut(LocatedTextureRequest) -> Result<Vec<u8>, TextureError>,
{
    let stats = crate::source_acquisition::thread_stats();
    let profile = crate::source_acquisition::thread_profile();
    take_budget_pressure();
    let mut copy = Copy {
        read: reader,
        pass_snapshot,
        budget,
        bytes: 0,
        reads: 0,
        refunded: 0,
    };
    let mut items = Vec::with_capacity(range.len());
    let mut pending = Vec::new();
    for ordinal in range {
        let item = &context.cohort.items()[ordinal];
        items.push(capture_item(
            context,
            ordinal,
            item,
            &mut copy,
            located_reader,
            &mut pending,
        ));
    }
    let end = crate::source_acquisition::thread_profile();
    ChunkResult {
        items,
        pending,
        bytes: copy.bytes,
        reads: copy.reads,
        end: copy.budget,
        refunded: copy.refunded,
        pressure: take_budget_pressure(),
        stats: crate::source_acquisition::thread_stats().since(stats),
        profile: std::array::from_fn(|i| end[i].wrapping_sub(profile[i])),
    }
}
/// Contiguous ranges with about the same number of partitions each (items
/// without resources count as one), so helpers finish at similar times.
fn chunk_ranges(
    cohort: &OwnedVisibilityCohort,
    resources: Option<&OwnedCohortResources>,
    chunks: usize,
) -> Vec<std::ops::Range<usize>> {
    let weights: Vec<usize> = (0..cohort.items().len())
        .map(|i| {
            resources
                .and_then(|r| r.items.get(i))
                .and_then(|i| i.partitions.as_ref())
                .map_or(1, |p| p.partitions().len().max(1))
        })
        .collect();
    let total: usize = weights.iter().sum();
    let mut ranges = Vec::with_capacity(chunks);
    let (mut start, mut acc) = (0, 0);
    for (i, w) in weights.iter().enumerate() {
        acc += w;
        let boundary = total * (ranges.len() + 1) / chunks;
        if acc >= boundary && ranges.len() + 1 < chunks && i + 1 < weights.len() {
            ranges.push(start..i + 1);
            start = i + 1;
        }
    }
    ranges.push(start..weights.len());
    ranges.retain(|r| !r.is_empty());
    ranges
}
/// The geometry capture with its item loop split into concurrent ranges.
///
/// Each range runs on its own copy state with half of every budget dimension;
/// the calling thread runs the first range with `reader`/`located_reader`,
/// helpers run the rest with `helpers`. The result is accepted only if
/// * no range refused anything for budget (`budget_pressure`), and
/// * the ranges' combined gross charges are at most that half.
///
/// Then it equals the serial schedule's. Serial order charges the same items,
/// so wherever it makes a check it has at least `budget - combined charges >=
/// half` left (refunds only add), while the range that made the same check had
/// at most `half`: every check passed in a range also passes serially, and no
/// check failed in a range. With equal check outcomes the per-item work is the
/// same; items share no other state, deferred preparations are joined in item
/// order as in the serial schedule, and source counters add. Otherwise the
/// serial schedule runs again from the start (`reruns`).
#[allow(clippy::too_many_arguments)]
pub fn copy_diagnostic_geometry_chunked(
    cohort: &OwnedVisibilityCohort,
    resources: Option<&OwnedCohortResources>,
    resource_base: u32,
    snapshot: MainPassSnapshot<'_>,
    budget: ResourceCopyBudget,
    helpers: &dyn ChunkReaders,
    chunks: usize,
    reader: &mut impl FnMut(StreamRequest) -> Result<Vec<u8>, Error>,
    located_reader: &mut impl FnMut(LocatedTextureRequest) -> Result<Vec<u8>, TextureError>,
) -> Result<(DiagnosticGeometry, ChunkOutcome), Error> {
    let serial = |reader: &mut _, located_reader: &mut _, reruns| {
        copy_diagnostic_geometry_with_snapshot(
            cohort,
            resources,
            resource_base,
            snapshot,
            budget,
            reader,
            located_reader,
        )
        .map(|g| {
            (
                g,
                ChunkOutcome {
                    reruns,
                    ..ChunkOutcome::default()
                },
            )
        })
    };
    let ranges = chunk_ranges(cohort, resources, chunks);
    if chunks < 2 || ranges.len() < 2 {
        return serial(reader, located_reader, 0);
    }
    let context = ItemContext {
        cohort,
        resources,
        resource_base,
        source_pass_address: Some(snapshot.address()),
        defer: defer_noninstanced(),
    };
    // Every range gets half of each budget dimension; see the guard below.
    let half = ResourceCopyBudget {
        bytes: budget.bytes / 2,
        reads: budget.reads / 2,
        partition_headers: budget.partition_headers / 2,
    };
    let mut results: Vec<Option<ChunkResult>> = (0..ranges.len()).map(|_| None).collect();
    {
        let (first, rest) = results.split_first_mut().expect("two or more ranges");
        let mut jobs: Vec<Box<dyn FnOnce() + Send + '_>> = Vec::new();
        let view = crate::capture_view::current_view();
        for (slot, range) in rest.iter_mut().zip(ranges[1..].iter().cloned()) {
            jobs.push(Box::new(move || {
                let _view = crate::capture_view::ViewScope::enter(view);
                *slot = Some(capture_chunk(
                    context,
                    range,
                    Some(snapshot),
                    half,
                    &mut |r| helpers.guest(r),
                    &mut |r| helpers.located(r),
                ));
            }));
        }
        let mut local = || {
            capture_chunk(
                context,
                ranges[0].clone(),
                Some(snapshot),
                half,
                reader,
                located_reader,
            )
        };
        *first = Some(if crate::capture_pool::enabled() {
            crate::capture_pool::join(jobs, local)
        } else {
            std::thread::scope(|scope| {
                let handles: Vec<_> = jobs
                    .into_iter()
                    .map(|job| {
                        std::thread::Builder::new()
                            .name("mag-capture-geometry".into())
                            .spawn_scoped(scope, job)
                    })
                    .collect();
                let first = local();
                // A job whose thread could not start never ran: its slot
                // stays empty and the serial schedule runs instead.
                for handle in handles.into_iter().flatten() {
                    if let Err(panic) = handle.join() {
                        std::panic::resume_unwind(panic);
                    }
                }
                first
            })
        });
    }
    let mut outcome = ChunkOutcome {
        chunks: ranges.len() as u32,
        ..ChunkOutcome::default()
    };
    let mut done = Vec::with_capacity(results.len());
    for (index, result) in results.into_iter().enumerate() {
        let Some(result) = result else {
            return serial(reader, located_reader, 1).map(|(g, o)| {
                (
                    g,
                    ChunkOutcome {
                        chunks: outcome.chunks,
                        helper_stats: outcome.helper_stats,
                        helper_profile: outcome.helper_profile,
                        ..o
                    },
                )
            });
        };
        if index > 0 {
            outcome.helper_stats = outcome.helper_stats.plus(result.stats);
            for (sum, v) in outcome.helper_profile.iter_mut().zip(result.profile) {
                *sum = sum.wrapping_add(v);
            }
        }
        done.push(result);
    }
    let charged = |f: fn(&ResourceCopyBudget) -> usize, refund: bool| -> usize {
        done.iter()
            .map(|r| f(&half) - f(&r.end) + if refund { r.refunded } else { 0 })
            .sum()
    };
    let exact = !done.iter().any(|r| r.pressure)
        && charged(|b| b.bytes, true) <= half.bytes
        && charged(|b| b.reads, false) <= half.reads
        && charged(|b| b.partition_headers, false) <= half.partition_headers;
    if !exact {
        let (geometry, rerun) = serial(reader, located_reader, 1)?;
        return Ok((
            geometry,
            ChunkOutcome {
                reruns: rerun.reruns,
                ..outcome
            },
        ));
    }
    let mut items = Vec::with_capacity(cohort.items().len());
    let mut pending = Vec::new();
    let (mut bytes, mut reads) = (0, 0);
    for result in done {
        items.extend(result.items);
        pending.extend(result.pending);
        bytes += result.bytes;
        reads += result.reads;
    }
    if !pending.is_empty() {
        let _timer = crate::source_acquisition::section(
            crate::source_acquisition::Section::NoninstancedPreparation,
        );
        for (job, result) in pending.iter().zip(run_pending_noninstanced(&pending)) {
            items[job.ordinal].draws[job.draw].prepared_noninstanced = Some(result);
        }
    }
    Ok((
        DiagnosticGeometry {
            items,
            source_bytes: bytes,
            source_reads: reads,
        },
        outcome,
    ))
}
struct CodecFailure<'a> {
    step: &'a str,
    error: &'a GeometryStreamError,
    ordinal: usize,
    partition: usize,
    group: u16,
    config: &'a [u8],
    header: &'a [u8; 544],
    count: usize,
    index_count: usize,
    source_len: usize,
    index_len: usize,
    format: Option<&'a [u8]>,
}
/// First 24 failing partition decodes per process: which step, and the
/// partition's layout metadata (config row, stream controls and address
/// phases, format header and position declaration). No asset contents.
fn report_codec_failure(f: CodecFailure<'_>) {
    use std::sync::atomic::{AtomicU64, Ordering};
    static REPORTED: AtomicU64 = AtomicU64::new(0);
    if REPORTED.fetch_add(1, Ordering::Relaxed) >= 24 {
        return;
    }
    let slot = |s: usize| {
        (
            word(f.header, 0x30 + 8 * s),
            word(f.header, 0x34 + 8 * s) & 15,
        )
    };
    let (header, position) = match f.format {
        Some(format) => {
            let header = format.get(..8).map(|h| h.to_vec());
            let declarations = format
                .get(8..)
                .map(|d| d.as_chunks::<8>().0)
                .unwrap_or_default();
            let count = format.first().map_or(0, |&n| usize::from(n));
            let position = declarations
                .iter()
                .take(count)
                .find(|d| d[3] == 1)
                .map(|d| d.to_vec());
            (header, position)
        }
        None => (None, None),
    };
    eprintln!(
        "[mag geometry codec refusal] step={} error={:?} ordinal={} partition={} group={} config={:?} vertices={} indices={} source_bytes={} index_bytes={} slot7(control,phase)={:?} slot1={:?} slot20={:?} slot19={:?} format_header={:?} position_declaration={:?}",
        f.step,
        f.error,
        f.ordinal,
        f.partition,
        f.group,
        f.config,
        f.count,
        f.index_count,
        f.source_len,
        f.index_len,
        slot(7),
        slot(1),
        slot(20),
        slot(19),
        header,
        position
    );
}
/// What one item's capture needs besides its own copy state.
#[derive(Clone, Copy)]
struct ItemContext<'a> {
    cohort: &'a OwnedVisibilityCohort,
    resources: Option<&'a OwnedCohortResources>,
    resource_base: u32,
    source_pass_address: Option<u32>,
    defer: bool,
}
/// One cohort item: all of its reads, budget charges and (deferred)
/// preparations, in source order. Items share nothing but `copy`'s budget.
fn capture_item<F, L>(
    context: ItemContext<'_>,
    ordinal: usize,
    item: &crate::cohort::CohortItem,
    copy: &mut Copy<'_, F>,
    located_reader: &mut L,
    pending: &mut Vec<PendingNoninstanced>,
) -> DiagnosticItemGeometry
where
    F: FnMut(StreamRequest) -> Result<Vec<u8>, Error>,
    L: FnMut(LocatedTextureRequest) -> Result<Vec<u8>, TextureError>,
{
    let ItemContext {
        cohort: _,
        resources,
        resource_base,
        source_pass_address,
        defer,
    } = context;
    let mut output = DiagnosticItemGeometry {
        ordinal,
        draws: Vec::new(),
        omissions: vec![
            DiagnosticGeometryOmission::OtherAttributesAndMaterial,
            DiagnosticGeometryOmission::RetainedProjectionPreparation,
            DiagnosticGeometryOmission::NativeViewportMappingMissing,
            DiagnosticGeometryOmission::EnvironmentPreparationRetained,
        ],
    };
    let work = (|| -> Result<(), DiagnosticGeometryOmission> {
        let transform = item
            .transform
            .as_ref()
            .ok_or(DiagnosticGeometryOmission::MissingTransform)?;
        let mut mesh_fallback = None;
        let existing = resources.and_then(|r| r.items.get(ordinal));
        let mesh = if let Some(m) = existing.and_then(|i| i.mesh.as_ref()) {
            m
        } else {
            let address = word(&item.mbi, 12).wrapping_mul(16);
            let bytes = copy.read(address, 192)?;
            mesh_fallback.replace(
                MeshSource::copy(
                    &GuestSnapshot::new(address, &bytes)
                        .map_err(DiagnosticGeometryOmission::Source)?,
                    address,
                )
                .map_err(|_| DiagnosticGeometryOmission::MissingMesh)?,
            );
            mesh_fallback.as_ref().expect("owned mesh")
        };
        let flags = existing
            .and_then(|i| i.primary_material.as_ref())
            .and_then(|m| m.bytes().get(0x58..0x5c))
            .map(|b| word(b, 0));
        if flags.is_none() && mesh.material_group() != 0 && word(mesh.bytes(), 0x9c) & 0x1000 == 0 {
            return Err(DiagnosticGeometryOmission::UnresolvedDispatch);
        }
        if mesh.material_group() == 11 {
            output
                .omissions
                .push(DiagnosticGeometryOmission::UnappliedDepthAxisAdjustment);
        }
        let dispatch = mesh.main_geometry_source(
            flags.unwrap_or(0),
            &item.mbi,
            transform[0x6f],
            resource_base,
        );
        // Main's static-partition caller is the only slot6 array writer.
        // The descriptor is per item; each partition supplies its first
        // source vertex. A failed copy is retained per draw, never dropped.
        let slot_six_descriptor = if matches!(dispatch, MainGeometrySource::StaticPartitions { .. })
        {
            match geometry_bindings::slot_six_source(
                mesh.material_group(),
                word(mesh.bytes(), 0x9c),
                flags.unwrap_or(0),
                &item.mbi,
            ) {
                geometry_bindings::SlotSixSource::Descriptor(address) => Some(
                    copy.read(address, 64)
                        .map_err(|e| match e {
                            DiagnosticGeometryOmission::Budget => AuxiliaryError::Budget,
                            DiagnosticGeometryOmission::Source(e) => AuxiliaryError::Source(e),
                            _ => AuxiliaryError::Extent,
                        })
                        .and_then(|b| {
                            <[u8; 64]>::try_from(b.as_slice()).map_err(|_| AuxiliaryError::Extent)
                        }),
                ),
                _ => None,
            }
        } else {
            None
        };
        let first = match dispatch {
            MainGeometrySource::DirectDraw { instanced } => {
                let resource = existing.ok_or(DiagnosticGeometryOmission::UnresolvedDispatch)?;
                let selected = resource
                    .primary_technique
                    .as_ref()
                    .ok_or(DiagnosticGeometryOmission::UnresolvedDispatch)?;
                let material = resource
                    .primary_material
                    .as_ref()
                    .ok_or(DiagnosticGeometryOmission::UnresolvedDispatch)?
                    .bytes();
                let map = material
                    .get(0x48..0x4a)
                    .ok_or(DiagnosticGeometryOmission::SourceExtent)?;
                let at = usize::from(u16::from_be_bytes(map.try_into().unwrap()))
                    .checked_add(
                        (selected.source.selector as usize)
                            .checked_mul(32)
                            .ok_or(DiagnosticGeometryOmission::SourceExtent)?,
                    )
                    .ok_or(DiagnosticGeometryOmission::SourceExtent)?;
                let technique = material
                    .get(
                        at..at
                            .checked_add(32)
                            .ok_or(DiagnosticGeometryOmission::SourceExtent)?,
                    )
                    .ok_or(DiagnosticGeometryOmission::SourceExtent)?;
                let layout = copy.read(mesh.vertex_layout_address(), 656)?;
                let budget = copy.budget.bytes;
                let direct = direct::copy_direct_draw(
                    mesh.bytes(),
                    layout.as_slice().try_into().unwrap(),
                    transform,
                    instanced,
                    word(technique, 16),
                    budget,
                    &mut |request| copy.located(request, located_reader),
                )
                .inspect_err(budget_refusal)
                .map_err(DiagnosticGeometryOmission::DirectDrawSource)?;
                let owned = direct.owned_bytes();
                if owned > copy.budget.bytes {
                    budget_pressure();
                    return Err(DiagnosticGeometryOmission::Budget);
                }
                copy.budget.bytes -= owned;
                output.draws.push(DiagnosticMeshDraw {
                    partition_ordinal: 0,
                    positions: std::sync::Arc::default(),
                    indices: std::sync::Arc::default(),
                    attributes: None,
                    secondary_attributes: None,
                    prepared_group_zero: None,
                    prepared_direct: Some(direct),
                    prepared_noninstanced: None,
                    output_bindings: Ok(Vec::new()),
                    auxiliary_attributes: Ok(OwnedAuxiliaryAttributes::default()),
                    retained_projection_vectors: core::array::from_fn(|r| {
                        core::array::from_fn(|c| word(transform, 32 + r * 16 + c * 4))
                    }),
                });
                return Ok(());
            }
            MainGeometrySource::SkinnedPartitions { first, .. } => {
                output
                    .omissions
                    .push(DiagnosticGeometryOmission::UnappliedSkinning);
                first
            }
            MainGeometrySource::GroupZeroPartitions { first, .. } => {
                if transform[0x6c] != 0 {
                    output
                        .omissions
                        .push(DiagnosticGeometryOmission::UnappliedInstances(u32::from(
                            transform[0x6c],
                        )));
                }
                first
            }
            MainGeometrySource::StaticPartitions { first, .. } => first,
        };
        let mut chain_fallback = None;
        let chain = if let Some(c) = existing.and_then(|i| i.partitions.as_ref()) {
            c
        } else {
            let mut error = None;
            chain_fallback.replace(
                PartitionChain::copy_with_reader(
                    first,
                    copy.budget.partition_headers,
                    &mut |address| {
                        copy.read(address, 544).map_err(|e| {
                            error = Some(e);
                            Error::OutsideSnapshot
                        })
                    },
                )
                .inspect_err(budget_refusal)
                .map_err(|_| error.unwrap_or(DiagnosticGeometryOmission::PartitionChain))?,
            );
            chain_fallback.as_ref().expect("owned chain")
        };
        copy.budget.partition_headers = copy
            .budget
            .partition_headers
            .checked_sub(chain.partitions().len())
            .or_else(|| {
                budget_pressure();
                None
            })
            .ok_or(DiagnosticGeometryOmission::Budget)?;
        let mut vertex_byte_offset = Some(0u32);
        for (partition_ordinal, partition) in chain.partitions().iter().enumerate() {
            let auxiliary_offset = vertex_byte_offset;
            vertex_byte_offset = vertex_byte_offset.and_then(|offset| {
                word(partition.bytes(), 0x10)
                    .checked_add(15)
                    .and_then(|bytes| offset.checked_add(bytes & !15))
            });
            let mark = pending.len();
            let draw = (|| -> Result<DiagnosticMeshDraw, DiagnosticGeometryOmission> {
                let config = copy.stream(partition.bytes(), 19)?;
                let config = config
                    .get(..16)
                    .ok_or(DiagnosticGeometryOmission::SourceExtent)?;
                let count = usize::from(u16::from_be_bytes([config[8], config[9]]));
                let index_count = usize::from(u16::from_be_bytes([config[10], config[11]]));
                // Bytes the position and index decodes need from their own
                // counts; only consulted for a 16 KiB slot control.
                let position_bytes = match config[2] {
                    0 => count.saturating_mul(12),
                    6 => count.saturating_mul(6),
                    _ => 0,
                };
                let source = copy.stream_at_least(partition.bytes(), 7, position_bytes)?;
                let index_source =
                    copy.stream_at_least(partition.bytes(), 1, index_count.saturating_mul(2))?;
                let format = if config[2] == 255 {
                    Some(copy.stream_shared(partition.bytes(), 20)?)
                } else {
                    None
                };
                let output_bytes = count
                    .checked_mul(16)
                    .and_then(|n| n.checked_add(index_count * 2))
                    .filter(|&n| n <= copy.budget.bytes || budget_refused())
                    .ok_or(DiagnosticGeometryOmission::Budget)?;
                // Reserve the retained decoded output in the same explicit
                // native budget before allocating either output vector.
                copy.budget.bytes -= output_bytes;
                // Scalar layout metadata of this partition, reported once per
                // failing step (rate-limited); never vertex or index contents.
                let codec_failure = |step: &str, error: &GeometryStreamError| {
                    report_codec_failure(CodecFailure {
                        step,
                        error,
                        ordinal,
                        partition: partition_ordinal,
                        group: mesh.material_group(),
                        config,
                        header: partition.bytes(),
                        count,
                        index_count,
                        source_len: source.len(),
                        index_len: index_source.len(),
                        format: format.as_deref(),
                    })
                };
                let codec = match config[2] {
                    0 => Ok(RowCodec::Float32x3),
                    6 => Ok(RowCodec::Rebased16x3),
                    255 => decoded_geometry_cache::position_codec(
                        format.as_ref().expect("format read"),
                        word(partition.bytes(), 0x34 + 8 * 7),
                    ),
                    other => Err(GeometryStreamError::UnsupportedPositionEncoding(other)),
                }
                .inspect_err(|e| codec_failure("position-codec", e))
                .map_err(DiagnosticGeometryOmission::Codec)?;
                // Positions and every primary declaration decode the same
                // (format, source) pair: intern it once for all of them.
                let interned = format.as_ref().and_then(|format| {
                    decoded_geometry_cache::intern_pair_known(
                        format,
                        &source,
                        format.content(),
                        source.content(),
                    )
                });
                let positions = decoded_geometry_cache::rows_interned(
                    codec,
                    interned.as_ref(),
                    format.as_deref().unwrap_or(&[]),
                    &source,
                    count,
                    output_bytes,
                )
                .inspect_err(|e| codec_failure("positions", e))
                .map_err(DiagnosticGeometryOmission::Codec)?;
                let indices = decoded_geometry_cache::indices_known(
                    &index_source,
                    index_source.content(),
                    index_count,
                    output_bytes,
                )
                .inspect_err(|e| codec_failure("indices", e))
                .map_err(DiagnosticGeometryOmission::Codec)?;
                if indices.iter().any(|&i| usize::from(i) >= positions.len()) {
                    codec_failure("index-range", &GeometryStreamError::SourceExtent);
                    return Err(DiagnosticGeometryOmission::SourceExtent);
                }
                let attributes = format.as_ref().map(|format| {
                    let decoded = crate::geometry_attributes::decode_primary_attributes_interned(
                        format,
                        &source,
                        interned.as_ref(),
                        word(partition.bytes(), 0x34 + 8 * 7),
                        count,
                        copy.budget.bytes,
                    )
                    .inspect_err(budget_refusal);
                    if let Ok(ref attributes) = decoded {
                        copy.budget.bytes -= attributes.owned_bytes();
                    }
                    decoded
                });
                let secondary_sizes: [u16; 3] = core::array::from_fn(|i| {
                    let at = 0x0a + 2 * i;
                    u16::from_be_bytes([partition.bytes()[at], partition.bytes()[at + 1]])
                });
                let secondary_attributes = if secondary_sizes == [0; 3] {
                    None
                } else {
                    Some((|| {
                        if secondary_sizes[1..].iter().any(|&size| size != 0) {
                            return Err(SecondaryAttributeError::SplitInputChunks(secondary_sizes));
                        }
                        if config[3] != 255 {
                            return Err(SecondaryAttributeError::Builtin(config[3]));
                        }
                        let format = copy
                            .stream(partition.bytes(), 21)
                            .map_err(SecondaryAttributeError::Source)?;
                        let address = word(partition.bytes(), 0x34 + 8 * 10);
                        let source = copy
                            .read(address, usize::from(secondary_sizes[0]))
                            .map_err(SecondaryAttributeError::Source)?;
                        let decoded = decode_primary_attributes(
                            &format,
                            &source,
                            address,
                            count,
                            copy.budget.bytes,
                        )
                        .inspect_err(budget_refusal)
                        .map_err(SecondaryAttributeError::Codec)?;
                        copy.budget.bytes -= decoded.owned_bytes();
                        Ok(decoded)
                    })())
                };
                let output_bindings = if config[4] > 3 {
                    // Custom output layouts (configuration byte 4 above 3)
                    // are described by stream slot 0. Stream 22 is a
                    // different input, not this output format.
                    let control = word(partition.bytes(), 0x30);
                    let address = word(partition.bytes(), 0x34);
                    copy.stream(partition.bytes(), 0)
                        .map_err(|e| match e {
                            DiagnosticGeometryOmission::Budget => BindingError::Budget,
                            DiagnosticGeometryOmission::Source(e) => BindingError::Source(e),
                            DiagnosticGeometryOmission::UnsupportedControl { slot, control } => {
                                BindingError::SourceControl { slot, control }
                            }
                            _ => BindingError::OutputSourceExtent {
                                selector: config[4],
                                control,
                                address,
                            },
                        })
                        .and_then(|format| {
                            let decoded = geometry_bindings::decode_output_bindings(&format)
                                .map_err(|error| {
                                    if error == BindingError::Extent {
                                        BindingError::OutputDescriptorExtent {
                                            selector: config[4],
                                            control,
                                            address,
                                            bytes: format.len(),
                                            header: format.get(..8).map(|h| h.try_into().unwrap()),
                                            required_bytes: format
                                                .first()
                                                .map_or(8, |&rows| 8 + 8 * usize::from(rows)),
                                        }
                                    } else {
                                        error
                                    }
                                })?;
                            if !decoded.is_empty() {
                                return Ok(decoded);
                            }
                            let primary_mask = existing.and_then(|r| {
                                let selected = r.primary_technique.as_ref()?;
                                let material = r.primary_material.as_ref()?.bytes();
                                let map = material.get(0x48..0x4a)?;
                                let at = usize::from(u16::from_be_bytes(map.try_into().ok()?))
                                    .checked_add(
                                        (selected.source.selector as usize).checked_mul(32)?,
                                    )?;
                                let record = material.get(at..at.checked_add(32)?)?;
                                Some(word(record, 16))
                            });
                            let vf_masks =
                                copy.read(mesh.vertex_layout_address(), 656).ok().map(|vf| {
                                    let mut masks = [0u16; 4];
                                    for i in 0..16 {
                                        if word(&vf, 0x120 + 20 * i) != 0 {
                                            masks[0] |= 1 << i;
                                        }
                                        let state = word(&vf, 8 * i + 4);
                                        if state != 0 {
                                            masks[1] |= 1 << i;
                                        }
                                        if (state >> 4) & 15 != 0 {
                                            masks[2] |= 1 << i;
                                        }
                                        if word(&vf, 8 * i) == 0x00041740 + 4 * i as u32 {
                                            masks[3] |= 1 << i;
                                        }
                                    }
                                    masks
                                });
                            Err(BindingError::UnresolvedEmptyOutput {
                                selector: config[4],
                                control,
                                address,
                                bytes: format.len(),
                                header: format.iter().take(16).copied().collect(),
                                config: config.try_into().expect("checked config"),
                                auxiliary_header: partition.bytes()[0x110..0x118]
                                    .try_into()
                                    .unwrap(),
                                primary_mask,
                                vertex_layout_address: mesh.vertex_layout_address(),
                                vf_masks,
                            })
                        })
                } else {
                    geometry_bindings::builtin_output_bindings(config[4])
                };
                // Optional direct streams never erase an otherwise useful
                // diagnostic position draw: every failure remains owned.
                let auxiliary_attributes = (|| {
                    let _timer = crate::source_acquisition::section(
                        crate::source_acquisition::Section::AuxiliaryAttributes,
                    );
                    if partition.bytes()[0x112] == 0 {
                        return Ok(OwnedAuxiliaryAttributes::default());
                    }
                    let resource =
                        existing.ok_or(AuxiliaryError::Binding(BindingError::MissingTechnique))?;
                    let selected = resource
                        .primary_technique
                        .as_ref()
                        .ok_or(AuxiliaryError::Binding(BindingError::MissingTechnique))?;
                    let material = resource
                        .primary_material
                        .as_ref()
                        .ok_or(AuxiliaryError::Binding(BindingError::MissingTechnique))?
                        .bytes();
                    let map = material.get(0x48..0x4a).ok_or(AuxiliaryError::Extent)?;
                    let map = usize::from(u16::from_be_bytes(
                        map.try_into().expect("two source bytes"),
                    ));
                    let start = usize::try_from(selected.source.selector)
                        .ok()
                        .and_then(|n| n.checked_mul(32))
                        .and_then(|n| n.checked_add(map))
                        .ok_or(AuxiliaryError::Extent)?;
                    let record: [u8; 32] = material
                        .get(start..start.checked_add(32).ok_or(AuxiliaryError::Extent)?)
                        .ok_or(AuxiliaryError::Extent)?
                        .try_into()
                        .expect("complete source technique");
                    let technique_mask = word(&record, 16);
                    let vertex_state =
                        copy.read(mesh.vertex_layout_address(), 656)
                            .map_err(|e| match e {
                                DiagnosticGeometryOmission::Budget => AuxiliaryError::Budget,
                                DiagnosticGeometryOmission::Source(e) => AuxiliaryError::Source(e),
                                _ => AuxiliaryError::Extent,
                            })?;
                    let plan = geometry_bindings::plan_auxiliary_bindings(
                        partition.bytes(),
                        vertex_state
                            .as_slice()
                            .try_into()
                            .map_err(|_| AuxiliaryError::Extent)?,
                        word(mesh.bytes(), 0x4c),
                        auxiliary_offset.ok_or(AuxiliaryError::Binding(BindingError::Range))?,
                        technique_mask,
                    )
                    .map_err(AuxiliaryError::Binding)?;
                    let mut result = OwnedAuxiliaryAttributes {
                        primary_technique: Some(PrimaryTechniqueBinding {
                            selector: selected.source.selector,
                            record,
                        }),
                        ..Default::default()
                    };
                    result
                        .attributes
                        .try_reserve_exact(plan.len())
                        .map_err(|_| AuxiliaryError::Source(Error::AllocationFailed))?;
                    result
                        .failures
                        .try_reserve_exact(plan.len())
                        .map_err(|_| AuxiliaryError::Source(Error::AllocationFailed))?;
                    for binding in plan {
                        let decoded = (|| {
                            let bytes = geometry_bindings::auxiliary_source_bytes(&binding, count)
                                .map_err(AuxiliaryError::Binding)?;
                            let output = count.checked_mul(16).ok_or(AuxiliaryError::Budget)?;
                            let charge = bytes
                                .checked_add(output)
                                .filter(|&n| n <= copy.budget.bytes || budget_refused())
                                .ok_or(AuxiliaryError::Budget)?;
                            if copy.budget.reads == 0 {
                                budget_pressure();
                                return Err(AuxiliaryError::Budget);
                            }
                            if u64::from(binding.offset) + bytes as u64 > 0x8000_0000 {
                                return Err(AuxiliaryError::Binding(BindingError::Range));
                            }
                            copy.budget.bytes -= charge;
                            copy.budget.reads -= 1;
                            let source = located_reader(LocatedTextureRequest {
                                location: if binding.location == 0 {
                                    TextureLocation::Local
                                } else {
                                    TextureLocation::Main
                                },
                                offset: binding.offset,
                                bytes,
                            })
                            .map_err(AuxiliaryError::Located)?;
                            if source.len() != bytes {
                                return Err(AuxiliaryError::Extent);
                            }
                            copy.bytes += bytes;
                            copy.reads += 1;
                            decoded_geometry_cache::auxiliary(&binding, &source, count, output)
                                .map(std::sync::Arc::unwrap_or_clone)
                                .map_err(AuxiliaryError::Binding)
                        })();
                        match decoded {
                            Ok(words) => result.attributes.push(DecodedAuxiliaryAttribute {
                                source: binding,
                                words,
                            }),
                            Err(error) => result.failures.push(AuxiliaryFailure {
                                source: binding,
                                error,
                            }),
                        }
                    }
                    Ok(result)
                })();
                let mut auxiliary_attributes = auxiliary_attributes;
                if let (Some(descriptor), Ok(auxiliary)) =
                    (&slot_six_descriptor, auxiliary_attributes.as_mut())
                {
                    attach_slot_six(
                        descriptor,
                        word(partition.bytes(), 0xf8),
                        count,
                        &mut *copy,
                        located_reader,
                        auxiliary,
                    );
                }
                let projection = core::array::from_fn(|r| {
                    core::array::from_fn(|c| word(transform, 32 + 16 * r + 4 * c))
                });
                let mut draw = DiagnosticMeshDraw {
                    partition_ordinal,
                    positions,
                    indices,
                    attributes,
                    secondary_attributes,
                    prepared_group_zero: None,
                    prepared_direct: None,
                    prepared_noninstanced: None,
                    output_bindings,
                    auxiliary_attributes,
                    retained_projection_vectors: projection,
                };
                if mesh.material_group() == 0 {
                    draw.prepared_group_zero = Some(capture_group_zero(
                        &mut *copy,
                        located_reader,
                        GroupZeroCaptureInput {
                            source_pass_address,
                            resource_base,
                            mesh,
                            partition: partition.bytes(),
                            transform,
                            mbi: &item.mbi,
                            material: existing.and_then(|i| i.primary_material.as_ref()),
                            draw: &draw,
                        },
                    ));
                }
                if matches!(mesh.material_group(), 3 | 4)
                    || (palette_skinned_group(mesh.material_group())
                        && !matches!(dispatch, MainGeometrySource::StaticPartitions { .. }))
                {
                    draw.prepared_noninstanced = match capture_noninstanced(
                        &mut *copy,
                        GroupZeroCaptureInput {
                            source_pass_address,
                            resource_base,
                            mesh,
                            partition: partition.bytes(),
                            transform,
                            mbi: &item.mbi,
                            material: existing.and_then(|i| i.primary_material.as_ref()),
                            draw: &draw,
                        },
                        defer.then_some(ordinal),
                    ) {
                        Ok(NoninstancedOutcome::Ready(prepared)) => Some(Ok(prepared)),
                        Ok(NoninstancedOutcome::Deferred(job)) => {
                            pending.push(job);
                            None
                        }
                        Err(error) => Some(Err(error)),
                    };
                }
                if mesh.material_group() != 0
                    && !matches!(mesh.material_group(), 3 | 4)
                    && matches!(dispatch, MainGeometrySource::StaticPartitions { .. })
                {
                    draw.prepared_noninstanced = Some(capture_static(
                        &mut *copy,
                        GroupZeroCaptureInput {
                            source_pass_address,
                            resource_base,
                            mesh,
                            partition: partition.bytes(),
                            transform,
                            mbi: &item.mbi,
                            material: existing.and_then(|i| i.primary_material.as_ref()),
                            draw: &draw,
                        },
                    ));
                }
                Ok(draw)
            })();
            match draw {
                Ok(draw) => {
                    for job in &mut pending[mark..] {
                        job.draw = output.draws.len();
                    }
                    output.draws.push(draw);
                }
                Err(error) => {
                    for job in pending.drain(mark..) {
                        copy.budget.bytes += job.charge;
                        copy.refunded += job.charge;
                    }
                    output.omissions.push(error);
                }
            }
        }
        if matches!(dispatch, MainGeometrySource::SkinnedPartitions { .. })
            && !output.draws.is_empty()
            && output.draws.len() == chain.partitions().len()
            && output.draws.iter().all(
                |d| matches!(&d.prepared_noninstanced, Some(Ok(p)) if p.skinning_source.is_some()),
            )
        {
            output
                .omissions
                .retain(|e| *e != DiagnosticGeometryOmission::UnappliedSkinning);
        }
        Ok(())
    })();
    if let Err(error) = work {
        output.omissions.push(error);
    }
    output
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct MetalViewportDepth {
    /// Actual source draw-state viewport Z values, before the SDK's zero-scale
    /// fallback. Supplying these bytes is not a target/order qualification.
    pub source_scale: u32,
    pub source_offset: u32,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DiagnosticProjectionError {
    Budget,
    AllocationFailed,
    NonFiniteInput,
    NonFiniteClip,
}
/// Project the diagnostic's source model points into final Metal/native clip.
/// The source matrix convention is established by the actual VP position
/// instructions and authored producer upload contract. Real current3D viewport
/// state MUST come from the qualified FIFO/target handoff, never a default or
/// later HUD state. The game's own projection preparation remains retained work.
/// This is diagnostic ordinary binary32 arithmetic, not full RSX rounding or
/// material shader/deformation parity.
pub fn project_metal_diagnostic(
    draw: &DiagnosticMeshDraw,
    viewport: MetalViewportDepth,
    byte_budget: usize,
) -> Result<Vec<[f32; 4]>, DiagnosticProjectionError> {
    let bytes = draw
        .positions
        .len()
        .checked_mul(16)
        .filter(|&n| n <= byte_budget)
        .ok_or(DiagnosticProjectionError::Budget)?;
    let raw_scale = f32::from_bits(viewport.source_scale);
    let (scale, offset) = if raw_scale == 0.0 {
        (1.0, 0.0)
    } else {
        (raw_scale, f32::from_bits(viewport.source_offset))
    };
    let columns = draw
        .retained_projection_vectors
        .map(|v| v.map(f32::from_bits));
    if !scale.is_finite() || !offset.is_finite() || columns.iter().flatten().any(|x| !x.is_finite())
    {
        return Err(DiagnosticProjectionError::NonFiniteInput);
    }
    let mut out = Vec::new();
    out.try_reserve_exact(bytes / 16)
        .map_err(|_| DiagnosticProjectionError::AllocationFailed)?;
    for p in draw.positions.iter() {
        let p = p.map(f32::from_bits);
        if p[..3].iter().any(|x| !x.is_finite()) {
            return Err(DiagnosticProjectionError::NonFiniteInput);
        }
        let mut clip = core::array::from_fn(|lane| {
            let y = p[1] * columns[1][lane];
            let xy = p[0] * columns[0][lane] + y;
            let xyz = p[2] * columns[2][lane] + xy;
            // The observed VP adds c6 directly; raw input W is not read.
            xyz + columns[3][lane]
        });
        clip[2] = clip[2] * scale + clip[3] * offset;
        if clip.iter().any(|x| !x.is_finite()) {
            return Err(DiagnosticProjectionError::NonFiniteClip);
        }
        out.push(clip);
    }
    Ok(out)
}

#[cfg(test)]
mod pending_noninstanced_tests {
    use super::*;
    use std::sync::Arc;

    fn bank(n: usize, seed: u32, f: impl Fn(f32) -> f32) -> Arc<Vec<[u32; 4]>> {
        let mut x = seed | 1;
        let mut r = move || {
            x ^= x << 13;
            x ^= x >> 17;
            x ^= x << 5;
            (x % 2000) as f32 / 1000.0 - 1.0
        };
        Arc::new(
            (0..n)
                .map(|_| {
                    [
                        f(r()).to_bits(),
                        f(r()).to_bits(),
                        f(r()).to_bits(),
                        1f32.to_bits(),
                    ]
                })
                .collect(),
        )
    }
    fn job(ordinal: usize, group: u8, n: usize, scale: f32) -> PendingNoninstanced {
        let f = |v: f32| v;
        PendingNoninstanced {
            ordinal,
            draw: 0,
            group,
            positions: bank(n, 3 + ordinal as u32, move |v| v * scale),
            normals: bank(n, 5 + ordinal as u32, f),
            weights: bank(n, 7 + ordinal as u32, f32::abs),
            tangents: (group == 4).then(|| bank(n, 11 + ordinal as u32, f)),
            material_rows: [
                [0.3f32, 0.2, 0.5, 0.1].map(f32::to_bits),
                [1.5f32, 0.7, 2.0, 0.0].map(f32::to_bits),
                [0.9f32, 0.4, 3.0, 0.0].map(f32::to_bits),
            ],
            wind: [1.1f32, 0.0, 0.4, 0.6, 123.25].map(f32::to_bits),
            object_origin: [10.5f32, 3.25, -7.0].map(f32::to_bits),
            pass_address: 0x1000,
            mode: 8,
            charge: n * if group == 4 { 64 } else { 48 },
        }
    }
    fn inline(j: &PendingNoninstanced) -> Result<Vec<[u32; 4]>, NoninstancedSourceError> {
        noninstanced::prepare_noninstanced_geometry(
            noninstanced::NoninstancedGeometryInput {
                group: j.group,
                positions: &j.positions,
                normals: &j.normals,
                weights: &j.weights,
                tangents: j.tangents.as_deref().map(Vec::as_slice),
                material_rows: j.material_rows,
                wind: j.wind,
                object_origin: j.object_origin,
            },
            usize::MAX,
        )
        .map(|g| g.positions)
        .map_err(NoninstancedSourceError::Native)
    }

    #[test]
    fn parallel_pending_preparation_is_bit_identical_to_the_inline_call_in_job_order() {
        // Enough total vertices to use helper threads, plus one late refusal
        // (finite inputs whose displaced positions overflow binary32, which
        // the deformation refuses) that must stay Err.
        let mut jobs: Vec<_> = (0..40)
            .map(|i| job(i, if i % 3 == 0 { 4 } else { 3 }, 97 + i * 13, 1.0))
            .collect();
        let mut overflow = job(40, 3, 64, 3.3e38);
        overflow.material_rows[0][0] = 1.0e38f32.to_bits();
        jobs.push(overflow);
        let parallel = run_pending_noninstanced(&jobs);
        assert_eq!(parallel.len(), jobs.len());
        let mut failures = 0;
        for (j, result) in jobs.iter().zip(&parallel) {
            let expected = inline(j);
            match (result, &expected) {
                (Ok(p), Ok(e)) => {
                    assert_eq!(&p.geometry.positions, e);
                    assert_eq!(p.geometry.normals, *j.normals);
                    assert_eq!(p.geometry.weights, *j.weights);
                    assert_eq!(p.geometry.tangents.as_ref(), j.tangents.as_deref());
                    assert_eq!((p.pass_address, p.mode), (0x1000, 8));
                }
                (Err(a), Err(b)) => {
                    assert_eq!(a, b);
                    failures += 1;
                }
                _ => panic!("parallel and inline outcomes differ for job {}", j.ordinal),
            }
        }
        assert_eq!(failures, 1);
        // Small batches run inline on the calling thread with equal results.
        let small = [job(0, 3, 5, 1.0)];
        let one = run_pending_noninstanced(&small);
        assert_eq!(
            one[0].as_ref().unwrap().geometry.positions,
            inline(&small[0]).unwrap()
        );
    }

    #[test]
    fn validation_then_compute_equals_prepare_including_early_refusals() {
        let j = job(0, 4, 50, 1.0);
        let mut input = noninstanced::NoninstancedGeometryInput {
            group: 4,
            positions: &j.positions,
            normals: &j.normals,
            weights: &j.weights,
            tangents: None, // group4 without tangents is an early refusal
            material_rows: j.material_rows,
            wind: j.wind,
            object_origin: j.object_origin,
        };
        assert_eq!(
            noninstanced::validate_noninstanced_geometry(&input, usize::MAX).err(),
            noninstanced::prepare_noninstanced_geometry(
                noninstanced::NoninstancedGeometryInput { ..input },
                usize::MAX
            )
            .err()
        );
        input.tangents = j.tangents.as_deref().map(Vec::as_slice);
        // A budget one byte short refuses in validation, before arithmetic.
        assert_eq!(
            noninstanced::validate_noninstanced_geometry(&input, 50 * 64 - 1),
            Err(noninstanced::NoninstancedGeometryError::Budget)
        );
    }
}

#[cfg(test)]
mod large_stream_tests {
    use super::*;
    fn header(control: u32, address: u32) -> [u8; 544] {
        let mut h = [0u8; 544];
        h[0x30 + 8 * 7..0x34 + 8 * 7].copy_from_slice(&control.to_be_bytes());
        h[0x34 + 8 * 7..0x38 + 8 * 7].copy_from_slice(&address.to_be_bytes());
        h
    }
    /// Requested sizes for one stream read of slot 7 needing `required`.
    fn requested(control: u32, required: usize) -> (Vec<usize>, usize) {
        let mut seen = Vec::new();
        let mut read = |r: StreamRequest| -> Result<Vec<u8>, Error> {
            seen.push(r.bytes);
            Ok(vec![0x5a; r.bytes])
        };
        let mut copy = Copy {
            read: &mut read,
            pass_snapshot: None,
            budget: ResourceCopyBudget {
                bytes: 64 << 20,
                reads: 64,
                partition_headers: 8,
            },
            bytes: 0,
            reads: 0,
            refunded: 0,
        };
        let got = copy
            .stream_at_least(&header(control, 0x10000), 7, required)
            .unwrap()
            .len();
        assert_eq!(copy.bytes, got, "charged exactly what was read");
        (seen, got)
    }
    #[test]
    fn a_16_kib_control_is_extended_to_what_the_counts_need() {
        // 1,728 Float32x3 positions need 20,736 bytes behind a 16 KiB control.
        assert_eq!(requested(16384, 20736), (vec![20736], 20736));
        // 9,726 indices need 19,452 bytes.
        assert_eq!(requested(16384, 19452), (vec![19452], 19452));
    }
    #[test]
    fn every_other_control_is_still_the_stream_size() {
        assert_eq!(requested(16384, 16000), (vec![16384], 16384));
        assert_eq!(requested(16384, 0), (vec![16384], 16384));
        assert_eq!(requested(16383, 20736), (vec![16383], 16383));
        assert_eq!(requested(1656, 1656), (vec![1656], 1656));
        // Beyond the bound the control is kept (the decode then refuses).
        assert_eq!(
            requested(16384, LARGE_STREAM_LIMIT + 1),
            (vec![16384], 16384)
        );
        assert_eq!(requested(0, 20736), (vec![], 0));
    }
}
