use mag_render_adapter_core::{
    error::Error,
    guest_memory::GuestSnapshot,
    source::{RetailAdapter, SHADOW_SOURCE_BYTES, ShadowSourceAddress},
};

const ADDRESS: ShadowSourceAddress = ShadowSourceAddress {
    manager: 0x1000,
    view: 0x3000,
};
fn fixture() -> Vec<u8> {
    let mut bytes = vec![0xa5; 0x5000];
    for (address, values) in [
        (0x2504, vec![1, 2, 3]),
        (0x3010, (4..20).collect()),
        (0x3060, vec![20]),
        (0x3a24, vec![21]),
    ] {
        for (index, word) in values.iter().enumerate() {
            bytes[address + index * 4..address + index * 4 + 4]
                .copy_from_slice(&u32::to_be_bytes(*word));
        }
    }
    bytes
}
#[test]
fn reads_complete_source_proven_helper_subset_in_guest_order() {
    let bytes = fixture();
    let mut adapter = RetailAdapter::new(2).unwrap();
    let result = adapter
        .publish_shadow_snapshot(&GuestSnapshot::new(0, &bytes).unwrap(), ADDRESS)
        .unwrap();
    assert_eq!(result.sources.parameters, [1, 2, 3]);
    assert_eq!(
        result.sources.pose_rows,
        [
            [4, 5, 6, 7],
            [8, 9, 10, 11],
            [12, 13, 14, 15],
            [16, 17, 18, 19]
        ]
    );
    assert_eq!(result.sources.view_scalar_60, 20);
    assert_eq!(result.sources.view_scalar_a24, 21);
    assert_eq!(adapter.stats().copied_bytes, SHADOW_SOURCE_BYTES as u64);
}
#[test]
fn address_reuse_changes_snapshot_identity() {
    let mut bytes = fixture();
    let mut adapter = RetailAdapter::new(2).unwrap();
    let first = adapter
        .publish_shadow_snapshot(&GuestSnapshot::new(0, &bytes).unwrap(), ADDRESS)
        .unwrap();
    bytes[0x2504..0x2508].copy_from_slice(&999u32.to_be_bytes());
    let second = adapter
        .publish_shadow_snapshot(&GuestSnapshot::new(0, &bytes).unwrap(), ADDRESS)
        .unwrap();
    assert_ne!(first.id, second.id);
    assert_eq!(first.sources.parameters[0], 1);
    assert_eq!(second.sources.parameters[0], 999);
}
#[test]
fn mutation_does_not_change_inflight_snapshot() {
    let mut bytes = fixture();
    let mut adapter = RetailAdapter::new(1).unwrap();
    let captured = adapter
        .publish_shadow_snapshot(&GuestSnapshot::new(0, &bytes).unwrap(), ADDRESS)
        .unwrap();
    bytes.fill(0);
    assert_eq!(captured.sources.parameters, [1, 2, 3]);
    assert_eq!(captured.sources.pose_rows[3], [16, 17, 18, 19]);
}
#[test]
fn staggered_writer_not_ready_for_complete_frame() {
    let bytes = fixture();
    let mut adapter = RetailAdapter::new(1).unwrap();
    let captured = adapter
        .publish_shadow_snapshot(&GuestSnapshot::new(0, &bytes).unwrap(), ADDRESS)
        .unwrap();
    assert_eq!(
        captured.require_complete_frame_input(),
        Err(Error::UnprovenProducerEpoch)
    );
}
#[test]
fn capacity_declines_before_effects() {
    let bytes = fixture();
    let memory = GuestSnapshot::new(0, &bytes).unwrap();
    let mut adapter = RetailAdapter::new(1).unwrap();
    let first = adapter.publish_shadow_snapshot(&memory, ADDRESS).unwrap();
    let before = adapter.stats();
    assert_eq!(
        adapter
            .publish_shadow_snapshot(&memory, ADDRESS)
            .unwrap_err(),
        Error::CapacityExceeded
    );
    assert_eq!(adapter.stats(), before);
    drop(first);
    let next = adapter.publish_shadow_snapshot(&memory, ADDRESS).unwrap();
    assert_eq!(next.id.serial, 2);
}
#[test]
fn guest_range_overflow_rejected() {
    let bytes = fixture();
    let memory = GuestSnapshot::new(0, &bytes).unwrap();
    let mut adapter = RetailAdapter::new(1).unwrap();
    assert_eq!(
        adapter
            .publish_shadow_snapshot(
                &memory,
                ShadowSourceAddress {
                    manager: 0xfffffff0,
                    view: 0x3000
                }
            )
            .unwrap_err(),
        Error::InvalidRange
    );
    assert_eq!(memory.words::<2>(u64::MAX), Err(Error::InvalidRange));
    assert_eq!(memory.words::<2>(0xffff_fffc), Err(Error::InvalidRange));
    assert_eq!(adapter.stats().copied_bytes, 0);
}
#[test]
fn incomplete_source_range_is_not_zero_filled() {
    let bytes = fixture();
    let memory = GuestSnapshot::new(0, &bytes[..0x3a27]).unwrap();
    let mut adapter = RetailAdapter::new(1).unwrap();
    assert_eq!(
        adapter
            .publish_shadow_snapshot(&memory, ADDRESS)
            .unwrap_err(),
        Error::OutsideSnapshot
    );
    assert_eq!(adapter.stats().snapshots, 0);
}
#[test]
fn reads_sparse_snapshot_with_nonzero_guest_base() {
    let words = [0x11223344u32, 0x3f800000];
    let bytes = words
        .into_iter()
        .flat_map(u32::to_be_bytes)
        .collect::<Vec<_>>();
    let memory = GuestSnapshot::new(0xfffffff8, &bytes).unwrap();
    assert_eq!(memory.words::<2>(0xfffffff8).unwrap(), words);
    assert_eq!(memory.words::<1>(0xfffffff4), Err(Error::OutsideSnapshot));
}
#[test]
fn scene_reset_keeps_owned_bytes_but_changes_new_identity() {
    let bytes = fixture();
    let memory = GuestSnapshot::new(0, &bytes).unwrap();
    let mut adapter = RetailAdapter::new(2).unwrap();
    let first = adapter.publish_shadow_snapshot(&memory, ADDRESS).unwrap();
    adapter.reset_scene().unwrap();
    let second = adapter.publish_shadow_snapshot(&memory, ADDRESS).unwrap();
    assert_ne!(first.id.scene, second.id.scene);
    assert_eq!(first.sources.parameters, [1, 2, 3]);
    assert_eq!(
        first.require_complete_frame_input(),
        Err(Error::UnprovenProducerEpoch)
    );
}
#[test]
fn invalid_capacity_rejected() {
    assert!(matches!(RetailAdapter::new(0), Err(Error::InvalidCapacity)));
}
