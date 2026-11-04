//! Packed-normal declarations (codec 9) in the stream decoder. The decode
//! itself is the behaviour crate's (`geometry-normals.md`), so values are
//! checked within that specification's tolerances, not bit for bit.
use mag_render_adapter_scene::geometry_streams::*;

/// One declaration: offset 0, codec 9, three components, semantic 2.
const FORMAT: [u8; 16] = [1, 3, 1, 0, 0, 0, 0, 0, 0, 9, 3, 2, 3, 2, 0, 0];

fn decode(bytes: [u8; 3]) -> [f32; 4] {
    decode_descriptor_attribute_at(&FORMAT, &bytes, 0x1000, 2, 1, 16).unwrap()[0]
        .map(f32::from_bits)
}

#[test]
fn packed_normals_match_the_specification_vectors() {
    for (bytes, expected) in [
        ([0x80, 0x12, 0xc5], [0.000_691, 0.956_302, -0.292_381, -1.0]),
        ([0xc8, 0x06, 0x4a], [0.398_828, -0.568_865, -0.719_256, 1.0]),
        ([0x7f, 0xdf, 0xf0], [-1.0, -0.000_691, -0.000_691, -1.0]),
        ([0x4b, 0x2b, 0xcb], [-0.292_381, 0.260_586, 0.920_113, 1.0]),
    ] {
        let lanes = decode(bytes);
        for lane in 0..3 {
            assert!(
                (lanes[lane] - expected[lane]).abs() <= 3.5e-4,
                "{bytes:02x?} lane {lane}: {} vs {}",
                lanes[lane],
                expected[lane]
            );
        }
        assert_eq!(lanes[3], expected[3]);
    }
}

#[test]
fn corner_codes_clamp_instead_of_refusing() {
    // u = v = 0 with selector 1: the planar lanes are -1/sqrt(2) and the
    // reconstructed lane 0 is a small non-negative value.
    let lanes = decode([0, 0, 1]);
    assert!((0.0..=0.025).contains(&lanes[0]));
    for lane in [1, 2] {
        assert!((lanes[lane] + std::f32::consts::FRAC_1_SQRT_2).abs() <= 1e-4);
    }
    assert_eq!(lanes[3], -1.0);
}

#[test]
fn normal_declarations_keep_extent_budget_and_selector_refusals() {
    assert_eq!(
        decode_descriptor_attribute_at(&FORMAT, &[0; 3], 0x1003, 2, 1, 16),
        Err(GeometryStreamError::SourceExtent)
    );
    assert_eq!(
        decode_descriptor_attribute_at(&FORMAT, &[0; 3], 0x1000, 2, 1, 15),
        Err(GeometryStreamError::BudgetExceeded)
    );
    assert_eq!(
        decode_descriptor_attribute_at(&FORMAT, &[0; 2], 0x1000, 2, 1, 16),
        Err(GeometryStreamError::SourceExtent)
    );
    for selector in [12, 15] {
        assert_eq!(
            decode_descriptor_attribute_at(&FORMAT, &[0, 0, selector], 0x1000, 2, 1, 16),
            Err(GeometryStreamError::UnqualifiedNormalSelector(selector))
        );
    }
}
