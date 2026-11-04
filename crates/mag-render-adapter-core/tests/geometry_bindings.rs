use mag_render_adapter_core::geometry_bindings::*;
fn word(b: &mut [u8], at: usize, v: u32) {
    b[at..at + 4].copy_from_slice(&v.to_be_bytes());
}
fn output() -> Vec<u8> {
    vec![
        2, 24, 1, 99, 0, 0, 0, 0, 0, 2, 3, 1, 12, 0, 0, 0, 16, 4, 4, 9, 4, 3, 0, 0,
    ]
}
#[test]
fn output_count_is_header_zero_and_preserves_semantics_slots_and_precision_metadata() {
    let plan = decode_output_bindings(&output()).unwrap();
    assert_eq!(plan.len(), 2);
    assert_eq!((plan[0].semantic, plan[0].slot, plan[0].stride), (1, 0, 24));
    assert_eq!(
        (
            plan[1].semantic,
            plan[1].slot,
            plan[1].format,
            plan[1].byte_offset
        ),
        (9, 3, 4, 16)
    );
    let mut f = output();
    f[21] = 0;
    assert_eq!(decode_output_bindings(&f).unwrap()[1].slot, 0); // later register write retained
    f[21] = 16;
    assert_eq!(decode_output_bindings(&f), Err(BindingError::Slot(16)));
    assert_eq!(
        decode_output_bindings(&output()[..23]),
        Err(BindingError::Extent)
    );
}
/// (semantic, slot, format, components, byte offset, encoded bytes)
type Shape = (u8, u8, u8, u8, u8, u8);
#[test]
fn builtin_output_selectors_preserve_source_registers_stride_and_distinct_tangent_precision() {
    let shapes: &[&[Shape]] = &[
        &[(1, 0, 2, 3, 0, 12)],
        &[
            (1, 0, 2, 3, 0, 12),
            (2, 2, 6, 1, 12, 4),
            (3, 14, 6, 1, 16, 4),
        ],
        &[
            (1, 0, 2, 3, 0, 12),
            (2, 2, 6, 1, 12, 4),
            (3, 14, 1, 4, 16, 8),
        ],
        &[
            (1, 0, 2, 3, 0, 12),
            (2, 2, 6, 1, 12, 4),
            (3, 14, 6, 1, 16, 4),
            (4, 15, 6, 1, 20, 4),
        ],
    ];
    for (selector, expected) in shapes.iter().enumerate() {
        let bindings = builtin_output_bindings(selector as u8).unwrap();
        assert_eq!(bindings.len(), expected.len());
        for (ordinal, (binding, expected)) in bindings.iter().zip(expected.iter()).enumerate() {
            assert_eq!(binding.declaration, ordinal);
            assert_eq!(binding.stride, [12, 20, 24, 24][selector]);
            assert_eq!(
                (
                    binding.semantic,
                    binding.slot,
                    binding.format,
                    binding.components,
                    binding.byte_offset,
                    binding.encoded_bytes
                ),
                *expected
            );
        }
    }
    assert_eq!(
        builtin_output_bindings(4),
        Err(BindingError::UnsupportedBuiltin(4))
    );
}
fn auxiliary() -> ([u8; 544], [u8; 656]) {
    let mut p = [0; 544];
    p[0x112] = 1;
    p[0x111] = 4;
    p[0x118..0x120].copy_from_slice(&[0, 3, 2, 5, 4, 8, 0, 0]);
    let mut v = [0; 656];
    word(&mut v, 8 * 8, 0x0004_1760);
    word(&mut v, 8 * 8 + 4, 0x423);
    word(&mut v, 0x120 + 8 * 20, 1);
    (p, v)
}
#[test]
fn auxiliary_offset_and_location_are_source_derived_and_actual_vertex_state_controls_format() {
    let (p, mut v) = auxiliary();
    let plan = plan_auxiliary_bindings(&p, &v, 0x8000_1234, 0x20, 1 << 14).unwrap();
    assert_eq!(plan.len(), 1);
    let a = &plan[0];
    assert_eq!(
        (
            a.binding.semantic,
            a.binding.slot,
            a.binding.format,
            a.binding.components
        ),
        (5, 8, 3, 2)
    );
    assert_eq!((a.location, a.offset, a.binding.stride), (1, 0x1254, 4));
    word(&mut v, 8 * 8 + 4, 0x832); // actual state: Float32x3 stride8; raw metadata differs
    assert_eq!(
        plan_auxiliary_bindings(&p, &v, 0, 0, 1 << 14).unwrap()[0]
            .binding
            .format,
        2
    );
    assert!(plan_auxiliary_bindings(&p, &v, 0, 0, 0).unwrap().is_empty());
    assert_eq!(
        plan_auxiliary_bindings(&p, &v, 0x7fff_ffff, 1, 1 << 14),
        Err(BindingError::Range)
    );
}
#[test]
fn gpu_half_flushes_subnormal_preserves_specials_and_differs_from_spu_rebased16() {
    let binding = VertexBinding {
        declaration: 0,
        semantic: 5,
        slot: 8,
        format: 3,
        components: 2,
        stride: 4,
        byte_offset: 0,
        encoded_bytes: 4,
    };
    let rows = decode_gpu_attribute(
        &binding,
        &[0, 1, 0x80, 1, 0x7c, 0, 0x7e, 1, 0x3c, 0, 0xbc, 0],
        3,
        48,
    )
    .unwrap();
    assert_eq!(
        rows,
        [
            [0, 0x80000000, 0, 0x3f800000],
            [0x7f800000, 0x7fc02000, 0, 0x3f800000],
            [0x3f800000, 0xbf800000, 0, 0x3f800000]
        ]
    );
    assert_eq!(
        decode_gpu_attribute(&binding, &[0; 12], 3, 47),
        Err(BindingError::Budget)
    );
    assert_eq!(
        decode_gpu_attribute(&binding, &[0; 11], 3, 48),
        Err(BindingError::Extent)
    );
}
#[test]
fn source_gpu_byte_normalization_is_not_spu_affine_and_stride_zero_repeats_element() {
    let b = VertexBinding {
        declaration: 0,
        semantic: 9,
        slot: 3,
        format: 4,
        components: 4,
        stride: 0,
        byte_offset: 0,
        encoded_bytes: 4,
    };
    let r = decode_gpu_attribute(&b, &[0, 1, 128, 255], 2, 32).unwrap();
    assert_eq!(
        r[0],
        [
            0,
            (1f32 / 255.).to_bits(),
            (128f32 / 255.).to_bits(),
            1f32.to_bits()
        ]
    );
    assert_eq!(r[0], r[1]);
}
#[test]
fn actual_auxiliary_frequency_uses_the_source_divider_pair_instead_of_a_default() {
    let (p, mut v) = auxiliary();
    word(&mut v, 8 * 8 + 4, 0x0002_0423);
    word(&mut v, 0x80, 0x0004_1fc0);
    word(&mut v, 0x84, 1 << 8);
    let plan = plan_auxiliary_bindings(&p, &v, 0, 0, 1 << 14).unwrap();
    let a = &plan[0];
    let data = [0x3c, 0, 0, 0, 0, 0, 0x3c, 0];
    assert_eq!(auxiliary_source_bytes(a, 5).unwrap(), 8);
    let out = decode_auxiliary_attribute(a, &data, 5, 80).unwrap();
    assert_eq!(out[0], out[2]);
    assert_eq!(out[1], out[3]);
    assert_ne!(out[0], out[1]);
    word(&mut v, 0x84, 0);
    let div = plan_auxiliary_bindings(&p, &v, 0, 0, 1 << 14).unwrap();
    assert_eq!(auxiliary_source_bytes(&div[0], 4).unwrap(), 8);
    let out = decode_auxiliary_attribute(&div[0], &data, 4, 64).unwrap();
    assert_eq!(out[0], out[1]);
    assert_eq!(out[2], out[3]);
    assert_ne!(out[1], out[2]);
    word(&mut v, 0x80, 0);
    assert_eq!(
        plan_auxiliary_bindings(&p, &v, 0, 0, 1 << 14),
        Err(BindingError::StateHeader(0))
    );
}
