use mag_render_adapter_scene::geometry_streams::{
    GeometryStreamError, decode_descriptor_positions_at,
};
fn format(codec: u8, components: u8, stride: u8, offset: u8, extent: u8) -> [u8; 16] {
    [
        1, stride, 0, 0, 0, 0, 0, 0, offset, codec, components, 1, extent, 0, 0, 0,
    ]
}
#[test]
fn generic_float_preserves_four_components_and_fills_only_missing_position_lanes() {
    let words = [0x80000000u32, 0x7fc01234, 0x3f800000, 0xbf000000];
    let source: Vec<_> = words.into_iter().flat_map(u32::to_be_bytes).collect();
    for count in [0, 1, 2, 3, 4, 255] {
        let mut expected = [0, 0, 0, 0x3f800000];
        let leading = usize::from(count).min(4);
        expected[..leading].copy_from_slice(&words[..leading]);
        assert_eq!(
            decode_descriptor_positions_at(&format(2, count, 16, 0, 16), &source, 0x1000, 1, 16)
                .unwrap(),
            [expected]
        );
    }
}
#[test]
fn float_cross_block_control_and_true_source_alignment_are_observable() {
    let source: Vec<_> = (0..64u8).collect();
    assert_eq!(
        decode_descriptor_positions_at(&format(2, 3, 32, 15, 0), &source, 0x1000, 1, 16).unwrap(),
        [[0x0f000102, 0x03040506, 0x0708090a, 0x3f800000]]
    );
    assert_eq!(
        decode_descriptor_positions_at(&format(2, 3, 32, 15, 16), &source, 0x1000, 1, 16).unwrap(),
        [[0x0f101112, 0x13141516, 0x1718191a, 0x3f800000]]
    );
    assert_eq!(
        decode_descriptor_positions_at(&format(2, 3, 32, 12, 0), &source, 0x1003, 1, 16),
        Err(GeometryStreamError::SourceExtent)
    );
    assert_eq!(
        decode_descriptor_positions_at(&format(2, 3, 32, 12, 16), &source, 0x1003, 1, 16).unwrap(),
        [[0x090a0b0c, 0x0d0e0f10, 0x11121314, 0x3f800000]]
    );
}
#[test]
fn generic_rebased_codec_is_distinct_from_builtin_six_and_keeps_declared_w() {
    let source = [0x80, 0, 0x3c, 0, 0, 1, 0xbc, 0];
    assert_eq!(
        decode_descriptor_positions_at(&format(3, 4, 8, 0, 8), &source, 0x1000, 1, 16).unwrap(),
        [[0x80000000, 0x3f800000, 0x38002000, 0xbf800000]]
    );
    assert_eq!(
        decode_descriptor_positions_at(&format(3, 3, 8, 0, 8), &source[..6], 0x1000, 1, 16)
            .unwrap(),
        [[0x80000000, 0x3f800000, 0x38002000, 0x3f800000]]
    );
    assert_eq!(
        decode_descriptor_positions_at(&format(3, 4, 8, 0, 8), &source[..7], 0x1000, 1, 16),
        Err(GeometryStreamError::SourceExtent)
    );
    assert_eq!(
        decode_descriptor_positions_at(&format(3, 4, 8, 0, 8), &source, 0x1000, 1, 15),
        Err(GeometryStreamError::BudgetExceeded)
    );
}
