use mag_render_adapter_scene::geometry_streams::{
    GeometryStreamError, decode_descriptor_positions,
};
fn format(stride: u8, offset: u8) -> Vec<u8> {
    let mut bytes = vec![0; 16];
    bytes[0] = 1;
    bytes[1] = stride;
    bytes[8] = offset;
    bytes[9] = 6;
    bytes[10] = 3;
    bytes[11] = 1;
    bytes[12] = 4;
    bytes
}
#[test]
fn packed_codec_is_distinct_from_static_half_triples() {
    let positions = decode_descriptor_positions(
        &format(4, 0),
        &[0, 0, 0, 0, 0xff, 0xff, 0xff, 0xff, 0, 0, 4, 0],
        3,
        48,
    )
    .unwrap();
    assert_eq!(
        positions,
        [
            [0x3a001002, 0x3a001002, 0x3a802008, 0x3f800000],
            [0xba001002, 0xba001002, 0xba802008, 0x3f800000],
            [0xbf7fffff, 0x3a001002, 0x3a802008, 0x3f800000]
        ]
    );
}
#[test]
fn descriptor_stride_offset_and_exact_logical_tail_drive_reads() {
    let mut source = vec![0x77; 15];
    source[3..7].copy_from_slice(&[0; 4]);
    source[11..15].copy_from_slice(&[0xff; 4]);
    let positions = decode_descriptor_positions(&format(8, 3), &source, 2, 32).unwrap();
    assert_eq!(
        positions[0],
        [0x3a001002, 0x3a001002, 0x3a802008, 0x3f800000]
    );
    assert_eq!(
        positions[1],
        [0xba001002, 0xba001002, 0xba802008, 0x3f800000]
    );
    assert_eq!(
        decode_descriptor_positions(&format(8, 3), &source[..14], 2, 32),
        Err(GeometryStreamError::SourceExtent)
    );
    assert_eq!(
        decode_descriptor_positions(&format(8, 3), &source, 2, 31),
        Err(GeometryStreamError::BudgetExceeded)
    );
}
#[test]
fn constant_source_stride_does_not_require_adjacent_rows() {
    let result = decode_descriptor_positions(&format(0, 0), &[0; 4], 3, 48).unwrap();
    assert_eq!(result.len(), 3);
    assert_eq!(result[0], result[2]);
}
#[test]
fn first_position_declaration_is_retained_without_dropping_other_semantic_obligations() {
    let mut layout = format(8, 0);
    layout[0] = 3;
    layout.extend_from_slice(&[0, 255, 4, 9, 0, 0, 0, 0]);
    layout.extend_from_slice(&[4, 6, 3, 1, 4, 0, 0, 0]);
    let result =
        decode_descriptor_positions(&layout, &[0, 0, 0, 0, 0xff, 0xff, 0xff, 0xff], 1, 16).unwrap();
    assert_eq!(result, [[0x3a001002, 0x3a001002, 0x3a802008, 0x3f800000]]);
}
#[test]
fn missing_truncated_or_unresolved_position_layout_is_explicit() {
    let mut layout = format(4, 0);
    layout[9] = 255;
    assert_eq!(
        decode_descriptor_positions(&layout, &[0; 4], 1, 16),
        Err(GeometryStreamError::UnsupportedPositionEncoding(255))
    );
    layout[11] = 2;
    assert_eq!(
        decode_descriptor_positions(&layout, &[0; 4], 1, 16),
        Err(GeometryStreamError::MissingPositionAttribute)
    );
    layout[0] = 2;
    assert_eq!(
        decode_descriptor_positions(&layout, &[0; 4], 1, 16),
        Err(GeometryStreamError::SourceExtent)
    );
    assert!(
        decode_descriptor_positions(&format(4, 255), &[], 0, 0)
            .unwrap()
            .is_empty()
    );
    assert_eq!(
        decode_descriptor_positions(&format(4, 0), &[], usize::MAX, usize::MAX),
        Err(GeometryStreamError::SourceExtent)
    );
}
#[test]
fn descriptor_operation_consumes_owned_raw_config_format_stream_and_indices() {
    use mag_render_adapter_scene::{
        geometry_streams::decode_descriptor_position_inputs,
        guest_memory::GuestSnapshot,
        mesh_sources::PartitionChain,
        partition_inputs::{NonInstancedSourceBases, copy_noninstanced_inputs},
    };
    let mut header = [0; 544];
    for (slot, size) in [(1, 6u32), (7, 4), (19, 16), (20, 16)] {
        header[0x30 + slot * 8..0x34 + slot * 8].copy_from_slice(&size.to_be_bytes());
        header[0x34 + slot * 8..0x38 + slot * 8]
            .copy_from_slice(&(0x3000u32 + slot as u32 * 0x100).to_be_bytes());
    }
    let chain =
        PartitionChain::copy(&GuestSnapshot::new(0x1000, &header).unwrap(), 0x1000, 1).unwrap();
    let mut config = vec![0; 16];
    config[2] = 255;
    config[8..10].copy_from_slice(&1u16.to_be_bytes());
    config[10..12].copy_from_slice(&3u16.to_be_bytes());
    let bundle = copy_noninstanced_inputs(
        &chain.partitions()[0],
        NonInstancedSourceBases {
            palette_base: 0,
            extra_stream: 0,
            environment: 0,
        },
        1000,
        &mut |r| {
            Ok(match r.address {
                0x3100 => vec![0; 6],
                0x3700 => vec![0; 4],
                0x4300 => config.clone(),
                0x4400 => format(4, 0),
                _ => panic!("unexpected source"),
            })
        },
    )
    .unwrap();
    assert_eq!(
        decode_descriptor_position_inputs(&bundle, 21),
        Err(GeometryStreamError::BudgetExceeded)
    );
    let result = decode_descriptor_position_inputs(&bundle, 22).unwrap();
    drop(bundle);
    assert_eq!(
        result.positions,
        [[0x3a001002, 0x3a001002, 0x3a802008, 0x3f800000]]
    );
    assert_eq!(result.indices, [0, 0, 0]);
}
#[test]
fn component_metadata_fills_missing_position_axes_with_positive_zero() {
    for (components, expected) in [
        (0, [0, 0, 0, 0x3f800000]),
        (1, [0xba001002, 0, 0, 0x3f800000]),
        (2, [0xba001002, 0xba001002, 0, 0x3f800000]),
        (3, [0xba001002, 0xba001002, 0xba802008, 0x3f800000]),
        (4, [0xba001002, 0xba001002, 0xba802008, 0x3f800000]),
        (255, [0xba001002, 0xba001002, 0xba802008, 0x3f800000]),
    ] {
        let mut layout = format(4, 0);
        layout[10] = components;
        assert_eq!(
            decode_descriptor_positions(&layout, &[0xff; 4], 1, 16).unwrap(),
            [expected]
        );
    }
}
