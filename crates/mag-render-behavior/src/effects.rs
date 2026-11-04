//! Main producer effects framework and stencil-mask producer (`main-effects.md`).

use crate::group_twelve::ImageRequest;
use crate::producer::ProgramUsage;
use crate::producers::{ProducerInputs, RendererObject};
use crate::raster::BlendConstant;
use crate::scene_plan::{ProducerEvent, SetupReferences};
use crate::setup::{Comparison, PipelineState, StencilFace, StencilOperation};
use crate::surfaces::{
    NativeViewport, SelectedSurface, SurfaceRecord, explicit_viewport, map_viewport, select,
    table_position,
};
use crate::texture::{
    DecodedUnit, TextureUnitRecord, ViewParameters, decode, native_sampler, view_parameters,
};
use mag_render_abi::packet::Sampler;

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum EffectsError {
    Reference,
    GenerationFlag,
    UnroutedEvent,
    CurrentSurface(u32),
    NonFinite(&'static str),
    Viewport,
    InconsistentBinding { address: u32 },
    MissingBinding { address: u32 },
    DepthTarget,
    SampledUnit(u32),
    SampledImage,
    Raster(&'static str),
    UnknownUnit(u32),
    Alias,
    ColourTarget,
    Unsupported(&'static str),
    Association,
    ProducerBinding(&'static str),
    Target(&'static str),
}

// --- Events ---------------------------------------------------------------------------------

/// What an event routes to.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Routed {
    NoOp,
    StencilMask,
    Lighting,
    Group12,
    Fog,
    Quincunx,
    Downsample,
    Finish,
}

/// Low eight bits nonzero.
pub fn generation_enabled(flag: u32) -> bool {
    flag & 0xff != 0
}

/// Validates an event against the setup references and routes it.
pub fn route_event(
    event: ProducerEvent,
    setup: &SetupReferences,
    generation: Option<u32>,
) -> Result<Routed, EffectsError> {
    let guarded = |route| match generation {
        None => Err(EffectsError::GenerationFlag),
        Some(flag) if !generation_enabled(flag) => Ok(Routed::NoOp),
        Some(_) => Ok(route),
    };
    match event {
        ProducerEvent::Initial { slot, target } => {
            let expected = setup
                .initial
                .get(slot as usize)
                .ok_or(EffectsError::Reference)?;
            if target == 0 || target != *expected {
                return Err(EffectsError::Reference);
            }
            match slot {
                0 => guarded(Routed::StencilMask),
                1 => guarded(Routed::Lighting),
                _ => Ok(Routed::NoOp),
            }
        }
        ProducerEvent::Group13 { target } => {
            if target == 0 || target != setup.group_thirteen {
                return Err(EffectsError::Reference);
            }
            guarded(Routed::Fog)
        }
        ProducerEvent::Group15 { slot, target } => {
            let expected = setup
                .group_fifteen
                .get(slot as usize)
                .ok_or(EffectsError::Reference)?;
            if target == 0 || target != *expected {
                return Err(EffectsError::Reference);
            }
            guarded(if slot == 0 {
                Routed::Quincunx
            } else {
                Routed::Downsample
            })
        }
        ProducerEvent::Group12Textures => Ok(Routed::Group12),
        ProducerEvent::Finish { .. } => Ok(Routed::Finish),
    }
}

// --- Current surface --------------------------------------------------------------------------

/// Index and role of a current surface address in the table at `table_base`.
pub fn current_index_role(table_base: u32, address: u32) -> Result<(u32, u32), EffectsError> {
    let offset = address.wrapping_sub(table_base);
    if address < table_base || offset >= 8 * 84 || !offset.is_multiple_of(84) {
        return Err(EffectsError::CurrentSurface(address));
    }
    Ok(((offset / 84) % 2, offset / 168))
}

// --- Supplied associations ------------------------------------------------------------------

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum LoadOp {
    Load,
    DontCare,
    Clear,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct AttachmentImage {
    pub image: u32,
    pub load: LoadOp,
}

/// A surface binding supplied by the caller.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct SurfaceBinding {
    pub address: u32,
    pub physical: (u32, u32),
    /// Packet formats (0 none).
    pub colour_format: u32,
    pub depth_format: u32,
    pub colour: Option<AttachmentImage>,
    pub depth: Option<AttachmentImage>,
    pub stencil: Option<AttachmentImage>,
    pub depth_initialised: bool,
    pub stencil_initialised: bool,
}

impl SurfaceBinding {
    /// Physical dimensions equal the record's physical size.
    pub fn consistent_with(&self, record: &SurfaceRecord) -> bool {
        record.physical_size() == Some(self.physical)
    }
}

/// Finds and checks the binding of a surface.
pub fn consistent_binding<'a>(
    bindings: &'a [SurfaceBinding],
    surface: &SelectedSurface,
) -> Result<&'a SurfaceBinding, EffectsError> {
    let binding = bindings
        .iter()
        .find(|b| b.address == surface.address)
        .ok_or(EffectsError::MissingBinding {
            address: surface.address,
        })?;
    if !binding.consistent_with(&surface.record) {
        return Err(EffectsError::InconsistentBinding {
            address: surface.address,
        });
    }
    Ok(binding)
}

/// Images in the packet being built.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PacketImageInfo {
    pub format: u32,
    pub width: u32,
    pub height: u32,
    pub dimension: u32,
    pub samples: u32,
    pub layers: u32,
}

/// A producer texture binding supplied by the caller.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ProducerTextureBinding {
    pub address: u32,
    pub words: [u32; 8],
    pub image: u32,
    pub format: u32,
    pub width: u32,
    pub height: u32,
}

/// Validates the supplied producer texture bindings against the known producer sources.
pub fn validate_producer_bindings(
    bindings: &[ProducerTextureBinding],
    images: &[PacketImageInfo],
    known_sources: &[(u32, [u32; 8])],
) -> Result<(), EffectsError> {
    if bindings.len() > 5 {
        return Err(EffectsError::ProducerBinding("too many bindings"));
    }
    for (index, binding) in bindings.iter().enumerate() {
        images
            .get(binding.image as usize)
            .ok_or(EffectsError::ProducerBinding("image"))?;
        if !(1..=16).contains(&binding.format) {
            return Err(EffectsError::ProducerBinding("format"));
        }
        let rect = (binding.words[6] >> 16, binding.words[6] & 0xffff);
        if binding.width == 0 || binding.height == 0 || (binding.width, binding.height) != rect {
            return Err(EffectsError::ProducerBinding("dimensions"));
        }
        if !known_sources.contains(&(binding.address, binding.words)) {
            return Err(EffectsError::ProducerBinding("source"));
        }
        if bindings[..index]
            .iter()
            .any(|b| b.address == binding.address)
        {
            return Err(EffectsError::ProducerBinding("duplicate address"));
        }
    }
    Ok(())
}

/// Associates group-twelve image requests with producer bindings. Returns, per request, the
/// associated packet image or `None`.
pub fn associate_group_twelve(
    requests: &[ImageRequest],
    bindings: &[ProducerTextureBinding],
    images: &[PacketImageInfo],
) -> Result<Vec<Option<u32>>, EffectsError> {
    requests
        .iter()
        .map(|request| {
            let Some(binding) = bindings
                .iter()
                .find(|b| b.address == request.source_address && b.words == request.descriptor)
            else {
                return Ok(None);
            };
            let image = images
                .get(binding.image as usize)
                .ok_or(EffectsError::Association)?;
            if image.format != binding.format
                || (image.width, image.height) != (binding.width, binding.height)
            {
                return Err(EffectsError::Association);
            }
            Ok(Some(binding.image))
        })
        .collect()
}

// --- Main passes ------------------------------------------------------------------------------

/// Checks a Main pass may open on the current surface's binding.
pub fn open_main_pass(selector: Option<u32>, binding: &SurfaceBinding) -> Result<(), EffectsError> {
    match selector {
        None => return Err(EffectsError::ColourTarget),
        Some(1) => {}
        Some(_) => return Err(EffectsError::Unsupported("colour target")),
    }
    if binding.colour.is_none_or(|c| c.load != LoadOp::Load) {
        return Err(EffectsError::Target("colour load"));
    }
    if binding.depth.is_some_and(|d| d.load != LoadOp::Load)
        || (binding.depth.is_some() && !binding.depth_initialised)
    {
        return Err(EffectsError::Target("depth load"));
    }
    if binding.stencil.is_some_and(|s| s.load != LoadOp::Load)
        || (binding.stencil.is_some() && !binding.stencil_initialised)
    {
        return Err(EffectsError::Target("stencil load"));
    }
    Ok(())
}

/// Inputs ordinary Main draws take from the current surface.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct MainDrawInputs {
    pub colour_format: u32,
    pub depth_format: u32,
    pub raster_colour_format: u32,
    pub sample_grid: (u32, u32),
}

/// Main draw inputs after producers. A colour-format change makes a known blend constant
/// unknown.
pub fn main_draw_inputs(
    binding: &SurfaceBinding,
    record: &SurfaceRecord,
    previous_raster_colour_format: Option<u32>,
    blend_constant: &mut BlendConstant,
) -> Result<MainDrawInputs, EffectsError> {
    let grid = record.sample_grid().ok_or(EffectsError::Viewport)?;
    let inputs = MainDrawInputs {
        colour_format: binding.colour_format,
        depth_format: binding.depth_format,
        raster_colour_format: record.colour_format(),
        sample_grid: grid,
    };
    if previous_raster_colour_format.is_some_and(|f| f != inputs.raster_colour_format)
        && matches!(blend_constant, BlendConstant::Known(_))
    {
        *blend_constant = BlendConstant::Unknown;
    }
    Ok(inputs)
}

// --- Common producer draw contract ------------------------------------------------------------

/// Full-screen geometry.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct FullscreenGeometry {
    /// 32-byte stride: position at byte 0, texture coordinate at byte 16, little-endian.
    pub vertices: Vec<u8>,
    pub indices: Vec<u32>,
    /// Current attributes 0 and 8 afterwards (the last vertex).
    pub attribute0: [u32; 4],
    pub attribute8: [u32; 4],
}

/// Enabled vertex-array mask of producer draws.
pub const PRODUCER_ARRAYS: u16 = 0x101;
const ZERO: u32 = 0;
const ONE: u32 = 0x3f80_0000;

fn geometry(rows: &[[u32; 8]], indices: &[u32]) -> FullscreenGeometry {
    let last = rows[rows.len() - 1];
    FullscreenGeometry {
        vertices: rows
            .iter()
            .flatten()
            .flat_map(|w| w.to_le_bytes())
            .collect(),
        indices: indices.to_vec(),
        attribute0: [last[0], last[1], last[2], last[3]],
        attribute8: [last[4], last[5], last[6], last[7]],
    }
}

fn check_finite(words: &[u32]) -> Result<(), EffectsError> {
    if words.iter().any(|w| !f32::from_bits(*w).is_finite()) {
        return Err(EffectsError::NonFinite("full-screen constant"));
    }
    Ok(())
}

/// The full-screen quad from `L` and `H`.
pub fn fullscreen_quad(l: u32, h: u32) -> Result<FullscreenGeometry, EffectsError> {
    check_finite(&[l, h])?;
    Ok(geometry(
        &[
            [l, l, l, ONE, ZERO, h, ZERO, ONE],
            [h, l, l, ONE, h, h, ZERO, ONE],
            [h, h, l, ONE, h, ZERO, ZERO, ONE],
            [l, h, l, ONE, ZERO, ZERO, ZERO, ONE],
        ],
        &[0, 1, 2, 0, 2, 3],
    ))
}

/// The full-screen triangle from `L`, `H`, `F` and `G`.
pub fn fullscreen_triangle(
    l: u32,
    h: u32,
    f: u32,
    g: u32,
) -> Result<FullscreenGeometry, EffectsError> {
    check_finite(&[l, h, f, g])?;
    Ok(geometry(
        &[
            [l, l, l, ONE, ZERO, h, ZERO, ONE],
            [f, l, l, ONE, g, h, ZERO, ONE],
            [l, f, l, ONE, ZERO, l, ZERO, ONE],
        ],
        &[0, 1, 2],
    ))
}

/// A producer viewport mapped onto its target.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct ProducerViewport {
    pub native: NativeViewport,
    pub shader_scale: [f32; 4],
    pub shader_offset: [f32; 4],
    /// Physical clip size associated with the pass.
    pub clip: (u32, u32),
}

/// The producer viewport of a target record on its binding.
pub fn producer_viewport(
    record: &SurfaceRecord,
    physical: (u32, u32),
    zmin: f32,
    zmax: f32,
) -> Result<ProducerViewport, EffectsError> {
    let grid = record.sample_grid().ok_or(EffectsError::Viewport)?;
    let source = explicit_viewport(record.width(), record.height(), zmin, zmax);
    let native = map_viewport(&source, physical, grid).map_err(|_| EffectsError::Viewport)?;
    Ok(ProducerViewport {
        native,
        shader_scale: [1.0, 1.0, source.scale[2], source.scale[3]],
        shader_offset: [0.0, 0.0, source.offset[2], source.offset[3]],
        clip: physical,
    })
}

/// Producer raster requirements: every recorded field known after the producer's writes.
/// Polygon offset needs its enable, and when enabled its units and factor; the bias clamp is a
/// draw input and is only required when the producer supplies one.
pub fn producer_raster(
    state: &PipelineState,
    bias_clamp: Option<u32>,
) -> Result<mag_render_abi::packet::RasterState, EffectsError> {
    let inputs = crate::raster::RasterDrawInputs {
        topology: Some(crate::raster::Topology::Triangles),
        samples: Some(1),
        colour_attachment: 0,
        blend_constant: BlendConstant::Unknown,
        bias_clamp,
        obligations: 0,
    };
    let raster = crate::raster::translate(state, &inputs)
        .map_err(|_| EffectsError::Raster("translation"))?;
    use crate::raster::*;
    for (bit, name) in [
        (KNOWN_CULL, "cull"),
        (KNOWN_DEPTH, "depth"),
        (KNOWN_BLEND, "blend"),
        (KNOWN_COLOUR_MASK, "colour mask"),
        (KNOWN_STENCIL, "stencil"),
        (KNOWN_DEPTH_BOUNDS, "depth bounds"),
    ] {
        if raster.known_fields & bit == 0 {
            return Err(EffectsError::Raster(name));
        }
    }
    match state.polygon_offset_fill {
        None => return Err(EffectsError::Raster("polygon offset")),
        Some(true) if state.offset_units.is_none() || state.offset_factor.is_none() => {
            return Err(EffectsError::Raster("polygon offset"));
        }
        _ => {}
    }
    Ok(raster)
}

/// Sets min/mag to NEAREST.
pub fn nearest(filter: u32) -> u32 {
    (filter & !0x0f0f_0000) | 0x0101_0000
}

// --- Stencil-mask producer --------------------------------------------------------------------

/// A sampled texture of a producer draw.
#[derive(Clone, Debug, PartialEq)]
pub struct ProducerTexture {
    pub unit: u32,
    pub image: u32,
    pub format: u32,
    pub view: ViewParameters,
    pub sampler: Sampler,
}

/// One producer pass with its single draw.
#[derive(Clone, Debug, PartialEq)]
pub struct ProducerPass {
    pub target: u32,
    pub colour: Option<AttachmentImage>,
    pub depth: Option<AttachmentImage>,
    pub stencil: Option<AttachmentImage>,
    pub colour_format: u32,
    pub depth_format: u32,
    pub viewport: ProducerViewport,
    pub geometry: FullscreenGeometry,
    pub state: PipelineState,
    pub textures: Vec<ProducerTexture>,
}

/// State a producer leaves behind.
#[derive(Clone, Debug, PartialEq)]
pub struct ProducerResult {
    pub pass: Option<ProducerPass>,
    pub state: PipelineState,
    pub units: [TextureUnitRecord; 16],
    pub attribute0: Option<[u32; 4]>,
    pub attribute8: Option<[u32; 4]>,
}

impl ProducerResult {
    pub fn no_op(state: &PipelineState, units: &[TextureUnitRecord; 16]) -> Self {
        Self {
            pass: None,
            state: *state,
            units: *units,
            attribute0: None,
            attribute8: None,
        }
    }
}

/// The module constants `L`, `H`, `F`, `G`, `Zmin` and `Zmax`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ModuleConstants {
    pub l: u32,
    pub h: u32,
    pub f: u32,
    pub g: u32,
    pub zmin: u32,
    pub zmax: u32,
}

impl ModuleConstants {
    pub fn of(inputs: &ProducerInputs) -> Self {
        let s = &inputs.scalars;
        Self {
            l: s.t_lo.value,
            h: s.t_hi.value,
            f: s.t_far.value,
            g: s.t_uvfar.value,
            zmin: s.z0.value,
            zmax: s.z1.value,
        }
    }
}

const MASK_FACE: StencilFace = StencilFace {
    write_mask: 0xff,
    comparison: Comparison::Always,
    reference: 0x81,
    read_mask: 0,
    fail: StencilOperation::Replace,
    depth_fail: StencilOperation::Replace,
    pass: StencilOperation::Replace,
};

/// Samples a colour surface through unit 0 at mip 0, layer 0.
fn sample_unit0(
    unit: &TextureUnitRecord,
    image: u32,
    format: u32,
    physical: (u32, u32),
) -> Result<ProducerTexture, EffectsError> {
    let controls = match decode(unit).map_err(|_| EffectsError::SampledImage)? {
        DecodedUnit::Enabled(c) => c,
        DecodedUnit::Disabled(_) => return Err(EffectsError::SampledImage),
    };
    if controls.dimension != 2
        || controls.cube
        || (controls.words[6] >> 16, controls.words[6] & 0xffff) != physical
    {
        return Err(EffectsError::SampledImage);
    }
    let mut view = view_parameters(&controls);
    view.mip_count = 1;
    view.layer_count = 1;
    let sampler = native_sampler(&controls).map_err(|_| EffectsError::SampledImage)?;
    Ok(ProducerTexture {
        unit: 0,
        image,
        format,
        view,
        sampler,
    })
}

/// Runs the stencil-mask producer (initial slot 0).
pub fn stencil_mask(
    renderer: &RendererObject,
    programs_present: bool,
    usage: &ProgramUsage,
    bindings: &[SurfaceBinding],
    constants: &ModuleConstants,
    state: &PipelineState,
    units: &[TextureUnitRecord; 16],
) -> Result<ProducerResult, EffectsError> {
    if !renderer.bank_zero_enabled() || !programs_present {
        return Ok(ProducerResult::no_op(state, units));
    }
    let s = select(renderer, 0, 1).map_err(|_| EffectsError::Association)?;
    let d = select(renderer, 1, 0).map_err(|_| EffectsError::Association)?;
    let s_binding = consistent_binding(bindings, &s)?;
    let d_binding = consistent_binding(bindings, &d)?;
    let depth = d_binding.depth.ok_or(EffectsError::DepthTarget)?;
    if !matches!(d_binding.depth_format, 15 | 16) || d.record.depth_format() != 2 {
        return Err(EffectsError::DepthTarget);
    }
    if let Some(unit) = (1..16).find(|u| usage.samples(*u)) {
        return Err(EffectsError::SampledUnit(unit));
    }
    let mut descriptor = renderer.surface_descriptors[table_position(s.role, s.index)].value;
    descriptor[5] = nearest(descriptor[5]);
    let mut units = *units;
    units[0].apply_compact(&descriptor);
    let mut textures = Vec::new();
    if usage.samples(0) {
        let image = s_binding.colour.ok_or(EffectsError::SampledImage)?;
        textures.push(sample_unit0(
            &units[0],
            image.image,
            s_binding.colour_format,
            s_binding.physical,
        )?);
    }
    let draw_state = PipelineState {
        stencil_enable: Some(true),
        two_sided_stencil: Some(true),
        front_stencil: Some(MASK_FACE),
        back_stencil: Some(MASK_FACE),
        depth_test: Some(false),
        depth_write: Some(false),
        alpha_test: Some(false),
        blend_enable: Some(false),
        colour_masks: Some([0; 4]),
        cull_enable: Some(false),
        ..*state
    };
    producer_raster(&draw_state, None)?;
    let geometry = fullscreen_quad(constants.l, constants.h)?;
    let viewport = producer_viewport(
        &d.record,
        d_binding.physical,
        f32::from_bits(constants.zmin),
        f32::from_bits(constants.zmax),
    )?;
    let load = |initialised| {
        if initialised {
            LoadOp::Load
        } else {
            LoadOp::DontCare
        }
    };
    let pass = ProducerPass {
        target: d.address,
        colour: None,
        depth: Some(AttachmentImage {
            image: depth.image,
            load: load(d_binding.depth_initialised),
        }),
        stencil: Some(AttachmentImage {
            image: depth.image,
            load: load(d_binding.stencil_initialised),
        }),
        colour_format: 0,
        depth_format: d_binding.depth_format,
        viewport,
        geometry: geometry.clone(),
        state: draw_state,
        textures,
    };
    units[0].disable_if_known();
    let mut after = draw_state;
    for face in [&mut after.front_stencil, &mut after.back_stencil]
        .into_iter()
        .flatten()
    {
        face.write_mask = 0;
    }
    Ok(ProducerResult {
        pass: Some(pass),
        state: after,
        units,
        attribute0: Some(geometry.attribute0),
        attribute8: Some(geometry.attribute8),
    })
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;
    use crate::producers::{acquire, tests as p};
    use crate::surfaces::tests::record;

    pub(crate) const R: u32 = p::RENDERER;

    pub(crate) fn f(x: f32) -> u32 {
        x.to_bits()
    }

    pub(crate) fn constants() -> ModuleConstants {
        ModuleConstants {
            l: f(-0.75),
            h: f(0.5),
            f: f(2.5),
            g: f(1.75),
            zmin: 0,
            zmax: f(1.0),
        }
    }

    /// The fixture renderer: every record 320×180, depth format 2, colour format 8.
    pub(crate) fn renderer(bank_zero: bool) -> RendererObject {
        let mut r = acquire(&p::image(), p::TOC).unwrap().renderer;
        for (position, entry) in r.surface_table.iter_mut().enumerate() {
            entry.value = record(320, 180, 8, 2, 0);
            entry.value.words[8] = 0x10_0000 * (position as u32 + 1);
        }
        r.stencil_branch = u8::from(bank_zero);
        r.surface_branch = u32::from(bank_zero);
        r
    }

    pub(crate) fn binding(address: u32, physical: (u32, u32), image: u32) -> SurfaceBinding {
        SurfaceBinding {
            address,
            physical,
            colour_format: 1,
            depth_format: 15,
            colour: Some(AttachmentImage {
                image,
                load: LoadOp::Load,
            }),
            depth: Some(AttachmentImage {
                image: image + 1000,
                load: LoadOp::Load,
            }),
            stencil: Some(AttachmentImage {
                image: image + 1000,
                load: LoadOp::Load,
            }),
            depth_initialised: true,
            stencil_initialised: true,
        }
    }

    pub(crate) fn all_bindings(r: &RendererObject) -> Vec<SurfaceBinding> {
        r.surface_table
            .iter()
            .enumerate()
            .map(|(i, e)| binding(e.address, (320, 180), i as u32 + 1))
            .collect()
    }

    #[test]
    fn v1_selection() {
        let r = renderer(true);
        let base = R + 0x2c10;
        assert_eq!(select(&r, 0, 1).unwrap().address, base + 0xa8);
        assert_eq!(select(&r, 1, 0).unwrap().address, base + 0x54);
        assert_eq!(select(&r, 1, 3).unwrap().address, base + 0x150 + 0x54);
        assert_eq!(
            select(&renderer(false), 0, 1).unwrap().address,
            base + 0x150
        );
    }

    #[test]
    fn v2_current_index_role() {
        let base = R + 0x2c10;
        assert_eq!(current_index_role(base, base + 252), Ok((1, 1)));
        assert_eq!(
            current_index_role(base, base + 250),
            Err(EffectsError::CurrentSurface(base + 250))
        );
        assert_eq!(
            current_index_role(base, base + 8 * 84),
            Err(EffectsError::CurrentSurface(base + 672))
        );
    }

    #[test]
    fn v3_to_v5_producer_viewport() {
        let d = record(320, 180, 8, 2, 3);
        let v = producer_viewport(&d, (640, 180), 0.0, 1.0).unwrap();
        assert_eq!(
            (v.native.x, v.native.y, v.native.width, v.native.height),
            (0.0, 0.0, 640.0, 180.0)
        );
        assert_eq!(v.native.scissor, [0, 0, 640, 180]);
        assert_eq!(
            (v.shader_scale, v.shader_offset),
            ([1.0, 1.0, 0.5, 0.0], [0.0, 0.0, 0.5, 0.0])
        );
        assert_eq!(v.clip, (640, 180));
        assert_eq!(
            producer_viewport(&d, (640, 180), 0.0, 0.5),
            Err(EffectsError::Viewport)
        );
        assert_eq!(
            producer_viewport(&d, (320, 180), 0.0, 1.0),
            Err(EffectsError::Viewport)
        );
        assert_eq!(
            producer_viewport(&d, (640, 180), -0.0, 1.0)
                .unwrap()
                .native
                .width,
            640.0
        );
    }

    fn rows(g: &FullscreenGeometry) -> Vec<[f32; 8]> {
        g.vertices
            .chunks(32)
            .map(|c| {
                std::array::from_fn(|i| f32::from_le_bytes(c[4 * i..4 * i + 4].try_into().unwrap()))
            })
            .collect()
    }

    #[test]
    fn v6_v7_geometry() {
        let c = constants();
        let quad = fullscreen_quad(c.l, c.h).unwrap();
        assert_eq!(
            rows(&quad),
            vec![
                [-0.75, -0.75, -0.75, 1.0, 0.0, 0.5, 0.0, 1.0],
                [0.5, -0.75, -0.75, 1.0, 0.5, 0.5, 0.0, 1.0],
                [0.5, 0.5, -0.75, 1.0, 0.5, 0.0, 0.0, 1.0],
                [-0.75, 0.5, -0.75, 1.0, 0.0, 0.0, 0.0, 1.0],
            ]
        );
        assert_eq!(quad.indices, vec![0, 1, 2, 0, 2, 3]);
        assert_eq!(
            quad.attribute0.map(f32::from_bits),
            [-0.75, 0.5, -0.75, 1.0]
        );
        assert_eq!(quad.attribute8.map(f32::from_bits), [0.0, 0.0, 0.0, 1.0]);
        let tri = fullscreen_triangle(c.l, c.h, c.f, c.g).unwrap();
        assert_eq!(
            rows(&tri),
            vec![
                [-0.75, -0.75, -0.75, 1.0, 0.0, 0.5, 0.0, 1.0],
                [2.5, -0.75, -0.75, 1.0, 1.75, 0.5, 0.0, 1.0],
                [-0.75, 2.5, -0.75, 1.0, 0.0, -0.75, 0.0, 1.0],
            ]
        );
        assert_eq!(tri.attribute0.map(f32::from_bits), [-0.75, 2.5, -0.75, 1.0]);
        assert_eq!(tri.attribute8.map(f32::from_bits), [0.0, -0.75, 0.0, 1.0]);
        assert_eq!(
            fullscreen_quad(f32::NAN.to_bits(), c.h),
            Err(EffectsError::NonFinite("full-screen constant"))
        );
        // Denormal constants are copied unchanged.
        assert_eq!(fullscreen_quad(1, c.h).unwrap().attribute0[0], 1);
    }

    const S_DESCRIPTOR: [u32; 8] = [
        0x1000,
        0x0001_8522,
        0x0003_0303,
        0x8000_0000,
        0x0000_aae4,
        0x0202_4abc,
        0x0140_00b4,
        0,
    ];

    /// A fully known state for producer draws, with depth bounds on, offset fill on and a
    /// counter-clockwise front face.
    pub(crate) fn producer_state() -> PipelineState {
        PipelineState {
            depth_bounds_enable: Some(true),
            depth_bounds: Some([0, 0x3f80_0000]),
            polygon_offset_fill: Some(true),
            offset_factor: Some(0),
            offset_units: Some(0),
            front_face: Some(crate::setup::FrontFace::CounterClockwise),
            ..crate::raster::tests::complete()
        }
    }

    fn mask_fixture() -> (RendererObject, Vec<SurfaceBinding>) {
        let mut r = renderer(true);
        r.surface_descriptors[2].value = S_DESCRIPTOR;
        let mut bindings = all_bindings(&r);
        for b in &mut bindings {
            b.depth_initialised = false;
            b.stencil_initialised = false;
        }
        (r, bindings)
    }

    #[test]
    fn v8_to_v10_stencil_mask() {
        let (r, bindings) = mask_fixture();
        let usage = ProgramUsage {
            sampled_units: 1,
            discards: true,
        };
        let out = stencil_mask(
            &r,
            true,
            &usage,
            &bindings,
            &constants(),
            &producer_state(),
            &Default::default(),
        )
        .unwrap();
        let pass = out.pass.as_ref().unwrap();
        assert_eq!(pass.target, select(&r, 1, 0).unwrap().address);
        assert_eq!(pass.textures.len(), 1);
        assert_eq!(pass.textures[0].view.mip_count, 1);
        assert_eq!(out.units[0].compact.unwrap()[5], 0x0101_4abc);
        assert_eq!(out.units[0].compact.unwrap()[3], 0);
        // Pipeline during the draw.
        let s = pass.state;
        assert_eq!(
            (s.stencil_enable, s.two_sided_stencil),
            (Some(true), Some(true))
        );
        assert_eq!(s.front_stencil, Some(MASK_FACE));
        assert_eq!(
            (s.depth_test, s.depth_write, s.colour_masks),
            (Some(false), Some(false), Some([0; 4]))
        );
        assert_eq!(
            (s.depth_bounds_enable, s.polygon_offset_fill),
            (Some(true), Some(true))
        );
        // After the draw.
        assert_eq!(out.state.front_stencil.unwrap().write_mask, 0);
        assert_eq!(out.state.back_stencil.unwrap().write_mask, 0);
        assert_eq!(out.state.front_stencil.unwrap().reference, 0x81);
        assert_eq!(
            out.state.front_face,
            Some(crate::setup::FrontFace::CounterClockwise)
        );
        // Attachments.
        assert_eq!(pass.colour, None);
        assert_eq!(
            (pass.depth.unwrap().load, pass.stencil.unwrap().load),
            (LoadOp::DontCare, LoadOp::DontCare)
        );
        assert_eq!(pass.geometry.indices.len(), 6);
        // A 320x360 image rect fails the sampled-size check.
        let mut tall = r.clone();
        tall.surface_descriptors[2].value[6] = 0x0140_0168;
        assert_eq!(
            stencil_mask(
                &tall,
                true,
                &usage,
                &bindings,
                &constants(),
                &producer_state(),
                &Default::default()
            ),
            Err(EffectsError::SampledImage)
        );
    }

    #[test]
    fn v11_wrong_depth_format_and_no_ops() {
        let (mut r, bindings) = mask_fixture();
        let usage = ProgramUsage {
            sampled_units: 1,
            discards: false,
        };
        let d = table_position(0, 1);
        r.surface_table[d].value.words[3] = 8 | (1 << 5);
        assert_eq!(
            stencil_mask(
                &r,
                true,
                &usage,
                &bindings,
                &constants(),
                &producer_state(),
                &Default::default()
            ),
            Err(EffectsError::DepthTarget)
        );
        let (r, bindings) = mask_fixture();
        let state = producer_state();
        let out = stencil_mask(
            &r,
            false,
            &usage,
            &bindings,
            &constants(),
            &state,
            &Default::default(),
        )
        .unwrap();
        assert!(out.pass.is_none());
        let disabled = renderer(false);
        assert!(
            stencil_mask(
                &disabled,
                true,
                &usage,
                &bindings,
                &constants(),
                &state,
                &Default::default()
            )
            .unwrap()
            .pass
            .is_none()
        );
        let unit2 = ProgramUsage {
            sampled_units: 4,
            discards: false,
        };
        assert_eq!(
            stencil_mask(
                &r,
                true,
                &unit2,
                &bindings,
                &constants(),
                &state,
                &Default::default()
            ),
            Err(EffectsError::SampledUnit(2))
        );
        let unknown = PipelineState {
            front_face: None,
            ..state
        };
        assert_eq!(
            stencil_mask(
                &r,
                true,
                &usage,
                &bindings,
                &constants(),
                &unknown,
                &Default::default()
            ),
            Err(EffectsError::Raster("cull"))
        );
    }

    #[test]
    fn v12_v13_events() {
        let setup = SetupReferences {
            lists_enabled: true,
            initial: [0x400, 0x404, 0x408],
            group_thirteen: 0x500,
            group_fifteen: [0, 0x600],
        };
        let initial = |slot, target| ProducerEvent::Initial { slot, target };
        assert_eq!(
            route_event(initial(0, 0x400), &setup, Some(0x100)),
            Ok(Routed::NoOp)
        );
        assert_eq!(
            route_event(initial(0, 0x400), &setup, None),
            Err(EffectsError::GenerationFlag)
        );
        assert_eq!(
            route_event(initial(0, 0x400), &setup, Some(1)),
            Ok(Routed::StencilMask)
        );
        assert_eq!(
            route_event(initial(1, 0x404), &setup, Some(1)),
            Ok(Routed::Lighting)
        );
        assert_eq!(
            route_event(initial(2, 0x408), &setup, None),
            Ok(Routed::NoOp)
        );
        assert_eq!(
            route_event(initial(2, 0x404), &setup, Some(1)),
            Err(EffectsError::Reference)
        );
        assert_eq!(
            route_event(initial(0, 0), &setup, Some(1)),
            Err(EffectsError::Reference)
        );
        assert_eq!(
            route_event(ProducerEvent::Group13 { target: 0x500 }, &setup, Some(1)),
            Ok(Routed::Fog)
        );
        assert_eq!(
            route_event(
                ProducerEvent::Group15 {
                    slot: 1,
                    target: 0x600
                },
                &setup,
                Some(1)
            ),
            Ok(Routed::Downsample)
        );
        assert_eq!(
            route_event(
                ProducerEvent::Group15 {
                    slot: 0,
                    target: 0x600
                },
                &setup,
                Some(1)
            ),
            Err(EffectsError::Reference)
        );
        assert_eq!(
            route_event(ProducerEvent::Finish { previous_group: 3 }, &setup, None),
            Ok(Routed::Finish)
        );
    }

    #[test]
    fn v14_main_draw_inputs() {
        let mut record13 = record(320, 180, 13, 2, 3);
        record13.words[1] = 320 << 16;
        let binding = SurfaceBinding {
            colour_format: 13,
            depth_format: 16,
            ..binding(0x1000, (640, 180), 1)
        };
        let mut constant = BlendConstant::Known([1; 4]);
        let inputs = main_draw_inputs(&binding, &record13, Some(8), &mut constant).unwrap();
        assert_eq!(
            inputs,
            MainDrawInputs {
                colour_format: 13,
                depth_format: 16,
                raster_colour_format: 13,
                sample_grid: (2, 1)
            }
        );
        assert_eq!(constant, BlendConstant::Unknown);
        let mut kept = BlendConstant::Known([1; 4]);
        main_draw_inputs(&binding, &record13, Some(13), &mut kept).unwrap();
        assert_eq!(kept, BlendConstant::Known([1; 4]));
    }

    #[test]
    fn v15_colour_target_and_pass_open() {
        let b = binding(0x1000, (320, 180), 1);
        assert_eq!(open_main_pass(Some(1), &b), Ok(()));
        for selector in [0, 2, 0x13] {
            assert_eq!(
                open_main_pass(Some(selector), &b),
                Err(EffectsError::Unsupported("colour target"))
            );
        }
        assert_eq!(open_main_pass(None, &b), Err(EffectsError::ColourTarget));
        let uninitialised = SurfaceBinding {
            depth_initialised: false,
            ..b
        };
        assert_eq!(
            open_main_pass(Some(1), &uninitialised),
            Err(EffectsError::Target("depth load"))
        );
        let cleared = SurfaceBinding {
            colour: Some(AttachmentImage {
                image: 1,
                load: LoadOp::Clear,
            }),
            ..b
        };
        assert_eq!(
            open_main_pass(Some(1), &cleared),
            Err(EffectsError::Target("colour load"))
        );
    }

    #[test]
    fn producer_bindings_and_group_twelve() {
        let images = [PacketImageInfo {
            format: 13,
            width: 160,
            height: 90,
            dimension: 2,
            samples: 1,
            layers: 1,
        }];
        let words = [0x2000, 0x0001_a522, 0, 0, 0, 0, 0x00a0_005a, 0];
        let binding = ProducerTextureBinding {
            address: 0x3120,
            words,
            image: 0,
            format: 13,
            width: 160,
            height: 90,
        };
        let known = [(0x3120, words)];
        assert_eq!(
            validate_producer_bindings(&[binding], &images, &known),
            Ok(())
        );
        assert_eq!(
            validate_producer_bindings(&[binding, binding], &images, &known),
            Err(EffectsError::ProducerBinding("duplicate address"))
        );
        assert_eq!(
            validate_producer_bindings(&[binding], &images, &[]),
            Err(EffectsError::ProducerBinding("source"))
        );
        let wrong = ProducerTextureBinding {
            width: 80,
            ..binding
        };
        assert_eq!(
            validate_producer_bindings(&[wrong], &images, &known),
            Err(EffectsError::ProducerBinding("dimensions"))
        );
        let missing = ProducerTextureBinding {
            image: 4,
            ..binding
        };
        assert_eq!(
            validate_producer_bindings(&[missing], &images, &known),
            Err(EffectsError::ProducerBinding("image"))
        );
        let requests = [
            ImageRequest {
                unit: 11,
                source_address: 0x3120,
                descriptor: words,
            },
            ImageRequest {
                unit: 12,
                source_address: 0x4000,
                descriptor: words,
            },
        ];
        assert_eq!(
            associate_group_twelve(&requests, &[binding], &images),
            Ok(vec![Some(0), None])
        );
        let mismatched = ProducerTextureBinding {
            format: 1,
            ..binding
        };
        assert_eq!(
            associate_group_twelve(&requests, &[mismatched], &images),
            Err(EffectsError::Association)
        );
    }
}
