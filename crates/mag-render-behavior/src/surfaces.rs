//! Main producer surfaces, viewports and texture associations (`main-producer-inputs.md`).

use crate::producers::{ProducerInputs, RendererObject, Tagged};
use crate::setup::MainSetupRecord;

/// DMA context of local video memory.
pub const DMA_LOCAL: u32 = 0xfeed_0000;
/// DMA context of main memory.
pub const DMA_MAIN: u32 = 0xfeed_0001;

/// An 84-byte surface record: the arguments of the RSX surface setup methods.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct SurfaceRecord {
    pub words: [u32; 21],
}

/// Memory location of an image: 1 local, 2 main.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub enum Location {
    Local = 1,
    Main = 2,
}

impl Location {
    pub fn from_dma(context: u32) -> Option<Self> {
        match context {
            DMA_LOCAL => Some(Self::Local),
            DMA_MAIN => Some(Self::Main),
            _ => None,
        }
    }

    pub fn from_descriptor(format_word: u32) -> Option<Self> {
        match format_word & 3 {
            1 => Some(Self::Local),
            2 => Some(Self::Main),
            _ => None,
        }
    }
}

/// Planes a target uses.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Uses {
    pub colour: bool,
    pub depth: bool,
    pub stencil: bool,
}

impl Uses {
    pub const NONE: Self = Self {
        colour: false,
        depth: false,
        stencil: false,
    };
    pub const COLOUR: Self = Self {
        colour: true,
        depth: false,
        stencil: false,
    };
    pub const DEPTH: Self = Self {
        colour: false,
        depth: true,
        stencil: false,
    };
    pub const DEPTH_STENCIL: Self = Self {
        colour: false,
        depth: true,
        stencil: true,
    };

    pub fn union(self, other: Self) -> Self {
        Self {
            colour: self.colour || other.colour,
            depth: self.depth || other.depth,
            stencil: self.stencil || other.stencil,
        }
    }
}

impl SurfaceRecord {
    pub fn from_bytes(bytes: &[u8; 84]) -> Self {
        Self {
            words: crate::guest::be_words(bytes, 0),
        }
    }

    fn word(&self, offset: usize) -> u32 {
        self.words[offset / 4]
    }

    pub fn width(&self) -> u32 {
        self.word(0x04) >> 16
    }
    pub fn x_origin(&self) -> u32 {
        self.word(0x04) & 0xffff
    }
    pub fn height(&self) -> u32 {
        self.word(0x08) >> 16
    }
    pub fn y_origin(&self) -> u32 {
        self.word(0x08) & 0xffff
    }
    pub fn colour_format(&self) -> u32 {
        self.word(0x0c) & 0x1f
    }
    pub fn depth_format(&self) -> u32 {
        (self.word(0x0c) >> 5) & 7
    }
    pub fn surface_type(&self) -> u32 {
        (self.word(0x0c) >> 8) & 0xf
    }
    pub fn aa_mode(&self) -> u32 {
        (self.word(0x0c) >> 12) & 0xf
    }
    pub fn colour_pitch(&self) -> u32 {
        self.word(0x10)
    }
    pub fn colour_offset(&self) -> u32 {
        self.word(0x20)
    }
    pub fn colour_dma(&self) -> u32 {
        self.word(0x30)
    }
    pub fn depth_offset(&self) -> u32 {
        self.word(0x40)
    }
    pub fn depth_pitch(&self) -> u32 {
        self.word(0x44)
    }
    pub fn depth_dma(&self) -> u32 {
        self.word(0x48)
    }
    pub fn window_offset(&self) -> u32 {
        self.word(0x4c)
    }

    /// Colour formats 8, 11, 12 and 13 and depth formats 0, 1 and 2 are accepted.
    pub fn formats_accepted(&self) -> bool {
        matches!(self.colour_format(), 8 | 11 | 12 | 13) && self.depth_format() <= 2
    }

    /// Sample grid of the anti-aliasing mode, or `None` when unsupported.
    pub fn sample_grid(&self) -> Option<(u32, u32)> {
        match self.aa_mode() {
            0 => Some((1, 1)),
            3 => Some((2, 1)),
            4 | 5 => Some((2, 2)),
            _ => None,
        }
    }

    /// `(width × grid_x, height × grid_y)`.
    pub fn physical_size(&self) -> Option<(u32, u32)> {
        let (gx, gy) = self.sample_grid()?;
        Some((self.width() * gx, self.height() * gy))
    }

    /// Colour, plus depth and stencil for Z24S8, plus depth only for Z16.
    pub fn target_uses(&self) -> Uses {
        match self.depth_format() {
            2 => Uses {
                colour: true,
                depth: true,
                stencil: true,
            },
            1 => Uses {
                colour: true,
                depth: true,
                stencil: false,
            },
            _ => Uses::COLOUR,
        }
    }
}

/// Fields of a compact texture descriptor used by the association rules.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct DescriptorFields {
    pub offset: u32,
    pub location_bits: u32,
    pub cube: bool,
    pub dimension: u32,
    pub base_format: u32,
    pub mip_count: u32,
    pub width: u32,
    pub height: u32,
    pub pitch: u32,
    pub depth: u32,
}

impl DescriptorFields {
    pub fn of(d: &[u32; 8]) -> Self {
        Self {
            offset: d[0],
            location_bits: d[1] & 3,
            cube: d[1] & 4 != 0,
            dimension: (d[1] >> 4) & 0xf,
            base_format: (d[1] >> 8) & 0x9f,
            mip_count: d[1] >> 16,
            width: d[6] >> 16,
            height: d[6] & 0xffff,
            pitch: d[7] & 0xf_ffff,
            depth: d[7] >> 20,
        }
    }

    fn single_2d(&self) -> bool {
        self.dimension == 2 && !self.cube && self.mip_count == 1 && self.depth <= 1
    }
}

/// Inputs to surface selection.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct SelectionControls {
    pub bank_zero_enabled: bool,
    pub aa_enable: bool,
    pub role_forcing: bool,
}

impl SelectionControls {
    pub fn of(renderer: &RendererObject) -> Self {
        Self {
            bank_zero_enabled: renderer.bank_zero_enabled(),
            aa_enable: renderer.aa_enable != 0,
            role_forcing: renderer.role_forcing(),
        }
    }
}

/// A selected surface record.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct SelectedSurface {
    pub index: u32,
    /// The effective role.
    pub role: u32,
    pub address: u32,
    pub record: SurfaceRecord,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SurfaceError {
    Selection {
        index: u32,
        role: u32,
    },
    NonzeroOrigin {
        address: u32,
    },
    UnsupportedAa {
        address: u32,
        mode: u32,
    },
    /// A target smaller than the record's physical size.
    TargetTooSmall {
        address: u32,
    },
    /// Clip range other than exactly `[0, 1]`.
    ClipRange,
    NonFiniteViewport,
    ViewportLimits,
    ScissorOutside,
    DepthAsColour,
    InvalidLocation(u32),
    /// A considered candidate whose colour DMA context is neither local nor main memory.
    InvalidDma {
        address: u32,
    },
    AmbiguousSource,
    UnsupportedDepthSource,
    /// Main cannot run its producers with this setup record or invocation.
    MainUnsupported,
    /// The lighting fragment program samples a unit other than 0 and 1.
    LightingUnit(u32),
    /// Two entries for one address with different record contents.
    ConflictingRecord {
        address: u32,
    },
}

/// The effective role of a request.
pub fn effective_role(role: u32, controls: SelectionControls) -> u32 {
    if (!controls.bank_zero_enabled && role <= 1)
        || (!controls.aa_enable && role == 3)
        || controls.role_forcing
    {
        2
    } else {
        role
    }
}

/// Table position `2·role + index` of a record.
pub fn table_position(role: u32, index: u32) -> usize {
    (2 * role + index) as usize
}

/// `select(i, r)` over a renderer's surface table.
pub fn select(
    renderer: &RendererObject,
    index: u32,
    role: u32,
) -> Result<SelectedSurface, SurfaceError> {
    if index > 1 || role > 3 {
        return Err(SurfaceError::Selection { index, role });
    }
    let role = effective_role(role, SelectionControls::of(renderer));
    let Tagged { address, value } = renderer.surface_table[table_position(role, index)];
    Ok(SelectedSurface {
        index,
        role,
        address,
        record: value,
    })
}

/// A source viewport in guest terms.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct SourceViewport {
    pub scale: [f32; 4],
    pub offset: [f32; 4],
    pub clip: [f32; 2],
    /// `(x, y, width, height)`.
    pub scissor: [u32; 4],
}

/// Viewport constants a source viewport needs.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct ViewportConstants {
    pub vp_hx: f32,
    pub vp_hy: f32,
    pub clip_min: f32,
    pub clip_max: f32,
    /// `f32` at `JITTER+4` and `JITTER+8`.
    pub jitter: [f32; 2],
}

impl ViewportConstants {
    pub fn of(inputs: &ProducerInputs) -> Self {
        let f = f32::from_bits;
        Self {
            vp_hx: f(inputs.scalars.vp_hx.value),
            vp_hy: f(inputs.scalars.vp_hy.value),
            clip_min: f(inputs.scalars.clip_min.value),
            clip_max: f(inputs.scalars.clip_max.value),
            jitter: [f(inputs.jitter.value[1]), f(inputs.jitter.value[2])],
        }
    }
}

/// The jittered source viewport of a surface record.
pub fn source_viewport(
    surface: &SelectedSurface,
    constants: &ViewportConstants,
) -> Result<SourceViewport, SurfaceError> {
    let record = &surface.record;
    if record.x_origin() != 0 || record.y_origin() != 0 {
        return Err(SurfaceError::NonzeroOrigin {
            address: surface.address,
        });
    }
    let (w, h) = (record.width(), record.height());
    let hx = constants.vp_hx;
    let mut jx = constants.jitter[0];
    let mut jy = constants.jitter[1];
    let aa = record.aa_mode();
    if aa != 0 {
        jx *= hx;
    }
    if aa != 0 && aa != 3 {
        jy *= hx;
    }
    let (wf, hf) = (w as f32, h as f32);
    Ok(SourceViewport {
        scale: [wf * hx, hf * constants.vp_hy, hx, 0.0],
        offset: [wf * hx + jx, hf * hx + jy, hx, 0.0],
        clip: [constants.clip_min, constants.clip_max],
        scissor: [0, 0, w, h],
    })
}

/// The explicit producer viewport for `W × H` and depth range `[z0, z1]`.
pub fn explicit_viewport(width: u32, height: u32, z0: f32, z1: f32) -> SourceViewport {
    let (w, h) = (width as f32, height as f32);
    SourceViewport {
        scale: [w / 2.0, -h / 2.0, (z1 - z0) / 2.0, 0.0],
        offset: [w / 2.0, h / 2.0, (z1 + z0) / 2.0, 0.0],
        clip: [z0, z1],
        scissor: [0, 0, width, height],
    }
}

/// A viewport on a native target. The source z scale and offset stay a shader-side transform.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct NativeViewport {
    pub x: f32,
    pub y: f32,
    pub width: f32,
    pub height: f32,
    pub min_depth: f32,
    pub max_depth: f32,
    pub scissor: [u32; 4],
    /// Source z scale and offset, applied once in the shader.
    pub z_scale: f32,
    pub z_offset: f32,
}

/// Maps a source viewport onto a target of physical size `(pw, ph)` with sample grid `(gx, gy)`.
pub fn map_viewport(
    source: &SourceViewport,
    physical: (u32, u32),
    grid: (u32, u32),
) -> Result<NativeViewport, SurfaceError> {
    if source.clip != [0.0, 1.0] {
        return Err(SurfaceError::ClipRange);
    }
    if source
        .scale
        .iter()
        .chain(&source.offset)
        .any(|value| !value.is_finite())
    {
        return Err(SurfaceError::NonFiniteViewport);
    }
    let (gx, gy) = (grid.0 as f32, grid.1 as f32);
    let [sx, sy, sz, _] = source.scale;
    let [ox, oy, oz, _] = source.offset;
    let x = (ox - sx) * gx;
    let y = (oy + sy) * gy;
    let width = 2.0 * sx * gx;
    let height = -2.0 * sy * gy;
    let within = width > 0.0
        && width <= 16384.0
        && height > 0.0
        && height <= 16384.0
        && x >= -32768.0
        && y >= -32768.0
        && x + width <= 32767.0
        && y + height <= 32767.0;
    if !within {
        return Err(SurfaceError::ViewportLimits);
    }
    let [sx0, sy0, sw, sh] = source.scissor;
    let scissor = [
        sx0 as u64 * grid.0 as u64,
        sy0 as u64 * grid.1 as u64,
        sw as u64 * grid.0 as u64,
        sh as u64 * grid.1 as u64,
    ];
    if scissor[0] + scissor[2] > physical.0 as u64 || scissor[1] + scissor[3] > physical.1 as u64 {
        return Err(SurfaceError::ScissorOutside);
    }
    Ok(NativeViewport {
        x,
        y,
        width,
        height,
        min_depth: 0.0,
        max_depth: 1.0,
        scissor: scissor.map(|value| value as u32),
        z_scale: sz,
        z_offset: oz,
    })
}

/// Builds and maps a viewport for a surface on a target of physical size `physical`.
pub fn surface_target_viewport(
    surface: &SelectedSurface,
    source: &SourceViewport,
    physical: (u32, u32),
) -> Result<NativeViewport, SurfaceError> {
    let record = &surface.record;
    let grid = record.sample_grid().ok_or(SurfaceError::UnsupportedAa {
        address: surface.address,
        mode: record.aa_mode(),
    })?;
    let needed = record.physical_size().expect("grid is supported");
    if physical.0 < needed.0 || physical.1 < needed.1 {
        return Err(SurfaceError::TargetTooSmall {
            address: surface.address,
        });
    }
    map_viewport(source, physical, grid)
}

/// Colour association of a compact descriptor over candidate records `(address, record)`.
/// Returns the index of the first matching candidate.
pub fn associate_colour(
    descriptor: &[u32; 8],
    candidates: &[(u32, SurfaceRecord)],
) -> Result<Option<usize>, SurfaceError> {
    let d = DescriptorFields::of(descriptor);
    let format = match d.base_format {
        0x85 => 8,
        0x9a => 11,
        0x9b => 12,
        0x9c => 13,
        0x90..=0x93 => return Err(SurfaceError::DepthAsColour),
        _ => return Ok(None),
    };
    if !d.single_2d() {
        return Ok(None);
    }
    let location = Location::from_descriptor(descriptor[1])
        .ok_or(SurfaceError::InvalidLocation(d.location_bits))?;
    // Candidates with the mapped colour format are considered in order; others are skipped
    // without looking at their mode. The image a match describes is identified by (location,
    // offset, pitch, format, physical size, sample grid); matches agree on the first five by
    // construction, so they can only differ in their sample grid.
    let mut first: Option<(usize, (u32, u32))> = None;
    for (position, (address, record)) in candidates.iter().enumerate() {
        if record.colour_format() != format {
            continue;
        }
        let grid = record.sample_grid().ok_or(SurfaceError::UnsupportedAa {
            address: *address,
            mode: record.aa_mode(),
        })?;
        let dma = Location::from_dma(record.colour_dma())
            .ok_or(SurfaceError::InvalidDma { address: *address })?;
        let matches = record.physical_size() == Some((d.width, d.height))
            && dma == location
            && record.colour_offset() == d.offset
            && record.colour_pitch() == d.pitch;
        if !matches {
            continue;
        }
        match first {
            None => first = Some((position, grid)),
            Some((_, earlier)) if earlier != grid => return Err(SurfaceError::AmbiguousSource),
            Some(_) => {}
        }
    }
    Ok(first.map(|(position, _)| position))
}

/// How the depth plane of a lighting source is read.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DepthPlaneKind {
    /// A Z24S8 plane read as packed A8R8G8B8 texels.
    PackedAlias,
    /// A Z24S8 plane read as normalised 24-bit depth.
    NormalisedDepth,
}

/// A depth plane named by a lighting source descriptor.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct DepthSource {
    pub kind: DepthPlaneKind,
    pub location: Location,
    pub offset: u32,
    pub pitch: u32,
    pub width: u32,
    pub height: u32,
    /// The owning surface record of a packed alias.
    pub owner: Option<u32>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DepthClass {
    Depth(DepthSource),
    Unsupported,
    NotDepth,
}

/// Classifies a lighting source descriptor read from `address`.
pub fn classify_depth(
    renderer: &RendererObject,
    descriptor: &[u32; 8],
    address: u32,
) -> DepthClass {
    let d = DescriptorFields::of(descriptor);
    let alias = d.base_format == 0x85
        && (address == renderer.address + 0x3120 || address == renderer.address + 0x3140);
    let native = d.base_format == 0x90;
    if !alias && !native {
        return if (0x91..=0x93).contains(&d.base_format) {
            DepthClass::Unsupported
        } else {
            DepthClass::NotDepth
        };
    }
    if !d.single_2d() || !(1..=16384).contains(&d.width) || !(1..=16384).contains(&d.height) {
        return DepthClass::Unsupported;
    }
    let Some(location) = Location::from_descriptor(descriptor[1]) else {
        return DepthClass::Unsupported;
    };
    if alias {
        let owner = if address == renderer.address + 0x3120 {
            renderer.surface_table[table_position(1, 0)]
        } else {
            renderer.downsample_record
        };
        let record = owner.value;
        if record.depth_format() != 2
            || Location::from_dma(record.depth_dma()) != Some(location)
            || record.depth_offset() != d.offset
            || record.depth_pitch() != d.pitch
        {
            return DepthClass::Unsupported;
        }
        return DepthClass::Depth(DepthSource {
            kind: DepthPlaneKind::PackedAlias,
            location,
            offset: d.offset,
            pitch: d.pitch,
            width: d.width,
            height: d.height,
            owner: Some(owner.address),
        });
    }
    if d.pitch < 4 * d.width || !d.pitch.is_multiple_of(4) {
        return DepthClass::Unsupported;
    }
    DepthClass::Depth(DepthSource {
        kind: DepthPlaneKind::NormalisedDepth,
        location,
        offset: d.offset,
        pitch: d.pitch,
        width: d.width,
        height: d.height,
        owner: None,
    })
}

/// The two lighting texture sources: unit 0 compact, unit 1 as its full descriptor.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct LightingSources {
    pub unit0: Tagged<[u32; 8]>,
    pub unit1: Tagged<[u32; 12]>,
}

impl LightingSources {
    pub fn of(inputs: &ProducerInputs) -> Self {
        Self {
            unit0: inputs.renderer.lighting_source(),
            unit1: inputs.params.lighting_unit1,
        }
    }

    /// The compact descriptor of a lighting unit (0 or 1).
    pub fn compact(&self, unit: u32) -> Tagged<[u32; 8]> {
        if unit == 0 {
            self.unit0
        } else {
            Tagged {
                address: self.unit1.address,
                value: crate::texture::compact_of_full(&self.unit1.value),
            }
        }
    }
}

/// One producer texture request: lighting units 0 and 1, group-twelve units 11 to 13.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ProducerTextureRequest {
    pub unit: u32,
    pub source_address: u32,
    /// The unmodified compact descriptor.
    pub descriptor: [u32; 8],
}

/// What a producer texture request is bound to.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum BindingSource {
    /// A colour image derived from a depth plane, by index into the derived list.
    Derived(usize),
    /// The guest image the descriptor describes, imported normally.
    Guest,
}

/// A colour image derived from a depth plane with one encoding and one size.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct DerivedImage {
    pub source: DepthSource,
}

/// The producer texture binding list, built before any producer runs.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ProducerBindingPlan {
    pub requests: Vec<(ProducerTextureRequest, BindingSource)>,
    pub derived: Vec<DerivedImage>,
}

/// The two lighting requests: unit 0 compact, unit 1 by the compact form of its full descriptor.
pub fn lighting_requests(inputs: &ProducerInputs) -> [ProducerTextureRequest; 2] {
    let sources = LightingSources::of(inputs);
    [0, 1].map(|unit| {
        let compact = sources.compact(unit);
        ProducerTextureRequest {
            unit,
            source_address: compact.address,
            descriptor: compact.value,
        }
    })
}

/// Classifies every producer texture request and shares derived images between requests with
/// the same depth plane, encoding and image size. An unsupported classification fails the list.
pub fn plan_producer_bindings(
    renderer: &RendererObject,
    requests: &[ProducerTextureRequest],
) -> Result<ProducerBindingPlan, SurfaceError> {
    let mut plan = ProducerBindingPlan {
        requests: Vec::new(),
        derived: Vec::new(),
    };
    for request in requests {
        let binding = match classify_depth(renderer, &request.descriptor, request.source_address) {
            DepthClass::Unsupported => return Err(SurfaceError::UnsupportedDepthSource),
            DepthClass::NotDepth => BindingSource::Guest,
            DepthClass::Depth(source) => {
                let key =
                    |s: &DepthSource| (s.kind, s.location, s.offset, s.pitch, s.width, s.height);
                let index = match plan
                    .derived
                    .iter()
                    .position(|d| key(&d.source) == key(&source))
                {
                    Some(index) => index,
                    None => {
                        plan.derived.push(DerivedImage { source });
                        plan.derived.len() - 1
                    }
                };
                BindingSource::Derived(index)
            }
        };
        plan.requests.push((*request, binding));
    }
    Ok(plan)
}

/// One surface Main needs before it starts.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct RequiredSurface {
    pub address: u32,
    pub record: SurfaceRecord,
    /// Union of attachment and prior uses.
    pub uses: Uses,
    /// Planes Main writes.
    pub attachment: Uses,
    /// Planes whose content must exist before Main writes anything.
    pub prior: Uses,
}

/// Invocation values that come with each Main call.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Invocation {
    /// Low eight bits nonzero when the initial producer banks run.
    pub initial_producer_enable: u32,
    /// Negative means the current index at `RENDERER+0x2FB0`.
    pub target_index: i64,
}

#[derive(Default)]
struct RequiredSet {
    entries: Vec<RequiredSurface>,
}

impl RequiredSet {
    fn add(
        &mut self,
        address: u32,
        record: SurfaceRecord,
        attachment: Uses,
        prior: Uses,
    ) -> Result<(), SurfaceError> {
        if let Some(entry) = self.entries.iter_mut().find(|e| e.address == address) {
            if entry.record != record {
                return Err(SurfaceError::ConflictingRecord { address });
            }
            entry.attachment = entry.attachment.union(attachment);
            entry.prior = entry.prior.union(prior);
            entry.uses = entry.attachment.union(entry.prior);
            return Ok(());
        }
        self.entries.push(RequiredSurface {
            address,
            record,
            uses: attachment.union(prior),
            attachment,
            prior,
        });
        Ok(())
    }

    fn attach(&mut self, surface: &SelectedSurface, uses: Uses) -> Result<(), SurfaceError> {
        self.add(surface.address, surface.record, uses, Uses::NONE)
    }

    fn prior(&mut self, surface: &SelectedSurface, uses: Uses) -> Result<(), SurfaceError> {
        self.add(surface.address, surface.record, Uses::NONE, uses)
    }
}

/// The surfaces Main requires before it starts, in order.
///
/// `lighting_units` is the set of texture units the lighting fragment program samples (bit `u`
/// for unit `u`), from the program decode contract. `inherited_unit10` is texture unit 10's
/// compact descriptor in the state inherited from before Main, when known.
pub fn required_surfaces(
    inputs: &ProducerInputs,
    setup: &MainSetupRecord,
    invocation: Invocation,
    lighting_units: u16,
    inherited_unit10: Option<[u32; 8]>,
) -> Result<Vec<RequiredSurface>, SurfaceError> {
    if !setup.lists_enabled
        || setup.initial[1] == 0
        || invocation.initial_producer_enable & 0xff == 0
    {
        return Err(SurfaceError::MainUnsupported);
    }
    let renderer = &inputs.renderer;
    let programs = &inputs.programs;
    let bank_zero = renderer.bank_zero_enabled();
    let k = if invocation.target_index < 0 {
        renderer.current_index
    } else {
        u32::try_from(invocation.target_index).unwrap_or(u32::MAX)
    };
    let s = |index, role| select(renderer, index, role);
    let mut set = RequiredSet::default();

    let main = s(k, 1)?;
    set.attach(&main, main.record.target_uses())?;
    if setup.initial[0] != 0 && bank_zero && programs.stencil.both_present() {
        set.prior(&s(0, 1)?, Uses::COLOUR)?;
        set.attach(&s(1, 0)?, Uses::DEPTH_STENCIL)?;
    }
    let lighting_target = s(1, 1)?;
    set.attach(&lighting_target, lighting_target.record.target_uses())?;
    if bank_zero {
        let second = s(1, 0)?;
        set.attach(&second, second.record.target_uses())?;
    }
    let fog = &programs.fog;
    if setup.group_thirteen != 0
        && fog.vertex.is_some()
        && (fog.untinted.is_some() || fog.tinted.is_some())
        && !renderer.role_forcing()
        && bank_zero
    {
        let target = s(k, 0)?;
        set.attach(&target, target.record.target_uses())?;
    }
    let quincunx = setup.group_fifteen[0] != 0 && bank_zero && programs.quincunx.both_present();
    let k2 = if quincunx { k ^ 1 } else { k };
    if (setup.group_fifteen[0] != 0 || setup.group_fifteen[1] != 0) && bank_zero {
        if quincunx {
            let target = s(k2, 2)?;
            set.attach(&target, target.record.target_uses())?;
        }
        if setup.group_fifteen[1] != 0 && programs.downsample.both_present() {
            let record = renderer.downsample_record;
            set.add(record.address, record.value, Uses::DEPTH, Uses::NONE)?;
            let target = s(k2, 2)?;
            set.attach(&target, target.record.target_uses())?;
        }
    }
    if setup.restore != 0 {
        let target = s(k2, 2)?;
        set.attach(&target, target.record.target_uses())?;
    }

    let sources = LightingSources::of(inputs);
    for unit in 0..16 {
        if lighting_units & (1 << unit) == 0 {
            continue;
        }
        if unit > 1 {
            return Err(SurfaceError::LightingUnit(unit));
        }
        let descriptor = sources.compact(unit).value;
        let d = DescriptorFields::of(&descriptor);
        if (0x90..=0x93).contains(&d.base_format) {
            let location = Location::from_descriptor(descriptor[1]);
            for entry in &mut set.entries {
                let record = &entry.record;
                if entry.uses.depth
                    && location.is_some()
                    && Location::from_dma(record.depth_dma()) == location
                    && record.depth_offset() == d.offset
                    && record.depth_pitch() == d.pitch
                {
                    entry.prior = entry.prior.union(Uses::DEPTH);
                }
            }
        } else {
            let candidates: Vec<_> = set.entries.iter().map(|e| (e.address, e.record)).collect();
            if let Some(position) = associate_colour(&descriptor, &candidates)? {
                let entry = &mut set.entries[position];
                entry.prior = entry.prior.union(Uses::COLOUR);
                entry.uses = entry.uses.union(Uses::COLOUR);
            }
        }
    }

    if let Some(descriptor) = inherited_unit10
        && associate_colour(
            &descriptor,
            &[(lighting_target.address, lighting_target.record)],
        )?
        .is_some()
    {
        set.prior(&lighting_target, Uses::COLOUR)?;
        let position = set
            .entries
            .iter()
            .position(|e| e.address == lighting_target.address)
            .expect("entry was just added");
        let entry = set.entries.remove(position);
        set.entries.insert(0, entry);
    }
    Ok(set.entries)
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;
    use crate::producers::{acquire, tests as p};

    pub(crate) fn record(
        width: u32,
        height: u32,
        colour: u32,
        depth: u32,
        aa: u32,
    ) -> SurfaceRecord {
        let mut words = [0u32; 21];
        words[1] = width << 16;
        words[2] = height << 16;
        words[3] = colour | (depth << 5) | (aa << 12);
        words[12] = DMA_LOCAL;
        words[18] = DMA_LOCAL;
        SurfaceRecord { words }
    }

    fn renderer_with(bank_zero: bool, aa: bool, forcing: bool) -> RendererObject {
        let mut renderer = acquire(&p::image(), p::TOC).unwrap().renderer;
        for (position, entry) in renderer.surface_table.iter_mut().enumerate() {
            entry.value.words[20] = position as u32;
        }
        renderer.stencil_branch = u8::from(bank_zero);
        renderer.surface_branch = u32::from(bank_zero);
        renderer.aa_enable = u8::from(aa);
        renderer.selection_controls[0] = u32::from(forcing);
        renderer
    }

    fn offset(surface: &SelectedSurface, renderer: &RendererObject) -> u32 {
        surface.address - (renderer.address + 0x2c10)
    }

    #[test]
    fn vector_1_role_forcing() {
        let r = renderer_with(false, true, false);
        assert_eq!(offset(&select(&r, 1, 1).unwrap(), &r), 0x1a4);
        // The specification's vector prints 0x228 for `0xA8·3 + 0x54`; the stated table formula
        // gives 0x24C, which is what is checked (recorded as a specification erratum).
        assert_eq!(offset(&select(&r, 1, 3).unwrap(), &r), 0xa8 * 3 + 0x54);
        let r = renderer_with(false, false, false);
        assert_eq!(offset(&select(&r, 1, 3).unwrap(), &r), 0x1a4);
        let r = renderer_with(true, true, true);
        assert_eq!(select(&r, 0, 0).unwrap().role, 2);
        let r = renderer_with(true, true, false);
        assert_eq!(select(&r, 0, 0).unwrap().role, 0);
        assert_eq!(
            select(&r, 2, 0),
            Err(SurfaceError::Selection { index: 2, role: 0 })
        );
        assert_eq!(
            select(&r, 0, 4),
            Err(SurfaceError::Selection { index: 0, role: 4 })
        );
    }

    fn constants(jitter: [f32; 2]) -> ViewportConstants {
        ViewportConstants {
            vp_hx: 0.5,
            vp_hy: -0.5,
            clip_min: 0.0,
            clip_max: 1.0,
            jitter,
        }
    }

    fn surface(record: SurfaceRecord) -> SelectedSurface {
        SelectedSurface {
            index: 0,
            role: 1,
            address: 0x1000,
            record,
        }
    }

    #[test]
    fn vector_2_surface_viewport() {
        let s = surface(record(1280, 720, 8, 2, 0));
        let v = source_viewport(&s, &constants([0.25, -0.25])).unwrap();
        assert_eq!(v.scale, [640.0, -360.0, 0.5, 0.0]);
        assert_eq!(v.offset, [640.25, 359.75, 0.5, 0.0]);
        let n = surface_target_viewport(&s, &v, (1280, 720)).unwrap();
        assert_eq!((n.x, n.y, n.width, n.height), (0.25, -0.25, 1280.0, 720.0));
        assert_eq!(n.scissor, [0, 0, 1280, 720]);
        assert_eq!(
            (n.min_depth, n.max_depth, n.z_scale, n.z_offset),
            (0.0, 1.0, 0.5, 0.5)
        );
        assert_eq!(
            surface_target_viewport(&s, &v, (1279, 720)),
            Err(SurfaceError::TargetTooSmall { address: 0x1000 })
        );
    }

    #[test]
    fn vector_3_aa_jitter() {
        let s = surface(record(1280, 720, 8, 2, 3));
        let v = source_viewport(&s, &constants([0.25, -0.25])).unwrap();
        assert_eq!(v.offset, [640.125, 359.75, 0.5, 0.0]);
        let n = surface_target_viewport(&s, &v, (2560, 720)).unwrap();
        assert_eq!((n.x, n.width), (0.25, 2560.0));
        assert_eq!(n.scissor, [0, 0, 2560, 720]);
    }

    #[test]
    fn vector_4_aa_mode_four() {
        let s = surface(record(1280, 720, 8, 2, 4));
        let v = source_viewport(&s, &constants([0.25, -0.25])).unwrap();
        assert_eq!(v.offset[0] - 640.0, 0.125);
        assert_eq!(v.offset[1] - 360.0, -0.125);
    }

    #[test]
    fn vector_5_explicit_viewport() {
        let v = explicit_viewport(640, 360, 0.0, 1.0);
        assert_eq!(v.scale, [320.0, -180.0, 0.5, 0.0]);
        assert_eq!(v.offset, [320.0, 180.0, 0.5, 0.0]);
        assert_eq!(v.scissor, [0, 0, 640, 360]);
    }

    #[test]
    fn vector_6_clip_range_refusal() {
        let s = surface(record(1280, 720, 8, 2, 0));
        let mut c = constants([0.0, 0.0]);
        c.clip_max = 0.999;
        let v = source_viewport(&s, &c).unwrap();
        assert_eq!(
            surface_target_viewport(&s, &v, (1280, 720)),
            Err(SurfaceError::ClipRange)
        );
    }

    #[test]
    fn viewport_refusals() {
        let mut s = surface(record(1280, 720, 8, 2, 0));
        s.record.words[1] |= 1;
        assert_eq!(
            source_viewport(&s, &constants([0.0, 0.0])),
            Err(SurfaceError::NonzeroOrigin { address: 0x1000 })
        );
        let s = surface(record(1280, 720, 8, 2, 6));
        let v = source_viewport(&s, &constants([0.0, 0.0])).unwrap();
        assert_eq!(
            surface_target_viewport(&s, &v, (4096, 4096)),
            Err(SurfaceError::UnsupportedAa {
                address: 0x1000,
                mode: 6
            })
        );
        let mut v = explicit_viewport(640, 360, 0.0, 1.0);
        v.offset[0] = f32::NAN;
        assert_eq!(
            map_viewport(&v, (640, 360), (1, 1)),
            Err(SurfaceError::NonFiniteViewport)
        );
        let v = explicit_viewport(20000, 360, 0.0, 1.0);
        assert_eq!(
            map_viewport(&v, (40000, 360), (1, 1)),
            Err(SurfaceError::ViewportLimits)
        );
        let v = explicit_viewport(640, 360, 0.0, 1.0);
        assert_eq!(
            map_viewport(&v, (639, 360), (1, 1)),
            Err(SurfaceError::ScissorOutside)
        );
    }

    fn colour_record() -> SurfaceRecord {
        let mut r = record(1280, 720, 8, 2, 3);
        r.words[8] = 0x7c_0000;
        r.words[4] = 10240;
        r
    }

    fn colour_descriptor() -> [u32; 8] {
        [
            0x7c_0000,
            0x0001_a521,
            0,
            0x8000_0000,
            0xaae4,
            0x0202_0000,
            (2560 << 16) | 720,
            10240,
        ]
    }

    #[test]
    fn vector_7_colour_association() {
        let candidates = [(0x1000, colour_record())];
        assert_eq!(
            associate_colour(&colour_descriptor(), &candidates),
            Ok(Some(0))
        );
        let mut d = colour_descriptor();
        d[6] = (1280 << 16) | 720;
        assert_eq!(associate_colour(&d, &candidates), Ok(None));
        let mut d = colour_descriptor();
        d[7] = 5120;
        assert_eq!(associate_colour(&d, &candidates), Ok(None));
        let mut d = colour_descriptor();
        d[1] = 0x0001_a522;
        assert_eq!(associate_colour(&d, &candidates), Ok(None));
        let mut d = colour_descriptor();
        d[1] = 0x0001_a520;
        assert_eq!(
            associate_colour(&d, &candidates),
            Err(SurfaceError::InvalidLocation(0))
        );
        let mut d = colour_descriptor();
        d[1] = 0x0002_a521;
        assert_eq!(associate_colour(&d, &candidates), Ok(None));
        let mut d = colour_descriptor();
        d[1] = 0x0001_0121;
        assert_eq!(associate_colour(&d, &candidates), Ok(None));
        // The first of several identical matches wins.
        let two = [
            (0x1000, record(64, 64, 8, 0, 0)),
            (0x2000, colour_record()),
            (0x3000, colour_record()),
        ];
        assert_eq!(associate_colour(&colour_descriptor(), &two), Ok(Some(1)));
    }

    #[test]
    fn vectors_11_to_13_ambiguity_aa_and_negative_zero() {
        let mut second = record(2560, 720, 8, 2, 0);
        second.words[8] = 0x7c_0000;
        second.words[4] = 10240;
        assert_eq!(
            associate_colour(
                &colour_descriptor(),
                &[(0x1000, colour_record()), (0x2000, second)]
            ),
            Err(SurfaceError::AmbiguousSource)
        );
        let mut other_format = second;
        other_format.words[3] = (other_format.words[3] & !0x1f) | 11;
        assert_eq!(
            associate_colour(
                &colour_descriptor(),
                &[(0x1000, colour_record()), (0x2000, other_format)]
            ),
            Ok(Some(0))
        );
        let unsupported = record(1280, 720, 8, 2, 2);
        assert_eq!(
            associate_colour(
                &colour_descriptor(),
                &[(0x1000, colour_record()), (0x3000, unsupported)]
            ),
            Err(SurfaceError::UnsupportedAa {
                address: 0x3000,
                mode: 2
            })
        );
        let skipped = record(1280, 720, 11, 2, 2);
        assert_eq!(
            associate_colour(
                &colour_descriptor(),
                &[(0x1000, colour_record()), (0x3000, skipped)]
            ),
            Ok(Some(0))
        );
        let mut bad_dma = colour_record();
        bad_dma.words[12] = 0x1234;
        assert_eq!(
            associate_colour(&colour_descriptor(), &[(0x4000, bad_dma)]),
            Err(SurfaceError::InvalidDma { address: 0x4000 })
        );
        let s = surface(record(1280, 720, 8, 2, 0));
        let mut c = constants([0.0, 0.0]);
        c.clip_min = -0.0;
        let v = source_viewport(&s, &c).unwrap();
        assert!(surface_target_viewport(&s, &v, (1280, 720)).is_ok());
    }

    #[test]
    fn producer_binding_list() {
        let mut inputs = inputs_for_requirements();
        let r = &mut inputs.renderer;
        r.downsample_record.value = record(1280, 720, 8, 2, 0);
        r.downsample_record.value.words[16] = 0x40_0000;
        r.downsample_record.value.words[17] = 5120;
        // Lighting unit 0 at +0x3140: a packed alias of the downsample record's depth plane.
        r.lighting_source_no_bank_zero.value =
            [0x40_0000, 0x0001_8521, 0, 0, 0, 0, (1280 << 16) | 720, 5120];
        let [unit0, unit1] = lighting_requests(&inputs);
        // A group-twelve request naming the same plane natively, and one naming a colour image.
        let native = ProducerTextureRequest {
            unit: 12,
            source_address: 0x9000,
            descriptor: [0x40_0000, 0x0001_9021, 0, 0, 0, 0, (1280 << 16) | 720, 5120],
        };
        let alias_again = ProducerTextureRequest {
            unit: 13,
            source_address: unit0.source_address,
            descriptor: unit0.descriptor,
        };
        let plan =
            plan_producer_bindings(&inputs.renderer, &[unit0, unit1, native, alias_again]).unwrap();
        assert_eq!(plan.requests[0].1, BindingSource::Derived(0));
        assert_eq!(plan.requests[1].1, BindingSource::Guest);
        assert_eq!(
            plan.requests[2].1,
            BindingSource::Derived(1),
            "a different encoding is a different image"
        );
        assert_eq!(
            plan.requests[3].1,
            BindingSource::Derived(0),
            "same plane, encoding and size share one image"
        );
        assert_eq!(plan.derived.len(), 2);
        let float_depth = ProducerTextureRequest {
            descriptor: [0, 0x0001_9121, 0, 0, 0, 0, 0x0001_0001, 4],
            ..native
        };
        assert_eq!(
            plan_producer_bindings(&inputs.renderer, &[float_depth]),
            Err(SurfaceError::UnsupportedDepthSource)
        );
    }

    #[test]
    fn vector_8_depth_as_colour() {
        let mut d = colour_descriptor();
        d[1] = 0x0001_9021;
        assert_eq!(
            associate_colour(&d, &[(0x1000, colour_record())]),
            Err(SurfaceError::DepthAsColour)
        );
        let r = renderer_with(true, true, false);
        d[7] = 4 * 2560;
        assert!(matches!(
            classify_depth(&r, &d, 0x9999_0000),
            DepthClass::Depth(DepthSource {
                kind: DepthPlaneKind::NormalisedDepth,
                ..
            })
        ));
        d[1] = 0x0001_9121;
        assert_eq!(classify_depth(&r, &d, 0x9999_0000), DepthClass::Unsupported);
        d[1] = 0x0001_8521;
        assert_eq!(classify_depth(&r, &d, 0x9999_0000), DepthClass::NotDepth);
    }

    #[test]
    fn packed_alias_classification() {
        let mut r = renderer_with(true, true, false);
        let mut owner = record(1280, 720, 8, 2, 0);
        owner.words[16] = 0x40_0000;
        owner.words[17] = 5120;
        r.surface_table[table_position(1, 0)].value = owner;
        let d = [0x40_0000, 0x0001_8521, 0, 0, 0, 0, (1280 << 16) | 720, 5120];
        let DepthClass::Depth(source) = classify_depth(&r, &d, r.address + 0x3120) else {
            panic!("expected a packed alias")
        };
        assert_eq!(source.kind, DepthPlaneKind::PackedAlias);
        assert_eq!(
            source.owner,
            Some(r.surface_table[table_position(1, 0)].address)
        );
        // From +0x3140 the owner is the downsample record, which is not Z24S8 here.
        assert_eq!(
            classify_depth(&r, &d, r.address + 0x3140),
            DepthClass::Unsupported
        );
        let mut wrong_pitch = d;
        wrong_pitch[7] = 2560;
        assert_eq!(
            classify_depth(&r, &wrong_pitch, r.address + 0x3120),
            DepthClass::Unsupported
        );
    }

    fn inputs_for_requirements() -> ProducerInputs {
        let mut inputs = acquire(&p::image(), p::TOC).unwrap();
        for (position, entry) in inputs.renderer.surface_table.iter_mut().enumerate() {
            entry.value = record(1280, 720, 8, 2, 0);
            entry.value.words[8] = 0x10_0000 * (position as u32 + 1);
            entry.value.words[4] = 5120;
            entry.value.words[16] = 0x100_0000 + 0x10_0000 * position as u32;
            entry.value.words[17] = 5120;
        }
        inputs
    }

    fn setup(references: [u32; 3]) -> MainSetupRecord {
        MainSetupRecord {
            enabled_group_mask: 0,
            lists_enabled: true,
            depth_control: 0,
            initial: references,
            group_thirteen: 0,
            group_fifteen: [0, 0],
            auxiliary: 0,
            restore: 0,
        }
    }

    const INVOKE: Invocation = Invocation {
        initial_producer_enable: 1,
        target_index: 0,
    };

    #[test]
    fn required_surfaces_preconditions() {
        let inputs = inputs_for_requirements();
        assert_eq!(
            required_surfaces(&inputs, &setup([0, 0, 0]), INVOKE, 0, None),
            Err(SurfaceError::MainUnsupported)
        );
        let disabled = Invocation {
            initial_producer_enable: 0x100,
            target_index: 0,
        };
        assert_eq!(
            required_surfaces(&inputs, &setup([0, 1, 0]), disabled, 0, None),
            Err(SurfaceError::MainUnsupported)
        );
        assert_eq!(
            required_surfaces(&inputs, &setup([0, 1, 0]), INVOKE, 1 << 2, None),
            Err(SurfaceError::LightingUnit(2))
        );
    }

    #[test]
    fn required_surfaces_without_bank_zero() {
        let inputs = inputs_for_requirements();
        let set = required_surfaces(&inputs, &setup([0, 1, 0]), INVOKE, 0, None).unwrap();
        // Without bank zero both select(0, 1) and select(1, 1) resolve to role 2.
        let base = inputs.renderer.address + 0x2c10;
        assert_eq!(
            set.iter().map(|e| e.address - base).collect::<Vec<_>>(),
            vec![0xa8 * 2, 0xa8 * 2 + 0x54]
        );
        assert!(
            set.iter()
                .all(|e| e.attachment == e.record.target_uses() && e.prior == Uses::NONE)
        );
    }

    #[test]
    fn vector_9_prior_depth_input() {
        let mut inputs = inputs_for_requirements();
        let target = select(&inputs.renderer, 1, 1).unwrap();
        let depth_offset = target.record.depth_offset();
        inputs.renderer.lighting_source_no_bank_zero.value = [
            depth_offset,
            0x0001_9021,
            0,
            0,
            0,
            0,
            (1280 << 16) | 720,
            5120,
        ];
        let set = required_surfaces(&inputs, &setup([0, 1, 0]), INVOKE, 1, None).unwrap();
        let entry = set.iter().find(|e| e.address == target.address).unwrap();
        assert_eq!(entry.prior, Uses::DEPTH);
        inputs.renderer.lighting_source_no_bank_zero.value[0] = depth_offset + 0x1000;
        let set = required_surfaces(&inputs, &setup([0, 1, 0]), INVOKE, 1, None).unwrap();
        assert!(set.iter().all(|e| e.prior == Uses::NONE));
    }

    #[test]
    fn colour_lighting_source_and_retained_input() {
        let mut inputs = inputs_for_requirements();
        let target = select(&inputs.renderer, 1, 1).unwrap();
        let descriptor = [
            target.record.colour_offset(),
            0x0001_8521,
            0,
            0,
            0,
            0,
            (1280 << 16) | 720,
            5120,
        ];
        inputs.renderer.lighting_source_no_bank_zero.value = descriptor;
        let set = required_surfaces(&inputs, &setup([0, 1, 0]), INVOKE, 1, None).unwrap();
        let entry = set.iter().find(|e| e.address == target.address).unwrap();
        assert_eq!(entry.prior, Uses::COLOUR);
        // The retained lighting input moves select(1, 1) first.
        let set =
            required_surfaces(&inputs, &setup([0, 1, 0]), INVOKE, 0, Some(descriptor)).unwrap();
        assert_eq!(set[0].address, target.address);
        assert_eq!(set[0].prior, Uses::COLOUR);
    }

    #[test]
    fn vector_10_merge() {
        let r = colour_record();
        let mut set = RequiredSet::default();
        set.add(0x1000, r, Uses::NONE, Uses::COLOUR).unwrap();
        set.add(
            0x1000,
            r,
            Uses {
                colour: true,
                depth: true,
                stencil: false,
            },
            Uses::NONE,
        )
        .unwrap();
        assert_eq!(set.entries.len(), 1);
        let e = set.entries[0];
        assert_eq!(
            e.uses,
            Uses {
                colour: true,
                depth: true,
                stencil: false
            }
        );
        assert_eq!(e.prior, Uses::COLOUR);
        assert_eq!(
            e.attachment,
            Uses {
                colour: true,
                depth: true,
                stencil: false
            }
        );
        let mut other = r;
        other.words[20] ^= 1;
        assert_eq!(
            set.add(0x1000, other, Uses::COLOUR, Uses::NONE),
            Err(SurfaceError::ConflictingRecord { address: 0x1000 })
        );
    }

    /// Property: for exactly representable inputs the mapped viewport covers the target exactly,
    /// up to the jitter offset.
    #[test]
    fn property_mapped_size_equals_physical_size() {
        let mut rng = crate::testutil::Rng::new(3);
        for _ in 0..2_000 {
            let aa = [0, 3, 4, 5][rng.below(4) as usize];
            let s = surface(record(1 + rng.below(4096), 1 + rng.below(4096), 8, 2, aa));
            let jitter = [
                rng.below(64) as f32 / 128.0 - 0.25,
                rng.below(64) as f32 / 128.0 - 0.25,
            ];
            let v = source_viewport(&s, &constants(jitter)).unwrap();
            let physical = s.record.physical_size().unwrap();
            let Ok(n) = surface_target_viewport(&s, &v, physical) else {
                continue;
            };
            assert_eq!(n.width, physical.0 as f32);
            assert_eq!(n.height, physical.1 as f32);
            assert_eq!(n.scissor, [0, 0, physical.0, physical.1]);
        }
    }
}
