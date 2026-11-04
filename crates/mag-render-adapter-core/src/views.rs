//! Authored bounded geometry algorithm, implemented from an own-words
//! behaviour specification of the Shadow view corners. No original
//! instruction organization, guest writes, hooks or frame claims.
use crate::source::ShadowCornerSources;

pub const SHADOW_VIEW_OUTPUT_BYTES: usize = 224;
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ViewMathError {
    NonFinite,
    Subnormal,
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ShadowCornerOutput {
    vectors: [[u32; 4]; 14],
}
impl ShadowCornerOutput {
    pub fn vectors(&self) -> &[[u32; 4]; 14] {
        &self.vectors
    }
    pub fn to_be_bytes(&self) -> [u8; SHADOW_VIEW_OUTPUT_BYTES] {
        let mut bytes = [0; SHADOW_VIEW_OUTPUT_BYTES];
        for (word, chunk) in self
            .vectors
            .iter()
            .flatten()
            .zip(bytes.as_chunks_mut::<4>().0)
        {
            chunk.copy_from_slice(&word.to_be_bytes());
        }
        bytes
    }
}
pub(crate) fn checked(value: f64) -> Result<f32, ViewMathError> {
    let rounded = value as f32;
    if !rounded.is_finite() {
        return Err(ViewMathError::NonFinite);
    }
    if value != 0.0 && value.abs() < f64::from(f32::MIN_POSITIVE) {
        return Err(ViewMathError::Subnormal);
    }
    Ok(rounded)
}
pub(crate) fn read(word: u32) -> Result<f32, ViewMathError> {
    checked(f64::from(f32::from_bits(word)))
}
pub(crate) fn add(a: f32, b: f32) -> Result<f32, ViewMathError> {
    checked(f64::from(a) + f64::from(b))
}
pub(crate) fn sub(a: f32, b: f32) -> Result<f32, ViewMathError> {
    checked(f64::from(a) - f64::from(b))
}
pub(crate) fn product(a: f32, b: f32) -> Result<f32, ViewMathError> {
    checked(f64::from(a) * f64::from(b))
}
pub(crate) fn displacement(axis: f32, scale: f32) -> Result<f32, ViewMathError> {
    add(product(axis, scale)?, 0.0)
}
/// Prepare fourteen owned vectors from a complete bounded helper readset.
/// Acceptance is finite normal/zero inputs and intermediate results. This does
/// not establish source readiness, a complete frame, or permission to suppress.
pub fn prepare_shadow_corners(
    source: &ShadowCornerSources,
) -> Result<ShadowCornerOutput, ViewMathError> {
    let mut distances = [0.0; 3];
    for (target, word) in distances.iter_mut().zip(source.parameters) {
        *target = read(word)?;
    }
    let scale = read(source.view_scalar_60)?;
    let aspect = read(source.view_scalar_a24)?;
    let mut basis = [[0.0; 4]; 3];
    for (target, row) in basis.iter_mut().zip(source.pose_rows) {
        for (lane, word) in target[..3].iter_mut().zip(row) {
            *lane = read(word)?;
        }
    }
    let mut position = [0.0; 4];
    for (target, word) in position.iter_mut().zip(source.pose_rows[3]) {
        *target = read(word)?;
    }
    let mut output = ShadowCornerOutput {
        vectors: [[0; 4]; 14],
    };
    for (plane, distance) in distances.into_iter().enumerate() {
        let height = product(distance, scale)?;
        let width = product(height, aspect)?;
        for lane in 0..4 {
            let axis = sub(0.0, basis[2][lane])?;
            let center = add(position[lane], displacement(axis, distance)?)?;
            let right = displacement(basis[0][lane], width)?;
            let up = displacement(basis[1][lane], height)?;
            for (corner, (positive_x, positive_y)) in
                [(true, true), (false, true), (false, false), (true, false)]
                    .into_iter()
                    .enumerate()
            {
                let horizontal = if positive_x {
                    add(center, right)?
                } else {
                    sub(center, right)?
                };
                let value = if positive_y {
                    add(horizontal, up)?
                } else {
                    sub(horizontal, up)?
                };
                output.vectors[plane * 4 + corner][lane] = value.to_bits();
            }
        }
    }
    for center in 0..2 {
        let distance = product(add(distances[center], distances[center + 1])?, 0.5)?;
        for (lane, value) in position.into_iter().enumerate() {
            let axis = sub(0.0, basis[2][lane])?;
            output.vectors[12 + center][lane] =
                add(value, displacement(axis, distance)?)?.to_bits();
        }
    }
    Ok(output)
}
