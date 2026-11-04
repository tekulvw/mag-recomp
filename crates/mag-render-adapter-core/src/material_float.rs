//! Scalar Cell SPU single-precision arithmetic on raw `f32` bit patterns.
//!
//! This is a hardware number-format model, not a program emulator. It follows
//! the published Cell SPU ISA single-precision rules: exponent 255 is an
//! ordinary finite exponent, exponent 0 reads as zero, results truncate
//! towards zero, overflow saturates to the largest magnitude and underflow
//! flushes to +0. No context, instructions, registers, guest memory or SDK
//! headers are used.
const SIGN: u32 = 0x80000000;
const ONE: u32 = 0x3f800000;

fn split(bits: u32) -> Option<(u32, i32)> {
    let exponent = (bits >> 23) & 255;
    (exponent != 0).then_some(((bits & 0x7fffff) | 0x800000, exponent as i32 - 150))
}
fn pack(sign: u32, magnitude: u128, power: i32) -> u32 {
    if magnitude == 0 {
        return 0;
    }
    let top = 127 - magnitude.leading_zeros();
    let exponent = top as i32 + power + 127;
    if exponent > 255 {
        return sign | 0x7fffffff;
    }
    if exponent < 1 {
        return 0;
    }
    let significand = if top > 23 {
        magnitude >> (top - 23)
    } else {
        magnitude << (23 - top)
    };
    sign | ((exponent as u32) << 23) | (significand as u32 & 0x7fffff)
}
// Common material operands fit in 64 bits after alignment. Preserve the wide
// path for large exponent gaps and carries; both packers truncate identically.
fn pack64(sign: u32, magnitude: u64, power: i32) -> u32 {
    if magnitude == 0 {
        return 0;
    }
    let top = 63 - magnitude.leading_zeros();
    let exponent = top as i32 + power + 127;
    if exponent > 255 {
        return sign | 0x7fffffff;
    }
    if exponent < 1 {
        return 0;
    }
    let significand = if top > 23 {
        magnitude >> (top - 23)
    } else {
        magnitude << (23 - top)
    };
    sign | ((exponent as u32) << 23) | (significand as u32 & 0x7fffff)
}
fn sum(mut sa: u32, mut ma: u64, mut ea: i32, mut sb: u32, mut mb: u64, mut eb: i32) -> u32 {
    if ea < eb {
        std::mem::swap(&mut sa, &mut sb);
        std::mem::swap(&mut ma, &mut mb);
        std::mem::swap(&mut ea, &mut eb);
    }
    let difference = (ea - eb) as u32;
    if difference > 79 {
        return if sa == sb {
            pack(sa, u128::from(ma), ea)
        } else {
            pack(sa, (u128::from(ma) << 1) - 1, ea - 1)
        };
    }
    if difference < 64 && ma <= (u64::MAX >> difference) {
        let a = ma << difference;
        if sa == sb {
            if let Some(total) = a.checked_add(mb) {
                return pack64(sa, total, eb);
            }
        } else if a >= mb {
            return pack64(sa, a - mb, eb);
        } else {
            return pack64(sb, mb - a, eb);
        }
    }
    let a = u128::from(ma) << difference;
    let b = u128::from(mb);
    if sa == sb {
        pack(sa, a + b, eb)
    } else if a >= b {
        pack(sa, a - b, eb)
    } else {
        pack(sb, b - a, eb)
    }
}
/// `a * b`.
pub fn multiply(a: u32, b: u32) -> u32 {
    let (Some((ma, ea)), Some((mb, eb))) = (split(a), split(b)) else {
        return 0;
    };
    pack64((a ^ b) & SIGN, u64::from(ma) * u64::from(mb), ea + eb)
}
/// `a + b`.
pub fn add(a: u32, b: u32) -> u32 {
    match (split(a), split(b)) {
        (None, None) => 0,
        (Some(_), None) => a,
        (None, Some(_)) => b,
        (Some((ma, ea)), Some((mb, eb))) => {
            sum(a & SIGN, u64::from(ma), ea, b & SIGN, u64::from(mb), eb)
        }
    }
}
/// `a * b + c`, or `c - a * b` when `negative_product`, with one final truncation.
pub fn fused(a: u32, b: u32, c: u32, negative_product: bool) -> u32 {
    let sign = ((a ^ b) & SIGN) ^ if negative_product { SIGN } else { 0 };
    let product = match (split(a), split(b)) {
        (Some((ma, ea)), Some((mb, eb))) => Some((u64::from(ma) * u64::from(mb), ea + eb)),
        _ => None,
    };
    match (product, split(c)) {
        (None, None) => 0,
        (None, Some(_)) => c,
        (Some((m, e)), None) => pack64(sign, m, e),
        (Some((m, e)), Some((mc, ec))) => sum(sign, m, e, c & SIGN, u64::from(mc), ec),
    }
}
fn greater(a: u32, b: u32) -> bool {
    fn key(bits: u32) -> i64 {
        if bits & 0x7f800000 == 0 {
            return 0;
        }
        let magnitude = i64::from(bits & !SIGN);
        if bits & SIGN != 0 {
            -magnitude
        } else {
            magnitude
        }
    }
    key(a) > key(b)
}
// Reciprocal-estimate fractions of the SPU hardware, indexed by the five
// highest fraction bits of the input. Interpolation below uses the documented
// base/step fields of the estimate.
//
// This table is reproduced from ps3recomp `runtime/spu/spu_helpers.h`
// (`spu_frest_fraction_lut`), MIT licence, Copyright (c) 2026 Ned Heller /
// sp00nznet. The licence text is in `LICENSE.ps3recomp` at the repository root.
const RECIPROCAL_FRACTIONS: [u32; 32] = [
    0x7ffbe0, 0x7f87a6, 0x70ef72, 0x708b40, 0x638b12, 0x633aea, 0x5792c4, 0x574aa0, 0x4cca7e,
    0x4c9262, 0x430a44, 0x42d62a, 0x3a2e12, 0x39fdfa, 0x3215e4, 0x31f1d2, 0x2aa9be, 0x2a85ac,
    0x23d59a, 0x23bd8e, 0x1d8576, 0x1d8576, 0x17ad5a, 0x17ad5a, 0x124543, 0x124543, 0x0d392d,
    0x0d392d, 0x08851a, 0x08851a, 0x041d07, 0x041d07,
];
fn reciprocal_estimate(bits: u32) -> u32 {
    let exponent = (bits >> 23) & 255;
    let out = if exponent == 0 {
        255
    } else {
        253u32.saturating_sub(exponent)
    };
    (bits & SIGN) | (out << 23) | RECIPROCAL_FRACTIONS[((bits >> 18) & 31) as usize]
}
fn interpolate(value: u32, estimate: u32) -> u32 {
    let mut exponent = (estimate >> 23) & 255;
    if exponent == 0 {
        return 0;
    }
    let base = u64::from((estimate >> 10) & 0x1fff);
    let step = u64::from(estimate & 0x3ff);
    let y = u64::from(value & 0x7ffff);
    let mut magnitude = ((0x2000 | base) << 19) - step * y;
    if magnitude < (1u64 << 32) {
        magnitude <<= 1;
        exponent -= 1;
        if exponent == 0 {
            return 0;
        }
    }
    (estimate & SIGN) | (exponent << 23) | ((magnitude >> 9) as u32 & 0x7fffff)
}
fn positive_reciprocal(value: u32) -> u32 {
    let estimate = interpolate(value, reciprocal_estimate(value));
    let initial = multiply(ONE, estimate);
    let residual = fused(initial, value, ONE, true);
    fused(residual, estimate, initial, false)
}
/// `numerator / denominator` through the hardware reciprocal estimate and one
/// refinement step.
pub fn ratio(numerator: u32, denominator: u32) -> u32 {
    let estimate = interpolate(denominator, reciprocal_estimate(denominator));
    let initial = multiply(numerator, estimate);
    let residual = fused(initial, denominator, numerator, true);
    fused(residual, estimate, initial, false)
}
/// Unsigned integer to single precision, truncating.
pub fn unsigned(value: u32) -> u32 {
    pack64(0, u64::from(value), 0)
}
/// Alpha-transfer row for a material: from an 8-bit alpha `a` and a scalar `s`
/// (raw `f32` bits), with `k = 0x3b808080` (1/255 truncated to `f32`), `n = a * k` and
/// `m = a * (s * k)`, returns
/// `[n > 0 ? 1 / n : 1, m < 1 ? 1 / (1 - m) : 1, m, 1]`. Every operation uses the
/// SPU rounding above; reciprocals use the hardware estimate, interpolation and
/// one refinement step, so they can differ from a host division by one ULP.
pub fn alpha_transfer(alpha: u8, scalar: u32) -> [u32; 4] {
    let source_alpha = pack64(0, u64::from(alpha), 0);
    let normalization = 0x3b808080;
    let normalized = multiply(source_alpha, normalization);
    let scaled = multiply(scalar, normalization);
    let modulation = multiply(source_alpha, scaled);
    let remainder = add(ONE, modulation ^ SIGN);
    [
        if greater(normalized, 0) {
            positive_reciprocal(normalized)
        } else {
            ONE
        },
        if greater(ONE, modulation) {
            positive_reciprocal(remainder)
        } else {
            ONE
        },
        modulation,
        ONE,
    ]
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn source_format_flush_saturation_and_fused_rounding_are_explicit() {
        assert_eq!(multiply(ONE, 0x7fffffff), 0x7fffffff);
        assert_eq!(multiply(ONE, 0x80000000), 0);
        assert_eq!(multiply(ONE, 1), 0);
        assert_eq!(add(ONE, ONE ^ SIGN), 0);
        assert_eq!(multiply(0x7fffffff, 0x40000000), 0x7fffffff);
        assert_eq!(fused(ONE, ONE, ONE, true), 0);
    }
    #[test]
    fn zero_alpha_retains_source_reciprocal_rounding() {
        // The hardware estimate, interpolation and refinement truncate the
        // second reciprocal to one ULP below 1. A host division would not.
        assert_eq!(alpha_transfer(0, ONE), [ONE, 0x3f7fffff, 0, ONE]);
        assert_eq!(alpha_transfer(0, 0xffffffff), [ONE, 0x3f7fffff, 0, ONE]);
    }
}
