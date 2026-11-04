use mag_render_adapter_scene::geometry_streams::{
    GeometryStreamError, InstanceRecord, PositionEncoding, decode_indices, decode_instance_rows,
    decode_positions, rebased_component_bits,
};

#[test]
fn rebased_values_preserve_sign_and_extended_numeric_encodings() {
    for (encoded, expected) in [
        (0, 0),
        (0x8000, 0x80000000),
        (1, 0x38002000),
        (0x8001, 0xb8002000),
        (0x3c00, 0x3f800000),
        (0xbc00, 0xbf800000),
        (0x7c00, 0x47800000),
        (0xffff, 0xc7ffe000),
    ] {
        assert_eq!(rebased_component_bits(encoded), expected);
    }
}
#[test]
fn odd_instance_population_needs_no_padding_source_row() {
    let words = [0x3c00u16, 0, 0x8000, 0x4000, 0, 0, 0, 0x3c00];
    let mut source: Vec<u8> = words
        .into_iter()
        .flat_map(u16::to_be_bytes)
        .cycle()
        .take(3 * 16)
        .collect();
    let rows = decode_instance_rows(&source, 3, 96).unwrap();
    source.fill(0xff);
    assert_eq!(rows.len(), 3);
    assert!(rows.iter().all(|r| *r
        == InstanceRecord {
            position_scale: [0x3f800000, 0, 0x80000000, 0x40000000],
            quaternion: [0, 0, 0, 0x3f800000],
        }));
    assert_eq!(
        decode_instance_rows(&source[..47], 3, 96),
        Err(GeometryStreamError::SourceExtent)
    );
    assert_eq!(
        decode_instance_rows(&source, 3, 95),
        Err(GeometryStreamError::BudgetExceeded)
    );
}
#[test]
fn static_rebased_triples_have_logical_population_and_point_w() {
    let source = [0x3c, 0, 0x80, 0, 0, 1, 0xbc, 0, 0x7c, 0, 0, 0];
    let positions = decode_positions(&source, 2, PositionEncoding::Rebased16x3, 32).unwrap();
    assert_eq!(
        positions,
        [
            [0x3f800000, 0x80000000, 0x38002000, 0x3f800000],
            [0xbf800000, 0x47800000, 0, 0x3f800000]
        ]
    );
    assert_eq!(
        decode_positions(&source, 3, PositionEncoding::Rebased16x3, 48),
        Err(GeometryStreamError::SourceExtent)
    );
}
#[test]
fn float_positions_preserve_payload_bits_and_ignore_extra_source_bytes() {
    let mut source: Vec<u8> = [0x7fc12345u32, 0x80000000, 0xff800000]
        .into_iter()
        .flat_map(u32::to_be_bytes)
        .collect();
    source.extend_from_slice(&[0xff; 16]);
    assert_eq!(
        decode_positions(&source, 1, PositionEncoding::Float32x3, 16).unwrap(),
        [[0x7fc12345, 0x80000000, 0xff800000, 0x3f800000]]
    );
    assert_eq!(
        decode_positions(&source, 1, PositionEncoding::Float32x3, 15),
        Err(GeometryStreamError::BudgetExceeded)
    );
}
#[test]
fn logical_indices_preserve_order_duplicates_and_do_not_read_simd_tail() {
    assert_eq!(
        decode_indices(&[0, 7, 0, 1, 0, 7, 0xff, 0xff], 3, 6).unwrap(),
        [7, 1, 7]
    );
    assert_eq!(
        decode_indices(&[0, 7, 0, 1, 0, 7], 4, 8),
        Err(GeometryStreamError::SourceExtent)
    );
    assert_eq!(
        decode_indices(&[0, 7, 0, 1, 0, 7], 3, 5),
        Err(GeometryStreamError::BudgetExceeded)
    );
}
#[test]
fn empty_inputs_and_impossible_extents_never_read_or_allocate() {
    assert!(decode_instance_rows(&[], 0, 0).unwrap().is_empty());
    assert!(
        decode_positions(&[], 0, PositionEncoding::Rebased16x3, 0)
            .unwrap()
            .is_empty()
    );
    assert!(decode_indices(&[], 0, 0).unwrap().is_empty());
    assert_eq!(
        decode_instance_rows(&[], usize::MAX, usize::MAX),
        Err(GeometryStreamError::SourceExtent)
    );
}

fn owned_static_sources(
    encoding: u8,
    vertex_bytes: &[u8],
) -> mag_render_adapter_scene::partition_inputs::OwnedPartitionInputs {
    use mag_render_adapter_scene::{
        guest_memory::GuestSnapshot,
        mesh_sources::PartitionChain,
        partition_inputs::{NonInstancedSourceBases, copy_noninstanced_inputs},
    };
    let mut header = [0u8; 544];
    for (slot, size) in [(1, 6u32), (7, vertex_bytes.len() as u32), (19, 16)] {
        header[0x30 + slot * 8..0x34 + slot * 8].copy_from_slice(&size.to_be_bytes());
        header[0x34 + slot * 8..0x38 + slot * 8]
            .copy_from_slice(&(0x3000u32 + slot as u32 * 0x100).to_be_bytes());
    }
    let chain =
        PartitionChain::copy(&GuestSnapshot::new(0x1000, &header).unwrap(), 0x1000, 1).unwrap();
    let mut config = vec![0; 16];
    config[2] = encoding;
    config[8..10].copy_from_slice(&1u16.to_be_bytes());
    config[10..12].copy_from_slice(&3u16.to_be_bytes());
    copy_noninstanced_inputs(
        &chain.partitions()[0],
        NonInstancedSourceBases {
            palette_base: 0,
            extra_stream: 0,
            environment: 0,
        },
        1000,
        &mut |request| {
            Ok(match request.address {
                0x3100 => vec![0, 0, 0, 0, 0, 0],
                0x3700 => vertex_bytes.to_vec(),
                0x4300 => config.clone(),
                _ => panic!("unexpected source read"),
            })
        },
    )
    .unwrap()
}
#[test]
fn owned_raw_static_bundle_feeds_position_and_index_consumers() {
    use mag_render_adapter_scene::geometry_streams::decode_static_position_inputs;
    let source = owned_static_sources(6, &[0x3c, 0, 0, 0, 0, 0]);
    let result = decode_static_position_inputs(&source, 22).unwrap();
    drop(source);
    assert_eq!(result.positions, [[0x3f800000, 0, 0, 0x3f800000]]);
    assert_eq!(result.indices, [0, 0, 0]);
    assert_eq!(
        decode_static_position_inputs(&owned_static_sources(6, &[0; 6]), 21),
        Err(GeometryStreamError::BudgetExceeded)
    );
    assert_eq!(
        decode_static_position_inputs(&owned_static_sources(9, &[0; 6]), 22),
        Err(GeometryStreamError::UnsupportedPositionEncoding(9))
    );
    assert_eq!(
        decode_static_position_inputs(&owned_static_sources(0, &[0; 6]), 22),
        Err(GeometryStreamError::SourceExtent)
    );
}
#[test]
fn actual_group_zero_planner_row_mode_is_preserved_through_decode() {
    use mag_render_adapter_scene::{
        geometry_streams::decode_group_zero_instance_rows,
        guest_memory::GuestSnapshot,
        mesh_sources::{MeshSource, PartitionChain},
        partition_inputs::{copy_group_zero_inputs, plan_group_zero_inputs},
    };
    let mut header = [0u8; 544];
    header[0xfc..0x100].copy_from_slice(&7u32.to_be_bytes());
    let chain =
        PartitionChain::copy(&GuestSnapshot::new(0x1000, &header).unwrap(), 0x1000, 1).unwrap();
    let raw_mesh = [0; 192];
    let mesh = MeshSource::copy(&GuestSnapshot::new(0x2000, &raw_mesh).unwrap(), 0x2000).unwrap();
    let mut transform = [0; 112];
    transform[0x60..0x64].copy_from_slice(&0x80000300u32.to_be_bytes());
    for count in [0, 3] {
        transform[0x6c] = count;
        let plan = plan_group_zero_inputs(
            &chain.partitions()[0],
            &mesh,
            &transform,
            0x4000,
            0,
            0,
            1000,
        )
        .unwrap();
        let bundle = copy_group_zero_inputs(plan, &mut |request| {
            assert_eq!(request.address, 0x4300);
            Ok(vec![0; request.bytes])
        })
        .unwrap();
        let result = decode_group_zero_instance_rows(&bundle, 96).unwrap();
        if count == 0 {
            assert_eq!(result, None);
        } else {
            assert_eq!(result.unwrap().len(), 3);
        }
    }
}
