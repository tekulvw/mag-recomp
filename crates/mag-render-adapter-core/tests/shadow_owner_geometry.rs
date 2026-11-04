use mag_render_adapter_core::{
    shadow_owner::{OwnerGeometryError, ShadowOwnerGeometry},
    source::ShadowCornerSources,
};
fn source() -> ShadowCornerSources {
    ShadowCornerSources {
        parameters: [2.0f32, 4.0, 8.0].map(f32::to_bits),
        pose_rows: [
            [1.0f32, 0., 0., 0.],
            [0., 1., 0., 0.],
            [0., 0., 1., 0.],
            [10., 20., 30., 1.],
        ]
        .map(|r| r.map(f32::to_bits)),
        view_scalar_60: 0.5f32.to_bits(),
        view_scalar_a24: 2.0f32.to_bits(),
    }
}
#[test]
fn prepares_two_owned_cascade_fits_from_raw_camera() {
    let mut owner = ShadowOwnerGeometry::default();
    let prepared = owner.prepare(&source()).unwrap();
    assert_eq!(prepared.refreshed, [true, true]);
    assert_eq!(prepared.fits[0].center_distance, 4.0f32.to_bits());
    assert_eq!(prepared.fits[1].center_distance, 8.0f32.to_bits());
    assert_eq!(prepared.fits[0].radius, 20.0f32.sqrt().to_bits());
    assert_eq!(prepared.fits[1].radius, 80.0f32.sqrt().to_bits());
    assert_eq!(
        prepared.corners.vectors()[0],
        [12.0f32, 21., 28., 1.].map(f32::to_bits)
    );
}
#[test]
fn cache_preserves_fit_across_camera_and_scale_changes() {
    let mut owner = ShadowOwnerGeometry::default();
    let first = owner.prepare(&source()).unwrap();
    let mut input = source();
    input.pose_rows[3][0] = 1000.125f32.to_bits();
    input.view_scalar_60 = 0.1f32.to_bits();
    let next = owner.prepare(&input).unwrap();
    assert_eq!(next.refreshed, [false, false]);
    assert_eq!(next.fits, first.fits);
    assert_ne!(next.corners, first.corners);
}
#[test]
fn shared_middle_distance_refreshes_first_cascade_only() {
    let mut owner = ShadowOwnerGeometry::default();
    let mut input = source();
    input.view_scalar_60 = 0.1f32.to_bits();
    input.view_scalar_a24 = 1.0f32.to_bits();
    let first = owner.prepare(&input).unwrap();
    input.parameters[1] = 5.0f32.to_bits();
    let next = owner.prepare(&input).unwrap();
    assert_eq!(next.refreshed, [true, false]);
    assert_eq!(next.fits[1], first.fits[1]);
    let fresh = ShadowOwnerGeometry::default().prepare(&input).unwrap();
    assert_ne!(next.fits[1], fresh.fits[1]);
    input.parameters[2] = 9.0f32.to_bits();
    assert_eq!(owner.prepare(&input).unwrap().refreshed, [false, true]);
}
#[test]
fn failed_geometry_does_not_partially_advance_native_cache() {
    let mut owner = ShadowOwnerGeometry::default();
    let first = owner.prepare(&source()).unwrap();
    let mut input = source();
    input.parameters[1] = input.parameters[0];
    assert_eq!(
        owner.prepare(&input),
        Err(OwnerGeometryError::InvalidDistances)
    );
    input = source();
    input.parameters[0] = 1.0f32.to_bits();
    input.pose_rows[3][0] = f32::INFINITY.to_bits();
    assert!(owner.prepare(&input).is_err());
    let recovered = owner.prepare(&source()).unwrap();
    assert_eq!(recovered.refreshed, [false, false]);
    assert_eq!(recovered.fits, first.fits);
    owner.reset();
    assert_eq!(owner.prepare(&source()).unwrap().refreshed, [true, true]);
}
