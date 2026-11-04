#![cfg(feature = "owned-scene")]
use mag_render_adapter_core::scene_viewport::{SourceViewport, ViewportError};
fn source() -> SourceViewport {
    SourceViewport {
        scale: [320.0f32, -240.0, 0.5, 0.0].map(f32::to_bits),
        offset: [336.25f32, 247.5, 0.5, 0.0].map(f32::to_bits),
        clip_min: 0,
        clip_max: 1.0f32.to_bits(),
        scissor: [16, 8, 624, 472],
    }
}
#[test]
fn real_window_mapping_preserves_fractional_jitter_without_moving_clip_planes() {
    let src = source();
    let mapped = src.map(640, 480).unwrap();
    let v = mapped.viewport;
    assert_eq!([v.x, v.y, v.width, v.height], [16.25, 7.5, 640.0, 480.0]);
    let scale = src.scale.map(f32::from_bits);
    let offset = src.offset.map(f32::from_bits);
    for p in [
        [-1.0f32, -1.0, -1.0, 1.0],
        [1.0, 1.0, 1.0, 1.0],
        [0.7, -0.2, 0.1, 2.0],
        [-3.0, 0.4, 0.6, 2.0],
    ] {
        let ndc = [p[0] / p[3], p[1] / p[3], p[2] / p[3]];
        let window = [
            v.x + (ndc[0] + 1.0) * v.width * 0.5,
            v.y + (1.0 - ndc[1]) * v.height * 0.5,
        ];
        let original = [ndc[0] * scale[0] + offset[0], ndc[1] * scale[1] + offset[1]];
        for i in 0..2 {
            assert!((window[i] - original[i]).abs() < 0.0001);
        }
        // The pre-viewport XY clip values remain exactly the unmodified HPOS.
        assert_eq!(mapped.shader_scale[0..2], [1.0f32.to_bits(); 2]);
        assert_eq!(mapped.shader_offset[0..2], [0; 2]);
    }
    assert_eq!(mapped.shader_scale[2], src.scale[2]);
    assert_eq!(mapped.shader_offset[2], src.offset[2]);
    assert_eq!([v.min_depth, v.max_depth], [0.0, 1.0]);
}
#[test]
fn clip_range_scissor_and_finite_source_values_are_not_inferred() {
    let mut s = source();
    s.clip_min = 0.25f32.to_bits();
    assert_eq!(s.map(640, 480).unwrap_err(), ViewportError::ClipRange);
    s = source();
    s.scissor[2] = u32::MAX;
    assert_eq!(s.map(640, 480).unwrap_err(), ViewportError::Extent);
    s = source();
    s.scale[0] = f32::NAN.to_bits();
    assert_eq!(s.map(640, 480).unwrap_err(), ViewportError::NonFinite);
    s = source();
    s.scissor[2] = 0;
    assert_eq!(s.map(640, 480).unwrap().viewport.scissor_width, 0);
}

#[test]
fn source_sample_grid_scales_actual_window_and_scissor_with_same_shader_wpos_grid() {
    let s = source();
    let m = s.map_with_sample_grid(1280, 960, [2, 2]).unwrap();
    assert_eq!(
        [
            m.viewport.x,
            m.viewport.y,
            m.viewport.width,
            m.viewport.height
        ],
        [32.5, 15.0, 1280.0, 960.0]
    );
    assert_eq!(
        [
            m.viewport.scissor_x,
            m.viewport.scissor_y,
            m.viewport.scissor_width,
            m.viewport.scissor_height
        ],
        [32, 16, 1248, 944]
    );
    assert_eq!(m.sample_grid, [2, 2]);
    assert_eq!(m.shader_scale[2], s.scale[2]);
    assert_eq!(
        s.map_with_sample_grid(640, 480, [2, 2]).unwrap_err(),
        ViewportError::Extent
    );
    assert_eq!(
        s.map_with_sample_grid(1280, 960, [4, 1]).unwrap_err(),
        ViewportError::Extent
    );
}
