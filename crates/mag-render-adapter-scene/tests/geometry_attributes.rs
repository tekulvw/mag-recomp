use mag_render_adapter_scene::{geometry_attributes::*, geometry_streams::*};
fn layout(declarations: &[[u8; 8]]) -> Vec<u8> {
    let mut bytes = vec![declarations.len() as u8, 32, 0, 0, 0, 0, 0, 0];
    bytes.extend(declarations.iter().flatten());
    bytes
}
#[test]
fn all_source_semantics_are_owned_with_first_declaration_order_and_explicit_failures() {
    let format = layout(&[
        [0, 2, 3, 1, 16, 0, 0, 0],
        [16, 3, 2, 9, 8, 0, 0, 0],
        [0, 6, 3, 1, 4, 0, 0, 0],
        [24, 8, 2, 2, 8, 0, 0, 0],
    ]);
    let mut source = vec![0; 32];
    source[..16].copy_from_slice(
        &[1f32, 2., 3., 4.]
            .into_iter()
            .flat_map(f32::to_be_bytes)
            .collect::<Vec<_>>(),
    );
    source[16..24].copy_from_slice(&[0x3c, 0, 0xbc, 0, 0, 1, 0x80, 0]);
    let result = decode_primary_attributes(&format, &source, 0x1000, 1, 1024).unwrap();
    source.fill(0xff);
    assert_eq!(result.declaration_count, 4);
    assert_eq!(result.attributes.len(), 3);
    assert_eq!(result.attributes[0].semantic, 1);
    assert_eq!(result.attributes[1].semantic, 9);
    assert_eq!(
        result.attributes[1].words.as_slice(),
        [[0x3f800000, 0xbf800000, 0x38002000, 0x80000000]]
    );
    assert_eq!(result.aliases[0].source_declaration, 2);
    assert_eq!(result.aliases[0].first_declaration, 0);
    assert_eq!(result.failures[0].source_declaration, 3);
    assert_eq!(result.failures[0].semantic, 2);
    assert_eq!(
        result.failures[0].error,
        GeometryStreamError::UnsupportedAttributeEncoding {
            semantic: 2,
            codec: 8
        }
    );
    assert_eq!(result.owned_bytes(), 48);
}
#[test]
fn normal_integer_and_color_field_endpoints_are_not_ieee_half_or_gpu_unorm() {
    for (codec, source, expected) in [
        (
            1,
            vec![0, 0, 0x7f, 0xff, 0x80, 0, 0xff, 0xff],
            [0x37800080, 0x3f7fffff, 0xbf7fffff, 0xb7800080],
        ),
        (
            4,
            vec![0, 1, 127, 255],
            [0, 0x3b808080, 0x3efefefe, 0x3f7fffff],
        ),
        (
            5,
            vec![0, 0, 0x7f, 0xff, 0x80, 0, 0xff, 0xff],
            [0, 0x46fffe00, 0xc7000000, 0xbf800000],
        ),
        (
            7,
            vec![0, 1, 127, 255],
            [0, 0x3f800000, 0x42fe0000, 0x437f0000],
        ),
    ] {
        let format = layout(&[[0, codec, 0, 2, 16, 0, 0, 0]]);
        assert_eq!(
            decode_primary_attributes(&format, &source, 0x1000, 1, 16)
                .unwrap()
                .attributes[0]
                .words
                .as_slice(),
            [expected]
        );
    }
}
#[test]
fn nonposition_metadata_does_not_apply_position_defaults_and_native_budget_keeps_all_obligations() {
    let format = layout(&[[0, 2, 0, 2, 16, 0, 0, 0], [0, 2, 4, 9, 16, 0, 0, 0]]);
    let source: Vec<_> = [1f32, 2., 3., 4.]
        .into_iter()
        .flat_map(f32::to_be_bytes)
        .collect();
    let result = decode_primary_attributes(&format, &source, 0x1000, 1, 16).unwrap();
    assert_eq!(
        result.attributes[0].words.as_slice(),
        [[
            1f32.to_bits(),
            2f32.to_bits(),
            3f32.to_bits(),
            4f32.to_bits()
        ]]
    );
    assert_eq!(result.failures[0].semantic, 9);
    assert_eq!(
        result.failures[0].error,
        GeometryStreamError::BudgetExceeded
    );
    assert_eq!(
        result.declaration_count,
        result.attributes.len() + result.failures.len()
    );
    assert_eq!(
        decode_primary_attributes(&format[..23], &source, 0x1000, 1, 1024).unwrap_err(),
        GeometryStreamError::SourceExtent
    );
}

#[test]
fn failed_first_semantic_cannot_be_silently_replaced_by_successful_duplicate() {
    let format = layout(&[[0, 8, 4, 2, 16, 0, 0, 0], [0, 2, 4, 2, 16, 0, 0, 0]]);
    let source = vec![0; 16];
    let result = decode_primary_attributes(&format, &source, 0x1000, 1, 32).unwrap();
    assert_eq!(result.declaration_count, 2);
    assert_eq!(result.attributes.len(), 1);
    assert_eq!(result.failures.len(), 1);
    assert_eq!(result.aliases.len(), 1);
    assert!(result.first_semantic(2).unwrap().is_err());
    assert!(result.first_semantic(9).is_none());
}
