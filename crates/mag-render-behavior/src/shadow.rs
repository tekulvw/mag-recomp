//! The Shadow pass (`shadow-pass.md`), the parts the specification settles.
//!
//! The specification is partial. This module implements:
//!
//! - the two cascade runs and caster selection by the run's enabled-group mask;
//! - the cascade views: plane corners, axial centres, the bounding-sphere fit and the refresh
//!   rule;
//! - the routing of a caster onto Main's geometry preparation;
//! - the fixed-function state the run builds, as a set of methods whose values are unknown;
//! - the native-replacement contract (the Depth-style terminal obligations and completion
//!   ordering), as a decision that keeps the original job.
//!
//! Every open question of the specification stays an explicit [`ShadowQuestion`] refusal: the
//! state values, the light view, the map layout, the colour target, the constant rows, the
//! terminal segment, the technique outcome, the cohort camera, groups outside the mask and the
//! group-7 skinning admission. Nothing here is wired into the frontend; [`ShadowSwitch`] is off
//! by default and, while any blocking question is open, [`ownership`] always keeps the original
//! job.

use crate::material_sources::GeometrySource;
use crate::setup::PipelineState;

/// The open questions of `shadow-pass.md`, by number.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub enum ShadowQuestion {
    /// 1: the values of the state the run builds, and whether they vary per run, group or
    /// material.
    StateValues = 1,
    /// 2: how a cascade sphere becomes an orthographic light view.
    LightView = 2,
    /// 3: the shadow map layout, size, format, viewport and scissor.
    MapLayout = 3,
    /// 4: whether a colour target is bound and read.
    ColourTarget = 4,
    /// 5: which vertex constant rows the one-, two- and eight-row uploads write.
    ConstantRows = 5,
    /// 6: the exact terminal command-ring segment of a run.
    TerminalSegment = 6,
    /// 7: which technique the shadow pass object selects, and discarding casters.
    TechniqueSelection = 7,
    /// 8: the cohort camera index of each run's caster list, and disjointness.
    FamiliesAndCameras = 8,
    /// 9: whether any setting enables groups outside `0xFE`.
    GroupsOutsideMask = 9,
    /// 10: the group-7 skinning admission conditions.
    GroupSevenSkinning = 10,
}

/// The questions that block native ownership of a run's output.
pub const OWNERSHIP_BLOCKERS: [ShadowQuestion; 5] = [
    ShadowQuestion::StateValues,
    ShadowQuestion::LightView,
    ShadowQuestion::MapLayout,
    ShadowQuestion::ConstantRows,
    ShadowQuestion::TerminalSegment,
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
pub const OBSERVED_GROUP_MASK: u32 = 0xfe;

/// The groups a run may draw under the established rules: 1–7.
pub const SUPPORTED_GROUPS: u32 = 0xfe;

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
/// A mask bit outside groups 1–7 is open question 9 and is refused, never drawn or dropped.
/// Which camera's counts the caller passes is open question 8; see [`run_camera`].
pub fn select_casters(mask: u32, group_counts: &[u32; 16]) -> Result<CasterSelection, ShadowError> {
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
pub fn caster_items(mask: u32, groups: &[u32]) -> Result<Vec<usize>, ShadowError> {
    if mask & !SUPPORTED_GROUPS != 0 {
        return Err(ShadowError::Unresolved(ShadowQuestion::GroupsOutsideMask));
    }
    if groups.windows(2).any(|w| w[0] > w[1]) {
        return Err(ShadowError::ItemRefused("cohort out of group order"));
    }
    Ok(groups
        .iter()
        .enumerate()
        .filter(|(_, g)| **g < 32 && mask & (1 << **g) != 0)
        .map(|(i, _)| i)
        .collect())
}

/// The cohort camera index of a run's caster list: open question 8.
pub fn run_camera(_run: &CascadeRun) -> Result<usize, ShadowError> {
    Err(ShadowError::Unresolved(ShadowQuestion::FamiliesAndCameras))
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

/// Where cascade `k` is written in the shadow map: open question 3.
pub fn cascade_region(_cascade: usize) -> Result<[u32; 4], ShadowError> {
    Err(ShadowError::Unresolved(ShadowQuestion::MapLayout))
}

/// Whether a colour target is bound during the runs: open question 4.
pub fn colour_target() -> Result<bool, ShadowError> {
    Err(ShadowError::Unresolved(ShadowQuestion::ColourTarget))
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

/// Every method of the run's own fixed-function state.
pub fn run_state_methods() -> Vec<u32> {
    use methods::*;
    let mut all = vec![ALPHA_TEST, BLEND_ENABLE, COLOUR_MASK, STENCIL_ENABLE];
    all.extend(FRONT_STENCIL);
    all.extend(BACK_STENCIL);
    all.extend([
        MRT_COLOUR_MASK,
        DEPTH_FUNCTION,
        DEPTH_WRITE,
        DEPTH_TEST,
        OFFSET_FACTOR,
        OFFSET_BIAS,
        SHADER_PACKER,
    ]);
    all
}

/// The state after the run's own writes, as far as it is known: every field the run writes
/// becomes unknown, because the values are open question 1. Fields the run does not write keep
/// their inherited knowledge.
pub fn after_run_state(state: &PipelineState) -> PipelineState {
    PipelineState {
        alpha_test: None,
        blend_enable: None,
        colour_masks: None,
        stencil_enable: None,
        front_stencil: None,
        back_stencil: None,
        depth_comparison: None,
        depth_write: None,
        depth_test: None,
        offset_factor: None,
        offset_units: None,
        ..*state
    }
}

/// The values of the run's fixed-function state: open question 1.
pub fn run_state_values() -> Result<Vec<(u32, u32)>, ShadowError> {
    Err(ShadowError::Unresolved(ShadowQuestion::StateValues))
}

/// The rows of the vertex constant uploads, including the eight-row light transform: open
/// question 5 (and 2 for the transform's values).
pub fn constant_rows() -> Result<[u32; 3], ShadowError> {
    Err(ShadowError::Unresolved(ShadowQuestion::ConstantRows))
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

/// The technique outcome for each group, including discarding casters: open question 7.
/// Selection itself is `material_sources::item_sources` with the run's own pass object.
pub fn technique_outcome(_mesh_group: u16) -> Result<bool, ShadowError> {
    Err(ShadowError::Unresolved(ShadowQuestion::TechniqueSelection))
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

/// The exact segment an obligation writes. The segment words, final state, continuation and
/// texture-read semaphore offset and value are open question 6.
pub fn obligation_words(
    _obligation: Obligation,
    _empty_run: bool,
) -> Result<Vec<u32>, ShadowError> {
    Err(ShadowError::Unresolved(ShadowQuestion::TerminalSegment))
}

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
        assert_eq!(
            run_camera(&RUNS[0]),
            Err(ShadowError::Unresolved(ShadowQuestion::FamiliesAndCameras))
        );
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
    fn run_state_methods_and_values() {
        let all = run_state_methods();
        assert_eq!(all.len(), 4 + 7 + 7 + 7);
        let mut unique = all.clone();
        unique.sort_unstable();
        unique.dedup();
        assert_eq!(unique.len(), all.len());
        assert_eq!(
            run_state_values(),
            Err(ShadowError::Unresolved(ShadowQuestion::StateValues))
        );
        // After the run, the fields it writes are unknown; the others are inherited.
        let state = PipelineState {
            alpha_test: Some(true),
            depth_test: Some(true),
            cull_enable: Some(false),
            offset_factor: Some(1),
            polygon_offset_fill: Some(true),
            ..PipelineState::default()
        };
        let after = after_run_state(&state);
        assert_eq!(after.alpha_test, None);
        assert_eq!(after.depth_test, None);
        assert_eq!(after.offset_factor, None);
        assert_eq!(after.cull_enable, Some(false));
        assert_eq!(after.polygon_offset_fill, Some(true));
        assert_eq!(
            constant_rows(),
            Err(ShadowError::Unresolved(ShadowQuestion::ConstantRows))
        );
        assert_eq!(
            cascade_region(0),
            Err(ShadowError::Unresolved(ShadowQuestion::MapLayout))
        );
        assert_eq!(
            colour_target(),
            Err(ShadowError::Unresolved(ShadowQuestion::ColourTarget))
        );
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
            Err(ShadowError::Unresolved(ShadowQuestion::TechniqueSelection))
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
                    Err(ShadowError::Unresolved(ShadowQuestion::TerminalSegment))
                );
            }
        }
        assert_eq!(OBLIGATIONS.last(), Some(&Obligation::TextureReadRelease));
        assert!(!methods::DEPTH_ONLY_REPORTS.contains(&methods::TEXTURE_READ_RELEASE));
        assert!(!lighting_may_sample([true, false]));
        assert!(lighting_may_sample([true, true]));
    }
}
