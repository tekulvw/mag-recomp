//! Authored synthetic mesh and partition records.
use mag_render_adapter_core::guest_memory::GuestSnapshot;
use mag_render_adapter_core::mesh_sources::{
    GeometrySource, MeshSource, MeshSourceError, PartitionChain,
};
fn word(bytes: &mut [u8], at: usize, value: u32) {
    bytes[at..at + 4].copy_from_slice(&value.to_be_bytes());
}
fn mesh() -> Vec<u8> {
    let mut bytes = vec![0; 192];
    word(&mut bytes, 0x3c, 0x7000);
    word(&mut bytes, 0x50, 0x8000_0200);
    bytes[0xa2] = 2;
    bytes[0xbc..0xbe].copy_from_slice(&11u16.to_be_bytes());
    bytes
}
#[test]
fn static_geometry_precedes_skinning_and_uses_raw_partition_asset() {
    let mut bytes = mesh();
    word(&mut bytes, 0x9c, 0x1000);
    let mesh = MeshSource::copy(&GuestSnapshot::new(0x1000, &bytes).unwrap(), 0x1000).unwrap();
    assert_eq!(mesh.vertex_layout_address(), 0x7000);
    assert_eq!(mesh.material_group(), 11);
    assert_eq!(
        mesh.depth_geometry_source(0x20, &[0; 32], 0x4000),
        GeometrySource::StaticPartitions {
            first: 0x4200,
            count: 2
        }
    );
}
#[test]
fn skinning_is_explicit_and_direct_draw_is_not_discarded() {
    let bytes = mesh();
    let mesh = MeshSource::copy(&GuestSnapshot::new(0x1000, &bytes).unwrap(), 0x1000).unwrap();
    let mut item = [0; 32];
    word(&mut item, 20, 0x1237);
    assert_eq!(
        mesh.depth_geometry_source(0x20, &item, 0x4000),
        GeometrySource::SkinnedPartitions {
            first: 0x4200,
            count: 2,
            palette_address: 0x1230
        }
    );
    item[27] = 1;
    assert_eq!(
        mesh.depth_geometry_source(0x20, &item, 0x4000),
        GeometrySource::SkinnedPartitions {
            first: 0x4200,
            count: 2,
            palette_address: 0
        }
    );
    assert_eq!(
        mesh.depth_geometry_source(0, &item, 0x4000),
        GeometrySource::DirectDraw
    );
}
#[test]
fn raw_partition_chain_is_owned_and_preserves_asset_order() {
    let mut bytes = vec![0; 1088];
    word(&mut bytes, 0xf0, 0x2220);
    bytes[544 + 4] = 19;
    let chain =
        PartitionChain::copy(&GuestSnapshot::new(0x2000, &bytes).unwrap(), 0x2000, 2).unwrap();
    bytes.fill(0);
    assert_eq!(chain.partitions().len(), 2);
    assert_eq!(chain.partitions()[0].source_address, 0x2000);
    assert_eq!(chain.partitions()[1].source_address, 0x2220);
    assert_eq!(chain.partitions()[1].bytes()[4], 19);
}
#[test]
fn chain_cycles_fail_but_actual_population_uses_native_budget() {
    let mut bytes = vec![0; 1088];
    word(&mut bytes, 0xf0, 0x2000);
    assert!(matches!(
        PartitionChain::copy(&GuestSnapshot::new(0x2000, &bytes).unwrap(), 0x2000, 2),
        Err(MeshSourceError::Cycle)
    ));
    word(&mut bytes, 0xf0, 0);
    assert_eq!(
        PartitionChain::copy(&GuestSnapshot::new(0x2000, &bytes).unwrap(), 0x2000, 2)
            .unwrap()
            .partitions()
            .len(),
        1
    );
    word(&mut bytes, 0xf0, 0x2220);
    assert!(matches!(
        PartitionChain::copy(&GuestSnapshot::new(0x2000, &bytes).unwrap(), 0x2000, 1),
        Err(MeshSourceError::BudgetExceeded)
    ));
}
#[test]
fn empty_partition_population_is_explicit_and_ranges_stay_checked() {
    let empty = GuestSnapshot::new(0, &[]).unwrap();
    assert!(
        PartitionChain::copy(&empty, 0, 0)
            .unwrap()
            .partitions()
            .is_empty()
    );
    assert!(
        PartitionChain::copy(&empty, 0, 9)
            .unwrap()
            .partitions()
            .is_empty()
    );
    assert!(matches!(
        PartitionChain::copy(&empty, 1, 0),
        Err(MeshSourceError::BudgetExceeded)
    ));
    assert!(matches!(
        PartitionChain::copy(&empty, 0xffff_fff0, 1),
        Err(MeshSourceError::Snapshot(_))
    ));
    assert!(matches!(
        MeshSource::copy(&empty, 0),
        Err(MeshSourceError::NullMesh)
    ));
}

#[test]
fn main_group_zero_uses_its_own_partition_dispatch_without_depth_flags() {
    use mag_render_adapter_core::mesh_sources::MainGeometrySource;
    let mut bytes = mesh();
    bytes[0xbc..0xbe].copy_from_slice(&0u16.to_be_bytes());
    word(&mut bytes, 0x9c, 0x100);
    let mesh = MeshSource::copy(&GuestSnapshot::new(0x1000, &bytes).unwrap(), 0x1000).unwrap();
    // Synthetic material flags with every bit set except the skinning bit
    // (0x20): Depth draws directly, while Main's group zero still wins.
    assert_eq!(
        mesh.depth_geometry_source(0xffff_ffdf, &[0; 32], 0x4000),
        GeometrySource::DirectDraw
    );
    assert_eq!(
        mesh.main_geometry_source(0xffff_ffdf, &[0; 32], 0, 0x4000),
        MainGeometrySource::GroupZeroPartitions {
            first: 0x4200,
            recorded_count: 2
        }
    );
}

#[test]
fn main_force_update_byte_selects_instanced_direct_instead_of_skinned_geometry() {
    use mag_render_adapter_core::mesh_sources::MainGeometrySource;
    let bytes = mesh();
    let mesh = MeshSource::copy(&GuestSnapshot::new(0x1000, &bytes).unwrap(), 0x1000).unwrap();
    let mut item = [0; 32];
    word(&mut item, 20, 0x1237);
    assert_eq!(
        mesh.main_geometry_source(0x20, &item, 1, 0x4000),
        MainGeometrySource::DirectDraw { instanced: true }
    );
    assert_eq!(
        mesh.main_geometry_source(0x20, &item, 2, 0x4000),
        MainGeometrySource::SkinnedPartitions {
            first: 0x4200,
            recorded_count: 2,
            palette_address: 0x1230
        }
    );
    assert_eq!(
        mesh.main_geometry_source(0, &item, 0, 0x4000),
        MainGeometrySource::DirectDraw { instanced: false }
    );
}

#[test]
fn main_special_mesh_precedes_forced_instanced_direct_draw() {
    use mag_render_adapter_core::mesh_sources::MainGeometrySource;
    let mut bytes = mesh();
    word(&mut bytes, 0x9c, 0x1000);
    let mesh = MeshSource::copy(&GuestSnapshot::new(0x1000, &bytes).unwrap(), 0x1000).unwrap();
    assert_eq!(
        mesh.main_geometry_source(0x20, &[0; 32], 1, 0x4000),
        MainGeometrySource::StaticPartitions {
            first: 0x4200,
            recorded_count: 2
        }
    );
}

#[test]
fn sparse_synchronous_partition_reads_own_the_complete_chain_and_check_before_read() {
    use mag_render_adapter_core::error::Error;
    use mag_render_adapter_core::mesh_sources::{MeshSourceError, PartitionChain};
    let mut calls = Vec::new();
    let mut reader = |address| {
        calls.push(address);
        let mut header = vec![0; 544];
        if address == 0x10000 {
            header[0xf0..0xf4].copy_from_slice(&0x40000u32.to_be_bytes());
        }
        Ok(header)
    };
    let copied = PartitionChain::copy_with_reader(0x10000, 2, &mut reader).unwrap();
    assert_eq!(calls, [0x10000, 0x40000]);
    assert_eq!(copied.partitions().len(), 2);
    calls.clear();
    assert!(matches!(
        PartitionChain::copy_with_reader(0xffff_ff00, 2, &mut |address| {
            calls.push(address);
            Ok(vec![0; 544])
        }),
        Err(MeshSourceError::Snapshot(Error::InvalidRange))
    ));
    assert!(calls.is_empty());
    assert!(matches!(
        PartitionChain::copy_with_reader(0x10000, 1, &mut |_| Ok(vec![0; 543])),
        Err(MeshSourceError::ReadExtent {
            expected: 544,
            actual: 543
        })
    ));
}
