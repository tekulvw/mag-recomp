use mag_render_adapter_scene::cohort::{CohortBuilder, CohortError, CohortHeader};
fn header(counts: [[u32; 16]; 4]) -> CohortHeader {
    CohortHeader {
        vm: 1,
        source_invocation: 7,
        ppu_owner: 3,
        pass_occurrence: 5,
        execution_update: 77,
        camera: 0,
        group_counts: counts,
    }
}
fn item(group: u8, value: u8) -> [u8; 32] {
    let mut b = [0; 32];
    b[0] = group << 4;
    b[4] = value;
    b
}
#[test]
fn owns_and_merges_all_shards_with_duplicates_and_standing_ties() {
    let mut counts = [[0; 16]; 4];
    counts[0][1] = 2;
    counts[1][1] = 2;
    let mut build = CohortBuilder::new(header(counts), 4, 2048).unwrap();
    let mut transform = [0x31; 112];
    let mut a = item(1, 1);
    build.push(0, 8, a, Some(transform)).unwrap();
    a.fill(0);
    transform.fill(0);
    build.push(0, 8, item(1, 3), None).unwrap();
    build.push(1, 8, item(1, 1), None).unwrap();
    build.push(1, 9, item(1, 2), None).unwrap();
    let cohort = build.finish().unwrap();
    assert_eq!(
        cohort.items().iter().map(|i| i.mbi[4]).collect::<Vec<_>>(),
        [1, 1, 2, 3]
    );
    assert_eq!(
        cohort.items().iter().map(|i| i.shard).collect::<Vec<_>>(),
        [0, 1, 1, 0]
    );
    assert_eq!(cohort.items()[0].transform, Some([0x31; 112]));
    assert_eq!(cohort.items()[1].transform, None);
}
#[test]
fn absent_transform_is_an_explicit_item_not_a_dropped_population() {
    let mut c = [[0; 16]; 4];
    c[2][15] = 1;
    let mut b = CohortBuilder::new(header(c), 1, 1024).unwrap();
    b.push(2, 3, item(15, 8), None).unwrap();
    let out = b.finish().unwrap();
    assert_eq!(out.items().len(), 1);
    assert_eq!(out.items()[0].mbi[0] >> 4, 15);
    assert!(out.items()[0].transform.is_none());
}
#[test]
fn incomplete_or_mismatched_group_population_never_publishes() {
    let mut c = [[0; 16]; 4];
    c[0][1] = 1;
    let b = CohortBuilder::new(header(c), 1, 1024).unwrap();
    assert!(matches!(b.finish(), Err(CohortError::Incomplete)));
    let mut b = CohortBuilder::new(header(c), 1, 1024).unwrap();
    assert_eq!(
        b.push(0, 1, item(2, 0), None),
        Err(CohortError::GroupPopulation)
    );
}
#[test]
fn source_order_errors_decline_instead_of_sorting_corrupt_shards() {
    let mut c = [[0; 16]; 4];
    c[0][1] = 2;
    let mut b = CohortBuilder::new(header(c), 2, 1024).unwrap();
    b.push(0, 0, item(1, 2), None).unwrap();
    assert_eq!(
        b.push(0, 1, item(1, 1), None),
        Err(CohortError::UnsortedShard)
    );
    assert!(matches!(b.finish(), Err(CohortError::Invalidated)));
}
#[test]
fn native_capacity_is_checked_before_accepting_items() {
    let mut c = [[0; 16]; 4];
    c[0][1] = 2;
    assert!(matches!(
        CohortBuilder::new(header(c), 1, 1024),
        Err(CohortError::BudgetExceeded)
    ));
    assert!(matches!(
        CohortBuilder::new(header(c), 2, 1),
        Err(CohortError::BudgetExceeded)
    ));
    let mut h = header(c);
    h.vm = 0;
    assert!(matches!(
        CohortBuilder::new(h, 2, 1024),
        Err(CohortError::InvalidIdentity)
    ));
}
#[test]
fn a_completed_empty_cohort_has_actual_source_identity() {
    let out = CohortBuilder::new(header([[0; 16]; 4]), 0, 0)
        .unwrap()
        .finish()
        .unwrap();
    assert!(out.items().is_empty());
    assert_eq!(out.header().source_invocation, 7);
    assert_eq!(out.header().pass_occurrence, 5);
}
#[test]
fn ffi_transaction_failure_vm_reuse_and_decline_revoke_stale_success() {
    // The adapter entry points do nothing while the master switch is off.
    mag_render_adapter_core::runtime::enable();
    use mag_render_adapter_scene::cohort_ffi::*;
    fn begin(invocation: u64, count: u32) -> CohortBegin {
        let mut b = CohortBegin {
            abi_version: 1,
            struct_bytes: size_of::<CohortBegin>() as u32,
            vm: 71,
            invocation,
            owner: 9,
            pass_occurrence: 31,
            ..CohortBegin::default()
        };
        b.group_counts[0][1] = count;
        b
    }
    assert_eq!(size_of::<CohortBegin>(), 312);
    assert_eq!(size_of::<CohortItemInput>(), 176);
    assert_eq!(mag_render_adapter_cohort_vm(71), 0);
    assert_eq!(mag_render_adapter_cohort_begin(begin(1, 1)), 0);
    assert_ne!(mag_render_adapter_cohort_finish(71, 1), 0);
    assert!(take_latest_cohort().is_none());
    assert_eq!(mag_render_adapter_cohort_begin(begin(2, 1)), 0);
    let mut item = CohortItemInput {
        vm: 71,
        invocation: 2,
        source_index: 17,
        ..CohortItemInput::default()
    };
    item.mbi[0] = 0x1000_0000;
    assert_eq!(mag_render_adapter_cohort_item(item), 0);
    assert_eq!(mag_render_adapter_cohort_finish(71, 2), 0);
    let owned = take_latest_cohort().unwrap();
    assert_eq!(owned.items()[0].source_index, 17);
    assert_eq!(owned.items()[0].mbi[0], 0x10);
    assert!(take_latest_cohort().is_none());
    assert_eq!(mag_render_adapter_cohort_begin(begin(3, 0)), 0);
    assert_eq!(mag_render_adapter_cohort_finish(71, 3), 0);
    assert_eq!(mag_render_adapter_cohort_decline(71), 0);
    assert!(take_latest_cohort().is_none());
    assert_eq!(mag_render_adapter_cohort_begin(begin(4, 1)), 0);
    item.invocation = 4;
    item.reserved = 1;
    assert_ne!(mag_render_adapter_cohort_item(item), 0);
    assert_ne!(mag_render_adapter_cohort_finish(71, 4), 0);
    assert!(take_latest_cohort().is_none());
    assert_ne!(mag_render_adapter_cohort_begin(begin(4, 0)), 0);
    assert_eq!(mag_render_adapter_cohort_begin(begin(5, 0)), 0);
    assert_eq!(mag_render_adapter_cohort_finish(71, 5), 0);
    assert_eq!(mag_render_adapter_cohort_vm(72), 0);
    assert!(take_latest_cohort().is_none());
    assert_ne!(mag_render_adapter_cohort_begin(begin(6, 0)), 0);
    // Previously obtained owned bytes remain independent after VM retirement.
    assert_eq!(owned.header().vm, 71);
    assert_eq!(owned.items()[0].source_index, 17);
}
