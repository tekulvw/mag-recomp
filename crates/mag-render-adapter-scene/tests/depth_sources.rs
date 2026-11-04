use mag_render_adapter_scene::{
    depth_sources::*,
    error::Error,
    mesh_sources::GeometrySource,
    shared_scene_sources::{SourceDomain, SourceSnapshot},
};
use std::{collections::BTreeMap, sync::Arc};
fn put(bytes: &mut [u8], at: usize, value: u32) {
    bytes[at..at + 4].copy_from_slice(&value.to_be_bytes());
}
fn fixture() -> (DepthInput, BTreeMap<u32, Vec<u8>>) {
    let identity = DepthIdentity {
        vm: 1,
        invocation: 2,
        owner: 3,
        occurrence: 4,
        execution_update: 5,
        allocation: 6,
        job: 0x20000,
        camera: 3,
    };
    let input = DepthInput {
        identity,
        bucket: 0x21000,
        marker: 0x22000,
        pass: 0x23000,
        user: 0x24000,
    };
    let mut pass = vec![0; 688];
    put(&mut pass, 0xe4, 0x25000);
    put(&mut pass, 0xe0, 0x26000);
    pass[0x19b] = 3;
    put(&mut pass, 0x284, 0x28000);
    let mut bucket = vec![0; 0x68];
    put(&mut bucket, 4, 0x25000);
    put(&mut bucket, 12, 0x26000);
    put(&mut bucket, 0x58, 0x27000);
    let mut marker = vec![0; 320];
    put(&mut marker, 4, 2);
    put(&mut marker, 64, 2);
    let mut mbi = vec![0; 32];
    mbi[0] = 0x10;
    mbi[7] = 3;
    put(&mut mbi, 12, 0x4000);
    put(&mut mbi, 20, 1);
    put(&mut mbi, 28, 6);
    let mut mesh = vec![0; 192];
    put(&mut mesh, 0x9c, 0x1000);
    put(&mut mesh, 0x50, 0x100);
    let material = vec![0; 96];
    let transform = vec![0; 112];
    (
        input,
        BTreeMap::from([
            (input.pass, pass),
            (input.user, vec![0; 160]),
            (input.bucket, bucket),
            (input.marker, marker),
            (0x25000, mbi),
            (0x26000, transform),
            (0x27000, vec![0, 0, 0, 0]),
            (0x30000, material),
            (0x40000, mesh),
        ]),
    )
}
#[test]
fn actual_family_three_population_routes_and_assets_share_main_content() {
    let (input, memory) = fixture();
    let mut source = copy_depth_sources(input, DepthBudget::default(), &mut |a, n| {
        memory
            .get(&a)
            .filter(|v| v.len() == n)
            .cloned()
            .ok_or(Error::OutsideSnapshot)
    })
    .unwrap();
    assert_eq!(source.items.len(), 2);
    assert_eq!(source.items[0].route, DepthItemRoute::Prepare);
    assert_eq!(source.items[1].route, DepthItemRoute::Repeated);
    assert_eq!(
        source.items[0].geometry,
        Some(GeometrySource::StaticPartitions {
            first: 0x28100,
            count: 0
        })
    );
    let material = source.items[0].material.take().unwrap();
    let mut main = SourceSnapshot::default();
    let same = main
        .observe(SourceDomain::Guest, 0x30000, &memory[&0x30000])
        .unwrap();
    assert!(Arc::ptr_eq(&material, &same));
    assert_eq!(source.pending.len(), 4);
    assert!(!source.source_snapshot.incomplete);
}
#[test]
fn depth_duplicate_has_no_main_instanced_exception_and_source_changes_refuse() {
    let (input, mut memory) = fixture();
    memory.get_mut(&0x26000).unwrap()[111] = 1;
    let copy = |memory: &BTreeMap<u32, Vec<u8>>| {
        copy_depth_sources(input, DepthBudget::default(), &mut |a, n| {
            memory
                .get(&a)
                .filter(|v| v.len() == n)
                .cloned()
                .ok_or(Error::OutsideSnapshot)
        })
    };
    assert_eq!(
        copy(&memory).unwrap().items[1].route,
        DepthItemRoute::Repeated
    );
    // No family0 list fallback: this real family3 pointer must exist.
    put(memory.get_mut(&input.bucket).unwrap(), 0x58, 0x99900);
    assert_eq!(copy(&memory).unwrap_err(), DepthError::Read);
}

#[test]
fn actual_depth_copy_retains_selected_override_and_primary_roles_in_one_window() {
    let (input, mut memory) = fixture();
    let primary = memory.get_mut(&0x30000).unwrap();
    primary.resize(256, 0);
    primary[0x5c] = 173;
    put(memory.get_mut(&0x25000).unwrap(), 28, 16);
    memory.get_mut(&0x40000).unwrap()[0xbd] = 1;
    put(memory.get_mut(&input.pass).unwrap(), 0x190, 1);
    for slot in 0..8 {
        let address = 0x50000 + slot as u32 * 0x1000;
        let resource = 0x60000 + slot as u32 * 0x1000;
        put(memory.get_mut(&input.pass).unwrap(), 0x150 + slot * 8, 256);
        put(
            memory.get_mut(&input.pass).unwrap(),
            0x154 + slot * 8,
            address,
        );
        let mut material = vec![0; 256];
        put(&mut material, 0x38, resource);
        material[0x40..0x42].copy_from_slice(&512u16.to_be_bytes());
        material[0x48..0x4a].copy_from_slice(&0xa0u16.to_be_bytes());
        material[0x4a..0x4c].copy_from_slice(&0xc0u16.to_be_bytes());
        put(&mut material, 0xc4, 512);
        let mut chunk = vec![0; 512];
        for (at, value) in [
            (0, 0x20),
            (0x2c, 0x20),
            (0x4c, 0x20),
            (0x50, 0x40),
            (0x70, 0x40),
            (0x90, 0xd0),
            (0x150, 16),
            (0x15c, 0x1002),
        ] {
            put(&mut chunk, at, value);
        }
        memory.insert(address, material);
        memory.insert(resource, chunk);
    }
    memory.insert(0x29000, vec![0x31; 16]);
    let source = copy_depth_sources(input, DepthBudget::default(), &mut |a, n| {
        memory
            .get(&a)
            .filter(|v| v.len() == n)
            .cloned()
            .ok_or(Error::OutsideSnapshot)
    })
    .unwrap();
    let materials = source.materials.as_ref().unwrap();
    assert_eq!(materials.overrides.len(), 8);
    assert_eq!(materials.items.len(), source.items.len());
    let selected = materials.items[0].as_ref().unwrap().as_ref().unwrap();
    assert_eq!(selected.primary.bytes()[0x5c], 173);
    assert_eq!(selected.selected_address, 0x50000);
    assert_eq!(selected.technique.resource_base, 0x60000);
    assert_eq!(selected.program.fragment_address, 0x29000);
    assert!(Arc::ptr_eq(
        &selected.primary,
        source.items[0].material.as_ref().unwrap()
    ));
    assert!(materials.items[1].is_none());
    assert_eq!(source.pending.len(), 4); // No preparation/native admission claimed.
    assert!(source.copied_bytes + materials.decoded_bytes <= DepthBudget::default().bytes);
    // A later Main observation can retain the SAME exact immutable resource;
    // the Depth occurrence and pass-local selection remain independent.
    let mut main = SourceSnapshot::default();
    let same = main
        .observe(SourceDomain::Guest, 0x60000, &memory[&0x60000])
        .unwrap();
    assert!(Arc::ptr_eq(&same, &selected.program.resource));
}

#[test]
fn actual_copy_abi_retains_exact_depth_identity_and_revokes_retired_capture() {
    // The adapter entry points do nothing while the master switch is off.
    mag_render_adapter_core::runtime::enable();
    use mag_render_adapter_scene::{cohort_ffi::mag_render_adapter_cohort_vm, depth_source_ffi::*};
    use std::ffi::c_void;
    struct Copy {
        memory: BTreeMap<u32, Vec<u8>>,
        retire: bool,
    }
    unsafe extern "C" fn read(
        opaque: *mut c_void,
        address: u32,
        bytes: u32,
        output: *mut u8,
    ) -> i32 {
        // SAFETY: test supplies this live exact Copy and initialized output.
        let copy = unsafe { &mut *opaque.cast::<Copy>() };
        if copy.retire {
            copy.retire = false;
            assert_eq!(mag_render_adapter_cohort_vm(0), 0);
        }
        let Some(data) = copy
            .memory
            .get(&address)
            .filter(|d| d.len() == bytes as usize)
        else {
            return -1;
        };
        unsafe {
            std::ptr::copy_nonoverlapping(data.as_ptr(), output, data.len());
        }
        0
    }
    let (source, memory) = fixture();
    let input = DepthSourceInput {
        abi_version: 1,
        struct_bytes: size_of::<DepthSourceInput>() as u32,
        vm: source.identity.vm,
        invocation: source.identity.invocation,
        owner: source.identity.owner,
        occurrence: source.identity.occurrence,
        execution_update: source.identity.execution_update,
        camera: source.identity.camera,
        bucket: source.bucket,
        marker: source.marker,
        pass: source.pass,
        user: source.user,
        job: source.identity.job,
        allocation: source.identity.allocation,
    };
    assert_eq!(size_of::<DepthSourceInput>(), 80);
    assert_eq!(mag_render_adapter_cohort_vm(1), 0);
    let mut copy = Copy {
        memory,
        retire: false,
    };
    let result = unsafe {
        mag_render_adapter_depth_sources(input, Some(read), (&mut copy as *mut Copy).cast())
    };
    assert_eq!(result.status, 0);
    assert_eq!(result.items, 2);
    let retained = retain_depth_sources(source.identity).unwrap();
    assert_eq!(retained.items.len(), 2);
    let mut wrong = source.identity;
    wrong.allocation += 1;
    assert!(retain_depth_sources(wrong).is_none());
    assert_eq!(mag_render_adapter_cohort_vm(1), 0);
    copy.retire = true;
    assert_ne!(
        unsafe {
            mag_render_adapter_depth_sources(input, Some(read), (&mut copy as *mut Copy).cast())
        }
        .status,
        0
    );
    assert!(retain_depth_sources(source.identity).is_none());
    // Already owned bytes survive retirement; they do not authorize a submission.
    assert_eq!(retained.items[0].mesh.as_ref().unwrap().bytes().len(), 192);
}

#[test]
fn actual_depth_and_main_partition_capture_share_the_same_decoded_banks() {
    // The adapter entry points do nothing while the master switch is off.
    mag_render_adapter_core::runtime::enable();
    const NAME: &str = "actual_depth_and_main_partition_capture_share_the_same_decoded_banks";
    if std::env::var_os("MAG_DEPTH_CACHE_CHILD").is_none() {
        let status = std::process::Command::new(std::env::current_exe().unwrap())
            .args(["--exact", NAME, "--nocapture"])
            .env("MAG_DEPTH_CACHE_CHILD", "1")
            .env("PS3_RENDER_DECODED_GEOMETRY_CACHE", "1")
            .status()
            .unwrap();
        assert!(status.success());
        return;
    }
    use mag_render_adapter_scene::{
        cohort::{CohortBuilder, CohortHeader},
        cohort_geometry::copy_diagnostic_geometry,
        cohort_resources::ResourceCopyBudget,
    };
    let (input, mut memory) = fixture();
    let mut header = vec![0; 544];
    for (slot, address, bytes) in [
        (19, 0x29000, 16),
        (7, 0x2a000, 32),
        (1, 0x2b000, 6),
        (20, 0x2c000, 24),
    ] {
        put(&mut header, 0x30 + 8 * slot, bytes);
        put(&mut header, 0x34 + 8 * slot, address)
    }
    let mut config = vec![0; 16];
    config[2] = 255;
    config[9] = 1;
    config[11] = 3;
    let source: Vec<u8> = [1f32, 2., 3., 1., 0., 1., 0., 0.]
        .into_iter()
        .flat_map(f32::to_be_bytes)
        .collect();
    memory.insert(0x28100, header);
    memory.insert(0x29000, config);
    memory.insert(0x2a000, source);
    memory.insert(0x2b000, vec![0; 6]);
    memory.insert(
        0x2c000,
        vec![
            2, 32, 0, 0, 0, 0, 0, 0, 0, 2, 3, 1, 16, 0, 0, 0, 16, 2, 4, 2, 16, 0, 0, 0,
        ],
    );
    memory.get_mut(&0x40000).unwrap()[0xbd] = 1;
    let depth = copy_depth_sources(input, DepthBudget::default(), &mut |a, n| {
        memory
            .get(&a)
            .filter(|v| v.len() == n)
            .cloned()
            .ok_or(Error::OutsideSnapshot)
    })
    .unwrap();
    let depth_banks = depth.items[0]
        .immutable_partitions
        .as_ref()
        .unwrap()
        .as_ref()
        .unwrap();
    assert_eq!(depth_banks.len(), 1);
    let depth_normal = &depth_banks[0]
        .attributes
        .as_ref()
        .unwrap()
        .as_ref()
        .unwrap()
        .attributes[1]
        .words;
    let mbi: [u8; 32] = memory[&0x25000].clone().try_into().unwrap();
    let mut counts = [[0; 16]; 4];
    counts[0][1] = 2;
    let mut builder = CohortBuilder::new(
        CohortHeader {
            vm: 1,
            source_invocation: 9,
            ppu_owner: 3,
            pass_occurrence: 10,
            execution_update: 5,
            camera: 0,
            group_counts: counts,
        },
        2,
        4096,
    )
    .unwrap();
    builder.push(0, 0, mbi, Some([0; 112])).unwrap();
    builder.push(0, 0, mbi, Some([0; 112])).unwrap();
    let cohort = builder.finish().unwrap();
    let mut reads = 0;
    let main = copy_diagnostic_geometry(
        &cohort,
        None,
        0x28000,
        ResourceCopyBudget {
            bytes: 65536,
            reads: 64,
            partition_headers: 8,
        },
        &mut |r| {
            reads += 1;
            memory
                .get(&r.address)
                .filter(|v| v.len() == r.bytes)
                .cloned()
                .ok_or(Error::OutsideSnapshot)
        },
    )
    .unwrap();
    assert_eq!(main.items.len(), 2);
    assert!(reads > 0);
    let first = &main.items[0].draws[0];
    let repeated = &main.items[1].draws[0];
    assert!(Arc::ptr_eq(
        depth_banks[0].positions.as_ref().unwrap(),
        &first.positions
    ));
    assert!(Arc::ptr_eq(
        depth_banks[0].indices.as_ref().unwrap(),
        &first.indices
    ));
    assert!(Arc::ptr_eq(&first.positions, &repeated.positions));
    assert!(Arc::ptr_eq(&first.indices, &repeated.indices));
    let main_normal = &main.items[0].draws[0]
        .attributes
        .as_ref()
        .unwrap()
        .as_ref()
        .unwrap()
        .attributes[1]
        .words;
    assert!(Arc::ptr_eq(depth_normal, main_normal));
    let second = &main.items[1].draws[0]
        .attributes
        .as_ref()
        .unwrap()
        .as_ref()
        .unwrap()
        .attributes[1]
        .words;
    assert!(Arc::ptr_eq(main_normal, second));
    let limited = mag_render_adapter_scene::geometry_attributes::decode_primary_attributes(
        &memory[&0x2c000],
        &memory[&0x2a000],
        0x2a000,
        1,
        16,
    )
    .unwrap();
    assert_eq!(limited.owned_bytes(), 16);
    assert_eq!(
        limited.failures[0].error,
        mag_render_adapter_scene::geometry_streams::GeometryStreamError::BudgetExceeded
    );
    assert!(limited.first_semantic(2).unwrap().is_err());
    let stats = mag_render_adapter_scene::decoded_geometry_cache::stats();
    assert!(stats.hits >= 5);
    assert!(stats.retained_bytes <= 64 * 1024 * 1024);
    let prior = main_normal.clone();
    mag_render_adapter_scene::cohort_ffi::mag_render_adapter_cohort_vm(1);
    let fresh = mag_render_adapter_scene::geometry_attributes::decode_primary_attributes(
        &memory[&0x2c000],
        &memory[&0x2a000],
        0x2a000,
        1,
        65536,
    )
    .unwrap();
    assert!(!Arc::ptr_eq(&prior, &fresh.attributes[1].words));
    assert_eq!(prior, fresh.attributes[1].words);
}

#[test]
fn a_shadow_camera_slot_reads_its_own_family_lists() {
    let (mut input, mut memory) = fixture();
    // Cascade 0 of the Shadow job: cohort camera slot 1, whose four lists sit
    // one 16-byte family row into the bucket's list table.
    input.identity.camera = 1;
    memory.get_mut(&input.pass).unwrap()[0x19b] = 1;
    // The item carries a transform for camera slot 1 (camera bit 1 << 2).
    memory.get_mut(&0x25000).unwrap()[20..24].copy_from_slice(&4u32.to_be_bytes());
    let bucket = memory.get_mut(&input.bucket).unwrap();
    bucket[0x58..0x5c].fill(0);
    bucket[0x38..0x3c].copy_from_slice(&0x27000u32.to_be_bytes());
    let copy = |memory: &BTreeMap<u32, Vec<u8>>| {
        copy_depth_sources(input, DepthBudget::default(), &mut |a, n| {
            memory
                .get(&a)
                .filter(|v| v.len() == n)
                .cloned()
                .ok_or(Error::OutsideSnapshot)
        })
    };
    let source = copy(&memory).unwrap();
    assert_eq!(source.items.len(), 2);
    assert_eq!(source.items[0].route, DepthItemRoute::Prepare);
    // The family-3 row is not read for camera slot 1.
    let bucket = memory.get_mut(&input.bucket).unwrap();
    bucket[0x38..0x3c].fill(0);
    bucket[0x58..0x5c].copy_from_slice(&0x27000u32.to_be_bytes());
    assert!(copy(&memory).is_err());
}
