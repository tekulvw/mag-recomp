use mag_render_adapter_scene::{
    cohort::{CohortBuilder, CohortHeader, OwnedVisibilityCohort},
    cohort_resources::*,
    error::Error,
};
fn word(bytes: &mut [u8], at: usize, value: u32) {
    bytes[at..at + 4].copy_from_slice(&value.to_be_bytes());
}
fn cohort(count: u32, transform: bool) -> OwnedVisibilityCohort {
    let mut counts = [[0; 16]; 4];
    counts[0][15] = count;
    let h = CohortHeader {
        vm: 3,
        source_invocation: 7,
        ppu_owner: 11,
        pass_occurrence: 9,
        execution_update: 27,
        camera: 0,
        group_counts: counts,
    };
    let mut b = CohortBuilder::new(h, count as usize, 8192).unwrap();
    let mut mbi = [0; 32];
    mbi[0] = 0xf0;
    mbi[7] = 1;
    word(&mut mbi, 12, 0x2000);
    word(&mut mbi, 20, 8);
    word(&mut mbi, 28, 8);
    for _ in 0..count {
        b.push(0, 17, mbi, transform.then_some([0; 112])).unwrap();
    }
    b.finish().unwrap()
}
fn read(address: u32) -> Result<Vec<u8>, Error> {
    match address {
        0x10000 => {
            let mut m = vec![0; 128];
            m[0x48..0x4a].copy_from_slice(&0x60u16.to_be_bytes());
            m[0x4a..0x4c].copy_from_slice(&0x50u16.to_be_bytes());
            word(&mut m, 0x38, 0x30000);
            word(&mut m, 0x54, 16);
            Ok(m)
        }
        0x20000 => Ok(vec![0; 192]),
        0x30000 => Ok(vec![0x31; 16]),
        0x40000 => Ok(vec![0; 544]),
        0x80000 => {
            let mut pass = vec![0; 688];
            word(&mut pass, 0x284, 0x40000);
            word(&mut pass, 0x1fc, 0x80000);
            Ok(pass)
        }
        _ => Err(Error::OutsideSnapshot),
    }
}
fn pass() -> MainPassResources {
    MainPassResources {
        render_mode: 0,
        technique_override_mask: 0,
        geometry_resource_base: 0x40000,
    }
}
fn budget() -> ResourceCopyBudget {
    ResourceCopyBudget {
        bytes: 4096,
        reads: 32,
        partition_headers: 8,
    }
}
#[test]
fn actual_cohort_order_and_duplicates_feed_owned_raw_material_mesh_and_partition_graph() {
    let c = cohort(2, true);
    let mut calls = Vec::new();
    let graph = copy_cohort_resources(&c, pass(), budget(), &mut |request| {
        calls.push(request);
        read(request.address)
    })
    .unwrap();
    assert_eq!(graph.items.len(), 2);
    assert_eq!(graph.copied_bytes, 1760);
    assert_eq!(graph.reads, 8);
    assert_eq!(
        calls.iter().map(|r| r.address).collect::<Vec<_>>(),
        [
            0x10000, 0x20000, 0x30000, 0x40000, 0x10000, 0x20000, 0x30000, 0x40000
        ]
    );
    assert_eq!(
        graph.items.iter().map(|i| i.ordinal).collect::<Vec<_>>(),
        [0, 1]
    );
    for item in graph.items {
        assert_eq!(item.primary_technique.as_ref().unwrap().bytes, [0x31; 16]);
        assert_eq!(item.partitions.as_ref().unwrap().partitions().len(), 1);
        assert!(
            item.obligations
                .contains(&ResourceObligation::NativeShaderAndDrawState)
        );
    }
    assert_eq!(graph.execution_update, 27);
    assert_eq!(graph.source_invocation, 7);
}
#[test]
fn actual_material_capture_moves_owned_bytes_and_shares_only_exact_content() {
    const CHILD: &str = "MAG_TEST_MATERIAL_OWNED_CHILD";
    let Some(mode) = std::env::var_os(CHILD) else {
        for mode in ["off", "on"] {
            let status = std::process::Command::new(std::env::current_exe().unwrap())
                .arg("--exact")
                .arg("actual_material_capture_moves_owned_bytes_and_shares_only_exact_content")
                .env(CHILD, mode)
                .env(
                    "PS3_RENDER_DECODED_GEOMETRY_CACHE",
                    if mode == "on" { "1" } else { "0" },
                )
                .status()
                .unwrap();
            assert!(status.success());
        }
        return;
    };
    let shared = mode == "on";
    let mut pointers = Vec::new();
    let mut calls = Vec::new();
    let graph = copy_cohort_resources(&cohort(2, true), pass(), budget(), &mut |request| {
        calls.push(request);
        let bytes = read(request.address)?;
        if request.address == 0x10000 {
            pointers.push(bytes.as_ptr() as usize);
        }
        Ok(bytes)
    })
    .unwrap();
    let materials: Vec<_> = graph
        .items
        .iter()
        .map(|item| item.primary_material.as_ref().unwrap())
        .collect();
    assert_eq!(graph.reads, 8);
    assert_eq!(graph.copied_bytes, 1760);
    assert_eq!(calls.len(), 8);
    if shared {
        assert_eq!(materials[0].bytes().as_ptr(), materials[1].bytes().as_ptr());
    } else {
        for (material, pointer) in materials.iter().zip(&pointers) {
            assert_eq!(material.bytes().as_ptr() as usize, *pointer);
        }
    }
    use mag_render_adapter_scene::material_sources::{MaterialSource, SourceError};
    assert!(matches!(
        MaterialSource::from_owned(0x10000, read(0x10000).unwrap(), 127),
        Err(SourceError::BudgetExceeded)
    ));
    assert!(matches!(
        MaterialSource::from_owned(u32::MAX - 63, vec![0; 128], 128),
        Err(SourceError::Snapshot(Error::InvalidRange))
    ));
    assert!(matches!(
        MaterialSource::from_owned(0, vec![0; 128], 128),
        Err(SourceError::NullResource)
    ));
    let mut reads = 0;
    let changed = copy_cohort_resources(&cohort(2, true), pass(), budget(), &mut |request| {
        let mut bytes = read(request.address)?;
        if request.address == 0x10000 {
            bytes[0x5c] = 17 + reads;
            reads += 1;
        }
        Ok(bytes)
    })
    .unwrap();
    let a = changed.items[0].primary_material.as_ref().unwrap();
    let b = changed.items[1].primary_material.as_ref().unwrap();
    assert_eq!((a.bytes()[0x5c], b.bytes()[0x5c]), (17, 18));
    assert_ne!(a.bytes().as_ptr(), b.bytes().as_ptr());
    assert_eq!(changed.reads, 8);
}
#[test]
fn missing_camera_mapping_preserves_item_and_issues_no_asset_read() {
    let graph = copy_cohort_resources(&cohort(1, false), pass(), budget(), &mut |_| {
        panic!("no source read for missing transform")
    })
    .unwrap();
    assert_eq!(graph.items.len(), 1);
    assert_eq!(graph.reads, 0);
    assert!(
        graph.items[0]
            .obligations
            .contains(&ResourceObligation::MissingCameraTransform)
    );
}
#[test]
fn resource_budget_range_and_truncated_source_never_expose_partial_graph() {
    let c = cohort(1, true);
    let mut b = budget();
    b.bytes = 128 + 192;
    let mut calls = 0;
    let error = copy_cohort_resources(&c, pass(), b, &mut |r| {
        calls += 1;
        read(r.address)
    })
    .err()
    .unwrap();
    assert_eq!(error, CohortResourceError::BudgetExceeded);
    assert_eq!(calls, 2);
    let error = copy_cohort_resources(&c, pass(), budget(), &mut |_| Ok(vec![0; 127]))
        .err()
        .unwrap();
    assert!(matches!(
        error,
        CohortResourceError::ReadExtent { actual: 127, .. }
    ));
    let mut b = budget();
    b.partition_headers = 0;
    let error = copy_cohort_resources(&c, pass(), b, &mut |r| read(r.address))
        .err()
        .unwrap();
    assert!(matches!(
        error,
        CohortResourceError::Mesh(
            mag_render_adapter_scene::mesh_sources::MeshSourceError::BudgetExceeded
        )
    ));
}
