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
