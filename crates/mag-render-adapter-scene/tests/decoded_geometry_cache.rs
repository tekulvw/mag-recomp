use mag_render_adapter_scene::{decoded_geometry_cache::*, geometry_streams::GeometryStreamError};
use std::sync::Arc;
fn format(codec: u8) -> Vec<u8> {
    vec![1, 16, 0, 0, 0, 0, 0, 0, 0, codec, 3, 1, 16, 0, 0, 0]
}
#[test]
fn exact_owned_content_metadata_and_current_budget_determine_reuse() {
    let mut cache = DecodedGeometryCache::default();
    let limits = CacheLimits::default();
    let source: Vec<u8> = [1f32, 2., 3., 4.]
        .into_iter()
        .flat_map(f32::to_be_bytes)
        .collect();
    let a = cache
        .rows(
            RowCodec::Descriptor {
                ordinal: 0,
                phase: 0,
            },
            &format(2),
            &source,
            1,
            16,
            limits,
        )
        .unwrap();
    let b = cache
        .rows(
            RowCodec::Descriptor {
                ordinal: 0,
                phase: 0,
            },
            &format(2),
            &source.clone(),
            1,
            16,
            limits,
        )
        .unwrap();
    assert!(Arc::ptr_eq(&a, &b));
    assert_eq!(
        cache.rows(
            RowCodec::Descriptor {
                ordinal: 0,
                phase: 4
            },
            &format(2),
            &source,
            1,
            16,
            limits
        ),
        Err(GeometryStreamError::SourceExtent)
    );
    let mut changed = source.clone();
    changed[0] ^= 1;
    let c = cache
        .rows(
            RowCodec::Descriptor {
                ordinal: 0,
                phase: 0,
            },
            &format(2),
            &changed,
            1,
            16,
            limits,
        )
        .unwrap();
    assert!(!Arc::ptr_eq(&a, &c));
    assert_ne!(a, c);
    let d = cache
        .rows(
            RowCodec::Descriptor {
                ordinal: 0,
                phase: 0,
            },
            &format(3),
            &source,
            1,
            16,
            limits,
        )
        .unwrap();
    assert!(!Arc::ptr_eq(&a, &d));
    assert_eq!(
        cache.rows(
            RowCodec::Descriptor {
                ordinal: 0,
                phase: 0
            },
            &format(2),
            &source,
            1,
            15,
            limits
        ),
        Err(GeometryStreamError::BudgetExceeded)
    );
    let e = cache
        .rows(
            RowCodec::Descriptor {
                ordinal: 0,
                phase: 0,
            },
            &format(2),
            &source,
            0,
            0,
            limits,
        )
        .unwrap();
    assert!(e.is_empty());
    assert!(!Arc::ptr_eq(&a, &e));
}
#[test]
fn bounded_retention_clear_and_oversize_never_drop_valid_outputs() {
    let mut cache = DecodedGeometryCache::default();
    let limits = CacheLimits {
        record: 1024,
        total: 1024,
        entries: 1,
    };
    let a = cache.indices(&[0, 2, 0, 1], 2, 4, limits).unwrap();
    let b = cache.indices(&[0, 2, 0, 1], 2, 4, limits).unwrap();
    assert!(Arc::ptr_eq(&a, &b));
    let _c = cache.indices(&[0, 3], 1, 2, limits).unwrap();
    let d = cache.indices(&[0, 2, 0, 1], 2, 4, limits).unwrap();
    assert!(!Arc::ptr_eq(&a, &d));
    assert_eq!(a, d);
    cache.clear();
    let e = cache.indices(&[0, 2, 0, 1], 2, 4, limits).unwrap();
    assert!(!Arc::ptr_eq(&d, &e));
    let no_cache = CacheLimits {
        record: 1,
        ..limits
    };
    let f = cache.indices(&[0, 9], 1, 2, no_cache).unwrap();
    let g = cache.indices(&[0, 9], 1, 2, no_cache).unwrap();
    assert_eq!(*f, vec![9]);
    assert!(!Arc::ptr_eq(&f, &g));
}

#[test]
fn recently_read_bank_survives_streaming_eviction_without_invalidating_old_consumers() {
    let mut cache = DecodedGeometryCache::default();
    let limits = CacheLimits {
        record: 1024,
        total: 2048,
        entries: 2,
    };
    let a = cache.indices(&[0, 1], 1, 2, limits).unwrap();
    let b = cache.indices(&[0, 2], 1, 2, limits).unwrap();
    let hit = cache.indices(&[0, 1], 1, 2, limits).unwrap();
    assert!(Arc::ptr_eq(&a, &hit));
    let _c = cache.indices(&[0, 3], 1, 2, limits).unwrap();
    let a_again = cache.indices(&[0, 1], 1, 2, limits).unwrap();
    assert!(Arc::ptr_eq(&a, &a_again));
    let b_again = cache.indices(&[0, 2], 1, 2, limits).unwrap();
    assert!(!Arc::ptr_eq(&b, &b_again));
    assert_eq!(*b, vec![2]);
    assert!(cache.stats().evictions >= 2);
    assert!(cache.stats().retained_bytes <= limits.total);
}

#[test]
fn auxiliary_rows_are_shared_only_for_equal_bytes_and_decoder_fields() {
    use mag_render_adapter_scene::geometry_bindings::{
        AuxiliaryBinding, BindingError, VertexBinding, decode_auxiliary_attribute,
    };
    let binding = |format, components, stride, frequency, modulo| AuxiliaryBinding {
        binding: VertexBinding {
            declaration: 0,
            semantic: 8,
            slot: 8,
            format,
            components,
            stride,
            byte_offset: 0,
            encoded_bytes: 4,
        },
        source_declaration: [0; 8],
        source_format_word: 0,
        frequency,
        modulo,
        location: 0,
        offset: 0x100,
    };
    let source: Vec<u8> = (0..96u8).map(|b| b.wrapping_mul(37)).collect();
    let mut cache = DecodedGeometryCache::default();
    let limits = CacheLimits::default();
    let cases = [
        binding(4, 4, 4, 0, false),
        binding(4, 3, 4, 0, false), // components differ
        binding(4, 4, 8, 0, false), // stride differs
        binding(3, 2, 4, 0, false), // format differs
        binding(4, 4, 4, 3, true),  // modulo frequency
        binding(4, 4, 4, 3, false), // division frequency
        binding(1, 2, 4, 0, false),
    ];
    for _ in 0..2 {
        for b in &cases {
            let expected = decode_auxiliary_attribute(b, &source, 12, 4096).unwrap();
            let got = cache.auxiliary(b, &source, 12, 4096, limits).unwrap();
            assert_eq!(*got, expected, "{:?}", b.binding);
        }
    }
    assert_eq!(cache.stats().misses as usize, cases.len());
    assert_eq!(cache.stats().hits as usize, cases.len());
    // Metadata that the decoder ignores (slot, declaration, location) shares.
    let mut moved = cases[0].clone();
    moved.binding.slot = 9;
    moved.offset = 0x900;
    let a = cache
        .auxiliary(&cases[0], &source, 12, 4096, limits)
        .unwrap();
    let b = cache
        .auxiliary(&moved, &source.clone(), 12, 4096, limits)
        .unwrap();
    assert!(Arc::ptr_eq(&a, &b));
    // A changed byte, a different count, or a short budget is never a hit.
    let mut changed = source.clone();
    changed[5] ^= 1;
    let c = cache
        .auxiliary(&cases[0], &changed, 12, 4096, limits)
        .unwrap();
    assert!(!Arc::ptr_eq(&a, &c));
    assert_eq!(
        *c,
        decode_auxiliary_attribute(&cases[0], &changed, 12, 4096).unwrap()
    );
    assert_eq!(
        cache
            .auxiliary(&cases[0], &source, 11, 4096, limits)
            .unwrap()
            .len(),
        11
    );
    assert_eq!(
        cache.auxiliary(&cases[0], &source, 12, 12 * 16 - 1, limits),
        Err(BindingError::Budget)
    );
    assert_eq!(
        decode_auxiliary_attribute(&cases[0], &source, 12, 12 * 16 - 1),
        Err(BindingError::Budget)
    );
}
