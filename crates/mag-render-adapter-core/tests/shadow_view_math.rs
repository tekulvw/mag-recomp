use mag_render_adapter_core::{
    source::ShadowCornerSources,
    views::{ViewMathError, prepare_shadow_corners},
};

fn source() -> ShadowCornerSources {
    ShadowCornerSources {
        parameters: [2.0f32, 4.0, 8.0].map(f32::to_bits),
        pose_rows: [
            [1.0f32, 0.0, 0.0, 0.0],
            [0.0, 1.0, 0.0, 0.0],
            [0.0, 0.0, 1.0, 0.0],
            [10.0, 20.0, 30.0, 1.0],
        ]
        .map(|v| v.map(f32::to_bits)),
        view_scalar_60: 0.5f32.to_bits(),
        view_scalar_a24: 2.0f32.to_bits(),
    }
}
#[test]
fn prepares_absolute_planes_and_adjacent_centers() {
    let output = prepare_shadow_corners(&source()).unwrap();
    let expected = [
        [12.0f32, 21.0, 28.0, 1.0],
        [8.0, 21.0, 28.0, 1.0],
        [8.0, 19.0, 28.0, 1.0],
        [12.0, 19.0, 28.0, 1.0],
        [14.0, 22.0, 26.0, 1.0],
        [6.0, 22.0, 26.0, 1.0],
        [6.0, 18.0, 26.0, 1.0],
        [14.0, 18.0, 26.0, 1.0],
        [18.0, 24.0, 22.0, 1.0],
        [2.0, 24.0, 22.0, 1.0],
        [2.0, 16.0, 22.0, 1.0],
        [18.0, 16.0, 22.0, 1.0],
        [10.0, 20.0, 27.0, 1.0],
        [10.0, 20.0, 24.0, 1.0],
    ]
    .map(|v| v.map(f32::to_bits));
    assert_eq!(output.vectors(), &expected);
    let bytes = output.to_be_bytes();
    assert_eq!(bytes.len(), 224);
    for (word, chunk) in expected.into_iter().flatten().zip(bytes.as_chunks::<4>().0) {
        assert_eq!(*chunk, word.to_be_bytes());
    }
}
#[test]
fn ignores_only_basis_fourth_lanes() {
    let expected = prepare_shadow_corners(&source()).unwrap();
    let mut input = source();
    for row in &mut input.pose_rows[..3] {
        row[3] = f32::NAN.to_bits();
    }
    assert_eq!(prepare_shadow_corners(&input).unwrap(), expected);
    input.pose_rows[3][3] = f32::NAN.to_bits();
    assert_eq!(
        prepare_shadow_corners(&input),
        Err(ViewMathError::NonFinite)
    );
}
#[test]
fn rejects_unsupported_numeric_domain_without_partial_outputs() {
    let mut input = source();
    input.parameters[0] = 1;
    assert_eq!(
        prepare_shadow_corners(&input),
        Err(ViewMathError::Subnormal)
    );
    input = source();
    input.view_scalar_60 = f32::MAX.to_bits();
    assert_eq!(
        prepare_shadow_corners(&input),
        Err(ViewMathError::NonFinite)
    );
    input = source();
    input.parameters = [f32::MIN_POSITIVE.to_bits(); 3];
    input.view_scalar_60 = 0.25f32.to_bits();
    assert_eq!(
        prepare_shadow_corners(&input),
        Err(ViewMathError::Subnormal)
    );
}
#[test]
fn preserves_signed_zero_arithmetic_and_does_not_require_affine_basis() {
    let mut input = source();
    input.pose_rows = [[0x80000000; 4]; 4];
    input.parameters = [0x80000000; 3];
    let output = prepare_shadow_corners(&input).unwrap();
    assert!(output.vectors().iter().flatten().all(|v| *v == 0));
    input = source();
    input.parameters = [8.0f32.to_bits(), (-2.0f32).to_bits(), 4.0f32.to_bits()];
    input.pose_rows[0][0] = 3.0f32.to_bits();
    assert!(prepare_shadow_corners(&input).is_ok());
}
