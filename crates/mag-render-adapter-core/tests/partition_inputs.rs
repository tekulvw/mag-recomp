use mag_render_adapter_core::partition_inputs::{
    NonInstancedSourceBases, PartitionInputError, copy_noninstanced_inputs,
    plan_noninstanced_inputs,
};
use mag_render_adapter_core::{
    error::Error, guest_memory::GuestSnapshot, mesh_sources::PartitionChain,
};
fn word(bytes: &mut [u8], at: usize, value: u32) {
    bytes[at..at + 4].copy_from_slice(&value.to_be_bytes());
}
fn header() -> Vec<u8> {
    let mut bytes = vec![0; 544];
    for slot in 0..24 {
        word(&mut bytes, 0x30 + 8 * slot, 4);
        word(&mut bytes, 0x34 + 8 * slot, 0x2000 + slot as u32 * 16);
    }
    bytes[0x14..0x16].copy_from_slice(&0x20u16.to_be_bytes());
    bytes[0x16..0x18].copy_from_slice(&0x50u16.to_be_bytes());
    for (at, value) in [(0xa, 6u16), (0xc, 8), (0xe, 10)] {
        bytes[at..at + 2].copy_from_slice(&value.to_be_bytes());
    }
    bytes
}
fn bases() -> NonInstancedSourceBases {
    NonInstancedSourceBases {
        palette_base: 0x3000,
        extra_stream: 0x3800,
        environment: 0x3900,
    }
}
#[test]
fn resolves_direct_asset_sources_and_real_semantic_overrides() {
    let bytes = header();
    let chain =
        PartitionChain::copy(&GuestSnapshot::new(0x1000, &bytes).unwrap(), 0x1000, 1).unwrap();
    let plan = plan_noninstanced_inputs(&chain.partitions()[0], bases(), 1000).unwrap();
    assert_eq!(plan.requests()[3].address, 0x3020);
    assert_eq!(plan.requests()[4].address, 0x3050);
    assert_eq!(
        [
            plan.requests()[10].bytes,
            plan.requests()[11].bytes,
            plan.requests()[12].bytes
        ],
        [6, 8, 10]
    );
    for slot in 13..17 {
        assert_eq!(plan.requests()[slot].bytes, 0);
        assert_eq!(plan.requests()[slot].address, 0x3800);
    }
    assert_eq!(plan.requests()[17].address, 0x3900);
    assert_eq!(plan.requests()[18].bytes, 0);
    assert_eq!(plan.requests()[18].address, 0);
    assert_eq!(plan.requests()[19].address, 0x2130);
    assert_eq!(plan.total_bytes(), 88);
}
#[test]
fn owns_all_requested_spans_without_reading_declared_empty_inputs() {
    let bytes = header();
    let chain =
        PartitionChain::copy(&GuestSnapshot::new(0x1000, &bytes).unwrap(), 0x1000, 1).unwrap();
    let mut memory = vec![0x6b; 0x2000];
    let mut calls = Vec::new();
    let bundle = {
        let snapshot = GuestSnapshot::new(0x2000, &memory).unwrap();
        copy_noninstanced_inputs(&chain.partitions()[0], bases(), 1000, &mut |request| {
            calls.push(request);
            snapshot.copy_bytes(u64::from(request.address), request.bytes)
        })
        .unwrap()
    };
    memory.fill(0);
    assert_eq!(calls.len(), 19);
    assert_eq!(bundle.inputs()[10].bytes(), &[0x6b; 6]);
    assert!(bundle.inputs()[13].bytes().is_empty());
    assert_eq!(bundle.inputs()[13].source_address(), 0x3800);
}
#[test]
fn budget_and_source_controls_refuse_before_any_reads() {
    let mut bytes = header();
    let mut reads = 0;
    {
        let chain =
            PartitionChain::copy(&GuestSnapshot::new(0x1000, &bytes).unwrap(), 0x1000, 1).unwrap();
        assert!(matches!(
            copy_noninstanced_inputs(&chain.partitions()[0], bases(), 87, &mut |_| {
                reads += 1;
                Ok(Vec::new())
            }),
            Err(PartitionInputError::BudgetExceeded)
        ));
    }
    word(&mut bytes, 0x30 + 6 * 8, 0x10004);
    let chain =
        PartitionChain::copy(&GuestSnapshot::new(0x1000, &bytes).unwrap(), 0x1000, 1).unwrap();
    assert!(matches!(
        copy_noninstanced_inputs(&chain.partitions()[0], bases(), 1000, &mut |_| {
            reads += 1;
            Ok(Vec::new())
        }),
        Err(PartitionInputError::UnsupportedSourceControl { slot: 6, .. })
    ));
    assert_eq!(reads, 0);
}
#[test]
fn overwritten_controls_do_not_create_false_source_dependencies() {
    let mut bytes = header();
    word(&mut bytes, 0x30 + 10 * 8, 0xffff_ffff);
    word(&mut bytes, 0x30 + 13 * 8, 0xffff_ffff);
    let chain =
        PartitionChain::copy(&GuestSnapshot::new(0x1000, &bytes).unwrap(), 0x1000, 1).unwrap();
    let plan = plan_noninstanced_inputs(&chain.partitions()[0], bases(), 1000).unwrap();
    assert_eq!(plan.requests()[10].bytes, 6);
    assert_eq!(plan.requests()[13].bytes, 0);
}
#[test]
fn unavailable_short_and_wrapping_inputs_never_publish_a_bundle() {
    let mut bytes = header();
    {
        let chain =
            PartitionChain::copy(&GuestSnapshot::new(0x1000, &bytes).unwrap(), 0x1000, 1).unwrap();
        assert!(matches!(
            copy_noninstanced_inputs(&chain.partitions()[0], bases(), 1000, &mut |_| Err(
                Error::OutsideSnapshot
            )),
            Err(PartitionInputError::Source { slot: 0, .. })
        ));
        assert!(matches!(
            copy_noninstanced_inputs(&chain.partitions()[0], bases(), 1000, &mut |_| Ok(vec![0])),
            Err(PartitionInputError::ReadExtent { slot: 0, .. })
        ));
    }
    word(&mut bytes, 0x34, 0xffff_fffe);
    let chain =
        PartitionChain::copy(&GuestSnapshot::new(0x1000, &bytes).unwrap(), 0x1000, 1).unwrap();
    assert!(matches!(
        plan_noninstanced_inputs(&chain.partitions()[0], bases(), 1000),
        Err(PartitionInputError::InvalidRange { slot: 0 })
    ));
}

fn group_zero_mesh() -> mag_render_adapter_core::mesh_sources::MeshSource {
    let bytes = [0u8; 192];
    mag_render_adapter_core::mesh_sources::MeshSource::copy(
        &GuestSnapshot::new(0x1000, &bytes).unwrap(),
        0x1000,
    )
    .unwrap()
}
fn instance_transform() -> [u8; 112] {
    let mut bytes = [0; 112];
    word(&mut bytes, 0x60, 0x8000_1000);
    word(&mut bytes, 0x64, 0x8000_0200);
    bytes[0x6c] = 5;
    bytes
}
#[test]
fn group_zero_derives_every_batch_source_from_raw_transform_and_asset() {
    use mag_render_adapter_core::partition_inputs::plan_group_zero_inputs;
    let mut bytes = header();
    word(&mut bytes, 0xfc, 3);
    let chain =
        PartitionChain::copy(&GuestSnapshot::new(0x1000, &bytes).unwrap(), 0x1000, 1).unwrap();
    let plan = plan_group_zero_inputs(
        &chain.partitions()[0],
        &group_zero_mesh(),
        &instance_transform(),
        0x8000,
        0x3900,
        1,
        1000,
    )
    .unwrap();
    assert_eq!((plan.first_instance, plan.instances), (3, 2));
    assert_eq!(
        (plan.index.location, plan.index.offset, plan.index.bytes),
        (1, 0x20c, 8)
    );
    assert_eq!(
        (
            plan.inputs.requests()[13].address,
            plan.inputs.requests()[13].bytes
        ),
        (0x9030, 32)
    );
    for slot in 14..17 {
        assert_eq!(
            (
                plan.inputs.requests()[slot].address,
                plan.inputs.requests()[slot].bytes
            ),
            (0x9050, 0)
        );
    }
    assert_eq!(plan.inputs.total_bytes(), 120);
}
#[test]
fn group_zero_zero_scale_has_one_instance_without_fabricated_rows() {
    use mag_render_adapter_core::partition_inputs::plan_group_zero_inputs;
    let bytes = header();
    let mut transform = instance_transform();
    transform[0x6c] = 0;
    let chain =
        PartitionChain::copy(&GuestSnapshot::new(0x1000, &bytes).unwrap(), 0x1000, 1).unwrap();
    let plan = plan_group_zero_inputs(
        &chain.partitions()[0],
        &group_zero_mesh(),
        &transform,
        0x8000,
        0x3900,
        0,
        1000,
    )
    .unwrap();
    assert_eq!((plan.first_instance, plan.instances), (0, 1));
    for slot in 13..17 {
        assert_eq!(
            (
                plan.inputs.requests()[slot].address,
                plan.inputs.requests()[slot].bytes
            ),
            (0x9000, 0)
        );
    }
}
#[test]
fn group_zero_rejects_unproven_location_wrong_batch_and_budget_before_reads() {
    use mag_render_adapter_core::partition_inputs::plan_group_zero_inputs;
    let mut bytes = header();
    word(&mut bytes, 0xfc, 3);
    let mut transform = instance_transform();
    let chain =
        PartitionChain::copy(&GuestSnapshot::new(0x1000, &bytes).unwrap(), 0x1000, 1).unwrap();
    assert!(matches!(
        plan_group_zero_inputs(
            &chain.partitions()[0],
            &group_zero_mesh(),
            &transform,
            0x8000,
            0x3900,
            2,
            1000
        ),
        Err(PartitionInputError::InvalidInstanceBatch)
    ));
    assert!(matches!(
        plan_group_zero_inputs(
            &chain.partitions()[0],
            &group_zero_mesh(),
            &transform,
            0x8000,
            0x3900,
            0,
            100
        ),
        Err(PartitionInputError::BudgetExceeded)
    ));
    word(&mut transform, 0x60, 0x1000);
    assert!(matches!(
        plan_group_zero_inputs(
            &chain.partitions()[0],
            &group_zero_mesh(),
            &transform,
            0x8000,
            0x3900,
            0,
            1000
        ),
        Err(PartitionInputError::UnresolvedInstanceLocation)
    ));
}
#[test]
fn group_zero_owns_the_resolved_instance_rows() {
    use mag_render_adapter_core::partition_inputs::copy_group_zero_inputs;
    let mut bytes = header();
    word(&mut bytes, 0xfc, 3);
    let chain =
        PartitionChain::copy(&GuestSnapshot::new(0x1000, &bytes).unwrap(), 0x1000, 1).unwrap();
    let mut memory = vec![0x5a; 0xa000];
    let owned = {
        let source = GuestSnapshot::new(0, &memory).unwrap();
        copy_group_zero_inputs(
            mag_render_adapter_core::partition_inputs::plan_group_zero_inputs(
                &chain.partitions()[0],
                &group_zero_mesh(),
                &instance_transform(),
                0x8000,
                0x3900,
                1,
                1000,
            )
            .unwrap(),
            &mut |r| source.copy_bytes(u64::from(r.address), r.bytes),
        )
        .unwrap()
    };
    memory.fill(0);
    assert_eq!(owned.inputs.inputs()[13].bytes(), &[0x5a; 32]);
    assert_eq!(owned.first_instance, 3);
}

#[test]
fn group_zero_preserves_empty_end_addresses_at_guest_boundary() {
    use mag_render_adapter_core::partition_inputs::plan_group_zero_inputs;
    let mut bytes = header();
    word(&mut bytes, 0xfc, 1);
    let mut transform = instance_transform();
    word(&mut transform, 0x60, 0x8000_0000);
    transform[0x6c] = 1;
    let chain =
        PartitionChain::copy(&GuestSnapshot::new(0x1000, &bytes).unwrap(), 0x1000, 1).unwrap();
    let plan = plan_group_zero_inputs(
        &chain.partitions()[0],
        &group_zero_mesh(),
        &transform,
        0xffff_fff0,
        0x3900,
        0,
        1000,
    )
    .unwrap();
    assert_eq!(
        (
            plan.inputs.requests()[13].address,
            plan.inputs.requests()[13].bytes
        ),
        (0xffff_fff0, 16)
    );
    assert_eq!(
        (
            plan.inputs.requests()[14].address,
            plan.inputs.requests()[14].bytes
        ),
        (0, 0)
    );
}
#[test]
fn group_zero_refuses_wrong_dispatch_and_actual_source_range_overflows() {
    use mag_render_adapter_core::partition_inputs::plan_group_zero_inputs;
    let mut bytes = header();
    word(&mut bytes, 0xfc, 2);
    let mut transform = instance_transform();
    transform[0x6c] = 2;
    let chain =
        PartitionChain::copy(&GuestSnapshot::new(0x1000, &bytes).unwrap(), 0x1000, 1).unwrap();
    let mut meshbytes = [0; 192];
    meshbytes[0xbc..0xbe].copy_from_slice(&1u16.to_be_bytes());
    let mesh = mag_render_adapter_core::mesh_sources::MeshSource::copy(
        &GuestSnapshot::new(0x1000, &meshbytes).unwrap(),
        0x1000,
    )
    .unwrap();
    assert!(matches!(
        plan_group_zero_inputs(
            &chain.partitions()[0],
            &mesh,
            &transform,
            0x8000,
            0x3900,
            0,
            1000
        ),
        Err(PartitionInputError::WrongDispatch)
    ));
    word(&mut transform, 0x60, 0x8000_0000);
    assert!(matches!(
        plan_group_zero_inputs(
            &chain.partitions()[0],
            &group_zero_mesh(),
            &transform,
            0xffff_fff0,
            0x3900,
            0,
            1000
        ),
        Err(PartitionInputError::InvalidRange { slot: 13 })
    ));
    word(&mut transform, 0x64, 0x7fff_fffc);
    assert!(matches!(
        plan_group_zero_inputs(
            &chain.partitions()[0],
            &group_zero_mesh(),
            &transform,
            0x8000,
            0x3900,
            0,
            1000
        ),
        Err(PartitionInputError::IndexMappingRange)
    ));
}
