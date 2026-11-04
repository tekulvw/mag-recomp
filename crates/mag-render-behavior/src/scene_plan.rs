//! Main scene planning (`scene-plan.md`): the planner's own rules.
//!
//! This module holds the planner core as pure functions over plain records: association, the
//! item traversal with its ordered producer events and pass openings, shader controls,
//! texture-unit binding and packet-image resolution, per-draw textures, the attribute-6 compile
//! view, the program variant ladder, statistics and retained clear replay. Writing the scene
//! packet itself (the `mag-render-abi` builders) is the adapter's integration step.

use crate::program_compilation::{ShaderControls, Variant};
use crate::setup::{Comparison, PipelineState};
use crate::texture::{
    DecodedUnit, DepthCompare, DepthCompareSignature, TextureUnitRecord, ViewParameters, decode,
    native_sampler, view_parameters,
};
use mag_render_abi::packet::Sampler;

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum PlanError {
    Association,
    Source(&'static str),
    Unsupported(&'static str),
    MaterialValues { stage: &'static str },
    GroupRelationship { census: u32, effective: u32 },
    GroupOrder { previous: i32, group: u32 },
    Sampler(String),
    AlphaReference(String),
}

// --- 1. Association -------------------------------------------------------------------------

/// The identity of a Main source window.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct SourceIdentity {
    pub engine_update: u64,
    pub cohort: u64,
    pub publication: u64,
    pub pass_occurrence: u64,
}

/// The cohort header.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct CohortHeader {
    pub source_invocation: u64,
    pub execution_update: u64,
    pub pass_occurrence: u64,
}

/// Checks the window association. `material_ordinals` and `geometry_ordinals` are the ordinals
/// each list entry carries, in list order.
#[allow(clippy::too_many_arguments)]
pub fn check_association(
    source: &SourceIdentity,
    header: &CohortHeader,
    sample_grid: (u32, u32),
    viewport_grid: (u32, u32),
    items: usize,
    material_ordinals: &[usize],
    geometry_ordinals: &[usize],
    setup_sources_present: bool,
) -> Result<(), PlanError> {
    let matches = source.cohort == header.source_invocation
        && source.engine_update == header.execution_update
        && source.pass_occurrence == header.pass_occurrence
        && source.publication != 0
        && sample_grid == viewport_grid
        && material_ordinals.len() == items
        && geometry_ordinals.len() == items
        && material_ordinals.iter().enumerate().all(|(i, o)| *o == i)
        && geometry_ordinals.iter().enumerate().all(|(i, o)| *o == i);
    if !matches {
        return Err(PlanError::Association);
    }
    if !setup_sources_present {
        return Err(PlanError::Source("Main setup sources"));
    }
    Ok(())
}

// --- 2–4. Traversal and producer events --------------------------------------------------------

/// Producer events, in the order the planner raises them.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ProducerEvent {
    Initial { slot: u32, target: u32 },
    Group12Textures,
    Group13 { target: u32 },
    Group15 { slot: u32, target: u32 },
    Finish { previous_group: i32 },
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum NoDrawReason {
    ListsDisabled,
    Repeated,
    UpdateOnly,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Route {
    Repeated,
    UpdateOnly,
    Prepare,
    Special14,
    Special15,
}

/// One step of the plan, in order. The caller performs each step (applying transitions,
/// producer effects and prepared items) and builds the packet.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PlanStep {
    Event(ProducerEvent),
    /// Write the two water-constant rows into vertex rows 46 and 47.
    WaterRows,
    /// The state transition `from → to` of `main-group-transitions.md`, with the setup
    /// references whose effects were emitted as events cleared.
    Transition {
        from: i32,
        to: u32,
    },
    /// Open a pass with the current viewport (asking the effects for current plan inputs).
    OpenPass,
    NoDraw {
        item: usize,
        reason: NoDrawReason,
    },
    /// An item record with zero unresolved obligations, followed by the prepared item.
    Prepare {
        item: usize,
        group: u32,
    },
    /// The material value state observes an update-only item.
    ObserveUpdateOnly {
        item: usize,
    },
}

/// The window's setup references.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct SetupReferences {
    pub lists_enabled: bool,
    pub initial: [u32; 3],
    pub group_thirteen: u32,
    pub group_fifteen: [u32; 2],
}

/// The draw sources a material entry carries.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct DrawSources {
    pub group: u32,
    pub transform: [u8; 112],
    pub pass_control: u32,
}

/// One cohort item with its material entry.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct TraversalItem {
    pub mbi: [u8; 32],
    pub transform: Option<[u8; 112]>,
    /// `None` is a refused material entry; `Some(None)` has no draw sources.
    pub material: Option<Option<DrawSources>>,
}

impl TraversalItem {
    pub fn census_group(&self) -> u32 {
        (self.mbi[0] >> 4) as u32
    }

    pub fn mesh_key(&self) -> u32 {
        u32::from_be_bytes([self.mbi[12], self.mbi[13], self.mbi[14], self.mbi[15]])
            .wrapping_mul(16)
    }
}

/// Plan statistics.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct MainPlanStats {
    pub intended: u32,
    pub source_disabled: u32,
    pub repeated: u32,
    pub update_only: u32,
    pub emitted_items: u32,
    pub draws: u32,
    pub approximated_fragment_draws: u32,
    pub restored_source_draws: u32,
    pub base_colour_draws: u32,
    pub restored_discarding_draws: u32,
    pub fallback_reasons: std::collections::BTreeMap<String, u32>,
    pub approximated_vertex_outputs: u32,
    pub approximated_position_bindings: u32,
    pub attribute6_approximations: u32,
    pub attribute6_stream_approximations: u32,
    pub owned_stream6_draws: u32,
    pub relaxed_depth_equal_draws: u32,
    pub omitted_builtin_requests: u32,
    pub materials_with_omissions: u32,
}

fn equal_or_both_tiny(a: u32, b: u32) -> bool {
    a == b || ((a >> 23) & 0xff == 0 && (b >> 23) & 0xff == 0)
}

fn leading_words(transform: &[u8; 112]) -> [u32; 8] {
    crate::guest::be_words(transform, 0)
}

/// Decides an item's route against the last committed item and the current group.
pub fn route(
    item: &TraversalItem,
    group: u32,
    pass_control: u32,
    committed: Option<(u32, [u8; 112])>,
    current_group: i32,
) -> Result<Route, PlanError> {
    let census = item.census_group();
    if census != group {
        return Err(PlanError::GroupRelationship {
            census,
            effective: group,
        });
    }
    let transform = item.transform.as_ref().expect("checked by the caller");
    if transform[111] != 1
        && let Some((key, previous)) = committed
    {
        let (a, b) = (leading_words(transform), leading_words(&previous));
        if key == item.mesh_key() && a.iter().zip(&b).all(|(x, y)| equal_or_both_tiny(*x, *y)) {
            return Ok(Route::Repeated);
        }
    }
    if (group as i64) < current_group as i64 {
        return Err(PlanError::GroupOrder {
            previous: current_group,
            group,
        });
    }
    Ok(match (group, pass_control) {
        (0, c) if c <= 2 => Route::UpdateOnly,
        (0..=13, _) => Route::Prepare,
        (14, _) => Route::Special14,
        (15, _) => Route::Special15,
        _ => return Err(PlanError::Source("group")),
    })
}

/// The steps of a group transition from `old` (−1 before any item) to `new`: producer events,
/// water rows and the per-group state transitions, in order. Any event closes the open pass.
pub fn transition_steps(old: i32, new: u32, setup: &SetupReferences) -> Vec<PlanStep> {
    let mut steps = Vec::new();
    if old == -1 {
        steps.push(PlanStep::Transition { from: -1, to: 0 });
    }
    for g in (old + 1).max(1) as u32..=new {
        if setup.lists_enabled {
            if g == 12 {
                steps.push(PlanStep::Event(ProducerEvent::Group12Textures));
                steps.push(PlanStep::WaterRows);
            }
            if g == 13 && setup.group_thirteen != 0 {
                steps.push(PlanStep::Event(ProducerEvent::Group13 {
                    target: setup.group_thirteen,
                }));
            }
            if g == 15 {
                for (slot, target) in setup.group_fifteen.iter().enumerate() {
                    if *target != 0 {
                        steps.push(PlanStep::Event(ProducerEvent::Group15 {
                            slot: slot as u32,
                            target: *target,
                        }));
                    }
                }
            }
        }
        steps.push(PlanStep::Transition {
            from: g as i32 - 1,
            to: g,
        });
    }
    steps
}

/// Walks the cohort and returns the ordered plan steps and the traversal statistics.
pub fn traverse(
    items: &[TraversalItem],
    setup: &SetupReferences,
    pass_control: u32,
) -> Result<(Vec<PlanStep>, MainPlanStats), PlanError> {
    let mut steps = Vec::new();
    let mut stats = MainPlanStats {
        intended: items.len() as u32,
        ..Default::default()
    };
    let mut pass_open = false;
    let emit = |steps: &mut Vec<PlanStep>, pass_open: &mut bool, event| {
        steps.push(PlanStep::Event(event));
        *pass_open = false;
    };
    if setup.lists_enabled {
        for (slot, target) in setup.initial.iter().enumerate() {
            if *target != 0 {
                emit(
                    &mut steps,
                    &mut pass_open,
                    ProducerEvent::Initial {
                        slot: slot as u32,
                        target: *target,
                    },
                );
            }
        }
    }
    let mut committed: Option<(u32, [u8; 112])> = None;
    let mut current_group: i32 = -1;
    for (index, item) in items.iter().enumerate() {
        if !setup.lists_enabled {
            if !pass_open {
                steps.push(PlanStep::OpenPass);
                pass_open = true;
            }
            steps.push(PlanStep::NoDraw {
                item: index,
                reason: NoDrawReason::ListsDisabled,
            });
            stats.source_disabled += 1;
            continue;
        }
        let material = item.material.ok_or(PlanError::Source("material item"))?;
        let sources = material.ok_or(PlanError::MaterialValues {
            stage: "captured-draw-source",
        })?;
        let transform = item.transform.ok_or(PlanError::Source("transform"))?;
        if sources.transform != transform || sources.pass_control != pass_control {
            return Err(PlanError::Association);
        }
        let group = sources.group;
        let route = route(item, group, pass_control, committed, current_group)?;
        if route != Route::Repeated {
            for step in transition_steps(current_group, group, setup) {
                if matches!(step, PlanStep::Event(_)) {
                    pass_open = false;
                }
                steps.push(step);
            }
        }
        if !pass_open {
            steps.push(PlanStep::OpenPass);
            pass_open = true;
        }
        match route {
            Route::Repeated => {
                steps.push(PlanStep::NoDraw {
                    item: index,
                    reason: NoDrawReason::Repeated,
                });
                stats.repeated += 1;
                continue;
            }
            Route::UpdateOnly => {
                steps.push(PlanStep::NoDraw {
                    item: index,
                    reason: NoDrawReason::UpdateOnly,
                });
                steps.push(PlanStep::ObserveUpdateOnly { item: index });
                stats.update_only += 1;
            }
            Route::Special14 | Route::Special15 => {
                return Err(PlanError::Unsupported("special Main caller"));
            }
            Route::Prepare => {
                steps.push(PlanStep::Prepare { item: index, group });
                stats.emitted_items += 1;
            }
        }
        committed = Some((item.mesh_key(), transform));
        current_group = group as i32;
    }
    steps.push(PlanStep::Event(ProducerEvent::Finish {
        previous_group: current_group,
    }));
    Ok((steps, stats))
}

// --- 6. Shader controls ----------------------------------------------------------------------

/// Shader controls with the alpha constant.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ControlsWithAlpha {
    pub controls: ShaderControls,
    pub alpha_constant: [u32; 4],
    /// Unsupported depth comparisons, for the caller's rate-limited diagnostics.
    pub depth_diagnostics: Vec<DepthCompareSignature>,
}

fn comparison_index(c: Comparison) -> u32 {
    crate::raster::comparison_code(c)
}

/// Builds shader controls for the units in `used`.
///
/// `alpha_reference` is the public `mag_render_programs::fragment_alpha_reference` conversion
/// (reference word, source colour format).
#[allow(clippy::too_many_arguments)]
pub fn shader_controls(
    state: &PipelineState,
    units: &[TextureUnitRecord; 16],
    used: u16,
    sample_grid: (u32, u32),
    source_colour_format: u32,
    alpha_window_tail: [u32; 3],
    gamma_off_mask: u16,
    alpha_reference: &dyn Fn(u32, u32) -> Result<u32, String>,
) -> Result<ControlsWithAlpha, PlanError> {
    let alpha_function = match state.alpha_test {
        None => return Err(PlanError::Source("alpha enable")),
        Some(false) => None,
        Some(true) => {
            let c = state
                .alpha_comparison
                .ok_or(PlanError::Source("alpha comparison"))?;
            Some(0x200 + comparison_index(c))
        }
    };
    let word0 = match alpha_function {
        None | Some(0x200) | Some(0x207) => 0,
        Some(_) => {
            let reference = state
                .alpha_reference
                .ok_or(PlanError::Source("alpha reference"))?;
            alpha_reference(reference, source_colour_format).map_err(PlanError::AlphaReference)?
        }
    };
    let srgb_output = state.srgb_output.ok_or(PlanError::Source("sRGB output"))?;
    let mut controls = flat_controls(sample_grid);
    controls.alpha_function = alpha_function;
    controls.srgb_output = srgb_output;
    let mut depth_diagnostics = Vec::new();
    for unit in 0..16 {
        if used & (1 << unit) == 0 {
            continue;
        }
        let c = match decode(&units[unit]).map_err(|e| PlanError::Sampler(format!("{e:?}")))? {
            DecodedUnit::Disabled(_) => {
                return Err(PlanError::Unsupported("sampled disabled texture"));
            }
            DecodedUnit::Enabled(c) => c,
        };
        if c.alpha_kill {
            return Err(PlanError::Unsupported("texture alpha kill"));
        }
        if c.dimension != 2 {
            return Err(PlanError::Unsupported("texture dimension"));
        }
        let bit = 1u16 << unit;
        if c.cube {
            controls.cube_mask |= bit;
        }
        if c.unnormalized {
            controls.unnormalized_mask |= bit;
        }
        controls.gamma[unit] = if gamma_off_mask & bit != 0 {
            0
        } else {
            c.gamma_mask
        };
        controls.expand[unit] = c.expand_mask;
        controls.texture_dimensions[unit] = (c.words[6] >> 16, c.words[6] & 0xffff);
        match crate::texture::depth_compare(&c) {
            DepthCompare::Qualified(r) => {
                controls.shadow[unit] = (r.enabled, r.function, r.linear, r.unnormalized)
            }
            DepthCompare::Unsupported(signature) => depth_diagnostics.push(signature),
            DepthCompare::None => {}
        }
    }
    Ok(ControlsWithAlpha {
        controls,
        alpha_constant: [
            word0,
            alpha_window_tail[0],
            alpha_window_tail[1],
            alpha_window_tail[2],
        ],
        depth_diagnostics,
    })
}

/// The flat controls: no alpha function, sRGB off, zero masks, the plan's sample grid.
pub fn flat_controls(sample_grid: (u32, u32)) -> ShaderControls {
    ShaderControls {
        vertex_texture_mask: 0,
        enabled_arrays: 0,
        cube_mask: 0,
        volume_mask: 0,
        unnormalized_mask: 0,
        shadow: [(0, 0, 0, 0); 16],
        gamma: [0; 16],
        expand: [0; 16],
        alpha_function: None,
        srgb_output: false,
        sample_grid,
        texture_dimensions: [(1, 1); 16],
    }
}

// --- 7. Texture-unit binding -----------------------------------------------------------------

/// Packet image format codes.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ImageFormat {
    Rgba8 = 1,
    R8 = 4,
    Rg8 = 5,
    Bc1 = 6,
    Bc2 = 7,
    Bc3 = 8,
    R16 = 9,
    Rg16 = 10,
    Rgba16f = 11,
    Rgba32f = 12,
    R32f = 13,
    Rg16f = 14,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Selection {
    ResidentObject,
    Fallback,
    Producer,
    ProducerAsset,
}

/// A CPU image produced for a unit.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct CpuImage {
    pub descriptor: [u32; 8],
    pub format: ImageFormat,
    /// Index of the image once added to the packet.
    pub packet_image: Option<u32>,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum CpuImageError {
    MissingSource,
    Failed(String),
}

/// An image in the packet being built.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PacketImage {
    pub format: ImageFormat,
    pub samples: u32,
    pub depth: u32,
    pub dimension: u32,
    pub layers: u32,
    pub mip_count: u32,
    pub width: u32,
    pub height: u32,
}

/// The packet being built and its identity.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct PacketContext {
    pub packet: u64,
    pub source: u64,
    pub images: Vec<PacketImage>,
}

/// An attached packet image.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PacketImageBinding {
    pub packet: u64,
    pub source: u64,
    pub image: u32,
    pub storage: [u32; 4],
    pub format: ImageFormat,
}

/// One texture unit's carried state.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct UnitState {
    pub record: TextureUnitRecord,
    pub cpu_image: Option<Result<CpuImage, CpuImageError>>,
    pub packet_image: Option<PacketImageBinding>,
    pub selection: Option<Selection>,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum BindingPayload {
    Cpu(Result<CpuImage, CpuImageError>),
    NativeProducer,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct TextureBindingRequest {
    pub unit: u32,
    pub selection: Selection,
    pub descriptor: [u32; 8],
    pub inherited_border: Option<u32>,
    pub payload: BindingPayload,
}

/// The storage words of a descriptor: `w[0]`, `w[1]`, `w[6]`, `w[7]`.
pub fn storage_words(w: &[u32; 8]) -> [u32; 4] {
    [w[0], w[1], w[6], w[7]]
}

/// Binds a texture record to a unit. All checks happen before any change.
pub fn bind(units: &mut [UnitState; 16], request: TextureBindingRequest) -> Result<(), PlanError> {
    let unit = request.unit as usize;
    if unit >= 16 {
        return Err(PlanError::Source("texture unit"));
    }
    if request.selection == Selection::ProducerAsset {
        let base = ((request.descriptor[1] >> 8) & 0xff) & !0x20;
        let expected = match base {
            0x86 => ImageFormat::Bc1,
            0x87 => ImageFormat::Bc2,
            0x88 => ImageFormat::Bc3,
            _ => return Err(PlanError::Association),
        };
        match &request.payload {
            BindingPayload::Cpu(Ok(image))
                if image.descriptor != request.descriptor || image.format != expected =>
            {
                return Err(PlanError::Association);
            }
            BindingPayload::Cpu(_) => {}
            BindingPayload::NativeProducer => return Err(PlanError::Association),
        }
    }
    if request.payload == BindingPayload::NativeProducer && request.selection != Selection::Producer
    {
        return Err(PlanError::Association);
    }
    let state = &mut units[unit];
    state.record.apply_compact(&request.descriptor);
    state.packet_image = None;
    if let Some(border) = request.inherited_border {
        state.record.border = Some(border);
    }
    state.cpu_image = match request.payload {
        BindingPayload::Cpu(result) => Some(result),
        BindingPayload::NativeProducer => None,
    };
    state.selection = Some(request.selection);
    Ok(())
}

/// The resolve check of a packet image binding against descriptor `w`.
pub fn resolve(
    binding: &PacketImageBinding,
    w: &[u32; 8],
    packet: &PacketContext,
) -> Result<PacketImage, PlanError> {
    let image = *packet
        .images
        .get(binding.image as usize)
        .ok_or(PlanError::Association)?;
    let cube = w[1] & 4 != 0;
    let shape_ok = image.samples == 1
        && image.depth == 1
        && if cube {
            image.dimension == 4 && image.layers == 6
        } else {
            image.dimension == 2 && image.layers == 1
        }
        && image.mip_count == w[1] >> 16
        && image.width == w[6] >> 16
        && image.height == w[6] & 0xffff;
    let identity_ok = binding.packet == packet.packet
        && binding.source == packet.source
        && binding.storage == storage_words(w)
        && binding.format == image.format;
    if !shape_ok || !identity_ok {
        return Err(PlanError::Association);
    }
    Ok(image)
}

/// Attaches an image already in this packet to a unit.
pub fn bind_packet_image(
    units: &mut [UnitState; 16],
    unit: u32,
    packet: &PacketContext,
    descriptor: &[u32; 8],
    image: u32,
) -> Result<(), PlanError> {
    let state = units.get_mut(unit as usize).ok_or(PlanError::Association)?;
    state.packet_image = None;
    state.cpu_image = None;
    let current = state.record.compact.ok_or(PlanError::Association)?;
    if storage_words(&current) != storage_words(descriptor) {
        return Err(PlanError::Association);
    }
    let format = packet
        .images
        .get(image as usize)
        .ok_or(PlanError::Association)?
        .format;
    let binding = PacketImageBinding {
        packet: packet.packet,
        source: packet.source,
        image,
        storage: storage_words(descriptor),
        format,
    };
    resolve(&binding, descriptor, packet)?;
    state.packet_image = Some(binding);
    Ok(())
}

// --- 8. One draw ------------------------------------------------------------------------------

/// The attribute-6 default of the compile view: `[0, 1, 0, 0.5]`.
pub const ATTRIBUTE6_DEFAULT: [u32; 4] = [0, 0x3f80_0000, 0, 0x3f00_0000];

/// The item's attribute-6 source.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Attribute6Source {
    None,
    Known([u32; 4]),
    Stream,
}

/// The compile view of current attribute 6 for one draw. Returns the value and whether the
/// default was used.
pub fn attribute6_view(
    carried: Option<[u32; 4]>,
    source: Attribute6Source,
    array6_enabled: bool,
    reduced_fidelity_default: bool,
    program_reads_input6: bool,
) -> (Option<[u32; 4]>, bool) {
    let mut value = carried;
    if source == Attribute6Source::Stream && !array6_enabled {
        value = None;
    }
    if reduced_fidelity_default && !array6_enabled && value.is_none() && program_reads_input6 {
        return (Some(ATTRIBUTE6_DEFAULT), true);
    }
    (value, false)
}

/// A classified coverage gap.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub enum GapReason {
    TextureCapability,
    SamplerCapability,
    TextureSource,
    ComputedFragmentValues,
    CurrentAttribute,
    VertexConstants,
    VertexArrays,
    ShaderCapability,
}

/// A compile attempt's failure: a classified gap or another error.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Attempt {
    Gap(GapReason),
    Error(PlanError),
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct FidelityPolicy {
    pub reduced_fidelity: bool,
    pub try_source: bool,
}

/// The program variant ladder: source, then base colour, then flat.
pub fn choose_programs<T>(
    policy: FidelityPolicy,
    mut fallback: Option<GapReason>,
    base_colour_candidate: bool,
    fragment_values_complete: bool,
    mut compile: impl FnMut(Variant) -> Result<T, Attempt>,
) -> Result<(Variant, T, Option<GapReason>), PlanError> {
    let error = |attempt| match attempt {
        Attempt::Gap(reason) => PlanError::Unsupported(gap_name(reason)),
        Attempt::Error(e) => e,
    };
    let source_allowed = !policy.reduced_fidelity || (policy.try_source && fallback.is_none());
    if source_allowed {
        match compile(Variant::Source) {
            Ok(value) => return Ok((Variant::Source, value, fallback)),
            Err(Attempt::Gap(reason)) if policy.reduced_fidelity && policy.try_source => {
                fallback = Some(reason)
            }
            Err(attempt) => return Err(error(attempt)),
        }
    }
    if policy.reduced_fidelity && base_colour_candidate && fragment_values_complete {
        match compile(Variant::BaseColour) {
            Ok(value) => return Ok((Variant::BaseColour, value, fallback)),
            Err(Attempt::Gap(_)) => {}
            Err(attempt) => return Err(error(attempt)),
        }
    }
    if policy.reduced_fidelity {
        return compile(Variant::Flat)
            .map(|value| (Variant::Flat, value, fallback))
            .map_err(error);
    }
    Err(PlanError::Unsupported("no program variant"))
}

fn gap_name(reason: GapReason) -> &'static str {
    match reason {
        GapReason::TextureCapability => "texture capability",
        GapReason::SamplerCapability => "sampler capability",
        GapReason::TextureSource => "texture source",
        GapReason::ComputedFragmentValues => "computed fragment values",
        GapReason::CurrentAttribute => "current attribute",
        GapReason::VertexConstants => "vertex constants",
        GapReason::VertexArrays => "vertex arrays",
        GapReason::ShaderCapability => "shader capability",
    }
}

/// Where a draw texture's image comes from.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum DrawImage {
    /// An image already in the packet.
    Packet(u32),
    /// A CPU image to add to the packet once per distinct image.
    Cpu(CpuImage),
}

/// One texture use of a draw.
#[derive(Clone, Debug, PartialEq)]
pub struct DrawTexture {
    pub unit: u32,
    pub image: DrawImage,
    pub view: ViewParameters,
    pub sampler: Sampler,
    /// (stage, texture slot, sampler slot, format code).
    pub layout: (u32, u32, u32, u32),
}

/// Collects the textures of a draw for the units in `mask`, in ascending order.
pub fn draw_textures(
    units: &[UnitState; 16],
    mask: u16,
    packet: &PacketContext,
) -> Result<Vec<DrawTexture>, PlanError> {
    let mut out = Vec::new();
    for unit in 0..16u32 {
        if mask & (1 << unit) == 0 {
            continue;
        }
        let state = &units[unit as usize];
        let controls =
            match decode(&state.record).map_err(|e| PlanError::Sampler(format!("{e:?}")))? {
                DecodedUnit::Enabled(c) => c,
                DecodedUnit::Disabled(_) => {
                    return Err(PlanError::Unsupported("sampled disabled texture"));
                }
            };
        let (image, format) = if let Some(binding) = &state.packet_image {
            let resolved = resolve(binding, &controls.words, packet)?;
            (DrawImage::Packet(binding.image), resolved.format)
        } else if state.selection == Some(Selection::Producer) {
            return Err(PlanError::Unsupported(
                "producer texture requires native image",
            ));
        } else {
            let cpu = state
                .cpu_image
                .as_ref()
                .ok_or(PlanError::Source("texture image carry"))?;
            let cpu = cpu
                .as_ref()
                .map_err(|_| PlanError::Source("texture payload"))?;
            if cpu.descriptor != controls.words {
                return Err(PlanError::Source("texture image association"));
            }
            (DrawImage::Cpu(cpu.clone()), cpu.format)
        };
        let sampler =
            native_sampler(&controls).map_err(|e| PlanError::Sampler(format!("{e:?}")))?;
        out.push(DrawTexture {
            unit,
            image,
            view: view_parameters(&controls),
            sampler,
            layout: (1, unit, unit, format as u32),
        });
    }
    Ok(out)
}

// --- 10. Retained clear replay -----------------------------------------------------------------

/// The target of a retained clear replay.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ClearTarget {
    pub width: u32,
    pub height: u32,
    pub source_colour_format: u32,
    pub depth_format: u32,
    pub scissor: [u32; 4],
}

/// Last clear values, each unknown until written.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct ClearState {
    pub colour: Option<u32>,
    pub depth_stencil: Option<u32>,
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Attachment<T> {
    Load,
    Clear(T),
}

/// One native clear pass; both attachments store.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct ClearPass {
    pub colour: Attachment<[f32; 4]>,
    pub depth: Attachment<f32>,
}

/// `[R, G, B, A] / 255` from an ARGB word.
pub fn clear_colour(argb: u32) -> [f32; 4] {
    let channel = |shift: u32| ((argb >> shift) & 0xff) as f32 / 255.0;
    [channel(16), channel(8), channel(0), channel(24)]
}

/// `(value >> 8) / 16777215`.
pub fn clear_depth(value: u32) -> f32 {
    ((value >> 8) as f64 / 16_777_215.0) as f32
}

/// Replays a clear-only bank's methods `(subchannel, method, value)` as native clear passes.
pub fn replay_clears(
    methods: &[(u32, u32, u32)],
    target: &ClearTarget,
    state: &mut ClearState,
) -> Result<Vec<ClearPass>, PlanError> {
    if target.width == 0
        || target.height == 0
        || target.scissor != [0, 0, target.width, target.height]
    {
        return Err(PlanError::Unsupported("scissored retained clear"));
    }
    let mut next = *state;
    let mut passes = Vec::new();
    for &(subchannel, method, value) in methods {
        if subchannel != 0 {
            return Err(PlanError::Unsupported("non-clear retained bank effect"));
        }
        match method {
            0 if value == 0 => {}
            0x1d90 => next.colour = Some(value),
            0x1d8c => next.depth_stencil = Some(value),
            0x1d94 => {
                if value & !0xf1 != 0 || !matches!(value & 0xf0, 0 | 0xf0) {
                    return Err(PlanError::Unsupported("partial color/stencil clear"));
                }
                if value == 0 {
                    continue;
                }
                let colour = if value & 0xf0 != 0 {
                    if target.source_colour_format != 8 {
                        return Err(PlanError::Unsupported("source clear color format"));
                    }
                    Attachment::Clear(clear_colour(
                        next.colour.ok_or(PlanError::Source("clear color"))?,
                    ))
                } else {
                    Attachment::Load
                };
                let depth = if value & 1 != 0 {
                    if target.depth_format != 2 {
                        return Err(PlanError::Unsupported("source clear depth format"));
                    }
                    Attachment::Clear(clear_depth(
                        next.depth_stencil.ok_or(PlanError::Source("clear depth"))?,
                    ))
                } else {
                    Attachment::Load
                };
                passes.push(ClearPass { colour, depth });
            }
            _ => return Err(PlanError::Unsupported("non-clear retained bank effect")),
        }
    }
    *state = next;
    Ok(passes)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn transform(first: u32) -> [u8; 112] {
        let mut t = [0u8; 112];
        t[..4].copy_from_slice(&first.to_be_bytes());
        t
    }

    fn item(census: u32, group: u32, t: [u8; 112], control: u32) -> TraversalItem {
        let mut mbi = [0u8; 32];
        mbi[0] = (census << 4) as u8;
        mbi[15] = 1;
        TraversalItem {
            mbi,
            transform: Some(t),
            material: Some(Some(DrawSources {
                group,
                transform: t,
                pass_control: control,
            })),
        }
    }

    fn committed(t: [u8; 112]) -> Option<(u32, [u8; 112])> {
        Some((16, t))
    }

    #[test]
    fn v1_v2_repeat_detection() {
        let one = transform(0x3f80_0000);
        assert_eq!(
            route(&item(1, 1, one, 3), 1, 3, committed(one), 0),
            Ok(Route::Repeated)
        );
        let two = transform(0x4000_0000);
        assert_eq!(
            route(&item(1, 1, two, 3), 1, 3, committed(one), 0),
            Ok(Route::Prepare)
        );
        let tiny = transform(0x0000_0001);
        assert_eq!(
            route(
                &item(2, 2, transform(0x807f_ffff), 3),
                2,
                3,
                committed(tiny),
                2
            ),
            Ok(Route::Repeated)
        );
        let mut opted_out = transform(0x807f_ffff);
        opted_out[111] = 1;
        assert_eq!(
            route(&item(2, 2, opted_out, 3), 2, 3, committed(tiny), 2),
            Ok(Route::Prepare)
        );
        assert_eq!(
            route(
                &item(2, 2, transform(0x0080_0000), 3),
                2,
                3,
                committed(tiny),
                2
            ),
            Ok(Route::Prepare)
        );
        // A different mesh key never repeats.
        assert_eq!(
            route(&item(1, 1, one, 3), 1, 3, Some((32, one)), 0),
            Ok(Route::Prepare)
        );
    }

    #[test]
    fn v3_to_v5_routes() {
        let t = transform(0);
        assert_eq!(
            route(&item(1, 3, t, 3), 3, 3, None, -1),
            Err(PlanError::GroupRelationship {
                census: 1,
                effective: 3
            })
        );
        assert_eq!(
            route(&item(4, 4, t, 3), 4, 3, None, 5),
            Err(PlanError::GroupOrder {
                previous: 5,
                group: 4
            })
        );
        assert_eq!(
            route(&item(4, 4, t, 3), 4, 3, committed(t), 5),
            Ok(Route::Repeated)
        );
        assert_eq!(
            route(&item(0, 0, t, 2), 0, 2, None, -1),
            Ok(Route::UpdateOnly)
        );
        assert_eq!(route(&item(0, 0, t, 3), 0, 3, None, -1), Ok(Route::Prepare));
        assert_eq!(
            route(&item(0, 0, t, 0x8000_0000), 0, 0x8000_0000, None, -1),
            Ok(Route::Prepare)
        );
        assert_eq!(route(&item(7, 7, t, 0), 7, 0, None, -1), Ok(Route::Prepare));
        assert_eq!(
            route(&item(14, 14, t, 0), 14, 0, None, -1),
            Ok(Route::Special14)
        );
        let setup = SetupReferences {
            lists_enabled: true,
            ..Default::default()
        };
        assert_eq!(
            traverse(&[item(14, 14, t, 0)], &setup, 0),
            Err(PlanError::Unsupported("special Main caller"))
        );
    }

    fn event_setup() -> SetupReferences {
        SetupReferences {
            lists_enabled: true,
            initial: [0xa00, 0, 0xb00],
            group_thirteen: 0xc00,
            group_fifteen: [0, 0xd00],
        }
    }

    fn events(steps: &[PlanStep]) -> Vec<ProducerEvent> {
        steps
            .iter()
            .filter_map(|s| {
                if let PlanStep::Event(e) = s {
                    Some(*e)
                } else {
                    None
                }
            })
            .collect()
    }

    #[test]
    fn v6_event_order() {
        let items = [
            item(0, 0, transform(0x3f80_0000), 3),
            item(12, 12, transform(0x4000_0000), 3),
            item(13, 13, transform(0x4040_0000), 3),
        ];
        let (steps, stats) = traverse(&items, &event_setup(), 3).unwrap();
        assert_eq!(
            events(&steps),
            vec![
                ProducerEvent::Initial {
                    slot: 0,
                    target: 0xa00
                },
                ProducerEvent::Initial {
                    slot: 2,
                    target: 0xb00
                },
                ProducerEvent::Group12Textures,
                ProducerEvent::Group13 { target: 0xc00 },
                ProducerEvent::Finish { previous_group: 13 },
            ]
        );
        let water = steps
            .iter()
            .position(|s| *s == PlanStep::WaterRows)
            .unwrap();
        assert_eq!(
            steps[water - 1],
            PlanStep::Event(ProducerEvent::Group12Textures)
        );
        // Passes: before item 0, after Group12Textures, after Group13.
        assert_eq!(
            steps.iter().filter(|s| **s == PlanStep::OpenPass).count(),
            3
        );
        assert_eq!(stats.emitted_items, 3);
        // The finish's remaining transitions from 13 to 15 raise Group15{1, 0xd00}.
        assert_eq!(
            events(&transition_steps(13, 15, &event_setup())),
            vec![ProducerEvent::Group15 {
                slot: 1,
                target: 0xd00
            }]
        );
        // An item in group 15 is refused as a special route instead.
        let mut fifteen = items.to_vec();
        fifteen[2] = item(15, 15, transform(0x4040_0000), 3);
        assert_eq!(
            traverse(&fifteen, &event_setup(), 3),
            Err(PlanError::Unsupported("special Main caller"))
        );
        // Transitions run once per group, after that group's events.
        let transitions: Vec<_> = steps
            .iter()
            .filter_map(|s| {
                if let PlanStep::Transition { from, to } = s {
                    Some((*from, *to))
                } else {
                    None
                }
            })
            .collect();
        assert_eq!(transitions.first(), Some(&(-1, 0)));
        assert_eq!(transitions.len(), 14);
    }

    #[test]
    fn group_fifteen_events_in_order() {
        // Vector 6's tail: the transition from group 12 to 15 raises Group13 then Group15.
        let steps = transition_steps(12, 15, &event_setup());
        assert_eq!(
            events(&steps),
            vec![
                ProducerEvent::Group13 { target: 0xc00 },
                ProducerEvent::Group15 {
                    slot: 1,
                    target: 0xd00
                }
            ]
        );
        assert_eq!(
            steps.last(),
            Some(&PlanStep::Transition { from: 14, to: 15 })
        );
        let disabled = SetupReferences {
            lists_enabled: false,
            ..event_setup()
        };
        assert!(events(&transition_steps(-1, 15, &disabled)).is_empty());
    }

    #[test]
    fn v7_empty_cohort() {
        let (steps, _) = traverse(&[], &event_setup(), 3).unwrap();
        assert_eq!(
            events(&steps),
            vec![
                ProducerEvent::Initial {
                    slot: 0,
                    target: 0xa00
                },
                ProducerEvent::Initial {
                    slot: 2,
                    target: 0xb00
                },
                ProducerEvent::Finish { previous_group: -1 },
            ]
        );
    }

    #[test]
    fn v8_lists_disabled() {
        let setup = SetupReferences {
            lists_enabled: false,
            ..event_setup()
        };
        let t = transform(0);
        let (steps, stats) = traverse(&[item(0, 0, t, 3), item(1, 1, t, 3)], &setup, 3).unwrap();
        assert_eq!(
            steps,
            vec![
                PlanStep::OpenPass,
                PlanStep::NoDraw {
                    item: 0,
                    reason: NoDrawReason::ListsDisabled
                },
                PlanStep::NoDraw {
                    item: 1,
                    reason: NoDrawReason::ListsDisabled
                },
                PlanStep::Event(ProducerEvent::Finish { previous_group: -1 }),
            ]
        );
        assert_eq!(stats.source_disabled, 2);
    }

    #[test]
    fn traversal_source_checks() {
        let setup = event_setup();
        let t = transform(0);
        let mut refused = item(0, 0, t, 3);
        refused.material = None;
        assert_eq!(
            traverse(&[refused], &setup, 3),
            Err(PlanError::Source("material item"))
        );
        let mut no_sources = item(0, 0, t, 3);
        no_sources.material = Some(None);
        assert_eq!(
            traverse(&[no_sources], &setup, 3),
            Err(PlanError::MaterialValues {
                stage: "captured-draw-source"
            })
        );
        let mut no_transform = item(0, 0, t, 3);
        no_transform.transform = None;
        assert_eq!(
            traverse(&[no_transform], &setup, 3),
            Err(PlanError::Source("transform"))
        );
        assert_eq!(
            traverse(&[item(0, 0, t, 3)], &setup, 4),
            Err(PlanError::Association)
        );
        // A run of identical items is all Repeated after the first; update-only commits.
        let (steps, stats) = traverse(
            &[item(0, 0, t, 2), item(0, 0, t, 2), item(0, 0, t, 2)],
            &setup,
            2,
        )
        .unwrap();
        assert_eq!((stats.update_only, stats.repeated), (1, 2));
        assert!(steps.contains(&PlanStep::ObserveUpdateOnly { item: 0 }));
    }

    #[test]
    fn association() {
        let source = SourceIdentity {
            engine_update: 1,
            cohort: 2,
            publication: 3,
            pass_occurrence: 4,
        };
        let header = CohortHeader {
            source_invocation: 2,
            execution_update: 1,
            pass_occurrence: 4,
        };
        assert_eq!(
            check_association(&source, &header, (1, 1), (1, 1), 2, &[0, 1], &[0, 1], true),
            Ok(())
        );
        assert_eq!(
            check_association(&source, &header, (1, 1), (2, 1), 2, &[0, 1], &[0, 1], true),
            Err(PlanError::Association)
        );
        assert_eq!(
            check_association(&source, &header, (1, 1), (1, 1), 2, &[1, 0], &[0, 1], true),
            Err(PlanError::Association)
        );
        let unpublished = SourceIdentity {
            publication: 0,
            ..source
        };
        assert_eq!(
            check_association(&unpublished, &header, (1, 1), (1, 1), 0, &[], &[], true),
            Err(PlanError::Association)
        );
        assert_eq!(
            check_association(&source, &header, (1, 1), (1, 1), 0, &[], &[], false),
            Err(PlanError::Source("Main setup sources"))
        );
    }

    fn alpha_state(test: Option<bool>, comparison: Option<Comparison>) -> PipelineState {
        PipelineState {
            alpha_test: test,
            alpha_comparison: comparison,
            alpha_reference: Some(0x80),
            srgb_output: Some(false),
            ..Default::default()
        }
    }

    fn convert(reference: u32, format: u32) -> Result<u32, String> {
        if matches!(format, 1..=5 | 8..=16) {
            Ok(reference * 2 + format)
        } else {
            Err("format".into())
        }
    }

    #[test]
    fn v9_shader_controls_alpha() {
        let units: [TextureUnitRecord; 16] = Default::default();
        let tail = [7, 8, 9];
        let c = shader_controls(
            &alpha_state(Some(true), Some(Comparison::GreaterOrEqual)),
            &units,
            0,
            (1, 1),
            8,
            tail,
            0,
            &convert,
        )
        .unwrap();
        assert_eq!(c.controls.alpha_function, Some(0x206));
        assert_eq!(c.alpha_constant, [convert(0x80, 8).unwrap(), 7, 8, 9]);
        let c = shader_controls(
            &alpha_state(Some(true), Some(Comparison::Always)),
            &units,
            0,
            (1, 1),
            8,
            tail,
            0,
            &convert,
        )
        .unwrap();
        assert_eq!(
            (c.controls.alpha_function, c.alpha_constant[0]),
            (Some(0x207), 0)
        );
        assert_eq!(
            shader_controls(
                &alpha_state(None, None),
                &units,
                0,
                (1, 1),
                8,
                tail,
                0,
                &convert
            ),
            Err(PlanError::Source("alpha enable"))
        );
        assert_eq!(
            shader_controls(
                &alpha_state(Some(true), None),
                &units,
                0,
                (1, 1),
                8,
                tail,
                0,
                &convert
            ),
            Err(PlanError::Source("alpha comparison"))
        );
        assert!(matches!(
            shader_controls(
                &alpha_state(Some(true), Some(Comparison::Less)),
                &units,
                0,
                (1, 1),
                6,
                tail,
                0,
                &convert
            ),
            Err(PlanError::AlphaReference(_))
        ));
        let c = shader_controls(
            &alpha_state(Some(false), None),
            &units,
            0,
            (2, 1),
            8,
            tail,
            0,
            &convert,
        )
        .unwrap();
        assert_eq!(
            (c.controls.alpha_function, c.controls.sample_grid),
            (None, (2, 1))
        );
        assert_eq!(c.controls.texture_dimensions, [(1, 1); 16]);
    }

    const D: [u32; 8] = [
        0x0001_0000,
        0x0001_8521,
        0x0000_0301,
        0x8006_8030,
        0x0000_aae4,
        0x0206_1f80,
        0x0008_0008,
        0x0000_0400,
    ];

    #[test]
    fn shader_controls_units() {
        let mut units: [TextureUnitRecord; 16] = Default::default();
        units[3].compact = Some(D);
        let state = alpha_state(Some(false), None);
        let c = shader_controls(&state, &units, 1 << 3, (1, 1), 8, [0; 3], 0, &convert).unwrap();
        assert_eq!(c.controls.texture_dimensions[3], (8, 8));
        let mut gamma = D;
        gamma[2] |= 0x00f0_0000;
        units[3].compact = Some(gamma);
        let c = shader_controls(&state, &units, 1 << 3, (1, 1), 8, [0; 3], 0, &convert).unwrap();
        assert_eq!(c.controls.gamma[3], 15);
        let c =
            shader_controls(&state, &units, 1 << 3, (1, 1), 8, [0; 3], 1 << 3, &convert).unwrap();
        assert_eq!(c.controls.gamma[3], 0);
        let mut disabled = D;
        disabled[3] = 0;
        units[3].compact = Some(disabled);
        assert_eq!(
            shader_controls(&state, &units, 1 << 3, (1, 1), 8, [0; 3], 0, &convert),
            Err(PlanError::Unsupported("sampled disabled texture"))
        );
        let mut kill = D;
        kill[3] |= 4;
        units[3].compact = Some(kill);
        assert_eq!(
            shader_controls(&state, &units, 1 << 3, (1, 1), 8, [0; 3], 0, &convert),
            Err(PlanError::Unsupported("texture alpha kill"))
        );
        let mut one_d = D;
        one_d[1] = 0x0001_8511;
        units[3].compact = Some(one_d);
        assert_eq!(
            shader_controls(&state, &units, 1 << 3, (1, 1), 8, [0; 3], 0, &convert),
            Err(PlanError::Unsupported("texture dimension"))
        );
        let srgb_unknown = PipelineState {
            srgb_output: None,
            ..state
        };
        assert_eq!(
            shader_controls(&srgb_unknown, &units, 0, (1, 1), 8, [0; 3], 0, &convert),
            Err(PlanError::Source("sRGB output"))
        );
    }

    fn packet(id: u64) -> PacketContext {
        PacketContext {
            packet: id,
            source: 5,
            images: vec![PacketImage {
                format: ImageFormat::Rgba8,
                samples: 1,
                depth: 1,
                dimension: 2,
                layers: 1,
                mip_count: 1,
                width: 8,
                height: 8,
            }],
        }
    }

    fn units_with_d() -> [UnitState; 16] {
        let mut units: [UnitState; 16] = Default::default();
        units[3].record.compact = Some(D);
        units
    }

    #[test]
    fn v10_packet_image_binding() {
        let mut units = units_with_d();
        let p = packet(1);
        bind_packet_image(&mut units, 3, &p, &D, 0).unwrap();
        let textures = draw_textures(&units, 1 << 3, &p).unwrap();
        assert_eq!(textures.len(), 1);
        assert_eq!(textures[0].layout, (1, 3, 3, 1));
        assert_eq!(textures[0].image, DrawImage::Packet(0));
        assert_eq!(
            draw_textures(&units, 1 << 3, &packet(2)),
            Err(PlanError::Association)
        );
        assert_eq!(
            bind_packet_image(&mut units, 3, &p, &D, 99),
            Err(PlanError::Association)
        );
        assert_eq!(units[3].packet_image, None);
        assert_eq!(
            bind_packet_image(&mut units, 16, &p, &D, 0),
            Err(PlanError::Association)
        );
        // A wrap change keeps the storage words.
        let mut rewrapped = D;
        rewrapped[2] = 0x0000_0101;
        bind_packet_image(&mut units, 3, &p, &rewrapped, 0).unwrap();
        // A size change does not.
        let mut other = D;
        other[6] = 0x0010_0010;
        assert_eq!(
            bind_packet_image(&mut units, 3, &p, &other, 0),
            Err(PlanError::Association)
        );
    }

    #[test]
    fn v11_producer_asset_binding() {
        let mut units: [UnitState; 16] = Default::default();
        let mut d = D;
        let request = |d: [u32; 8], payload| TextureBindingRequest {
            unit: 2,
            selection: Selection::ProducerAsset,
            descriptor: d,
            inherited_border: None,
            payload,
        };
        let cpu = |d: [u32; 8], format| {
            BindingPayload::Cpu(Ok(CpuImage {
                descriptor: d,
                format,
                packet_image: None,
            }))
        };
        assert_eq!(
            bind(&mut units, request(d, cpu(d, ImageFormat::Bc1))),
            Err(PlanError::Association)
        );
        d[1] = 0x0001_8621;
        bind(&mut units, request(d, cpu(d, ImageFormat::Bc1))).unwrap();
        assert_eq!(units[2].record.compact, Some(d));
        assert_eq!(
            bind(&mut units, request(d, cpu(d, ImageFormat::Bc2))),
            Err(PlanError::Association)
        );
        // The linear bit does not matter for the format check.
        d[1] = 0x0001_a621;
        bind(&mut units, request(d, cpu(d, ImageFormat::Bc1))).unwrap();
        let native = TextureBindingRequest {
            unit: 2,
            selection: Selection::ResidentObject,
            descriptor: D,
            inherited_border: None,
            payload: BindingPayload::NativeProducer,
        };
        assert_eq!(bind(&mut units, native), Err(PlanError::Association));
        let wide = TextureBindingRequest {
            unit: 16,
            selection: Selection::Fallback,
            descriptor: D,
            inherited_border: None,
            payload: BindingPayload::Cpu(Err(CpuImageError::MissingSource)),
        };
        assert_eq!(
            bind(&mut units, wide),
            Err(PlanError::Source("texture unit"))
        );
    }

    #[test]
    fn bind_clears_packet_image_and_keeps_border() {
        let mut units = units_with_d();
        units[3].record.border = Some(0x55);
        let p = packet(1);
        bind_packet_image(&mut units, 3, &p, &D, 0).unwrap();
        let request = TextureBindingRequest {
            unit: 3,
            selection: Selection::Fallback,
            descriptor: D,
            inherited_border: None,
            payload: BindingPayload::Cpu(Ok(CpuImage {
                descriptor: D,
                format: ImageFormat::Rgba8,
                packet_image: None,
            })),
        };
        bind(&mut units, request).unwrap();
        assert_eq!(units[3].packet_image, None);
        assert_eq!(units[3].record.border, Some(0x55));
        let textures = draw_textures(&units, 1 << 3, &p).unwrap();
        assert!(matches!(textures[0].image, DrawImage::Cpu(_)));
        // CPU image errors.
        units[3].cpu_image = Some(Err(CpuImageError::Failed("x".into())));
        assert_eq!(
            draw_textures(&units, 1 << 3, &p),
            Err(PlanError::Source("texture payload"))
        );
        units[3].cpu_image = None;
        assert_eq!(
            draw_textures(&units, 1 << 3, &p),
            Err(PlanError::Source("texture image carry"))
        );
        units[3].selection = Some(Selection::Producer);
        assert_eq!(
            draw_textures(&units, 1 << 3, &p),
            Err(PlanError::Unsupported(
                "producer texture requires native image"
            ))
        );
        units[3].selection = Some(Selection::Fallback);
        let mut other = D;
        other[0] = 9;
        units[3].cpu_image = Some(Ok(CpuImage {
            descriptor: other,
            format: ImageFormat::Rgba8,
            packet_image: None,
        }));
        assert_eq!(
            draw_textures(&units, 1 << 3, &p),
            Err(PlanError::Source("texture image association"))
        );
    }

    #[test]
    fn attribute_six_view() {
        let value = [1, 2, 3, 4];
        assert_eq!(
            attribute6_view(Some(value), Attribute6Source::Stream, false, false, true),
            (None, false)
        );
        assert_eq!(
            attribute6_view(Some(value), Attribute6Source::Stream, true, false, true),
            (Some(value), false)
        );
        assert_eq!(
            attribute6_view(None, Attribute6Source::None, false, true, true),
            (Some(ATTRIBUTE6_DEFAULT), true)
        );
        assert_eq!(
            attribute6_view(None, Attribute6Source::None, false, true, false),
            (None, false)
        );
        assert_eq!(
            attribute6_view(
                Some(value),
                Attribute6Source::Known(value),
                false,
                true,
                true
            ),
            (Some(value), false)
        );
    }

    #[test]
    fn variant_ladder() {
        let exact = FidelityPolicy {
            reduced_fidelity: false,
            try_source: false,
        };
        let reduced = FidelityPolicy {
            reduced_fidelity: true,
            try_source: false,
        };
        let trying = FidelityPolicy {
            reduced_fidelity: true,
            try_source: true,
        };
        assert_eq!(
            choose_programs(exact, None, true, true, Ok::<_, Attempt>),
            Ok((Variant::Source, Variant::Source, None))
        );
        assert!(
            choose_programs(exact, None, true, true, |_| Err::<(), _>(Attempt::Gap(
                GapReason::VertexArrays
            )))
            .is_err()
        );
        let gap_then_ok = |v: Variant| {
            if v == Variant::Source {
                Err(Attempt::Gap(GapReason::TextureSource))
            } else {
                Ok(v)
            }
        };
        assert_eq!(
            choose_programs(trying, None, true, true, gap_then_ok),
            Ok((
                Variant::BaseColour,
                Variant::BaseColour,
                Some(GapReason::TextureSource)
            ))
        );
        assert_eq!(
            choose_programs(trying, None, false, true, gap_then_ok),
            Ok((Variant::Flat, Variant::Flat, Some(GapReason::TextureSource)))
        );
        assert_eq!(
            choose_programs(reduced, None, true, false, Ok::<_, Attempt>),
            Ok((Variant::Flat, Variant::Flat, None))
        );
        let fatal = |_| Err::<(), _>(Attempt::Error(PlanError::Source("x")));
        assert_eq!(
            choose_programs(trying, None, true, true, fatal),
            Err(PlanError::Source("x"))
        );
    }

    fn target() -> ClearTarget {
        ClearTarget {
            width: 8,
            height: 8,
            source_colour_format: 8,
            depth_format: 2,
            scissor: [0, 0, 8, 8],
        }
    }

    #[test]
    fn v12_v13_clear_replay() {
        let mut state = ClearState::default();
        let passes = replay_clears(
            &[
                (0, 0x1d90, 0x40ff_8000),
                (0, 0x1d8c, 0xffff_ff00),
                (0, 0x1d94, 0xf1),
                (0, 0x1d94, 0x01),
            ],
            &target(),
            &mut state,
        )
        .unwrap();
        assert_eq!(passes.len(), 2);
        assert_eq!(
            passes[0].colour,
            Attachment::Clear([1.0, 0.501_960_8, 0.0, 0.250_980_4])
        );
        assert_eq!(passes[0].depth, Attachment::Clear(1.0));
        assert_eq!(passes[1].colour, Attachment::Load);
        assert_eq!(passes[1].depth, Attachment::Clear(1.0));
        assert_eq!(state.colour, Some(0x40ff_8000));
        let mut state = ClearState::default();
        let passes = replay_clears(
            &[(0, 0x1d8c, 0x100), (0, 0x1d94, 1), (0, 0, 0)],
            &target(),
            &mut state,
        )
        .unwrap();
        assert_eq!(passes[0].depth, Attachment::Clear(5.960_465e-8));
    }

    #[test]
    fn v14_clear_refusals() {
        type Case<'a> = (&'a [(u32, u32, u32)], PlanError);
        let cases: [Case; 5] = [
            (
                &[(0, 0x1d94, 0x02)],
                PlanError::Unsupported("partial color/stencil clear"),
            ),
            (
                &[(0, 0x1d94, 0x31)],
                PlanError::Unsupported("partial color/stencil clear"),
            ),
            (&[(0, 0x1d94, 0xf0)], PlanError::Source("clear color")),
            (
                &[(0, 0x1814, 0)],
                PlanError::Unsupported("non-clear retained bank effect"),
            ),
            (
                &[(1, 0x1d90, 0)],
                PlanError::Unsupported("non-clear retained bank effect"),
            ),
        ];
        for (methods, error) in cases {
            let mut state = ClearState {
                colour: None,
                depth_stencil: Some(5),
            };
            let before = state;
            assert_eq!(
                replay_clears(
                    &[(0, 0x1d8c, 9)]
                        .iter()
                        .chain(methods)
                        .copied()
                        .collect::<Vec<_>>(),
                    &target(),
                    &mut state
                ),
                Err(error)
            );
            assert_eq!(state, before);
        }
        let scissored = ClearTarget {
            scissor: [0, 0, 4, 8],
            ..target()
        };
        assert_eq!(
            replay_clears(&[], &scissored, &mut ClearState::default()),
            Err(PlanError::Unsupported("scissored retained clear"))
        );
        let wrong_colour = ClearTarget {
            source_colour_format: 11,
            ..target()
        };
        assert_eq!(
            replay_clears(
                &[(0, 0x1d90, 0), (0, 0x1d94, 0xf0)],
                &wrong_colour,
                &mut ClearState::default()
            ),
            Err(PlanError::Unsupported("source clear color format"))
        );
        let no_depth = ClearTarget {
            depth_format: 1,
            ..target()
        };
        assert_eq!(
            replay_clears(
                &[(0, 0x1d8c, 0), (0, 0x1d94, 1)],
                &no_depth,
                &mut ClearState::default()
            ),
            Err(PlanError::Unsupported("source clear depth format"))
        );
        assert_eq!(
            replay_clears(&[(0, 0x1d94, 0)], &target(), &mut ClearState::default()),
            Ok(Vec::new())
        );
    }

    /// Property: clear depth and colour meet their stated tolerances.
    #[test]
    fn property_clear_tolerances() {
        assert_eq!(clear_depth(0xffff_ff00), 1.0);
        assert_eq!(clear_depth(0), 0.0);
        let mut rng = crate::testutil::Rng::new(91);
        for _ in 0..20_000 {
            let d = rng.below(16_777_216);
            let x = clear_depth(d << 8);
            assert_eq!((x as f64 * 16_777_215.0).round() as u32, d);
            assert!(crate::testutil::ulps(x, (d as f64 / 16_777_215.0) as f32) <= 1);
        }
        for n in 0..=255u32 {
            let c = clear_colour(n << 16)[0];
            assert_eq!((c * 255.0).round() as u32, n);
        }
    }
}
