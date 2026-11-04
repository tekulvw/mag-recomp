//! Native owned geometry and persistent fit state for the Shadow source owner.
//! Derived renderer objects/guest allocation machinery are not reproduced.
//! Implemented from an own-words behaviour specification of the Shadow owner;
//! light views, clipping and consumer closure remain.
use crate::{
    source::ShadowCornerSources,
    views::{self, ShadowCornerOutput, ViewMathError},
};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct CascadeFit {
    pub radius: u32,
    pub center_distance: u32,
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ShadowOwnerGeometryOutput {
    pub corners: ShadowCornerOutput,
    pub fits: [CascadeFit; 2],
    pub refreshed: [bool; 2],
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum OwnerGeometryError {
    InvalidDistances,
    Numeric(ViewMathError),
}
impl From<ViewMathError> for OwnerGeometryError {
    fn from(value: ViewMathError) -> Self {
        Self::Numeric(value)
    }
}
#[derive(Default)]
/// Native renderer state, not a guest snapshot or source-readiness certificate.
/// Its owner must bind initialization/reset to real configuration lifetime and
/// establish complete source history before using it for owner replacement.
pub struct ShadowOwnerGeometry {
    distances: Option<[u32; 3]>,
    fits: [Option<CascadeFit>; 2],
}
impl ShadowOwnerGeometry {
    pub fn reset(&mut self) {
        self.distances = None;
        self.fits = [None; 2];
    }
    /// Compute from raw camera inputs; never accept original prepared fits.
    /// All fallible work precedes state update, so refusal cannot advance cache.
    pub fn prepare(
        &mut self,
        source: &ShadowCornerSources,
    ) -> Result<ShadowOwnerGeometryOutput, OwnerGeometryError> {
        let distances = source.parameters.map(f32::from_bits);
        if distances.iter().any(|d| !d.is_finite() || *d <= 0.0)
            || !(distances[0] < distances[1] && distances[1] < distances[2])
        {
            return Err(OwnerGeometryError::InvalidDistances);
        }
        let corners = views::prepare_shadow_corners(source)?;
        let refreshed = match self.distances {
            None => [true, true],
            Some(old) => [
                old[0] != source.parameters[0] || old[1] != source.parameters[1],
                old[2] != source.parameters[2],
            ],
        };
        let mut fits = [CascadeFit {
            radius: 0,
            center_distance: 0,
        }; 2];
        for cascade in 0..2 {
            fits[cascade] = if refreshed[cascade] {
                fit(
                    source,
                    &corners,
                    cascade,
                    distances[cascade],
                    distances[cascade + 1],
                )?
            } else {
                self.fits[cascade].ok_or(OwnerGeometryError::InvalidDistances)?
            };
        }
        self.distances = Some(source.parameters);
        self.fits = fits.map(Some);
        Ok(ShadowOwnerGeometryOutput {
            corners,
            fits,
            refreshed,
        })
    }
}
fn scalar_fma(a: f32, b: f32, c: f32) -> Result<f32, ViewMathError> {
    // The specified scalar math evaluates a fused multiply-add in binary64 and
    // then rounds once to binary32. This is distinct from vector multiply/add.
    views::checked(f64::from(a).mul_add(f64::from(b), f64::from(c)))
}
fn fit(
    source: &ShadowCornerSources,
    corners: &ShadowCornerOutput,
    cascade: usize,
    near: f32,
    far: f32,
) -> Result<CascadeFit, ViewMathError> {
    use views::{add, checked, displacement, product, read, sub};
    let scale = read(source.view_scalar_60)?;
    let aspect = read(source.view_scalar_a24)?;
    let near_height = product(near, scale)?;
    let near_width = product(near_height, aspect)?;
    let far_height = product(far, scale)?;
    let far_width = product(far_height, aspect)?;
    let near_radial = scalar_fma(near_width, near_width, product(near_height, near_height)?)?;
    let far_radial = scalar_fma(far_width, far_width, product(far_height, far_height)?)?;
    let separation = sub(far, near)?;
    let numerator = add(
        far_radial,
        scalar_fma(separation, separation, -near_radial)?,
    )?;
    let quotient = checked(f64::from(numerator) / f64::from(add(separation, separation)?))?;
    let center = add(near, quotient)?.min(far);
    let mut squared = [0.0; 3];
    for (lane, value) in squared.iter_mut().enumerate() {
        let backward = sub(0.0, read(source.pose_rows[2][lane])?)?;
        let position = read(source.pose_rows[3][lane])?;
        let center_world = add(position, displacement(backward, center)?)?;
        let far_corner = f32::from_bits(corners.vectors()[(cascade + 1) * 4][lane]);
        let delta = sub(center_world, far_corner)?;
        *value = displacement(delta, delta)?;
    }
    let squared_length = add(add(squared[0], squared[1])?, squared[2])?;
    let radius = if squared_length <= f32::MIN_POSITIVE {
        0.0
    } else {
        let estimate = checked(1.0 / f64::from(squared_length.sqrt()))?;
        let estimate_squared = displacement(estimate, estimate)?;
        let residual = sub(1.0, product(squared_length, estimate_squared)?)?;
        let half_estimate = displacement(estimate, 0.5)?;
        let refined = add(product(residual, half_estimate)?, estimate)?;
        displacement(squared_length, refined)?
    };
    Ok(CascadeFit {
        radius: radius.to_bits(),
        center_distance: center.to_bits(),
    })
}
