//! Packed spherical normal decoding, attribute codec 9 (`geometry-normals.md`).

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum NormalError {
    /// Selectors 12–15 have no known meaning and are refused.
    UnqualifiedSelector(u32),
}

/// The ideal planar map `L(q) = −1/√2 + q·√2/1023`.
pub fn planar(q: u32) -> f64 {
    let root2 = std::f64::consts::SQRT_2;
    -root2 / 2.0 + q as f64 * root2 / 1023.0
}

/// Decodes three stream bytes into (unit vector, handedness).
pub fn decode_packed_normal(bytes: [u8; 3]) -> Result<[f32; 4], NormalError> {
    let n = u32::from_be_bytes([0, bytes[0], bytes[1], bytes[2]]);
    let u = n >> 14;
    let v = (n >> 4) & 0x3ff;
    let sel = n & 0xf;
    if sel >= 12 {
        return Err(NormalError::UnqualifiedSelector(sel));
    }
    let axis = (sel / 4) as usize;
    let (a, b) = (planar(u), planar(v));
    let remainder = (1.0 - (a * a + b * b)).max(0.0);
    let magnitude = remainder.sqrt();
    let c = if sel & 1 != 0 { magnitude } else { -magnitude };
    let handedness = if sel & 2 != 0 { 1.0 } else { -1.0 };
    let mut out = [0.0f32; 4];
    let mut planar_lanes = [a, b].into_iter();
    for (lane, value) in out.iter_mut().enumerate().take(3) {
        *value = if lane == axis {
            c as f32
        } else {
            planar_lanes.next().expect("two planar lanes") as f32
        };
    }
    out[3] = handedness;
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::testutil::Rng;

    fn check(bytes: [u8; 3], expected: [f64; 3], handedness: f32) {
        let out = decode_packed_normal(bytes).unwrap();
        for lane in 0..3 {
            assert!(
                (out[lane] as f64 - expected[lane]).abs() <= 1e-5,
                "{bytes:02x?} lane {lane}: {} vs {}",
                out[lane],
                expected[lane]
            );
        }
        assert_eq!(out[3], handedness);
    }

    #[test]
    fn table_vectors() {
        check([0x80, 0x12, 0xc5], [0.000_691, 0.956_302, -0.292_381], -1.0);
        check([0xc8, 0x06, 0x4a], [0.398_828, -0.568_865, -0.719_256], 1.0);
        check([0x7f, 0xdf, 0xf0], [-1.0, -0.000_691, -0.000_691], -1.0);
        check([0x4b, 0x2b, 0xcb], [-0.292_381, 0.260_586, 0.920_113], 1.0);
    }

    #[test]
    fn corner_codes_clamp() {
        let out = decode_packed_normal([0xff, 0xc0, 0x03]).unwrap();
        assert!((0.0..=0.025).contains(&out[0]));
        assert!(
            (out[1] - std::f32::consts::FRAC_1_SQRT_2).abs() < 1e-5
                && (out[2] + std::f32::consts::FRAC_1_SQRT_2).abs() < 1e-5
        );
        assert_eq!(out[3], 1.0);
        let out = decode_packed_normal([0x00, 0x00, 0x06]).unwrap();
        assert!((-0.025..=0.0).contains(&out[1]));
        assert!(
            (out[0] + std::f32::consts::FRAC_1_SQRT_2).abs() < 1e-5
                && (out[2] + std::f32::consts::FRAC_1_SQRT_2).abs() < 1e-5
        );
        assert_eq!(out[3], 1.0);
    }

    #[test]
    fn unqualified_selectors() {
        assert_eq!(
            decode_packed_normal([0, 0, 0x0c]),
            Err(NormalError::UnqualifiedSelector(12))
        );
        assert_eq!(
            decode_packed_normal([0, 0, 0x0f]),
            Err(NormalError::UnqualifiedSelector(15))
        );
    }

    #[test]
    fn every_selector_at_the_centre() {
        for sel in 0..12u32 {
            let n = (512 << 14) | (512 << 4) | sel;
            let bytes = [(n >> 16) as u8, (n >> 8) as u8, n as u8];
            let out = decode_packed_normal(bytes).unwrap();
            let axis = (sel / 4) as usize;
            assert!((out[axis].abs() - 0.999_999_5).abs() < 1e-6);
            assert_eq!(out[axis] > 0.0, sel & 1 != 0);
            assert_eq!(out[3], if sel & 2 != 0 { 1.0 } else { -1.0 });
            for lane in (0..3).filter(|lane| *lane != axis) {
                assert!((out[lane] - 0.000_691).abs() < 1e-6);
            }
        }
    }

    /// Property: over every (u, v) pair the planar lanes match the reference map within 1e-4,
    /// `|c² − c_ref²| ≤ 4e-4`, the sign follows bit 0, and the vector has unit length.
    #[test]
    fn property_all_planar_codes() {
        let mut rng = Rng::new(31);
        for u in 0..1024u32 {
            for v in (0..1024u32).step_by(7).chain([1023]) {
                let sel = rng.below(12);
                let n = (u << 14) | (v << 4) | sel;
                let out = decode_packed_normal([(n >> 16) as u8, (n >> 8) as u8, n as u8]).unwrap();
                let axis = (sel / 4) as usize;
                let others: Vec<usize> = (0..3).filter(|lane| *lane != axis).collect();
                let (a, b) = (planar(u), planar(v));
                assert!((out[others[0]] as f64 - a).abs() <= 1e-4);
                assert!((out[others[1]] as f64 - b).abs() <= 1e-4);
                let c_ref2 = (1.0 - a * a - b * b).max(0.0);
                let c = out[axis] as f64;
                assert!((c * c - c_ref2).abs() <= 4e-4);
                if c != 0.0 {
                    assert_eq!(c > 0.0, sel & 1 != 0);
                }
                let length: f64 = out[..3].iter().map(|x| (*x as f64).powi(2)).sum();
                assert!((length - 1.0).abs() < 1e-3);
            }
        }
    }
}
