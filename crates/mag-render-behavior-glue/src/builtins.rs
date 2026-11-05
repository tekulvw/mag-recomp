//! Kind-31 and kind-32 animated fragment built-in rows for a caller that has
//! already decoded the material's role values and read the pass time.
//!
//! The behaviour crate computes these rows inside
//! [`mag_render_behavior::fragment_builtins::builtin_rows`], which reads the
//! material, chunk and pass itself. The adapter decodes requests earlier and
//! keeps only the role values and the time word, so this function presents
//! exactly those inputs to `builtin_rows` as a one-record material. The rows,
//! the scale factors and every refusal are the behaviour crate's.
use mag_render_behavior::fragment_builtins::{BuiltinError, builtin_rows};
use mag_render_behavior::guest::SparseMemory;

/// Role values `R_0 … R_7`, three words each; `None` for a role no record carries.
pub type RoleWords = [Option<[u32; 3]>; 8];

const PASS: u32 = 0x1000;
const BUILTINS: usize = 0x100;
const PARAMETERS: usize = 0x200;
const VALUES: usize = 0x300;
const TECHNIQUES: usize = 0x80;

fn put16(bytes: &mut [u8], at: usize, value: u16) {
    bytes[at..at + 2].copy_from_slice(&value.to_be_bytes());
}
fn put32(bytes: &mut [u8], at: usize, value: u32) {
    bytes[at..at + 4].copy_from_slice(&value.to_be_bytes());
}

/// The rows of one kind-31 (four rows) or kind-32 (two rows) request, in slot
/// order from its destination slot. `time` is the pass time word
/// (`PASS+0x1E0`). Any other kind returns no rows.
pub fn animated_rows(
    kind: u32,
    roles: &RoleWords,
    time: u32,
) -> Result<Vec<[u32; 4]>, BuiltinError> {
    if !matches!(kind, 31 | 32) {
        return Ok(Vec::new());
    }
    let mut material = vec![0u8; 0x400];
    put16(&mut material, 0x42, BUILTINS as u16);
    put16(&mut material, 0x44, PARAMETERS as u16);
    put16(&mut material, 0x46, VALUES as u16);
    put16(&mut material, 0x48, TECHNIQUES as u16);
    // Technique 0 uses membership bit 0; one built-in record of this kind
    // with mask bit 0, whose destination word is chunk word 0.
    material[0x53] = 1;
    put32(&mut material, BUILTINS + 4, kind);
    put32(&mut material, BUILTINS + 8, 1);
    put32(&mut material, BUILTINS + 12, 0);
    let mut count = 0u8;
    for (role, words) in roles.iter().enumerate() {
        let Some(words) = words else { continue };
        let record = PARAMETERS + 16 * usize::from(count);
        let index = 3 * u16::from(count);
        put16(&mut material, record + 8, index);
        put16(&mut material, record + 14, role as u16);
        for (w, word) in words.iter().enumerate() {
            put32(&mut material, VALUES + 4 * (usize::from(index) + w), *word);
        }
        count += 1;
    }
    material[0x52] = count;
    // Destination kind 3 (fragment constants), slot 0.
    let chunk = (3u32 << 24).to_be_bytes();
    let mut pass = SparseMemory::new();
    pass.put(PASS + 0x1e0, &time.to_be_bytes());
    Ok(builtin_rows(&pass, PASS, &material, &chunk, 0)?
        .into_iter()
        .map(|(_, row)| row)
        .collect())
}

#[cfg(test)]
mod tests {
    use super::*;
    use mag_render_behavior::fragment_builtins::{K1, K2};

    fn word(v: f32) -> [u32; 3] {
        [v.to_bits(), 0, 0]
    }

    #[test]
    fn kind_31_rows_use_the_behaviour_scale_factors() {
        let mut roles: RoleWords = [None; 8];
        roles[0] = Some([2f32.to_bits(), 4f32.to_bits(), 0]);
        roles[1] = Some(word(2.0));
        roles[2] = Some(word(0.5));
        let rows = animated_rows(31, &roles, 3f32.to_bits()).unwrap();
        let f = |r: [u32; 4]| r.map(f32::from_bits);
        assert_eq!(rows.len(), 4);
        // u = 3·(2/2) = 3, s = 3·(4/2) = 6, angle = π/2: a ≈ 0, b = 6.
        let close = |a: f32, b: f32| (a - b).abs() <= 1e-5 * b.abs().max(1.0);
        let r0 = f(rows[0]);
        assert!(close(r0[0], -3f32.sqrt() / 2.0 * 3.0) && close(r0[1], 7.5));
        assert_eq!(f(rows[2]), [2.0, 2.0, -K1 * 2.0, K1 * 2.0]);
        assert_eq!(f(rows[3]), [-K2 * 2.0, K2 * 2.0, -2.0, -2.0]);
    }

    #[test]
    fn kind_32_rows_are_products_and_missing_roles_refuse() {
        let mut roles: RoleWords = [None; 8];
        for (r, v) in [(3, 2.0f32), (4, 3.0), (5, 4.0), (7, 0.5)] {
            roles[r] = Some(word(v));
        }
        roles[6] = Some([1f32.to_bits(), 2f32.to_bits(), 4f32.to_bits()]);
        let rows = animated_rows(32, &roles, 0.5f32.to_bits()).unwrap();
        assert_eq!(
            rows.iter()
                .map(|r| r.map(f32::from_bits))
                .collect::<Vec<_>>(),
            [[6.0, 2.0, 0.0, 0.0], [0.5, 1.0, 2.0, 0.0]]
        );
        roles[5] = None;
        assert!(matches!(
            animated_rows(32, &roles, 0),
            Err(BuiltinError::UnqualifiedAnimated { kind: 32, role: 5 })
        ));
        assert_eq!(animated_rows(3, &roles, 0), Ok(Vec::new()));
    }
}
