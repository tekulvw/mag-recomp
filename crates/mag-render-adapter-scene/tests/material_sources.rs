//! Synthetic source records; no original game data.
use mag_render_adapter_scene::guest_memory::GuestSnapshot;
use mag_render_adapter_scene::material_sources::{
    MaterialSource, SourceError, TechniqueResourceBase, selected_resource_addresses,
};

fn word(bytes: &mut [u8], at: usize, value: u32) {
    bytes[at..at + 4].copy_from_slice(&value.to_be_bytes());
}
fn material() -> Vec<u8> {
    let mut bytes = vec![0; 0x300];
    word(&mut bytes, 0x38, 0x4000);
    bytes[0x48..0x4a].copy_from_slice(&0x80u16.to_be_bytes());
    bytes[0x4a..0x4c].copy_from_slice(&0x200u16.to_be_bytes());
    bytes[0x80 + 2 * 32 + 1] = 3;
    word(&mut bytes, 0x200 + 3 * 8, 0x90);
    word(&mut bytes, 0x200 + 3 * 8 + 4, 17);
    bytes
}
#[test]
fn selected_item_decodes_source_identities_and_camera_slot() {
    let mut item = [0; 32];
    item[0] = 0xb0;
    item[6..9].copy_from_slice(&[0x12, 0x34, 0x56]);
    word(&mut item, 12, 0x0123_4567);
    word(&mut item, 16, 0b0011);
    word(&mut item, 20, 0b0100);
    // Upper halfword is transform index, low nine bits are material size/16.
    word(&mut item, 28, (9 << 16) | 20);
    let decoded = selected_resource_addresses(&item, 1, 0x1000).unwrap();
    assert_eq!(decoded.material_address, 0x1234_5600);
    assert_eq!(decoded.material_bytes, 320);
    assert_eq!(decoded.mesh_address, 0x1234_5670);
    assert_eq!(decoded.transform_address, 0x1000 + 11 * 112);
    assert_eq!(decoded.mesh_group, 11);
}
#[test]
fn missing_camera_has_no_fabricated_transform() {
    assert_eq!(
        selected_resource_addresses(&[0; 32], 4, 0),
        Err(SourceError::InvalidCamera)
    );
    assert_eq!(
        selected_resource_addresses(&[0; 32], 0, 0),
        Err(SourceError::MissingCameraTransform)
    );
}
#[test]
fn full_encoded_material_size_does_not_inherit_worker_scratch_limit() {
    let mut item = [0; 32];
    item[6] = 1;
    word(&mut item, 20, 8);
    word(&mut item, 28, 289);
    assert_eq!(
        selected_resource_addresses(&item, 0, 0)
            .unwrap()
            .material_bytes,
        4624
    );
}
#[test]
fn copied_material_selects_primary_chunk_and_owns_its_bytes() {
    let mut bytes = material();
    let owned = MaterialSource::copy(
        &GuestSnapshot::new(0x1000, &bytes).unwrap(),
        0x1000,
        bytes.len(),
        8192,
    )
    .unwrap();
    bytes.fill(0);
    let chunk = owned
        .select_technique(2 << 3, 1, 0, 0, TechniqueResourceBase::Primary)
        .unwrap();
    assert_eq!(chunk.selector, 2);
    assert_eq!(chunk.table_index, 3);
    assert_eq!(chunk.source_address, 0x4090);
    assert_eq!(chunk.declared_bytes, 17);
    assert_eq!(chunk.transfer_bytes, 32);
    assert_eq!(owned.source_address(), 0x1000);
}
#[test]
fn explicit_override_resource_does_not_use_primary_base() {
    let bytes = material();
    let owned = MaterialSource::copy(
        &GuestSnapshot::new(0x1000, &bytes).unwrap(),
        0x1000,
        bytes.len(),
        8192,
    )
    .unwrap();
    let chunk = owned
        .select_technique(0xfa, 15, 99, 0, TechniqueResourceBase::Override(0x9000))
        .unwrap();
    assert_eq!(chunk.selector, 2);
    assert_eq!(chunk.source_address, 0x9090);
    assert_eq!(
        owned.select_technique(0xfa, 15, 99, 0, TechniqueResourceBase::Override(0)),
        Err(SourceError::NullResource)
    );
}
#[test]
fn selector_mask_and_high_render_mode_change_real_map_selection() {
    let mut bytes = material();
    bytes[0x80 + 1] = 3;
    let owned = MaterialSource::copy(
        &GuestSnapshot::new(0x1000, &bytes).unwrap(),
        0x1000,
        bytes.len(),
        8192,
    )
    .unwrap();
    for (mode, mask) in [(16, 0), (0, 2)] {
        let chunk = owned
            .select_technique(2 << 3, 1, mode, mask, TechniqueResourceBase::Primary)
            .unwrap();
        assert_eq!(chunk.selector, 0);
        assert_eq!(chunk.source_address, 0x4090);
    }
}
#[test]
fn malformed_maps_and_tables_return_errors() {
    let mut bytes = material();
    bytes[0x48..0x4a].copy_from_slice(&0x2f0u16.to_be_bytes());
    let owned = MaterialSource::copy(
        &GuestSnapshot::new(0x1000, &bytes).unwrap(),
        0x1000,
        bytes.len(),
        8192,
    )
    .unwrap();
    assert_eq!(
        owned.select_technique(0, 1, 0, 0, TechniqueResourceBase::Primary),
        Err(SourceError::MaterialBounds)
    );
    bytes[0x48..0x4a].copy_from_slice(&0x80u16.to_be_bytes());
    bytes[0x80 + 2 * 32 + 1] = 255;
    let owned = MaterialSource::copy(
        &GuestSnapshot::new(0x1000, &bytes).unwrap(),
        0x1000,
        bytes.len(),
        8192,
    )
    .unwrap();
    assert_eq!(
        owned.select_technique(16, 1, 0, 0, TechniqueResourceBase::Primary),
        Err(SourceError::MaterialBounds)
    );
}
#[test]
fn source_copy_rejects_zero_oversize_and_unavailable_spans() {
    let bytes = material();
    let snapshot = GuestSnapshot::new(0x1000, &bytes).unwrap();
    assert!(matches!(
        MaterialSource::copy(&snapshot, 0, bytes.len(), 8192),
        Err(SourceError::NullResource)
    ));
    assert!(matches!(
        MaterialSource::copy(&snapshot, 0x1000, 4609, 4608),
        Err(SourceError::BudgetExceeded)
    ));
    assert!(matches!(
        MaterialSource::copy(&snapshot, 0x1000, 0x301, 8192),
        Err(SourceError::Snapshot(_))
    ));
}

#[test]
fn material_larger_than_a_fixed_scratch_buffer_uses_explicit_native_budget() {
    let mut bytes = material();
    bytes.resize(4624, 0);
    let snapshot = GuestSnapshot::new(0x1000, &bytes).unwrap();
    let owned = MaterialSource::copy(&snapshot, 0x1000, bytes.len(), 8176).unwrap();
    assert_eq!(owned.bytes().len(), 4624);
    assert_eq!(
        owned
            .select_technique(16, 1, 0, 0, TechniqueResourceBase::Primary)
            .unwrap()
            .source_address,
        0x4090
    );
    assert!(matches!(
        MaterialSource::copy(&snapshot, 0x1000, bytes.len(), 4608),
        Err(SourceError::BudgetExceeded)
    ));
}
