//! Fragment constant rows for a material's decoded built-in requests.
//!
//! Alpha transfer (kind 3) and the pass matrix (kind 4) are written from the
//! pass values the capture read. The animated kinds 31 and 32 are computed by
//! the clean-room behaviour crate through `mag_render_behavior_glue::builtins`,
//! with its scale factors. Requests are decoded by
//! `mag_render_adapter_core::material_constants::decode_fragment_builtin_requests`.
use mag_render_adapter_core::material_constants::{
    BuiltinError, BuiltinPassValues, ConstantRowWrite, FragmentBuiltinRequest,
    alpha_transfer_values,
};
pub fn prepare_fragment_builtin_rows(
    requests: &[FragmentBuiltinRequest],
    values: BuiltinPassValues,
) -> Result<Vec<ConstantRowWrite>, BuiltinError> {
    let mut rows = Vec::new();
    for request in requests {
        match *request {
            FragmentBuiltinRequest::AlphaTransfer {
                logical_slot,
                primary_alpha,
                ..
            } => {
                let scalar = values.alpha_modulation.ok_or(BuiltinError::MissingSource)?;
                rows.try_reserve(1).map_err(|_| BuiltinError::Allocation)?;
                rows.push(ConstantRowWrite {
                    logical_slot: u32::from(logical_slot),
                    words: alpha_transfer_values(primary_alpha, scalar),
                });
            }
            FragmentBuiltinRequest::Matrix4 { logical_slot, .. } => {
                if logical_slot > 1020 {
                    return Err(BuiltinError::Mapping);
                }
                let words = values.matrix_words.ok_or(BuiltinError::MissingSource)?;
                rows.try_reserve(4).map_err(|_| BuiltinError::Allocation)?;
                for (i, words) in words.as_chunks::<4>().0.iter().enumerate() {
                    rows.push(ConstantRowWrite {
                        logical_slot: u32::from(logical_slot) + i as u32,
                        words: *words,
                    });
                }
            }
            FragmentBuiltinRequest::Animated {
                semantic,
                logical_slot,
                roles,
                ..
            } => {
                // A missing role or time keeps the request explicitly
                // unqualified, so the caller's omission path handles it.
                let unqualified = |_| BuiltinError::UnqualifiedSemantic(semantic);
                let time = values
                    .animation_time
                    .ok_or(BuiltinError::MissingSource)
                    .map_err(unqualified)?;
                // The rows, scale factors and refusals are the behaviour
                // crate's (`fragment-builtins.md`); any refusal keeps the
                // request unqualified.
                let block =
                    mag_render_behavior_glue::builtins::animated_rows(semantic, &roles, time)
                        .map_err(|_| BuiltinError::UnqualifiedSemantic(semantic))?;
                if usize::from(logical_slot) + block.len() > 1024 {
                    return Err(BuiltinError::Mapping);
                }
                rows.try_reserve(block.len())
                    .map_err(|_| BuiltinError::Allocation)?;
                for (i, row) in block.into_iter().enumerate() {
                    rows.push(ConstantRowWrite {
                        logical_slot: u32::from(logical_slot) + i as u32,
                        words: row,
                    });
                }
            }
            FragmentBuiltinRequest::NoOp { .. } => {}
            // Built-ins write fragment constants only. A built-in whose
            // destination is a vertex constant or a current attribute writes
            // nothing here; those destinations have their own writers.
            FragmentBuiltinRequest::OtherDestination {
                semantic: 3 | 4 | 31 | 32,
                ..
            } => {}
            FragmentBuiltinRequest::OtherDestination {
                semantic, target, ..
            } => return Err(BuiltinError::UnqualifiedDestination { semantic, target }),
            FragmentBuiltinRequest::Unqualified { semantic, .. } => {
                return Err(BuiltinError::UnqualifiedSemantic(semantic));
            }
        }
    }
    Ok(rows)
}

#[cfg(test)]
mod tests {
    use super::*;
    use mag_render_adapter_core::material_constants::{
        ParameterTarget, decode_fragment_builtin_requests,
    };
    fn word(b: &mut [u8], at: usize, value: u32) {
        b[at..at + 4].copy_from_slice(&value.to_be_bytes());
    }
    #[test]
    fn source_out_of_dispatch_semantics_zero_one_two_are_noops_without_descriptor_reads() {
        let mut selected = vec![0; 192];
        selected[0x42..0x44].copy_from_slice(&96u16.to_be_bytes());
        selected[0x48..0x4a].copy_from_slice(&160u16.to_be_bytes());
        selected[0x53] = 3;
        for (i, semantic) in [0, 1, 2].into_iter().enumerate() {
            let at = 96 + 16 * i;
            word(&mut selected, at + 4, semantic);
            word(&mut selected, at + 8, 1);
            word(&mut selected, at + 12, u32::MAX);
        }
        let requests = decode_fragment_builtin_requests(&selected, &[], 0, &[]).unwrap();
        assert_eq!(
            requests,
            (0..3)
                .map(|record_index| FragmentBuiltinRequest::NoOp { record_index })
                .collect::<Vec<_>>()
        );
        assert!(
            prepare_fragment_builtin_rows(&requests, BuiltinPassValues::default())
                .unwrap()
                .is_empty()
        );
        // Kinds 31/32 resolve their destination descriptor like kind 4.
        for semantic in [31, 32] {
            word(&mut selected, 100, semantic);
            assert_eq!(
                decode_fragment_builtin_requests(&selected, &[], 0, &[]),
                Err(BuiltinError::Bounds)
            );
        }
    }
    fn role_material(roles: &[(u16, &[f32])]) -> Vec<u8> {
        // Records at 0x100, values at 0x200, one record per role.
        let mut m = vec![0; 0x300];
        m[0x44..0x46].copy_from_slice(&0x100u16.to_be_bytes());
        m[0x46..0x48].copy_from_slice(&0x200u16.to_be_bytes());
        m[0x52] = roles.len() as u8;
        let mut next = 0u16;
        for (i, (role, values)) in roles.iter().enumerate() {
            let record = 0x100 + 16 * i;
            m[record + 8..record + 10].copy_from_slice(&next.to_be_bytes());
            m[record + 14..record + 16].copy_from_slice(&role.to_be_bytes());
            for (j, v) in values.iter().enumerate() {
                let at = 0x200 + 4 * (usize::from(next) + j);
                m[at..at + 4].copy_from_slice(&v.to_bits().to_be_bytes());
            }
            next += values.len() as u16;
        }
        m
    }
    #[test]
    fn animated_requests_write_consecutive_fragment_rows_from_the_destination_slot() {
        let mut selected = role_material(&[
            (3, &[2.]),
            (4, &[3.]),
            (5, &[5.]),
            (6, &[1., 2., 3.]),
            (7, &[4.]),
        ]);
        // One builtin record (kind 32) at 0x60 with map bit 0, technique at 0x80.
        selected[0x42..0x44].copy_from_slice(&0x60u16.to_be_bytes());
        selected[0x48..0x4a].copy_from_slice(&0x80u16.to_be_bytes());
        selected[0x53] = 1;
        word(&mut selected, 0x64, 32);
        word(&mut selected, 0x68, 1);
        word(&mut selected, 0x6c, 0);
        let mut chunk = vec![0; 4];
        word(&mut chunk, 0, (3 << 24) | (10 << 14));
        let requests = decode_fragment_builtin_requests(&selected, &selected, 0, &chunk).unwrap();
        assert!(matches!(
            requests.as_slice(),
            [FragmentBuiltinRequest::Animated {
                semantic: 32,
                logical_slot: 10,
                ..
            }]
        ));
        // Without the pass time scalar the request stays explicitly unqualified.
        assert_eq!(
            prepare_fragment_builtin_rows(&requests, BuiltinPassValues::default()),
            Err(BuiltinError::UnqualifiedSemantic(32))
        );
        let rows = prepare_fragment_builtin_rows(
            &requests,
            BuiltinPassValues {
                animation_time: Some(0.5f32.to_bits()),
                ..BuiltinPassValues::default()
            },
        )
        .unwrap();
        assert_eq!(rows.len(), 2);
        assert_eq!((rows[0].logical_slot, rows[1].logical_slot), (10, 11));
        assert_eq!(rows[1].words, [4f32, 8., 12., 0.].map(f32::to_bits));
        // A non-fragment destination writes nothing, like kind 4.
        word(&mut chunk, 0, (2 << 24) | (10 << 14));
        let requests = decode_fragment_builtin_requests(&selected, &selected, 0, &chunk).unwrap();
        assert!(
            prepare_fragment_builtin_rows(&requests, BuiltinPassValues::default())
                .unwrap()
                .is_empty()
        );
    }
    #[test]
    fn complete_source_builtin_dispatch_preserves_computations_and_ignores_all_other_semantics() {
        let mut selected = vec![0; 192];
        selected[0x42..0x44].copy_from_slice(&96u16.to_be_bytes());
        selected[0x48..0x4a].copy_from_slice(&160u16.to_be_bytes());
        selected[0x53] = 1;
        selected[0x5c] = 7;
        let mut chunk = vec![0; 4];
        word(&mut chunk, 0, (3 << 24) | (8 << 14));
        word(&mut selected, 104, 1);
        for semantic in (0..=33).chain([u32::MAX, 0x80000000]) {
            word(&mut selected, 100, semantic);
            word(
                &mut selected,
                108,
                if matches!(semantic, 3 | 4 | 31 | 32) {
                    0
                } else {
                    u32::MAX
                },
            );
            let requests =
                decode_fragment_builtin_requests(&selected, &selected, 0, &chunk).unwrap();
            match semantic {
                3 => assert!(matches!(
                    requests.as_slice(),
                    [FragmentBuiltinRequest::AlphaTransfer {
                        logical_slot: 8,
                        primary_alpha: 7,
                        ..
                    }]
                )),
                4 => assert!(matches!(
                    requests.as_slice(),
                    [FragmentBuiltinRequest::Matrix4 {
                        logical_slot: 8,
                        ..
                    }]
                )),
                31 | 32 => assert_eq!(
                    prepare_fragment_builtin_rows(&requests, BuiltinPassValues::default()),
                    Err(BuiltinError::UnqualifiedSemantic(semantic))
                ),
                _ => {
                    assert_eq!(
                        requests,
                        [FragmentBuiltinRequest::NoOp { record_index: 0 }],
                        "semantic{semantic}"
                    );
                    assert!(
                        prepare_fragment_builtin_rows(&requests, BuiltinPassValues::default())
                            .unwrap()
                            .is_empty()
                    );
                }
            }
        }
    }
    #[test]
    fn alpha_and_matrix_builtin_only_write_fragment_destinations() {
        let mut selected = vec![0; 192];
        selected[0x42..0x44].copy_from_slice(&96u16.to_be_bytes());
        selected[0x48..0x4a].copy_from_slice(&160u16.to_be_bytes());
        selected[0x53] = 1;
        selected[0x5c] = 7;
        word(&mut selected, 104, 1);
        let mut chunk = vec![0; 4];
        for semantic in [3, 4] {
            word(&mut selected, 100, semantic);
            for kind in 0..16 {
                word(&mut chunk, 0, (kind << 24) | (8 << 14));
                let requests =
                    decode_fragment_builtin_requests(&selected, &selected, 0, &chunk).unwrap();
                let rows = prepare_fragment_builtin_rows(&requests, BuiltinPassValues::default());
                if kind == 3 {
                    assert_eq!(rows, Err(BuiltinError::MissingSource));
                } else {
                    assert_eq!(rows, Ok(vec![]), "semantic{semantic}kind{kind}");
                }
            }
        }
        // Kinds 31/32 deliver only to fragment constants, like kind 4: a
        // vertex destination writes nothing. Other kinds stay unqualified.
        assert!(
            prepare_fragment_builtin_rows(
                &[FragmentBuiltinRequest::OtherDestination {
                    record_index: 0,
                    semantic: 31,
                    target: ParameterTarget::VertexConstants(8)
                }],
                BuiltinPassValues::default()
            )
            .unwrap()
            .is_empty()
        );
        assert!(matches!(
            prepare_fragment_builtin_rows(
                &[FragmentBuiltinRequest::OtherDestination {
                    record_index: 0,
                    semantic: 9,
                    target: ParameterTarget::VertexConstants(8)
                }],
                BuiltinPassValues::default()
            ),
            Err(BuiltinError::UnqualifiedDestination { semantic: 9, .. })
        ));
    }
    #[test]
    fn actual_matrix_and_modulation_sources_are_required_and_all_rows_survive() {
        let requests = [
            FragmentBuiltinRequest::Matrix4 {
                record_index: 0,
                logical_slot: 4,
            },
            FragmentBuiltinRequest::AlphaTransfer {
                record_index: 1,
                logical_slot: 5,
                primary_alpha: 0,
            },
        ];
        assert_eq!(
            prepare_fragment_builtin_rows(&requests, BuiltinPassValues::default()),
            Err(BuiltinError::MissingSource)
        );
        let values = BuiltinPassValues {
            animation_time: None,
            matrix_words: Some(std::array::from_fn(|i| i as u32)),
            alpha_modulation: Some(0x3f800000),
        };
        let rows = prepare_fragment_builtin_rows(&requests, values).unwrap();
        assert_eq!(rows.len(), 5);
        assert_eq!(rows[0].words, [0, 1, 2, 3]);
        assert_eq!(rows[3].words, [12, 13, 14, 15]);
        assert_eq!(rows[4].logical_slot, 5);
        assert_eq!(rows[4].words, [0x3f800000, 0x3f7fffff, 0, 0x3f800000]);
    }
    /// `role_material` plus one built-in record of `kind` (mask bit 0,
    /// technique at 0x80) whose destination is fragment slot `slot`.
    fn animated_material(kind: u32, roles: &[(u16, &[f32])], slot: u32) -> (Vec<u8>, Vec<u8>) {
        let mut selected = role_material(roles);
        selected[0x42..0x44].copy_from_slice(&0x60u16.to_be_bytes());
        selected[0x48..0x4a].copy_from_slice(&0x80u16.to_be_bytes());
        selected[0x53] = 1;
        word(&mut selected, 0x64, kind);
        word(&mut selected, 0x68, 1);
        word(&mut selected, 0x6c, 0);
        let mut chunk = vec![0; 4];
        word(&mut chunk, 0, (3 << 24) | (slot << 14));
        (selected, chunk)
    }
    fn timed(time: f32) -> BuiltinPassValues {
        BuiltinPassValues {
            animation_time: Some(time.to_bits()),
            ..BuiltinPassValues::default()
        }
    }
    #[test]
    fn kind_31_rows_follow_role_values_time_and_the_behaviour_scale_factors() {
        use mag_render_behavior::fragment_builtins::{K1, K2};
        let (m, chunk) = animated_material(
            31,
            &[(1, &[9.]), (0, &[2., 4.]), (2, &[0.5]), (1, &[2.])],
            20,
        );
        let requests = decode_fragment_builtin_requests(&m, &m, 0, &chunk).unwrap();
        let rows = prepare_fragment_builtin_rows(&requests, timed(3.)).unwrap();
        assert_eq!(
            rows.iter().map(|r| r.logical_slot).collect::<Vec<_>>(),
            [20, 21, 22, 23]
        );
        // The later role-1 record wins (d = 2). u = 3*(2/2) = 3,
        // s = 3*(4/2) = 6, angle = pi/2 -> a ~ 0, b = 6.
        let h = 3f32.sqrt() / 2.;
        let expect = [
            [-h * 3., 3. * 0.5 + 6., 0., -3. + 6.],
            [h * 3., 3. * 0.5 + 6., 0., -3. + 6.],
            [2., 2., -K1 * 2., K1 * 2.],
            [-K2 * 2., K2 * 2., -2., -2.],
        ];
        let close = |x: f32, y: f32| (x - y).abs() < 1e-4;
        for (r, e) in rows.iter().zip(expect) {
            let r = r.words.map(f32::from_bits);
            assert!(r.iter().zip(e).all(|(a, b)| close(*a, b)), "{r:?}");
        }
        // A missing role keeps the request unqualified instead of reading an
        // unrelated value.
        let (m, chunk) = animated_material(31, &[(0, &[1., 1.]), (1, &[1.])], 20);
        let requests = decode_fragment_builtin_requests(&m, &m, 0, &chunk).unwrap();
        assert_eq!(
            prepare_fragment_builtin_rows(&requests, timed(1.)),
            Err(BuiltinError::UnqualifiedSemantic(31))
        );
    }
    #[test]
    fn kind_32_rows_are_role_products_with_unwritten_lanes_zero() {
        let (m, chunk) = animated_material(
            32,
            &[
                (3, &[2.]),
                (4, &[3.]),
                (5, &[5.]),
                (6, &[1., 2., 3.]),
                (7, &[4.]),
            ],
            0,
        );
        let requests = decode_fragment_builtin_requests(&m, &m, 0, &chunk).unwrap();
        let rows = prepare_fragment_builtin_rows(&requests, timed(0.5)).unwrap();
        assert_eq!(
            rows.iter()
                .map(|r| r.words.map(f32::from_bits))
                .collect::<Vec<_>>(),
            [[6., 2.5, 0., 0.], [4., 8., 12., 0.]]
        );
    }
}
