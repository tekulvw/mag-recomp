//! Named quaternion, affine/view and depth-axis products on source float bits.
//! No worker, emulated registers, local store, packet stream or SDK dependency.
use crate::material_constants::{
    source_add as add, source_fused as fused, source_multiply as multiply, source_ratio as ratio,
    source_unsigned as unsigned,
};
const ONE: u32 = 0x3f800000;

pub fn affine(quaternion_translation_scale: [u32; 8]) -> [[u32; 4]; 3] {
    let [x, y, z, w, tx, ty, tz, scale] = quaternion_translation_scale;
    let twice = [add(x, x), add(y, y), add(z, z), add(w, w)];
    let diagonal = |a: u32, twice_a: u32, b: u32, twice_b: u32| {
        fused(b, twice_b, fused(a, twice_a, ONE, true), true)
    };
    let rows = [
        [
            diagonal(y, twice[1], z, twice[2]),
            fused(z, twice[3], multiply(x, twice[1]), true),
            fused(y, twice[3], multiply(x, twice[2]), false),
            tx,
        ],
        [
            fused(z, twice[3], multiply(y, twice[0]), false),
            diagonal(z, twice[2], x, twice[0]),
            fused(x, twice[3], multiply(y, twice[2]), true),
            ty,
        ],
        [
            fused(y, twice[3], multiply(z, twice[0]), true),
            fused(x, twice[3], multiply(z, twice[1]), false),
            diagonal(x, twice[0], y, twice[1]),
            tz,
        ],
    ];
    std::array::from_fn(|r| {
        std::array::from_fn(|c| {
            if c == 3 {
                rows[r][3]
            } else {
                multiply(rows[r][c], scale)
            }
        })
    })
}
pub fn view(affine: [[u32; 4]; 3], world_to_view: [[u32; 4]; 4]) -> [[u32; 4]; 3] {
    std::array::from_fn(|column| {
        std::array::from_fn(|lane| {
            let mut value = multiply(world_to_view[0][column], affine[0][lane]);
            value = fused(world_to_view[1][column], affine[1][lane], value, false);
            value = fused(world_to_view[2][column], affine[2][lane], value, false);
            fused(
                world_to_view[3][column],
                if lane == 3 { ONE } else { 0 },
                value,
                false,
            )
        })
    })
}
pub fn atlas(numerators: [u32; 4], denominators: [u32; 4]) -> [u32; 4] {
    std::array::from_fn(|i| ratio(unsigned(numerators[i]), unsigned(denominators[i])))
}
fn product(row: [u32; 4], matrix: [[u32; 4]; 4]) -> [u32; 4] {
    std::array::from_fn(|lane| {
        let mut value = multiply(row[0], matrix[0][lane]);
        for i in 1..4 {
            value = fused(row[i], matrix[i][lane], value, false);
        }
        value
    })
}
pub fn depth_axis_projection(projection: [[u32; 4]; 4], anchor: u32, scale: u32) -> [[u32; 4]; 4] {
    let identity = std::array::from_fn(|r| std::array::from_fn(|c| if r == c { ONE } else { 0 }));
    let mut translate = identity;
    translate[3][2] = anchor;
    let mut scaling = identity;
    scaling[2][2] = scale;
    let mut inverse = identity;
    inverse[3][2] = anchor ^ 0x80000000;
    let first = translate.map(|row| product(row, scaling));
    let combined = first.map(|row| product(row, inverse));
    projection.map(|row| product(row, combined))
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn actual_quaternion_scale_translation_and_view_inputs_are_used() {
        let source = [0, 0, 0, ONE, 0x3f800000, 0x40000000, 0x40400000, 0x40000000];
        let a = affine(source);
        assert_eq!(
            a,
            [
                [0x40000000, 0, 0, ONE],
                [0, 0x40000000, 0, 0x40000000],
                [0, 0, 0x40000000, 0x40400000]
            ]
        );
        let identity =
            std::array::from_fn(|r| std::array::from_fn(|c| if r == c { ONE } else { 0 }));
        assert_eq!(view(a, identity), a);
        assert_eq!(depth_axis_projection(identity, ONE, ONE), identity);
    }
}
