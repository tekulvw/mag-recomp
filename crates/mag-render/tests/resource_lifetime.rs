use mag_render::{resources::*, types::ResourceVersion};
fn store(resources: usize, bytes: usize) -> ResourceStore {
    ResourceStore::new(ResourceLimits {
        max_resources: resources,
        max_bytes: bytes,
    })
    .unwrap()
}
fn create(s: &mut ResourceStore, bytes: &[u8]) -> ResourceVersion {
    s.apply(ResourceDelta::Create {
        bytes: bytes.to_vec(),
    })
    .unwrap()
}
fn patch(
    s: &mut ResourceStore,
    v: ResourceVersion,
    offset: u64,
    bytes: &[u8],
) -> Result<ResourceVersion, ResourceError> {
    s.apply(ResourceDelta::Update {
        expected: v,
        ranges: vec![DirtyRange {
            offset,
            bytes: bytes.to_vec(),
        }],
    })
}
#[test]
fn mutation_does_not_change_inflight_frame() {
    let mut s = store(4, 32);
    let first = create(&mut s, &[1, 2, 3, 4]);
    let inflight = s.lease(first).unwrap();
    let second = patch(&mut s, first, 1, &[8, 9]).unwrap();
    assert_eq!(second.id, first.id);
    assert_eq!(second.content_version, 2);
    assert_eq!(inflight.bytes(), [1, 2, 3, 4]);
    let now = s.lease(second).unwrap();
    assert_eq!(now.bytes(), [1, 8, 9, 4]);
    assert_eq!(
        now.dirty_ranges(),
        [DirtySpan {
            offset: 1,
            length: 2
        }]
    );
    assert_eq!(s.stats().resident_bytes, 8);
    assert_eq!(s.stats().resident_resources, 2);
    assert_eq!(s.stats().bytes_copied, 4);
}
#[test]
fn address_reuse_changes_generation_after_retirement_barrier() {
    let mut s = store(1, 8);
    let first = create(&mut s, &[1, 2]);
    let held = s.lease(first).unwrap();
    s.retire(first).unwrap();
    assert_eq!(
        s.apply(ResourceDelta::Create { bytes: vec![3, 4] }),
        Err(ResourceError::CapacityExceeded)
    );
    assert_eq!(held.bytes(), [1, 2]);
    drop(held);
    let reused = create(&mut s, &[3, 4]);
    assert_eq!(reused.id.slot, first.id.slot);
    assert_eq!(reused.id.generation, first.id.generation + 1);
    assert_eq!(reused.content_version, 1);
    assert_eq!(
        patch(&mut s, first, 0, &[0]),
        Err(ResourceError::StaleVersion)
    );
    assert_eq!(s.retire(first), Err(ResourceError::StaleVersion));
}
#[test]
fn unchanged_resource_not_reuploaded() {
    let mut s = store(1, 4);
    let first = create(&mut s, &[1, 2, 3, 4]);
    let held = s.lease(first).unwrap();
    let before = s.stats();
    assert_eq!(patch(&mut s, first, 1, &[2, 3]).unwrap(), first);
    assert_eq!(
        s.apply(ResourceDelta::Update {
            expected: first,
            ranges: vec![]
        })
        .unwrap(),
        first
    );
    assert_eq!(s.stats(), before);
    assert_eq!(held.bytes(), [1, 2, 3, 4]);
}
#[test]
fn guest_range_overflow_rejected() {
    let mut s = store(3, 16);
    let first = create(&mut s, &[1, 2, 3, 4]);
    let before = s.stats();
    for (offset, bytes) in [
        (u64::MAX, vec![9]),
        (4, vec![9]),
        (3, vec![9, 9]),
        (0, vec![]),
    ] {
        assert_eq!(
            s.apply(ResourceDelta::Update {
                expected: first,
                ranges: vec![DirtyRange { offset, bytes }]
            }),
            Err(ResourceError::InvalidRange)
        );
        assert_eq!(s.lease(first).unwrap().bytes(), [1, 2, 3, 4]);
        assert_eq!(s.stats(), before);
    }
}
#[test]
fn stale_versions_and_duplicate_retire_are_rejected() {
    let mut s = store(2, 16);
    let first = create(&mut s, &[1, 2]);
    let next = patch(&mut s, first, 0, &[4]).unwrap();
    assert_eq!(
        patch(&mut s, first, 0, &[9]),
        Err(ResourceError::StaleVersion)
    );
    assert_eq!(s.retire(first), Err(ResourceError::StaleVersion));
    s.retire(next).unwrap();
    assert_eq!(s.retire(next), Err(ResourceError::StaleVersion));
    assert!(s.lease(next).is_err());
}
#[test]
fn capacity_declines_before_effects() {
    let mut s = store(2, 7);
    let first = create(&mut s, &[1, 2, 3, 4]);
    let held = s.lease(first).unwrap();
    let before = s.stats();
    assert_eq!(
        patch(&mut s, first, 0, &[7]),
        Err(ResourceError::CapacityExceeded)
    );
    assert_eq!(s.stats(), before);
    assert_eq!(s.lease(first).unwrap().bytes(), [1, 2, 3, 4]);
    drop(held);
    let next = patch(&mut s, first, 0, &[7]).unwrap();
    assert_eq!(s.lease(next).unwrap().bytes(), [7, 2, 3, 4]);
    assert_eq!(s.stats().resident_bytes, 4);
}
#[test]
fn capacity_includes_retained_versions_and_delayed_release() {
    let mut s = store(2, 12);
    let first = create(&mut s, &[1, 2, 3, 4]);
    let old = s.lease(first).unwrap();
    let old_copy = old.clone();
    let next = patch(&mut s, first, 0, &[8]).unwrap();
    let current = s.lease(next).unwrap();
    assert_eq!(
        patch(&mut s, next, 0, &[9]),
        Err(ResourceError::CapacityExceeded)
    );
    drop(old);
    assert_eq!(s.stats().resident_resources, 2);
    assert_eq!(
        patch(&mut s, next, 0, &[9]),
        Err(ResourceError::CapacityExceeded)
    );
    drop(old_copy);
    assert_eq!(s.stats().resident_resources, 1);
    let third = patch(&mut s, next, 0, &[9]).unwrap();
    assert_eq!(current.bytes(), [8, 2, 3, 4]);
    assert_eq!(s.lease(third).unwrap().bytes(), [9, 2, 3, 4]);
}
#[test]
fn all_ranges_validate_before_any_write() {
    let mut s = store(2, 8);
    let first = create(&mut s, &[1, 2, 3, 4]);
    let before = s.stats();
    assert_eq!(
        s.apply(ResourceDelta::Update {
            expected: first,
            ranges: vec![
                DirtyRange {
                    offset: 0,
                    bytes: vec![9]
                },
                DirtyRange {
                    offset: 4,
                    bytes: vec![9]
                }
            ]
        }),
        Err(ResourceError::InvalidRange)
    );
    assert_eq!(s.lease(first).unwrap().bytes(), [1, 2, 3, 4]);
    assert_eq!(s.stats(), before);
    assert_eq!(
        s.apply(ResourceDelta::Update {
            expected: first,
            ranges: vec![
                DirtyRange {
                    offset: 1,
                    bytes: vec![7, 8]
                },
                DirtyRange {
                    offset: 2,
                    bytes: vec![9]
                }
            ]
        }),
        Err(ResourceError::OverlappingRanges)
    );
    assert_eq!(s.lease(first).unwrap().bytes(), [1, 2, 3, 4]);
    assert_eq!(s.stats(), before);
}
#[test]
fn lease_owns_bytes_after_store_drop() {
    let lease = {
        let mut s = store(1, 4);
        let v = create(&mut s, &[1, 2, 3, 4]);
        s.lease(v).unwrap()
    };
    assert_eq!(lease.bytes(), [1, 2, 3, 4]);
}
#[test]
fn unleased_update_reuses_storage_and_only_counts_changed_ranges() {
    let mut s = store(1, 4);
    let first = create(&mut s, &[1, 2, 3, 4]);
    let before = s.stats();
    let next = s
        .apply(ResourceDelta::Update {
            expected: first,
            ranges: vec![
                DirtyRange {
                    offset: 3,
                    bytes: vec![9],
                },
                DirtyRange {
                    offset: 0,
                    bytes: vec![1],
                },
            ],
        })
        .unwrap();
    let lease = s.lease(next).unwrap();
    assert_eq!(lease.bytes(), [1, 2, 3, 9]);
    assert_eq!(
        lease.dirty_ranges(),
        [DirtySpan {
            offset: 3,
            length: 1
        }]
    );
    assert_eq!(s.stats().snapshot_allocations, before.snapshot_allocations);
    assert_eq!(s.stats().bytes_copied, 0);
    assert_eq!(s.stats().patch_bytes_written, 1);
}
#[test]
fn invalid_limits_and_empty_resources_decline() {
    assert!(matches!(
        ResourceStore::new(ResourceLimits {
            max_resources: 0,
            max_bytes: 4
        }),
        Err(ResourceError::InvalidLimits)
    ));
    assert!(matches!(
        ResourceStore::new(ResourceLimits {
            max_resources: 1,
            max_bytes: 0
        }),
        Err(ResourceError::InvalidLimits)
    ));
    let mut s = store(1, 4);
    assert_eq!(
        s.apply(ResourceDelta::Create { bytes: vec![] }),
        Err(ResourceError::EmptyResource)
    );
    assert_eq!(s.stats().resident_bytes, 0);
}

#[test]
fn old_content_lease_blocks_slot_reuse_after_latest_version_retired() {
    let mut s = store(2, 8);
    let first = create(&mut s, &[1, 2]);
    let old = s.lease(first).unwrap();
    let latest = patch(&mut s, first, 0, &[3]).unwrap();
    s.retire(latest).unwrap();
    assert_eq!(s.stats().resident_bytes, 2);
    let other = create(&mut s, &[4, 5]);
    assert_ne!(other.id.slot, first.id.slot);
    drop(old);
    let reused = create(&mut s, &[6, 7]);
    assert_eq!(reused.id.slot, first.id.slot);
    assert_eq!(reused.id.generation, 2);
}
#[test]
fn failed_creation_does_not_consume_generation_or_change_accounting() {
    let mut s = store(1, 4);
    let before = s.stats();
    assert_eq!(
        s.apply(ResourceDelta::Create { bytes: vec![1; 5] }),
        Err(ResourceError::CapacityExceeded)
    );
    assert_eq!(s.stats(), before);
    let first = create(&mut s, &[1, 2]);
    assert_eq!(first.id.generation, 1);
    s.retire(first).unwrap();
    let before = s.stats();
    assert_eq!(
        s.apply(ResourceDelta::Create { bytes: vec![1; 5] }),
        Err(ResourceError::CapacityExceeded)
    );
    assert_eq!(s.stats(), before);
    let next = create(&mut s, &[3, 4]);
    assert_eq!(next.id.generation, 2);
}

#[test]
fn cache_namespace_survives_store_drop_and_cannot_alias_later_store() {
    let old = {
        let mut s = store(1, 4);
        let v = create(&mut s, &[1, 2]);
        s.lease(v).unwrap()
    };
    let mut next = store(1, 4);
    let v = create(&mut next, &[9, 8]);
    let new = next.lease(v).unwrap();
    assert_eq!(old.version(), new.version());
    assert_ne!(old.cache_key(), new.cache_key());
    assert_eq!(old.bytes(), [1, 2]);
    assert_eq!(new.bytes(), [9, 8]);
}
#[test]
fn cache_namespace_is_stable_across_updates_and_slot_generations() {
    let mut s = store(1, 4);
    let first = create(&mut s, &[1, 2]);
    let key = s.lease(first).unwrap().cache_key();
    assert_eq!(key, s.lease(first).unwrap().cache_key());
    let next = patch(&mut s, first, 0, &[9]).unwrap();
    let changed = s.lease(next).unwrap().cache_key();
    assert_eq!(key.namespace, changed.namespace);
    assert_eq!(key.resource_id, changed.resource_id);
    assert_ne!(key.content_version, changed.content_version);
    s.retire(next).unwrap();
    let reused = create(&mut s, &[7, 8]);
    let replaced = s.lease(reused).unwrap().cache_key();
    assert_eq!(key.namespace, replaced.namespace);
    assert_eq!(key.resource_id.slot, replaced.resource_id.slot);
    assert_ne!(key.resource_id.generation, replaced.resource_id.generation);
}
#[test]
fn shared_bytes_are_adopted_without_copy_and_never_written_in_place() {
    use std::sync::{Arc, Weak};
    let source: Arc<Vec<u8>> = Arc::new(vec![1, 2, 3, 4]);
    let weak: Weak<Vec<u8>> = Arc::downgrade(&source);
    let mut s = store(4, 64);
    let v = s
        .apply(ResourceDelta::CreateShared {
            bytes: SharedBytes::new(source.clone()),
        })
        .unwrap();
    let lease = s.lease(v).unwrap();
    // The snapshot is the shared allocation itself.
    assert_eq!(lease.bytes().as_ptr(), source.as_ptr());
    assert_eq!(s.stats().bytes_copied, 0);
    assert_eq!(s.stats().resident_bytes, 4);
    // An update copies (copy on write); the shared bytes stay unchanged.
    drop(lease);
    let v2 = patch(&mut s, v, 1, &[9]).unwrap();
    assert_eq!(s.stats().bytes_copied, 4);
    assert_eq!(&source[..], &[1, 2, 3, 4]);
    assert_eq!(s.lease(v2).unwrap().bytes(), &[1, 9, 3, 4]);
    // Retiring the store's version drops its reference to the shared bytes.
    s.retire(v2).unwrap();
    drop(source);
    assert!(weak.upgrade().is_none());
}
