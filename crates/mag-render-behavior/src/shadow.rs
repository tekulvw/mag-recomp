//! The Shadow pass (`shadow-pass.md`), the parts the specification settles.
//!
//! The specification is partial. This module implements:
//!
//! - the two cascade runs, their cohort camera slots and semaphore offsets, and caster selection
//!   by the run's enabled-group mask;
//! - the cascade views: plane corners, axial centres, the bounding-sphere fit and the refresh
//!   rule;
//! - the routing of a caster onto Main's geometry preparation;
//! - the pass state the run writes and the native pass state (depth-only, less-or-equal with
//!   writes, polygon offset 2.0/60.0, back-face culling with counter-clockwise front faces), the
//!   vertex arrays and the per-draw constant rows;
//! - the shared 2048×1024 linear surface, cascade 0's region, viewport and scissor, cascade 1's
//!   inferred half behind [`LayoutAssumptions`], and the once-per-frame clear;
//! - the unit-1 sampling record checks;
//! - [`describe_cascade_pass`] and [`describe_frame`]: a pure description of each native
//!   depth-only cascade pass (region, viewport, state, clear and caster list) that the frontend
//!   can turn into a plan pass;
//! - the native-replacement contract: the obligations, the ring-generation release rule, the
//!   side-slot continuation, the writer publication and the completion ordering, behind a
//!   decision that keeps the original job.
//!
//! Every question the specification still lists as open stays an explicit [`ShadowQuestion`]
//! refusal: the light view, the confirmation of cascade 1's half, row 42, the per-item child work,
//! groups outside the mask, group-7 skinning and the shadow-resolve draws. The exact command words
//! of a segment belong to the shared native ring writer and are not re-encoded here. Nothing here
//! is wired into the frontend; [`ShadowSwitch`] is off by default and, while any blocking question
//! is open, [`ownership`] always keeps the original job.

use crate::material_sources::GeometrySource;
use crate::setup::{Comparison, CullFace, FrontFace, PipelineState};
use crate::texture::{DecodedUnit, Filter, TextureUnitRecord, decode};
use crate::vertex_inputs::VertexBinding;

/// The questions `shadow-pass.md` still lists as open, by number. Former questions 1, 4, 6, 8 and
/// 11 are settled, and question 3 is settled apart from cascade 1's half.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub enum ShadowQuestion {
    /// 2: how a cascade sphere becomes the light view that rows 3–6 carry.
    LightView = 2,
    /// 3: that cascade 1 uses x 1024–2047 with a viewport like cascade 0's (inferred, not
    /// confirmed).
    CascadeOneHalf = 3,
    /// 5: what vertex constant row 42 holds.
    RowFortyTwo = 5,
    /// 7: the per-item child work: technique selection, programs and per-material state,
    /// including which materials disable culling.
    ChildWork = 7,
    /// 9: whether any setting enables groups outside `0xFE`.
    GroupsOutsideMask = 9,
    /// 10: the group-7 skinning admission conditions.
    GroupSevenSkinning = 10,
    /// 12: how the shadow-resolve draws split by cascade.
    ShadowResolveDraws = 12,
}

/// The questions that block native ownership of a run's output.
pub const OWNERSHIP_BLOCKERS: [ShadowQuestion; 4] = [
    ShadowQuestion::LightView,
    ShadowQuestion::CascadeOneHalf,
    ShadowQuestion::RowFortyTwo,
    ShadowQuestion::ChildWork,
];

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum ShadowError {
    /// The specification leaves this open; an implementation must not guess.
    Unresolved(ShadowQuestion),
    /// A cascade whose distances are not positive and strictly increasing, or whose inputs or fit
    /// are not finite.
    CascadeDeclined(&'static str),
    /// An item a shared specification refuses.
    ItemRefused(&'static str),
    /// Encoded by the shared native ring writer, not by this module.
    RingWriter,
    /// The unit-1 sampling record does not describe the native shadow surface.
    SamplingRecord(&'static str),
}

// --- Runs and caster selection -----------------------------------------------------------------

/// One of the two Shadow job runs per update.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct CascadeRun {
    pub cascade: u8,
    pub family: u32,
    pub bucket: u32,
}

/// Cascade 0 (family 1, bucket 1) and cascade 1 (family 2, bucket 2).
pub const RUNS: [CascadeRun; 2] = [
    CascadeRun {
        cascade: 0,
        family: 1,
        bucket: 1,
    },
    CascadeRun {
        cascade: 1,
        family: 2,
        bucket: 2,
    },
];

/// The enabled-group mask the game passes for both runs. A replacement reads the mask from the
/// run's input; this value is only what the specification observed.
pub const OBSERVED_GROUP_MASK: u16 = 0xfe;

/// The groups a run may draw under the established rules: 1–7.
pub const SUPPORTED_GROUPS: u16 = 0xfe;

/// The selected casters of one run.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct CasterSelection {
    /// The enabled groups, ascending.
    pub groups: Vec<u32>,
    /// The item budget: the sum of the cohort's group counts over the enabled groups.
    pub budget: u64,
}

impl CasterSelection {
    /// An empty run (mask 0): no items, but its terminal obligations still apply.
    pub fn is_empty(&self) -> bool {
        self.budget == 0
    }
}

/// Selects the enabled groups of a run from its mask and the cohort's group counts.
///
/// The mask is 16 bits, bit `g` for mesh group `g`, the width of the setup record's enabled-group
/// mask (the low half of its group-enable word, `material-setup.md`); a caller never passes the
/// whole 32-bit word. A mask bit outside groups 1–7 is open question 9 and is refused, never
/// drawn or dropped.
/// Which camera's counts the caller passes is open question 8; see [`run_camera`].
pub fn select_casters(mask: u16, group_counts: &[u32; 16]) -> Result<CasterSelection, ShadowError> {
    if mask & !SUPPORTED_GROUPS != 0 {
        return Err(ShadowError::Unresolved(ShadowQuestion::GroupsOutsideMask));
    }
    let groups: Vec<u32> = (0..16).filter(|g| mask & (1 << g) != 0).collect();
    let budget = groups
        .iter()
        .map(|&g| group_counts[g as usize] as u64)
        .sum();
    Ok(CasterSelection { groups, budget })
}

/// The indices of the enabled-group items of a cohort, in cohort order.
///
/// `groups` are the items' census groups, in cohort order (ascending group, then sorted order).
/// Items out of ascending group order are refused, because the run draws in cohort order.
pub fn caster_items(mask: u16, groups: &[u32]) -> Result<Vec<usize>, ShadowError> {
    if mask & !SUPPORTED_GROUPS != 0 {
        return Err(ShadowError::Unresolved(ShadowQuestion::GroupsOutsideMask));
    }
    if groups.windows(2).any(|w| w[0] > w[1]) {
        return Err(ShadowError::ItemRefused("cohort out of group order"));
    }
    Ok(groups
        .iter()
        .enumerate()
        .filter(|(_, g)| **g < 16 && mask & (1 << **g) != 0)
        .map(|(i, _)| i)
        .collect())
}

/// The cohort camera slot of a run's caster list: slot 1 for cascade 0 and slot 2 for cascade 1
/// (Main uses slot 3). The two lists are not disjoint: an item may be drawn by both runs.
pub fn run_camera(run: &CascadeRun) -> usize {
    run.cascade as usize + 1
}

/// The progress semaphore offsets that identify the runs: `0x490` for cascade 0 and `0x4A0` for
/// cascade 1.
pub const SEMAPHORE_OFFSETS: [u32; 2] = [0x490, 0x4a0];
/// The Depth job's progress semaphore offset, for comparison.
pub const DEPTH_SEMAPHORE_OFFSET: u32 = 0x4b0;

/// The run a progress semaphore offset identifies.
pub fn run_for_semaphore(offset: u32) -> Option<CascadeRun> {
    SEMAPHORE_OFFSETS
        .iter()
        .position(|o| *o == offset)
        .map(|cascade| RUNS[cascade])
}

// --- Cascade views -----------------------------------------------------------------------------

/// The view-preparation inputs.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct ViewInputs {
    /// Plane distances `d0 < d1 < d2`, all positive.
    pub distances: [f32; 3],
    /// Camera basis `X`, `Y`, `Z`.
    pub basis: [[f32; 3]; 3],
    /// Camera position `P`.
    pub position: [f32; 3],
    /// Vertical half-extent scale `v`.
    pub vertical: f32,
    /// Aspect scale `r`.
    pub aspect: f32,
    /// Directional-light vector `L`.
    pub light: [f32; 3],
}

fn finite(values: &[f32]) -> Result<(), ShadowError> {
    if values.iter().all(|v| v.is_finite()) {
        Ok(())
    } else {
        Err(ShadowError::CascadeDeclined("non-finite input"))
    }
}

fn narrow(value: f64) -> Result<f32, ShadowError> {
    let narrowed = value as f32;
    if value.is_finite() && narrowed.is_finite() {
        Ok(narrowed)
    } else {
        Err(ShadowError::CascadeDeclined("non-finite result"))
    }
}

fn point(base: [f32; 3], terms: &[([f32; 3], f64)]) -> Result<[f32; 3], ShadowError> {
    let mut out = [0f32; 3];
    for lane in 0..3 {
        let mut value = base[lane] as f64;
        for (axis, scale) in terms {
            value += axis[lane] as f64 * scale;
        }
        out[lane] = narrow(value)?;
    }
    Ok(out)
}

impl ViewInputs {
    fn check(&self) -> Result<(), ShadowError> {
        finite(&self.distances)?;
        for axis in &self.basis {
            finite(axis)?;
        }
        finite(&self.position)?;
        finite(&[self.vertical, self.aspect])?;
        finite(&self.light)?;
        let [d0, d1, d2] = self.distances;
        if !(d0 > 0.0 && d0 < d1 && d1 < d2) {
            return Err(ShadowError::CascadeDeclined(
                "distances not positive and increasing",
            ));
        }
        Ok(())
    }

    /// The four corners of the plane at distance `d`: (+X,+Y), (−X,+Y), (−X,−Y), (+X,−Y).
    pub fn corners(&self, d: f32) -> Result<[[f32; 3]; 4], ShadowError> {
        finite(&[d, self.vertical, self.aspect])?;
        let [x, y, z] = self.basis;
        let h = d as f64 * self.vertical as f64;
        let w = h * self.aspect as f64;
        let mut out = [[0f32; 3]; 4];
        for (corner, (sx, sy)) in
            out.iter_mut()
                .zip([(1.0, 1.0), (-1.0, 1.0), (-1.0, -1.0), (1.0, -1.0)])
        {
            *corner = point(self.position, &[(z, -(d as f64)), (x, sx * w), (y, sy * h)])?;
        }
        Ok(out)
    }

    /// The point on the backward axis at distance `d`: `P − Z·d`.
    pub fn axial(&self, d: f64) -> Result<[f32; 3], ShadowError> {
        point(self.position, &[(self.basis[2], -d)])
    }

    /// The two axial centres, at `(d0+d1)/2` and `(d1+d2)/2`.
    pub fn axial_centres(&self) -> Result<[[f32; 3]; 2], ShadowError> {
        self.check()?;
        let [d0, d1, d2] = self.distances.map(f64::from);
        Ok([self.axial((d0 + d1) / 2.0)?, self.axial((d1 + d2) / 2.0)?])
    }

    /// Fits cascade `k`'s bounding sphere (distances `d_k` to `d_(k+1)`).
    pub fn fit(&self, cascade: usize) -> Result<SphereFit, ShadowError> {
        self.check()?;
        let (n, f) = (
            self.distances[cascade] as f64,
            self.distances[cascade + 1] as f64,
        );
        let (v, r) = (self.vertical as f64, self.aspect as f64);
        let extent = |d: f64| (d * v * r).powi(2) + (d * v).powi(2);
        let span = f - n;
        let c = f.min(n + (span * span + extent(f) - extent(n)) / (2.0 * span));
        // The radius is the distance from the centre to the far (+X,+Y) corner, for any basis.
        let [x, y, z] = self.basis;
        let (h, w) = (f * v, f * v * r);
        let mut squared = 0.0;
        for lane in 0..3 {
            let offset = -(z[lane] as f64) * (f - c) + x[lane] as f64 * w + y[lane] as f64 * h;
            squared += offset * offset;
        }
        Ok(SphereFit {
            centre_distance: narrow(c)?,
            radius: narrow(squared.sqrt())?,
        })
    }
}

/// One cascade's bounding sphere: its centre lies on the backward axis at `centre_distance`.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct SphereFit {
    pub centre_distance: f32,
    pub radius: f32,
}

/// The two persistent fits and the refresh rule.
///
/// A fit is the pair (centre distance, radius). Cascade 0 is refitted when `d0` or `d1` changes,
/// cascade 1 only when `d2` changes. Camera pose, `v` and `r` changes alone do not refit. Which
/// pose a persisted fit is placed with belongs to the light view, open question 2.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct CascadeFits {
    fits: [Option<SphereFit>; 2],
    keys: [Option<[u32; 2]>; 2],
}

impl CascadeFits {
    pub fn new() -> Self {
        Self::default()
    }

    /// A real (re)initialisation of the shadow configuration: both fits are invalid.
    pub fn invalidate(&mut self) {
        *self = Self::default();
    }

    pub fn fit(&self, cascade: usize) -> Option<SphereFit> {
        self.fits[cascade]
    }

    /// Applies one update's inputs. Returns which cascades were refitted. A declined update
    /// changes nothing.
    pub fn update(&mut self, inputs: &ViewInputs) -> Result<[bool; 2], ShadowError> {
        inputs.check()?;
        let [d0, d1, d2] = inputs.distances.map(f32::to_bits);
        let keys = [[d0, d1], [d2, 0]];
        let stale: [bool; 2] = std::array::from_fn(|k| self.keys[k] != Some(keys[k]));
        let mut fits = self.fits;
        for k in 0..2 {
            if stale[k] {
                fits[k] = Some(inputs.fit(k)?);
            }
        }
        self.fits = fits;
        for k in 0..2 {
            if stale[k] {
                self.keys[k] = Some(keys[k]);
            }
        }
        Ok(stale)
    }
}

/// A cascade's orthographic light view: open question 2.
pub fn light_view(_fit: &SphereFit, _inputs: &ViewInputs) -> Result<[[f32; 4]; 4], ShadowError> {
    Err(ShadowError::Unresolved(ShadowQuestion::LightView))
}

// --- Shadow map --------------------------------------------------------------------------------

/// The full texture descriptor of the shadow map, at `PARAMS + 0x1520`.
pub const MAP_DESCRIPTOR: u32 = 0x1520;
/// The shadow texture transforms of cascades 0 and 1, at `PARAMS + 0xD0` and `PARAMS + 0xB50`.
pub const CASCADE_TRANSFORMS: [u32; 2] = [0xd0, 0xb50];

/// The shared depth surface both cascade runs render into.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ShadowSurface {
    pub width: u32,
    pub height: u32,
    /// Z24S8 depth.
    pub depth_stencil: bool,
    /// Linear layout, `pitch` bytes per row.
    pub pitch: u32,
}

pub const SHADOW_SURFACE: ShadowSurface = ShadowSurface {
    width: 2048,
    height: 1024,
    depth_stencil: true,
    pitch: 8192,
};

/// The two 1024×1024 halves `(x, y, width, height)`: x 0–1023 and x 1024–2047.
pub const SURFACE_HALVES: [[u32; 4]; 2] = [[0, 0, 1024, 1024], [1024, 0, 1024, 1024]];

/// One cascade's viewport, scissor and depth transform.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct CascadeViewport {
    /// `(x, y, width, height)`; the scissor is the same rectangle.
    pub rectangle: [u32; 4],
    pub scissor: [u32; 4],
    /// `(x, y, z)` scale and offset of the viewport transform.
    pub scale: [f32; 3],
    pub offset: [f32; 3],
    pub depth_range: (f32, f32),
}

/// Cascade 0: x 0–1023, y 0–1023, viewport and scissor 1024×1024 at the origin, scale
/// `(512, −512, 0.5)`, offset `(512, 512, 0.5)`, depth range 0–1.
pub const CASCADE_ZERO_VIEWPORT: CascadeViewport = CascadeViewport {
    rectangle: SURFACE_HALVES[0],
    scissor: SURFACE_HALVES[0],
    scale: [512.0, -512.0, 0.5],
    offset: [512.0, 512.0, 0.5],
    depth_range: (0.0, 1.0),
};

/// Assumptions the specification states as inferred, not confirmed. Each is off by default, so an
/// unconfirmed fact is refused unless a caller opts in.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct LayoutAssumptions {
    /// Cascade 1 uses x 1024–2047 with cascade 0's viewport moved to an x origin of 1024 (open
    /// question 3: both halves are written and the first run uses the left half).
    pub cascade_one_right_half: bool,
}

/// The viewport, scissor and region of cascade `k`.
pub fn cascade_viewport(
    cascade: usize,
    assumptions: &LayoutAssumptions,
) -> Result<CascadeViewport, ShadowError> {
    match cascade {
        0 => Ok(CASCADE_ZERO_VIEWPORT),
        1 if assumptions.cascade_one_right_half => {
            let x = SURFACE_HALVES[1][0] as f32;
            Ok(CascadeViewport {
                rectangle: SURFACE_HALVES[1],
                scissor: SURFACE_HALVES[1],
                offset: [x + CASCADE_ZERO_VIEWPORT.offset[0], 512.0, 0.5],
                ..CASCADE_ZERO_VIEWPORT
            })
        }
        1 => Err(ShadowError::Unresolved(ShadowQuestion::CascadeOneHalf)),
        _ => Err(ShadowError::CascadeDeclined("no such cascade")),
    }
}

/// The surface's clear: once per frame, before the first cascade.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct FrameClear {
    pub depth: f32,
    pub stencil: u32,
}

pub const FRAME_CLEAR: FrameClear = FrameClear {
    depth: 1.0,
    stencil: 0,
};

/// The colour value the frame's clear group also uses. The native pass has no colour output and
/// clears depth and stencil only.
pub const CAPTURED_COLOUR_CLEAR: u32 = 0xff00_0000;

/// The colour target bound during the pass aliases the depth surface's memory and is A8R8G8B8,
/// but colour mask 0 means it is never written.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ColourTarget {
    pub aliases_depth: bool,
    pub written: bool,
}

pub const COLOUR_TARGET: ColourTarget = ColourTarget {
    aliases_depth: true,
    written: false,
};

/// Checks that the unit-1 sampling record (the twelve words at `PARAMS+0x1520`, read at run time)
/// describes the native shadow surface: masked format `0x90` with the linear-layout bit,
/// 2048×1024, one mip, s and t clamp-to-border (`4`), r clamp-to-edge (`3`), depth comparison
/// greater-or-equal (`6`), linear minification and magnification and remap `0xAAE4`. A match means
/// the native pass writes the image unit 1 imports.
pub fn check_sampling_record(full: &[u32; 12]) -> Result<(), ShadowError> {
    let mut record = TextureUnitRecord::default();
    record.apply_full(full);
    let controls = match decode(&record) {
        Ok(DecodedUnit::Enabled(controls)) => controls,
        Ok(DecodedUnit::Disabled(_)) => return Err(ShadowError::SamplingRecord("unit disabled")),
        Err(_) => return Err(ShadowError::SamplingRecord("undecodable")),
    };
    let fail = |what| Err(ShadowError::SamplingRecord(what));
    if controls.base != 0x90 || controls.format & 0x20 == 0 {
        return fail("format");
    }
    let size = (controls.words[6] >> 16, controls.words[6] & 0xffff);
    if size != (SHADOW_SURFACE.width, SHADOW_SURFACE.height) {
        return fail("size");
    }
    if controls.mip_count != 1 {
        return fail("mip count");
    }
    if controls.wrap.map(|axis| axis.code) != [4, 4, 3] {
        return fail("wrap");
    }
    if controls.compare_function != 6 {
        return fail("comparison");
    }
    if (controls.min, controls.mag) != (Filter::Linear, Filter::Linear) {
        return fail("filters");
    }
    if controls.words[4] != 0xaae4 {
        return fail("remap");
    }
    Ok(())
}

/// Which cascade each shadow-resolve draw resolves: open question 12.
pub fn resolve_draw_cascades() -> Result<Vec<usize>, ShadowError> {
    Err(ShadowError::Unresolved(ShadowQuestion::ShadowResolveDraws))
}

// --- Fixed-function state ----------------------------------------------------------------------

/// NV4097 methods whose values the run builds itself, in addition to its copied setup banks.
pub mod methods {
    pub const ALPHA_TEST: u32 = 0x0304;
    pub const BLEND_ENABLE: u32 = 0x0310;
    pub const COLOUR_MASK: u32 = 0x0324;
    pub const STENCIL_ENABLE: u32 = 0x0328;
    /// Front write mask, function, reference, read mask and operations.
    pub const FRONT_STENCIL: [u32; 7] = [0x032c, 0x0330, 0x0334, 0x0338, 0x033c, 0x0340, 0x0344];
    /// Back write mask, function, reference, read mask and operations.
    pub const BACK_STENCIL: [u32; 7] = [0x034c, 0x0350, 0x0354, 0x0358, 0x035c, 0x0360, 0x0364];
    pub const MRT_COLOUR_MASK: u32 = 0x0370;
    pub const DEPTH_FUNCTION: u32 = 0x0a6c;
    pub const DEPTH_WRITE: u32 = 0x0a70;
    pub const DEPTH_TEST: u32 = 0x0a74;
    pub const OFFSET_FACTOR: u32 = 0x0a78;
    pub const OFFSET_BIAS: u32 = 0x0a7c;
    pub const SHADER_PACKER: u32 = 0x1fec;

    /// Per-item geometry: vertex array offsets and formats of slots 6 and 12, the current value
    /// of attribute 6, vertex constant uploads and indexed draws.
    pub const ARRAY_OFFSET_6: u32 = 0x1698;
    pub const ARRAY_OFFSET_12: u32 = 0x16b0;
    pub const ARRAY_FORMAT_6: u32 = 0x1758;
    pub const ARRAY_FORMAT_12: u32 = 0x1770;
    pub const ATTRIBUTE_6: u32 = 0x1c60;
    pub const CONSTANT_UPLOAD: u32 = 0x1efc;
    /// Data words of the one-, two- and eight-row uploads.
    pub const UPLOAD_WORDS: [u32; 3] = [4, 8, 32];
    pub const BEGIN_END: u32 = 0x1808;
    pub const INDEX_ARRAY: u32 = 0x181c;

    /// Completion: the texture-read semaphore offset, then its release.
    pub const SEMAPHORE_OFFSET: u32 = 0x1d6c;
    pub const TEXTURE_READ_RELEASE: u32 = 0x1d74;
    /// The report methods the Depth job writes and the Shadow job does not.
    pub const DEPTH_ONLY_REPORTS: [u32; 2] = [0x17c8, 0x1800];
}

/// The pass state the run writes once, in the order the specification lists it. The colour and
/// MRT masks are written all-on and then immediately off; the net effect is depth-only rendering.
pub fn run_state_values() -> Vec<(u32, u32)> {
    use methods::*;
    vec![
        (SHADER_PACKER, 0),
        (DEPTH_TEST, 1),
        (DEPTH_WRITE, 1),
        (DEPTH_FUNCTION, 0x0203),
        (BLEND_ENABLE, 0),
        (ALPHA_TEST, 0),
        (COLOUR_MASK, 0x0101_0101),
        (COLOUR_MASK, 0),
        (MRT_COLOUR_MASK, 0xfff0),
        (MRT_COLOUR_MASK, 0),
    ]
}

/// Every method the run writes for its pass state.
pub fn run_state_methods() -> Vec<u32> {
    let mut all: Vec<u32> = run_state_values().iter().map(|(m, _)| *m).collect();
    all.dedup();
    all
}

/// Methods the job can emit but no recorded run used: stencil and polygon offset. A native pass
/// does not write them.
pub fn static_only_methods() -> Vec<u32> {
    use methods::*;
    let mut all = vec![STENCIL_ENABLE];
    all.extend(FRONT_STENCIL);
    all.extend(BACK_STENCIL);
    all.extend([OFFSET_FACTOR, OFFSET_BIAS]);
    all
}

/// The state after the run's own writes. Fields the run does not write (stencil, polygon offset,
/// cull, the surface) keep their inherited knowledge.
pub fn after_run_state(state: &PipelineState) -> PipelineState {
    PipelineState {
        srgb_output: Some(false),
        depth_test: Some(true),
        depth_write: Some(true),
        depth_comparison: Comparison::from_code(0x0203),
        blend_enable: Some(false),
        alpha_test: Some(false),
        colour_masks: Some([0; 4]),
        ..*state
    }
}

/// The explicit polygon offset of the pass: factor 2.0, units 60.0, applied by the native pass
/// rather than relied on from whatever state is current.
pub const DEPTH_BIAS: (f32, f32) = (2.0, 60.0);

/// The state a native pass draws a caster with: the run's writes, plus the raster state the
/// captures give for both cascades. Polygon offset fill is on with [`DEPTH_BIAS`] (point and line
/// offset off), stencil and two-sided stencil off, depth bounds off, and back faces culled with
/// counter-clockwise front faces unless the caster's material disables culling. Which materials
/// do that is per-item child work (open question 7), so the caller supplies it.
pub fn native_pass_state(inherited: &PipelineState, culling_disabled: bool) -> PipelineState {
    PipelineState {
        polygon_offset_fill: Some(true),
        offset_factor: Some(DEPTH_BIAS.0.to_bits()),
        offset_units: Some(DEPTH_BIAS.1.to_bits()),
        stencil_enable: Some(false),
        two_sided_stencil: Some(false),
        depth_bounds_enable: Some(false),
        cull_enable: Some(!culling_disabled),
        cull_face: Some(CullFace::Back),
        front_face: Some(FrontFace::CounterClockwise),
        ..after_run_state(inherited)
    }
}

/// Which casters' materials disable culling: open question 7.
pub fn caster_culling_disabled(_material_flags: u32) -> Result<bool, ShadowError> {
    Err(ShadowError::Unresolved(ShadowQuestion::ChildWork))
}

/// Vertex arrays 1–15 are disabled once per run with format `0x2` (float, size 0), and the
/// frequency divider is cleared.
pub const DISABLED_ARRAY_FORMAT: u32 = 0x2;

/// The disabled arrays `(slot, format)`.
pub fn disabled_arrays() -> Vec<(u32, u32)> {
    (1..16).map(|slot| (slot, DISABLED_ARRAY_FORMAT)).collect()
}

/// Only vertex array 0, the positions, is enabled per draw.
pub const ENABLED_ARRAYS: u16 = 0x0001;

/// The position-only output binding of a caster: the first semantic-1 binding to slot 0. The
/// partition's stream dictates its format and stride.
pub fn position_binding(outputs: &[VertexBinding]) -> Result<VertexBinding, ShadowError> {
    outputs
        .iter()
        .find(|b| b.semantic == 1 && b.slot == 0)
        .copied()
        .ok_or(ShadowError::ItemRefused("no position binding in slot 0"))
}

/// The vertex constant rows each draw writes.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct DrawConstantRows {
    /// Rows 3–6: the draw's four transform rows. Their construction is open question 2.
    pub transform: [u32; 4],
    /// Row 24, when it changes: the binding row of `material-values.md`.
    pub binding: u32,
    /// Row 42, when it changes: opaque, open question 5.
    pub opaque: u32,
}

pub const DRAW_CONSTANT_ROWS: DrawConstantRows = DrawConstantRows {
    transform: [3, 4, 5, 6],
    binding: 24,
    opaque: 42,
};

/// The values of the transform rows 3–6: open question 2.
pub fn transform_rows(_fit: &SphereFit) -> Result<[[u32; 4]; 4], ShadowError> {
    Err(ShadowError::Unresolved(ShadowQuestion::LightView))
}

/// The value of row 42: open question 5.
pub fn row_forty_two() -> Result<[u32; 4], ShadowError> {
    Err(ShadowError::Unresolved(ShadowQuestion::RowFortyTwo))
}

// --- Caster preparation ------------------------------------------------------------------------

/// How a caster's geometry is prepared: Main's preparation for its geometry source.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct CasterPreparation {
    /// The geometry source, as `material_sources` chose it with the run's own pass object.
    pub source: GeometrySource,
    /// Groups 3 and 4 run the non-instanced wind or ripple deformation on that source.
    pub deform: bool,
}

/// Routes an enabled-group caster onto Main's preparation.
///
/// Palette-skinned partitions are refused: group 7 is open question 10, and the published
/// skinning admission covers group 11 only.
pub fn caster_preparation(
    mesh_group: u16,
    source: GeometrySource,
) -> Result<CasterPreparation, ShadowError> {
    if !(1..=7).contains(&mesh_group) {
        return Err(ShadowError::ItemRefused("mesh group outside 1–7"));
    }
    match source {
        GeometrySource::GroupZero { .. } => Err(ShadowError::ItemRefused("group-zero source")),
        GeometrySource::Skinned { .. } if mesh_group == 7 => {
            Err(ShadowError::Unresolved(ShadowQuestion::GroupSevenSkinning))
        }
        GeometrySource::Skinned { .. } => Err(ShadowError::ItemRefused(
            "skinning admission covers group 11 only",
        )),
        source => Ok(CasterPreparation {
            source,
            deform: matches!(mesh_group, 3 | 4),
        }),
    }
}

/// The technique, programs and per-material state of a caster, which the per-item child jobs
/// supply: open question 7. Selection inputs follow `material_sources::item_sources` with the
/// run's own pass object.
pub fn technique_outcome(_mesh_group: u16) -> Result<bool, ShadowError> {
    Err(ShadowError::Unresolved(ShadowQuestion::ChildWork))
}

// --- Pass description --------------------------------------------------------------------------

/// One native depth-only cascade pass, as a plan pass: where it renders, with what state, and
/// which casters.
#[derive(Clone, Debug, PartialEq)]
pub struct CascadePass {
    pub run: CascadeRun,
    /// The cohort camera slot whose caster list the pass draws.
    pub camera_slot: usize,
    pub semaphore_offset: u32,
    pub viewport: CascadeViewport,
    /// The surface clear, present for the first cascade of the frame only.
    pub clear: Option<FrameClear>,
    /// The state of a caster whose material keeps culling; [`CascadePass::caster_state`] gives
    /// the state of one whose material disables it.
    pub state: PipelineState,
    /// Cohort indices of the casters, in cohort order.
    pub casters: Vec<usize>,
    pub enabled_arrays: u16,
}

impl CascadePass {
    /// A caster's state, given whether its material disables culling.
    pub fn caster_state(&self, culling_disabled: bool) -> PipelineState {
        PipelineState {
            cull_enable: Some(!culling_disabled),
            ..self.state
        }
    }
}

/// Describes cascade `k`'s pass from its run's mask and its cohort's census groups (in cohort
/// order), with `inherited` the state at run start.
pub fn describe_cascade_pass(
    cascade: usize,
    mask: u16,
    groups: &[u32],
    inherited: &PipelineState,
    assumptions: &LayoutAssumptions,
) -> Result<CascadePass, ShadowError> {
    let run = *RUNS
        .get(cascade)
        .ok_or(ShadowError::CascadeDeclined("no such cascade"))?;
    Ok(CascadePass {
        run,
        camera_slot: run_camera(&run),
        semaphore_offset: SEMAPHORE_OFFSETS[cascade],
        viewport: cascade_viewport(cascade, assumptions)?,
        clear: (cascade == 0).then_some(FRAME_CLEAR),
        state: native_pass_state(inherited, false),
        casters: caster_items(mask, groups)?,
        enabled_arrays: ENABLED_ARRAYS,
    })
}

/// Describes both cascade passes of a frame, cascade 0 first. Either refusal refuses the frame.
pub fn describe_frame(
    masks: [u16; 2],
    groups: [&[u32]; 2],
    inherited: &PipelineState,
    assumptions: &LayoutAssumptions,
) -> Result<[CascadePass; 2], ShadowError> {
    Ok([
        describe_cascade_pass(0, masks[0], groups[0], inherited, assumptions)?,
        describe_cascade_pass(1, masks[1], groups[1], inherited, assumptions)?,
    ])
}

// --- Native-replacement contract ---------------------------------------------------------------

/// The guest-visible obligations a native replacement meets for each run, as for the native
/// Depth cutover, plus the Shadow run's texture-read release.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Obligation {
    SegmentProgress,
    SegmentContinuation,
    FinalTransitionState,
    FenceAndPut,
    WriterState,
    ReadyWorkNotification,
    /// The PPU's wait on the run returns only after the native result is published.
    ReturnAfterPublish,
    /// The texture-read semaphore after the last draw; there is no report write.
    TextureReadRelease,
}

/// Every obligation of one run, in the order they are met.
pub const OBLIGATIONS: [Obligation; 8] = [
    Obligation::SegmentProgress,
    Obligation::SegmentContinuation,
    Obligation::FinalTransitionState,
    Obligation::FenceAndPut,
    Obligation::WriterState,
    Obligation::ReadyWorkNotification,
    Obligation::ReturnAfterPublish,
    Obligation::TextureReadRelease,
];

/// The exact command words of a segment. They are the shared native ring writer's; this module
/// states the rules they follow ([`release_value`], [`Ring`], [`continuation_target`],
/// [`writer_publication`]) but does not re-encode the words.
pub fn obligation_words(
    _obligation: Obligation,
    _empty_run: bool,
) -> Result<Vec<u32>, ShadowError> {
    Err(ShadowError::RingWriter)
}

/// The largest command segment; each ends in a jump to the next.
pub const SEGMENT_BYTES: u32 = 8 << 10;
/// One segment descriptor per segment in the writer's descriptor table.
pub const SEGMENT_DESCRIPTOR_BYTES: u32 = 32;
/// The closing segment.
pub const TERMINAL_SEGMENT_BYTES: u32 = 128;

/// The elements of the closing segment, in order.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum TerminalElement {
    NoOperation,
    SemaphoreOffset,
    TextureReadRelease,
    JumpToContinuation,
}

pub const TERMINAL_SEGMENT: [TerminalElement; 4] = [
    TerminalElement::NoOperation,
    TerminalElement::SemaphoreOffset,
    TerminalElement::TextureReadRelease,
    TerminalElement::JumpToContinuation,
];

/// A texture-read semaphore release value: the ring offset of the word after the release in the
/// low 30 bits, and the ring generation in force when it is written in bits 30–31.
pub fn release_value(next_word_offset: u32, generation: u8) -> u32 {
    (next_word_offset & 0x3fff_ffff) | (((generation & 3) as u32) << 30)
}

/// The ring generation: a 2-bit counter kept in the writer's epoch word, advanced modulo 4 each
/// time the ring wraps back to its start.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Ring {
    pub generation: u8,
}

impl Ring {
    /// The generation read from the epoch word.
    pub fn from_epoch(epoch: u32) -> Self {
        Self {
            generation: (epoch & 3) as u8,
        }
    }

    /// The release value for a word at `next_word_offset` under the current generation.
    pub fn release(&self, next_word_offset: u32) -> u32 {
        release_value(next_word_offset, self.generation)
    }

    /// The ring wrapped back to its start.
    pub fn wrap(&mut self) {
        self.generation = (self.generation + 1) & 3;
    }

    /// The epoch word after the run. A run that did not wrap leaves it unchanged.
    pub fn epoch(&self) -> u32 {
        self.generation as u32
    }
}

/// The closing jump of a Shadow run (side slots 1 and 2): the writer's address base plus the
/// run's continuation offset, whether or not the run wrapped the ring. (Main's slot 0 jumps to
/// the ring start only on a wrap.)
pub fn continuation_target(address_base: u32, continuation_offset: u32) -> u32 {
    address_base.wrapping_add(continuation_offset)
}

/// What the run publishes after each batch of segments.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct WriterPublication {
    /// The new ring write offset, to the put word.
    pub put: u32,
    /// The writer slot's word: `offset | 2`.
    pub slot_word: u32,
    /// The epoch word: the current ring generation.
    pub epoch: u32,
    /// The next-writer-state record: begin, end, current, `0xFFFFFFFF`.
    pub next_state: [u32; 4],
}

pub fn writer_publication(offset: u32, ring: &Ring, begin: u32, end: u32) -> WriterPublication {
    WriterPublication {
        put: offset,
        slot_word: offset | 2,
        epoch: ring.epoch(),
        next_state: [begin, end, offset, 0xffff_ffff],
    }
}

/// A native run does not create the per-item child jobs: their work is part of the native draw.
pub const CREATES_CHILD_JOBS: bool = false;

/// The opt-in switch for a native Shadow pass. Off by default.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum ShadowSwitch {
    #[default]
    Off,
    On,
}

/// Why a run keeps the original job.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum KeepOriginal {
    SwitchOff,
    /// A frame whose Main is not natively owned keeps the original Shadow job.
    MainNotNative,
    /// An item, cascade or selection refusal: the whole update falls back.
    Refused(ShadowError),
    /// Questions that must be settled before the native result can replace the job.
    Unresolved(Vec<ShadowQuestion>),
}

/// Who produces a run's output.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Ownership {
    Original(KeepOriginal),
    /// Never returned while any of [`OWNERSHIP_BLOCKERS`] is open.
    Native,
}

/// Decides one update's ownership. `prepared` is the result of preparing both runs (selection,
/// cascade fits and every caster); any refusal falls the whole update back.
pub fn ownership(
    switch: ShadowSwitch,
    main_native: bool,
    prepared: Result<(), ShadowError>,
) -> Ownership {
    if switch == ShadowSwitch::Off {
        return Ownership::Original(KeepOriginal::SwitchOff);
    }
    if !main_native {
        return Ownership::Original(KeepOriginal::MainNotNative);
    }
    if let Err(error) = prepared {
        return Ownership::Original(KeepOriginal::Refused(error));
    }
    Ownership::Original(KeepOriginal::Unresolved(OWNERSHIP_BLOCKERS.to_vec()))
}

/// Main's lighting producer may sample the shadow map only after both runs released it.
pub fn lighting_may_sample(released: [bool; 2]) -> bool {
    released.iter().all(|r| *r)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn inputs(distances: [f32; 3], v: f32, r: f32) -> ViewInputs {
        ViewInputs {
            distances,
            basis: [[1.0, 0.0, 0.0], [0.0, 1.0, 0.0], [0.0, 0.0, 1.0]],
            position: [0.0; 3],
            vertical: v,
            aspect: r,
            light: [0.0, -1.0, 0.0],
        }
    }

    fn relative(actual: f32, expected: f64, bound: f64) {
        assert!(
            ((actual as f64 - expected) / expected).abs() <= bound,
            "{actual} vs {expected}"
        );
    }

    #[test]
    fn v1_group_mask() {
        let mut counts = [0u32; 16];
        counts[..9].copy_from_slice(&[5, 3, 0, 2, 0, 0, 0, 4, 9]);
        let selection = select_casters(0xfe, &counts).unwrap();
        assert_eq!(selection.groups, vec![1, 2, 3, 4, 5, 6, 7]);
        assert_eq!(selection.budget, 9);
        let groups: Vec<u32> = [0, 0, 1, 1, 1, 3, 3, 7, 7, 7, 7, 8].into_iter().collect();
        let items = caster_items(0xfe, &groups).unwrap();
        assert_eq!(items, (2..11).collect::<Vec<_>>());
        let drawn: Vec<u32> = items.iter().map(|&i| groups[i]).collect();
        assert_eq!(drawn, vec![1, 1, 1, 3, 3, 7, 7, 7, 7]);
        // Mask 0: an empty run, not an error.
        let empty = select_casters(0, &counts).unwrap();
        assert!(empty.is_empty());
        assert_eq!(caster_items(0, &groups), Ok(Vec::new()));
        // The mask is read from the run's input: a narrower mask selects fewer groups.
        assert_eq!(select_casters(0x08, &counts).unwrap().budget, 2);
    }

    #[test]
    fn groups_outside_the_mask_are_unresolved() {
        let counts = [1u32; 16];
        for mask in [0x01, 0xff, 0x0800 | 0xfe] {
            assert_eq!(
                select_casters(mask, &counts),
                Err(ShadowError::Unresolved(ShadowQuestion::GroupsOutsideMask))
            );
            assert_eq!(
                caster_items(mask, &[1]),
                Err(ShadowError::Unresolved(ShadowQuestion::GroupsOutsideMask))
            );
        }
        assert!(caster_items(0xfe, &[3, 1]).is_err());
    }

    #[test]
    fn v2_v4_sphere_fits() {
        let bound = 2f64.powi(-20);
        // n = 10, f = 30: unclamped.
        let fit = inputs([10.0, 30.0, 60.0], 0.1, 1.0).fit(0).unwrap();
        relative(fit.centre_distance, 20.4, bound);
        relative(fit.radius, 110.16f64.sqrt(), bound);
        // The near corner is at the same distance.
        let near = ((20.4f64 - 10.0).powi(2) + 2.0).sqrt();
        relative(fit.radius, near, bound);
        // n = 1, f = 3: clamped to the far plane.
        let fit = inputs([1.0, 3.0, 4.0], 0.5, 2.0).fit(0).unwrap();
        relative(fit.centre_distance, 3.0, bound);
        relative(fit.radius, 11.25f64.sqrt(), bound);
        // n = 5, f = 20, as cascade 1.
        let fit = inputs([1.0, 5.0, 20.0], 0.25, 1.5).fit(1).unwrap();
        relative(fit.centre_distance, 15.0390625, bound);
        relative(fit.radius, 10.288873, 1e-6);
        relative(
            fit.radius,
            ((20.0f64 - 15.0390625).powi(2) + 81.25).sqrt(),
            bound,
        );
    }

    #[test]
    fn sphere_fit_uses_the_actual_basis() {
        // A rotated orthonormal basis gives the same fit.
        let mut i = inputs([10.0, 30.0, 60.0], 0.1, 1.0);
        let (s, c) = (0.6f32, 0.8f32);
        i.basis = [[c, s, 0.0], [-s, c, 0.0], [0.0, 0.0, 1.0]];
        i.position = [100.0, -50.0, 3.0];
        let fit = i.fit(0).unwrap();
        relative(fit.radius, 110.16f64.sqrt(), 2f64.powi(-20));
    }

    #[test]
    fn v5_refresh_rule() {
        let mut fits = CascadeFits::new();
        assert_eq!(
            fits.update(&inputs([10.0, 30.0, 60.0], 0.1, 1.0)),
            Ok([true, true])
        );
        let cascade1 = fits.fit(1).unwrap();
        assert_eq!(
            fits.update(&inputs([10.0, 31.0, 60.0], 0.1, 1.0)),
            Ok([true, false])
        );
        // Cascade 1 keeps its old fit although d1 moved.
        assert_eq!(fits.fit(1), Some(cascade1));
        assert_eq!(
            fits.update(&inputs([10.0, 31.0, 61.0], 0.1, 1.0)),
            Ok([false, true])
        );
        assert_eq!(
            fits.update(&inputs([11.0, 31.0, 61.0], 0.1, 1.0)),
            Ok([true, false])
        );
        // Pose, v and r changes alone do not refit.
        let mut moved = inputs([11.0, 31.0, 61.0], 0.2, 1.5);
        moved.position = [5.0, 5.0, 5.0];
        assert_eq!(fits.update(&moved), Ok([false, false]));
        // Invalidation refits both.
        fits.invalidate();
        assert_eq!(fits.fit(0), None);
        assert_eq!(fits.update(&moved), Ok([true, true]));
        // A declined update changes nothing.
        let before = fits.clone();
        assert!(fits.update(&inputs([11.0, 5.0, 61.0], 0.1, 1.0)).is_err());
        assert_eq!(fits, before);
    }

    #[test]
    fn v6_corners_and_axial_centres() {
        let i = inputs([4.0, 8.0, 12.0], 0.5, 2.0);
        assert_eq!(
            i.corners(4.0).unwrap(),
            [
                [4.0, 2.0, -4.0],
                [-4.0, 2.0, -4.0],
                [-4.0, -2.0, -4.0],
                [4.0, -2.0, -4.0]
            ]
        );
        assert_eq!(
            i.axial_centres().unwrap(),
            [[0.0, 0.0, -6.0], [0.0, 0.0, -10.0]]
        );
        // A general pose: within 2^-21 of the largest coordinate magnitude.
        let mut g = i;
        g.position = [1000.5, -20.25, 7.0];
        g.basis = [[0.8, 0.6, 0.0], [-0.6, 0.8, 0.0], [0.0, 0.0, 1.0]];
        let corners = g.corners(4.0).unwrap();
        let expected = [
            1000.5 + 0.8 * 4.0 - 0.6 * 2.0,
            -20.25 + 0.6 * 4.0 + 0.8 * 2.0,
            7.0 - 4.0,
        ];
        let scale = expected.iter().fold(0.0f64, |m, v: &f64| m.max(v.abs()));
        for lane in 0..3 {
            assert!((corners[0][lane] as f64 - expected[lane]).abs() <= scale * 2f64.powi(-21));
        }
    }

    #[test]
    fn cascades_decline_bad_distances_and_non_finite_fits() {
        for distances in [
            [0.0, 1.0, 2.0],
            [-1.0, 1.0, 2.0],
            [1.0, 1.0, 2.0],
            [1.0, 3.0, 2.0],
            [1.0, 2.0, f32::INFINITY],
        ] {
            assert!(matches!(
                inputs(distances, 0.5, 1.0).fit(0),
                Err(ShadowError::CascadeDeclined(_))
            ));
        }
        assert!(matches!(
            inputs([1.0, 2.0, 3.0], f32::NAN, 1.0).fit(0),
            Err(ShadowError::CascadeDeclined(_))
        ));
        // An overflowing fit is declined, not clamped.
        assert!(matches!(
            inputs([1.0, 2.0, 3.0], 1e30, 1e30).fit(0),
            Err(ShadowError::CascadeDeclined("non-finite result"))
        ));
        assert_eq!(
            light_view(
                &SphereFit {
                    centre_distance: 1.0,
                    radius: 1.0
                },
                &inputs([1.0, 2.0, 3.0], 0.5, 1.0)
            ),
            Err(ShadowError::Unresolved(ShadowQuestion::LightView))
        );
    }

    #[test]
    fn runs_cameras_and_semaphores() {
        assert_eq!(run_camera(&RUNS[0]), 1);
        assert_eq!(run_camera(&RUNS[1]), 2);
        assert_eq!(run_for_semaphore(0x490), Some(RUNS[0]));
        assert_eq!(run_for_semaphore(0x4a0), Some(RUNS[1]));
        assert_eq!(run_for_semaphore(DEPTH_SEMAPHORE_OFFSET), None);
    }

    #[test]
    fn run_state_values_and_native_state() {
        let values = run_state_values();
        assert_eq!(values.len(), 10);
        assert_eq!(values[0], (methods::SHADER_PACKER, 0));
        assert_eq!(values[3], (methods::DEPTH_FUNCTION, 0x203));
        // The masks end off.
        let last = |method: u32| values.iter().rev().find(|(m, _)| *m == method).unwrap().1;
        assert_eq!(last(methods::COLOUR_MASK), 0);
        assert_eq!(last(methods::MRT_COLOUR_MASK), 0);
        assert_eq!(run_state_methods().len(), 8);
        for method in static_only_methods() {
            assert!(!run_state_methods().contains(&method));
        }
        // The run's writes are known afterwards; stencil, offset and cull stay inherited.
        let inherited = PipelineState {
            alpha_test: Some(true),
            blend_enable: Some(true),
            stencil_enable: Some(true),
            cull_enable: Some(false),
            offset_factor: Some(7),
            polygon_offset_fill: Some(true),
            ..PipelineState::default()
        };
        let after = after_run_state(&inherited);
        assert_eq!(after.alpha_test, Some(false));
        assert_eq!(after.blend_enable, Some(false));
        assert_eq!(after.depth_test, Some(true));
        assert_eq!(after.depth_write, Some(true));
        assert_eq!(after.depth_comparison, Some(Comparison::LessOrEqual));
        assert_eq!(after.colour_masks, Some([0; 4]));
        assert_eq!(after.srgb_output, Some(false));
        assert_eq!(after.stencil_enable, Some(true));
        assert_eq!(after.cull_enable, Some(false));
        assert_eq!(after.offset_factor, Some(7));
        // The native pass draws with the stencil test off and the inherited bias.
        // The native pass applies the captured raster state explicitly.
        let native = native_pass_state(&inherited, false);
        assert_eq!(native.stencil_enable, Some(false));
        assert_eq!(native.two_sided_stencil, Some(false));
        assert_eq!(native.depth_bounds_enable, Some(false));
        assert_eq!(native.polygon_offset_fill, Some(true));
        assert_eq!(
            (native.offset_factor, native.offset_units),
            (Some(2f32.to_bits()), Some(60f32.to_bits()))
        );
        assert_eq!(
            (native.cull_enable, native.cull_face, native.front_face),
            (
                Some(true),
                Some(CullFace::Back),
                Some(FrontFace::CounterClockwise)
            )
        );
        // An unknown inherited bias no longer matters: the bias is explicit.
        let unknown = native_pass_state(&PipelineState::default(), false);
        assert_eq!(unknown.offset_factor, Some(2f32.to_bits()));
        // A material that disables culling.
        assert_eq!(native_pass_state(&inherited, true).cull_enable, Some(false));
        assert_eq!(
            caster_culling_disabled(0x1),
            Err(ShadowError::Unresolved(ShadowQuestion::ChildWork))
        );
    }

    #[test]
    fn vertex_arrays_and_constant_rows() {
        let disabled = disabled_arrays();
        assert_eq!(disabled.len(), 15);
        assert!(
            disabled
                .iter()
                .all(|&(slot, format)| (1..16).contains(&slot) && format == 0x2)
        );
        assert_eq!(ENABLED_ARRAYS, 1);
        let position = VertexBinding {
            declaration: 0,
            semantic: 1,
            slot: 0,
            format: 2,
            components: 3,
        };
        let normal = VertexBinding {
            semantic: 2,
            slot: 2,
            ..position
        };
        assert_eq!(position_binding(&[normal, position]), Ok(position));
        assert!(position_binding(&[normal]).is_err());
        assert_eq!(DRAW_CONSTANT_ROWS.transform, [3, 4, 5, 6]);
        assert_eq!(
            (DRAW_CONSTANT_ROWS.binding, DRAW_CONSTANT_ROWS.opaque),
            (24, 42)
        );
        assert_eq!(
            row_forty_two(),
            Err(ShadowError::Unresolved(ShadowQuestion::RowFortyTwo))
        );
        assert_eq!(
            transform_rows(&SphereFit {
                centre_distance: 1.0,
                radius: 1.0
            }),
            Err(ShadowError::Unresolved(ShadowQuestion::LightView))
        );
    }

    #[test]
    fn shared_surface_viewports_and_clear() {
        assert_eq!(
            SHADOW_SURFACE,
            ShadowSurface {
                width: 2048,
                height: 1024,
                depth_stencil: true,
                pitch: 8192
            }
        );
        // The halves tile the surface without overlap.
        assert_eq!(
            SURFACE_HALVES[0][0] + SURFACE_HALVES[0][2],
            SURFACE_HALVES[1][0]
        );
        assert_eq!(
            SURFACE_HALVES[1][0] + SURFACE_HALVES[1][2],
            SHADOW_SURFACE.width
        );
        assert!(SURFACE_HALVES.iter().all(|h| h[3] == SHADOW_SURFACE.height));
        const { assert!(!COLOUR_TARGET.written) };

        // Cascade 0 is settled.
        let off = LayoutAssumptions::default();
        let zero = cascade_viewport(0, &off).unwrap();
        assert_eq!(zero.rectangle, [0, 0, 1024, 1024]);
        assert_eq!(zero.scissor, zero.rectangle);
        assert_eq!(
            (zero.scale, zero.offset),
            ([512.0, -512.0, 0.5], [512.0, 512.0, 0.5])
        );
        assert_eq!(zero.depth_range, (0.0, 1.0));
        // Cascade 1 only under the named assumption.
        assert_eq!(
            cascade_viewport(1, &off),
            Err(ShadowError::Unresolved(ShadowQuestion::CascadeOneHalf))
        );
        let on = LayoutAssumptions {
            cascade_one_right_half: true,
        };
        let one = cascade_viewport(1, &on).unwrap();
        assert_eq!(one.rectangle, [1024, 0, 1024, 1024]);
        assert_eq!(one.scissor, one.rectangle);
        assert_eq!(
            (one.scale, one.offset),
            ([512.0, -512.0, 0.5], [1536.0, 512.0, 0.5])
        );
        assert!(cascade_viewport(2, &on).is_err());

        assert_eq!((FRAME_CLEAR.depth, FRAME_CLEAR.stencil), (1.0, 0));
        assert_eq!(
            resolve_draw_cascades(),
            Err(ShadowError::Unresolved(ShadowQuestion::ShadowResolveDraws))
        );
    }

    /// The captured unit-1 record: enabled, masked format 0x90 with the linear bit (0xB0),
    /// 2D, one mip, wraps (4, 4, 3) with comparison 6, linear filters, remap 0xAAE4, 2048×1024.
    fn sampling_record() -> [u32; 12] {
        let mut w = [0u32; 12];
        w[1] = (1 << 16) | (0xb0 << 8) | (2 << 4);
        w[2] = 4 | (4 << 8) | (3 << 16) | (6 << 28);
        w[3] = 0x8000_0000;
        w[4] = 0xaae4;
        w[5] = (2 << 24) | (2 << 16);
        w[6] = (2048 << 16) | 1024;
        w[7] = 0xffff_ffff;
        w
    }

    #[test]
    fn unit_one_sampling_record_checks() {
        assert_eq!(check_sampling_record(&sampling_record()), Ok(()));
        let mismatch = |edit: fn(&mut [u32; 12]), what: &'static str| {
            let mut w = sampling_record();
            edit(&mut w);
            assert_eq!(
                check_sampling_record(&w),
                Err(ShadowError::SamplingRecord(what))
            );
        };
        mismatch(|w| w[3] = 0, "unit disabled");
        mismatch(|w| w[1] = (1 << 16) | (0x90 << 8) | (2 << 4), "format");
        mismatch(|w| w[1] = (1 << 16) | (0xb5 << 8) | (2 << 4), "format");
        mismatch(|w| w[6] = (1024 << 16) | 1024, "size");
        mismatch(|w| w[1] = (2 << 16) | (0xb0 << 8) | (2 << 4), "mip count");
        mismatch(|w| w[2] = 3 | (4 << 8) | (3 << 16) | (6 << 28), "wrap");
        mismatch(
            |w| w[2] = 4 | (4 << 8) | (3 << 16) | (4 << 28),
            "comparison",
        );
        mismatch(|w| w[5] = (1 << 24) | (2 << 16), "filters");
        mismatch(|w| w[4] = 0xaae5, "remap");
        mismatch(|w| w[5] = 7 << 16, "undecodable");
    }

    #[test]
    fn cascade_pass_descriptions() {
        let groups = [0u32, 1, 1, 3, 7, 8];
        let inherited = PipelineState::default();
        let on = LayoutAssumptions {
            cascade_one_right_half: true,
        };
        let [first, second] =
            describe_frame([0xfe, 0xfe], [&groups, &groups[1..]], &inherited, &on).unwrap();
        assert_eq!((first.run, first.camera_slot), (RUNS[0], 1));
        assert_eq!((second.run, second.camera_slot), (RUNS[1], 2));
        assert_eq!(
            (first.semaphore_offset, second.semaphore_offset),
            (0x490, 0x4a0)
        );
        // The clear precedes the first cascade only.
        assert_eq!(first.clear, Some(FRAME_CLEAR));
        assert_eq!(second.clear, None);
        assert_eq!(first.casters, vec![1, 2, 3, 4]);
        assert_eq!(second.casters, vec![0, 1, 2, 3]);
        assert_eq!(first.viewport.rectangle, SURFACE_HALVES[0]);
        assert_eq!(second.viewport.rectangle, SURFACE_HALVES[1]);
        assert_eq!(first.enabled_arrays, ENABLED_ARRAYS);
        assert_eq!(first.state, native_pass_state(&inherited, false));
        assert_eq!(first.caster_state(true).cull_enable, Some(false));
        // An empty run still describes its pass.
        assert!(
            describe_cascade_pass(0, 0, &groups, &inherited, &on)
                .unwrap()
                .casters
                .is_empty()
        );
        // Without the assumption the frame is refused at cascade 1, not fabricated.
        assert_eq!(
            describe_frame(
                [0xfe, 0xfe],
                [&groups, &groups],
                &inherited,
                &LayoutAssumptions::default()
            ),
            Err(ShadowError::Unresolved(ShadowQuestion::CascadeOneHalf))
        );
        assert_eq!(
            describe_cascade_pass(0, 0x1fe, &groups, &inherited, &on),
            Err(ShadowError::Unresolved(ShadowQuestion::GroupsOutsideMask))
        );
    }

    #[test]
    fn v7_ring_generation() {
        // Generation 0b10, no wrap: every release carries 0b10 and the epoch is unchanged.
        let ring = Ring::from_epoch(0b10);
        assert!(
            [0x40u32, 0x2000, 0x3fff_fff0]
                .iter()
                .all(|&o| ring.release(o) >> 30 == 0b10)
        );
        assert_eq!(ring.epoch(), 0b10);
        // Generation 0b11 and one wrap: 0b11 before, 0b00 after, epoch 0b00.
        let mut ring = Ring::from_epoch(0b11);
        let before = ring.release(0x7f00);
        ring.wrap();
        let after = ring.release(0x40);
        assert_eq!((before >> 30, after >> 30), (0b11, 0b00));
        assert_eq!(ring.epoch(), 0b00);
        assert_eq!(release_value(0x001230, 0b11), 0xc000_1230);
        // The offset keeps only its low 30 bits.
        assert_eq!(release_value(0xffff_ffff, 0), 0x3fff_ffff);
    }

    #[test]
    fn continuation_and_publication() {
        // Side slots always return through the continuation, wrapped or not.
        assert_eq!(continuation_target(0x1000_0000, 0x3400), 0x1000_3400);
        let ring = Ring::from_epoch(1);
        let publication = writer_publication(0x2340, &ring, 0x100, 0x8000);
        assert_eq!(publication.put, 0x2340);
        assert_eq!(publication.slot_word, 0x2342);
        assert_eq!(publication.epoch, 1);
        assert_eq!(publication.next_state, [0x100, 0x8000, 0x2340, 0xffff_ffff]);
        assert_eq!(TERMINAL_SEGMENT_BYTES, 128);
        assert_eq!(
            TERMINAL_SEGMENT.last(),
            Some(&TerminalElement::JumpToContinuation)
        );
        assert_eq!(SEGMENT_BYTES, 8192);
        const { assert!(!CREATES_CHILD_JOBS) };
    }

    #[test]
    fn caster_routing() {
        let first = 0x1000;
        assert_eq!(
            caster_preparation(1, GeometrySource::Static { first }),
            Ok(CasterPreparation {
                source: GeometrySource::Static { first },
                deform: false
            })
        );
        for group in [3, 4] {
            assert!(
                caster_preparation(group, GeometrySource::Static { first })
                    .unwrap()
                    .deform
            );
        }
        assert!(
            !caster_preparation(5, GeometrySource::Direct { instanced: true })
                .unwrap()
                .deform
        );
        let skinned = GeometrySource::Skinned { first, palette: 0 };
        assert_eq!(
            caster_preparation(7, skinned),
            Err(ShadowError::Unresolved(ShadowQuestion::GroupSevenSkinning))
        );
        assert!(matches!(
            caster_preparation(5, skinned),
            Err(ShadowError::ItemRefused(_))
        ));
        for group in [0, 8, 11] {
            assert!(matches!(
                caster_preparation(group, GeometrySource::Static { first }),
                Err(ShadowError::ItemRefused(_))
            ));
        }
        assert_eq!(
            technique_outcome(3),
            Err(ShadowError::Unresolved(ShadowQuestion::ChildWork))
        );
    }

    #[test]
    fn ownership_keeps_the_original_job() {
        assert_eq!(
            ownership(ShadowSwitch::default(), true, Ok(())),
            Ownership::Original(KeepOriginal::SwitchOff)
        );
        assert_eq!(
            ownership(ShadowSwitch::On, false, Ok(())),
            Ownership::Original(KeepOriginal::MainNotNative)
        );
        let refused = ShadowError::ItemRefused("geometry");
        assert_eq!(
            ownership(ShadowSwitch::On, true, Err(refused.clone())),
            Ownership::Original(KeepOriginal::Refused(refused))
        );
        // Every precondition met: still the original job while the blockers are open.
        assert_eq!(
            ownership(ShadowSwitch::On, true, Ok(())),
            Ownership::Original(KeepOriginal::Unresolved(OWNERSHIP_BLOCKERS.to_vec()))
        );
        for obligation in OBLIGATIONS {
            for empty in [false, true] {
                assert_eq!(
                    obligation_words(obligation, empty),
                    Err(ShadowError::RingWriter)
                );
            }
        }
        assert_eq!(OBLIGATIONS.last(), Some(&Obligation::TextureReadRelease));
        assert!(!methods::DEPTH_ONLY_REPORTS.contains(&methods::TEXTURE_READ_RELEASE));
        assert!(!lighting_may_sample([true, false]));
        assert!(lighting_may_sample([true, true]));
    }
}
