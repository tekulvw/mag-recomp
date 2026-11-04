//! Source-qualified fragment builtin requests and direct values for one material.
//! Every active record survives as a request/no-op/unqualified operation.
use super::{ConstantRowWrite, ParameterTarget, alpha_transfer_values};

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum BuiltinError {
    Bounds,
    Mapping,
    Allocation,
    MissingSource,
    UnqualifiedSemantic(u32),
    UnqualifiedDestination {
        semantic: u32,
        target: ParameterTarget,
    },
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum FragmentBuiltinRequest {
    AlphaTransfer {
        record_index: usize,
        logical_slot: u16,
        primary_alpha: u8,
    },
    Matrix4 {
        record_index: usize,
        logical_slot: u16,
    },
    NoOp {
        record_index: usize,
    },
    OtherDestination {
        record_index: usize,
        semantic: u32,
        target: ParameterTarget,
    },
    Unqualified {
        record_index: usize,
        semantic: u32,
    },
    /// Kinds 31/32: a block of rows derived from the selected material's
    /// role-tagged parameter values and the pass time scalar.
    Animated {
        record_index: usize,
        semantic: u32,
        logical_slot: u16,
        roles: RoleValues,
    },
}
/// Last value per parameter role (record half +14) of the selected material,
/// up to three words each; `None` when no record carries that role.
pub type RoleValues = [Option<[u32; 3]>; 8];
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct BuiltinPassValues {
    pub matrix_words: Option<[u32; 16]>,
    pub alpha_modulation: Option<u32>,
    /// Pass binding payload scalar at +0x1E0, read by kinds 31/32.
    pub animation_time: Option<u32>,
}
/// Role values come from every record of the material's parameter table
/// (records at half(0x44), values at half(0x46), byte(0x52) records), not only
/// the technique's ordinary run. A later record with the same role wins.
fn role_values(material: &[u8]) -> Result<RoleValues, BuiltinError> {
    let records = usize::from(half(material, 0x44)?);
    let values = usize::from(half(material, 0x46)?);
    let count = usize::from(*material.get(0x52).ok_or(BuiltinError::Bounds)?);
    let mut roles: RoleValues = [None; 8];
    for index in 0..count {
        let record = records
            .checked_add(index * 16)
            .ok_or(BuiltinError::Bounds)?;
        let role = usize::from(half(material, record + 14)?);
        if role >= roles.len() {
            continue;
        }
        let at = values
            .checked_add(usize::from(half(material, record + 8)?) * 4)
            .ok_or(BuiltinError::Bounds)?;
        // Roles read one word except 0 (two) and 6 (three); unread words stay 0.
        let needed = match role {
            0 => 2,
            6 => 3,
            _ => 1,
        };
        let mut words = [0; 3];
        for (i, w) in words.iter_mut().take(needed).enumerate() {
            *w = word(material, at + 4 * i)?;
        }
        roles[role] = Some(words);
    }
    Ok(roles)
}
fn role(roles: &RoleValues, index: usize) -> Result<[f32; 3], BuiltinError> {
    roles[index]
        .map(|w| w.map(f32::from_bits))
        .ok_or(BuiltinError::MissingSource)
}
/// Kind 31: two scrolling directions plus a rotating offset scaled by time,
/// and two scale rows. The record and role layouts are those of
/// `fragment-builtins.md`; the scale factors are the game's values.
pub fn animated_rows_31(roles: &RoleValues, time: f32) -> Result<[[f32; 4]; 4], BuiltinError> {
    let [p, q, _] = role(roles, 0)?;
    let [d, ..] = role(roles, 1)?;
    let [w, ..] = role(roles, 2)?;
    let u = time * (p / d);
    let s = time * (q / d);
    let angle = core::f32::consts::PI * w;
    let (a, b) = (s * angle.cos(), s * angle.sin());
    Ok([
        [u * -0.866 + a, u * 0.5 + b, a, -u + b],
        [u * 0.866 + a, u * 0.5 + b, a, -u + b],
        [d, d, d * -0.93, d * 0.93],
        [d * -0.97, d * 0.97, -d, -d],
    ])
}
/// Kind 32: two rows of products; lanes the source leaves unwritten are zero.
pub fn animated_rows_32(roles: &RoleValues, time: f32) -> Result<[[f32; 4]; 2], BuiltinError> {
    let [a3, ..] = role(roles, 3)?;
    let [a4, ..] = role(roles, 4)?;
    let [a5, ..] = role(roles, 5)?;
    let [b0, b1, b2] = role(roles, 6)?;
    let [a7, ..] = role(roles, 7)?;
    Ok([
        [a3 * a4, a5 * time, 0., 0.],
        [b0 * a7, b1 * a7, b2 * a7, 0.],
    ])
}
fn word(b: &[u8], at: usize) -> Result<u32, BuiltinError> {
    Ok(u32::from_be_bytes(
        b.get(at..at.checked_add(4).ok_or(BuiltinError::Bounds)?)
            .ok_or(BuiltinError::Bounds)?
            .try_into()
            .map_err(|_| BuiltinError::Bounds)?,
    ))
}
fn half(b: &[u8], at: usize) -> Result<u16, BuiltinError> {
    Ok(u16::from_be_bytes(
        b.get(at..at.checked_add(2).ok_or(BuiltinError::Bounds)?)
            .ok_or(BuiltinError::Bounds)?
            .try_into()
            .map_err(|_| BuiltinError::Bounds)?,
    ))
}
pub fn decode_fragment_builtin_requests(
    selected: &[u8],
    primary: &[u8],
    selector: u32,
    chunk: &[u8],
) -> Result<Vec<FragmentBuiltinRequest>, BuiltinError> {
    let technique = usize::from(half(selected, 0x48)?)
        .checked_add(
            (selector as usize)
                .checked_mul(32)
                .ok_or(BuiltinError::Bounds)?,
        )
        .ok_or(BuiltinError::Bounds)?;
    let map = *selected.get(technique + 1).ok_or(BuiltinError::Bounds)?;
    if map >= 32 {
        return Err(BuiltinError::Mapping);
    }
    let count = usize::from(*selected.get(0x53).ok_or(BuiltinError::Bounds)?);
    if count == 0 {
        return Ok(Vec::new());
    }
    let table = usize::from(half(selected, 0x42)?);
    let records = selected
        .get(table..table + count * 16)
        .ok_or(BuiltinError::Bounds)?;
    let mut requests = Vec::new();
    requests
        .try_reserve_exact(count)
        .map_err(|_| BuiltinError::Allocation)?;
    for (record_index, record) in records.as_chunks::<16>().0.iter().enumerate() {
        if word(record, 8)? & (1u32 << map) == 0 {
            continue;
        }
        let semantic = word(record, 4)?;
        // Only kinds 3, 4, 31 and 32 are built-ins. Every other kind,
        // including 5..=30 and anything above 32, is a no-op whose
        // destination word is never read.
        if !matches!(semantic, 3 | 4 | 31 | 32) {
            requests.push(FragmentBuiltinRequest::NoOp { record_index });
            continue;
        }
        let descriptor = word(chunk, word(record, 12)? as usize)?;
        let slot = ((descriptor >> 14) & 1023) as u16;
        let target = match ((descriptor >> 24) & 15) as u8 {
            1 => ParameterTarget::ConstantVertexAttribute(slot),
            2 => ParameterTarget::VertexConstants(slot),
            3 => ParameterTarget::FragmentConstants(slot),
            kind => ParameterTarget::IgnoredKind(kind),
        };
        if !matches!(target, ParameterTarget::FragmentConstants(_)) {
            requests.push(FragmentBuiltinRequest::OtherDestination {
                record_index,
                semantic,
                target,
            });
            continue;
        }
        if matches!(semantic, 31 | 32) {
            let ParameterTarget::FragmentConstants(logical_slot) = target else {
                unreachable!("checked fragment destination")
            };
            requests.push(FragmentBuiltinRequest::Animated {
                record_index,
                semantic,
                logical_slot,
                roles: role_values(selected)?,
            });
            continue;
        }
        requests.push(if semantic == 3 {
            FragmentBuiltinRequest::AlphaTransfer {
                record_index,
                logical_slot: slot,
                primary_alpha: *primary.get(0x5c).ok_or(BuiltinError::Bounds)?,
            }
        } else {
            FragmentBuiltinRequest::Matrix4 {
                record_index,
                logical_slot: slot,
            }
        });
    }
    Ok(requests)
}
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
                let time = f32::from_bits(
                    values
                        .animation_time
                        .ok_or(BuiltinError::MissingSource)
                        .map_err(unqualified)?,
                );
                let block: Vec<[f32; 4]> = if semantic == 31 {
                    animated_rows_31(&roles, time)
                        .map_err(unqualified)?
                        .to_vec()
                } else {
                    animated_rows_32(&roles, time)
                        .map_err(unqualified)?
                        .to_vec()
                };
                if usize::from(logical_slot) + block.len() > 1024 {
                    return Err(BuiltinError::Mapping);
                }
                rows.try_reserve(block.len())
                    .map_err(|_| BuiltinError::Allocation)?;
                for (i, row) in block.into_iter().enumerate() {
                    rows.push(ConstantRowWrite {
                        logical_slot: u32::from(logical_slot) + i as u32,
                        words: row.map(f32::to_bits),
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
    fn word(b: &mut [u8], at: usize, value: u32) {
        b[at..at + 4].copy_from_slice(&value.to_be_bytes());
    }
    #[test]
    fn source_masks_do_not_terminate_builtin_scan_and_noop_has_no_descriptor_read() {
        let mut selected = vec![0; 192];
        let mut primary = selected.clone();
        let mut chunk = vec![0; 8];
        selected[0x42..0x44].copy_from_slice(&96u16.to_be_bytes());
        selected[0x48..0x4a].copy_from_slice(&160u16.to_be_bytes());
        selected[0x53] = 4;
        primary[0x5c] = 7;
        for (i, semantic) in [3, 4, 5, 3].into_iter().enumerate() {
            let at = 96 + i * 16;
            word(&mut selected, at + 4, semantic);
            word(&mut selected, at + 8, if i == 1 { 0 } else { 1 });
            word(
                &mut selected,
                at + 12,
                if i == 0 {
                    0
                } else if i == 2 {
                    u32::MAX
                } else {
                    4
                },
            );
        }
        word(&mut chunk, 0, (3 << 24) | (8 << 14));
        word(&mut chunk, 4, (3 << 24) | (9 << 14));
        let requests = decode_fragment_builtin_requests(&selected, &primary, 0, &chunk).unwrap();
        assert_eq!(
            requests,
            [
                FragmentBuiltinRequest::AlphaTransfer {
                    record_index: 0,
                    logical_slot: 8,
                    primary_alpha: 7
                },
                FragmentBuiltinRequest::NoOp { record_index: 2 },
                FragmentBuiltinRequest::AlphaTransfer {
                    record_index: 3,
                    logical_slot: 9,
                    primary_alpha: 7
                }
            ]
        );
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
    fn kind_31_rows_follow_role_values_and_time() {
        let m = role_material(&[(1, &[9.]), (0, &[2., 4.]), (2, &[0.5]), (1, &[2.])]);
        let roles = role_values(&m).unwrap();
        // The later role1 record wins.
        assert_eq!(roles[1].map(|w| f32::from_bits(w[0])), Some(2.));
        let rows = animated_rows_31(&roles, 3.).unwrap();
        // u = 3*(2/2) = 3, s = 3*(4/2) = 6, angle = pi/2 -> a ~ 0, b = 6.
        let close = |x: f32, y: f32| (x - y).abs() < 1e-4;
        let expect = [
            [3. * -0.866, 3. * 0.5 + 6., 0., -3. + 6.],
            [3. * 0.866, 3. * 0.5 + 6., 0., -3. + 6.],
            [2., 2., -1.86, 1.86],
            [-1.94, 1.94, -2., -2.],
        ];
        for (r, e) in rows.iter().zip(expect) {
            assert!(r.iter().zip(e).all(|(a, b)| close(*a, b)), "{rows:?}");
        }
        // A missing role refuses instead of reading an unrelated value.
        let partial = role_values(&role_material(&[(0, &[1., 1.]), (1, &[1.])])).unwrap();
        assert_eq!(
            animated_rows_31(&partial, 1.),
            Err(BuiltinError::MissingSource)
        );
    }
    #[test]
    fn kind_32_rows_are_role_products_with_unwritten_lanes_zero() {
        let m = role_material(&[
            (3, &[2.]),
            (4, &[3.]),
            (5, &[5.]),
            (6, &[1., 2., 3.]),
            (7, &[4.]),
        ]);
        let rows = animated_rows_32(&role_values(&m).unwrap(), 0.5).unwrap();
        assert_eq!(rows, [[6., 2.5, 0., 0.], [4., 8., 12., 0.]]);
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
}
