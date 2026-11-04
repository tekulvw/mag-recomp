//! Waveform helpers shared by the deformation specifications (defined in
//! `group-zero-deformation.md`, used unchanged by `noninstanced-deformation.md`). They involve no
//! game-chosen constants.

/// `frac(x) = x − floor(x)`, in `[0, 1)`.
pub fn frac(x: f64) -> f64 {
    let f = x - x.floor();
    // Guard the rounding case where a tiny negative x gives exactly 1.
    if f >= 1.0 { 0.0 } else { f }
}

/// Triangle wave `|2·frac(x) − 1|`: 1 at integers, 0 at half-integers.
pub fn tri(x: f64) -> f64 {
    (2.0 * frac(x) - 1.0).abs()
}

/// Cubic smoothstep `t²·(3 − 2t)`.
pub fn smoothstep(t: f64) -> f64 {
    t * t * (3.0 - 2.0 * t)
}

/// `wave(φ; f) = S(tri(f·(2·frac(φ) − 1) + 1/2))`, in `[0, 1]`.
pub fn wave(phase: f64, frequency: f64) -> f64 {
    smoothstep(tri(frequency * (2.0 * frac(phase) - 1.0) + 0.5))
}

/// `ripple(φ) = 2·S(tri(φ)) − 1`, in `[−1, 1]`.
pub fn ripple(phase: f64) -> f64 {
    2.0 * smoothstep(tri(phase)) - 1.0
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::testutil::Rng;

    /// The binary32 value nearest a decimal, widened.
    fn f(x: f64) -> f64 {
        x as f32 as f64
    }

    #[test]
    fn frac_of_negative_values() {
        assert_eq!(frac(-0.25), 0.75);
        assert_eq!(frac(2.5), 0.5);
        assert_eq!(frac(-3.0), 0.0);
    }

    #[test]
    fn gz4_wave_function() {
        let rows = [
            (0.0, 1.0, 0.0),
            (0.25, 1.0, 1.0),
            (0.125, 1.0, 0.5),
            (-0.375, 2.0, 1.0),
            (1_000_000.25, 1.0, 1.0),
            (3.7, 0.3, 0.145_152),
            (-2.6, 1.5, 0.648),
        ];
        for (phase, frequency, expected) in rows {
            let got = wave(f(phase), f(frequency));
            assert!(
                (got - expected).abs() <= 1e-5,
                "wave({phase}; {frequency}) = {got}"
            );
        }
    }

    #[test]
    fn ni5_ripple_helper() {
        let rows = [
            (0.0, 1.0),
            (0.25, 0.0),
            (0.5, -1.0),
            (0.75, 0.0),
            (-0.1, 0.792),
            (2.3, -0.296),
        ];
        for (phase, expected) in rows {
            let got = ripple(f(phase));
            assert!((got - expected).abs() <= 1e-5, "ripple({phase}) = {got}");
        }
    }

    /// Property: ranges, continuity of `wave` across integers, and the slope bounds.
    #[test]
    fn property_ranges_and_slopes() {
        let mut rng = Rng::new(41);
        let h = 1e-7;
        for _ in 0..20_000 {
            let phase = rng.range_f64(-100.0, 100.0);
            let frequency = rng.range_f64(-3.0, 3.0);
            let w = wave(phase, frequency);
            assert!((0.0..=1.0).contains(&w));
            let r = ripple(phase);
            assert!((-1.0..=1.0).contains(&r));
            let w_slope = (wave(phase + h, frequency) - w).abs() / h;
            assert!(w_slope <= 6.0 * frequency.abs() + 1e-3);
            let r_slope = (ripple(phase + h) - r).abs() / h;
            assert!(r_slope <= 6.0 + 1e-3);
        }
        for integer in -5..=5 {
            let x = integer as f64;
            for frequency in [0.3, 1.0, 1.7] {
                assert!((wave(x - 1e-9, frequency) - wave(x + 1e-9, frequency)).abs() < 1e-6);
            }
        }
    }
}
