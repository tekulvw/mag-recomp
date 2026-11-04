use mag_render_adapter_scene::shared_scene_sources::{CatalogBudget, CatalogError, SourceCatalog};
use std::sync::Arc;
#[test]
fn depth_and_main_share_exact_owned_content_not_guest_address_or_hash_alone() {
    let mut catalog = SourceCatalog::default();
    let budget = CatalogBudget {
        entries: 2,
        bytes: 8,
        observations: 4,
    };
    let depth = catalog.intern(&[1, 2, 3, 4], budget).unwrap();
    let main = catalog.intern(&[1, 2, 3, 4], budget).unwrap();
    assert!(Arc::ptr_eq(&depth, &main));
    let changed_same_address = catalog.intern(&[1, 2, 3, 5], budget).unwrap();
    assert_ne!(depth.identity, changed_same_address.identity);
    assert_eq!(depth.bytes(), [1, 2, 3, 4]);
    assert_eq!(
        catalog.intern(&[9], budget).unwrap_err(),
        CatalogError::Budget
    );
    drop(changed_same_address);
    let next = catalog.intern(&[9], budget).unwrap();
    assert!(next.identity > main.identity);
    assert_eq!(depth.bytes(), [1, 2, 3, 4]);
}
