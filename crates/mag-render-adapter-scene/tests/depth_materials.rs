//! Authored records exercise the actual Depth selector, not original assets.
use mag_render_adapter_scene::{
    depth_materials::*,
    depth_sources::{DepthError, DepthItem, DepthItemRoute},
    material_constants::FragmentBuiltinRequest,
    shared_scene_sources::{SourceContent, SourceDomain, SourceSnapshot},
};
use std::{collections::BTreeMap, sync::Arc};

fn word(b: &mut [u8], at: usize, value: u32) {
    b[at..at + 4].copy_from_slice(&value.to_be_bytes());
}
fn half(b: &mut [u8], at: usize, value: u16) {
    b[at..at + 2].copy_from_slice(&value.to_be_bytes());
}
fn owned(b: &[u8]) -> Arc<SourceContent> {
    SourceSnapshot::default()
        .observe(SourceDomain::Guest, 0x1000, b)
        .unwrap()
}
fn material(base: u32, alpha: u8) -> Vec<u8> {
    let mut b = vec![0; 256];
    word(&mut b, 0x38, base);
    half(&mut b, 0x40, 512);
    half(&mut b, 0x42, 0xe0);
    half(&mut b, 0x48, 0xa0);
    half(&mut b, 0x4a, 0xc0);
    b[0x53] = 1;
    b[0x5c] = alpha;
    word(&mut b, 0xc4, 512);
    word(&mut b, 0xe4, 3);
    word(&mut b, 0xe8, 1);
    word(&mut b, 0xec, 0x1f0);
    b
}
fn chunk() -> Vec<u8> {
    let mut b = vec![0; 512];
    for (at, value) in [
        (0, 0x20),
        (0x2c, 0x20),
        (0x4c, 0x20),
        (0x50, 0x40),
        (0x70, 0x40),
        (0x90, 0xd0),
    ] {
        word(&mut b, at, value);
    }
    word(&mut b, 0x150, 16);
    word(&mut b, 0x15c, 0x1002);
    word(&mut b, 0x1f0, (3 << 24) | (7 << 14));
    b
}
fn item(group: u16, primary: &[u8], route: DepthItemRoute) -> DepthItem {
    let mut mesh = vec![0; 192];
    half(&mut mesh, 0xbc, group);
    let mut mbi = [0; 32];
    mbi[0] = (group as u8) << 4;
    mbi[7] = 1;
    DepthItem {
        shard: 0,
        source_index: 0,
        mbi,
        transform: Some([0; 112]),
        route,
        material: Some(owned(primary)),
        mesh: Some(owned(&mesh)),
        geometry: None,
        immutable_partitions: None,
    }
}
fn fixture() -> (Vec<u8>, BTreeMap<u32, Vec<u8>>) {
    let mut pass = vec![0; 688];
    word(&mut pass, 0x190, 1);
    word(&mut pass, 0x284, 0x80000);
    let mut memory = BTreeMap::new();
    for slot in 0..8 {
        let address = 0x20000 + slot as u32 * 0x1000;
        let base = 0x40000 + slot as u32 * 0x1000;
        word(&mut pass, 0x150 + slot * 8, 256);
        word(&mut pass, 0x154 + slot * 8, address);
        memory.insert(address, material(base, slot as u8));
        memory.insert(base, chunk());
    }
    memory.insert(0x81000, vec![0x5a; 16]);
    (pass, memory)
}
#[test]
fn source_override_categories_include_all_depth_groups_and_primary_flag_routes() {
    for row in [0u8, 4] {
        let style = if row == 0 { 0 } else { 2 };
        for (group, category) in [(1, 0), (2, 2), (3, 3), (4, 2), (6, 0), (11, 2)] {
            assert_eq!(depth_override_slot(group, 0, style), row + category);
        }
        assert_eq!(depth_override_slot(5, 0, style), 1);
        assert_eq!(depth_override_slot(7, 1 << 24, style), 1);
        assert_eq!(depth_override_slot(7, 1 << 23, style), row + 2);
        assert_eq!(depth_override_slot(7, 0, style), row);
        assert_eq!(depth_override_slot(7, (1 << 24) | (1 << 23), style), 1);
    }
}
#[test]
fn owned_override_spans_share_programs_but_keep_primary_alpha_and_every_ordinal() {
    let (pass, memory) = fixture();
    let a = material(0xdead0000, 31);
    let b = material(0xbeef0000, 219);
    let items = [
        item(1, &a, DepthItemRoute::Prepare),
        item(1, &b, DepthItemRoute::Prepare),
        item(1, &a, DepthItemRoute::Repeated),
        item(15, &a, DepthItemRoute::Terminates),
    ];
    let mut reads = Vec::new();
    let result = copy_depth_materials(&pass, &items, 1024 * 1024, &mut |a, n| {
        reads.push((a, n));
        memory
            .get(&a)
            .filter(|v| v.len() == n)
            .map(|v| owned(v))
            .ok_or(DepthError::Read)
    })
    .unwrap();
    assert_eq!(result.items.len(), items.len());
    let a = result.items[0].as_ref().unwrap().as_ref().unwrap();
    let b = result.items[1].as_ref().unwrap().as_ref().unwrap();
    assert!(Arc::ptr_eq(&a.program, &b.program));
    assert!(Arc::ptr_eq(&a.selected, &result.overrides[0].material));
    assert!(!Arc::ptr_eq(&a.primary, &a.selected));
    assert_eq!(
        a.builtins,
        Ok(vec![FragmentBuiltinRequest::AlphaTransfer {
            record_index: 0,
            logical_slot: 7,
            primary_alpha: 31
        }])
    );
    assert_eq!(
        b.builtins,
        Ok(vec![FragmentBuiltinRequest::AlphaTransfer {
            record_index: 0,
            logical_slot: 7,
            primary_alpha: 219
        }])
    );
    assert_eq!(a.program.fragment_template.bytes(), &[0x5a; 16]);
    assert_eq!(a.technique.resource_base, 0x40000);
    assert!(result.items[2].is_none() && result.items[3].is_none());
    assert_eq!(reads.len(), 17); // eight whole override pairs + one actual FP.
    assert!(result.decoded_bytes > 0);
}
#[test]
fn selected_override_range_and_input_failure_never_fall_back_to_primary() {
    let (pass, mut memory) = fixture();
    let primary = material(0x90000, 255);
    let items = [item(1, &primary, DepthItemRoute::Prepare)];
    word(memory.get_mut(&0x20000).unwrap(), 0xc0, 16);
    let result = copy_depth_materials(&pass, &items, 1024 * 1024, &mut |a, n| {
        memory
            .get(&a)
            .filter(|v| v.len() == n)
            .map(|v| owned(v))
            .ok_or(DepthError::Read)
    })
    .unwrap();
    assert!(matches!(
        result.items[0],
        Some(Err(DepthMaterialError::TechniqueExtent))
    ));
    assert!(matches!(
        copy_depth_materials(&pass, &items, 1024, &mut |_, _| Err(DepthError::Read)),
        Err(DepthMaterialError::Capture(DepthError::Read))
    ));
}

#[test]
fn zero_source_bank_can_resolve_a_real_nonzero_fragment_address() {
    let (mut pass, mut memory) = fixture();
    word(&mut pass, 0x284, 0);
    for slot in 0..8 {
        word(
            memory.get_mut(&(0x40000 + slot * 0x1000)).unwrap(),
            0x15c,
            0x81002,
        );
    }
    let items = [item(1, &material(0xdead0000, 31), DepthItemRoute::Prepare)];
    let result = copy_depth_materials(&pass, &items, 1024 * 1024, &mut |a, n| {
        memory
            .get(&a)
            .filter(|v| v.len() == n)
            .map(|v| owned(v))
            .ok_or(DepthError::Read)
    })
    .unwrap();
    let actual = result.items[0].as_ref().unwrap().as_ref().unwrap();
    assert_eq!(actual.program.fragment_address, 0x81000);
    assert_eq!(actual.program.fragment_template.bytes(), &[0x5a; 16]);
    word(memory.get_mut(&0x40000).unwrap(), 0x15c, 2);
    let refused = copy_depth_materials(&pass, &items, 1024 * 1024, &mut |a, n| {
        memory
            .get(&a)
            .filter(|v| v.len() == n)
            .map(|v| owned(v))
            .ok_or(DepthError::Read)
    })
    .unwrap();
    assert!(matches!(
        refused.items[0],
        Some(Err(DepthMaterialError::Capture(DepthError::Range)))
    ));
}
