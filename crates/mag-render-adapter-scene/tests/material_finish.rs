use mag_render_adapter_scene::material_finish::{
    FinishError, FinishRequirement, OwnedMainFinish, SourceFinishWords,
};
fn word(bytes: &mut [u8], at: usize, value: u32) {
    bytes[at..at + 4].copy_from_slice(&value.to_be_bytes());
}
fn user(enabled: bool) -> [u8; 160] {
    let mut user = [0; 160];
    user[0x4c] = u8::from(enabled);
    user[0x30..0x32].copy_from_slice(&7u16.to_be_bytes());
    user[0x4e..0x50].copy_from_slice(&11u16.to_be_bytes());
    for offset in (0..0x30).step_by(4).chain((0x64..=0x90).step_by(4)) {
        word(&mut user, offset, 0x10000 + offset as u32 * 4);
    }
    word(&mut user, 0x40, 0x10100);
    word(&mut user, 0x44, 0x10110);
    user
}
fn words() -> SourceFinishWords {
    SourceFinishWords {
        reference_increment_address: 0x20000,
        reference_exchange_address: 0x20004,
        auxiliary_table_address: 0x30000,
        auxiliary_table_count: 13,
        pass_flags: 3,
    }
}
#[test]
fn owns_terminal_sources_and_preserves_actual_call_order_and_report_offsets() {
    let mut bytes = user(true);
    let owned = OwnedMainFinish::decode(0x40000, 0x50000, &bytes, words()).unwrap();
    bytes.fill(0);
    assert_eq!(owned.source_user_address, 0x40000);
    assert_eq!(owned.source_pass_address, 0x50000);
    assert_eq!(owned.writer.ring_start, 0x10000);
    assert_eq!(owned.writer.descriptor_ring_address, 0x10110);
    assert_eq!(owned.source_words.auxiliary_table_count, 13);
    let required = owned.terminal_requirements().unwrap();
    assert_eq!(
        required[0],
        FinishRequirement::AuxiliaryDepthZero {
            restore_target: 0x101b0
        }
    );
    assert_eq!(
        required[1],
        FinishRequirement::ReferencePublication {
            increment_address: 0x20000,
            exchange_address: 0x20004,
        }
    );
    assert_eq!(
        required[2],
        FinishRequirement::Report {
            report_type: 1,
            offset: 176
        }
    );
    assert_eq!(
        required[3],
        FinishRequirement::Report {
            report_type: 1,
            offset: 112
        }
    );
    let calls: Vec<_> = required
        .iter()
        .filter_map(|r| match r {
            FinishRequirement::Continuation { user_offset, .. } => Some(*user_offset),
            _ => None,
        })
        .collect();
    assert_eq!(
        calls,
        [0x6c, 0x70, 0x74, 0x78, 0x7c, 0x80, 0x8c, 0x90, 0x84, 0x88]
    );
    assert_eq!(
        required.last(),
        Some(&FinishRequirement::FinalProducerPublication)
    );
}
#[test]
fn disabled_lists_do_not_require_unused_reference_or_cleanup_targets() {
    let mut bytes = user(false);
    bytes[0x30..0x32].fill(0);
    word(&mut bytes, 0x6c, 3);
    for at in (0x70..0x80).step_by(4) {
        word(&mut bytes, at, 3);
    }
    word(&mut bytes, 0x90, u32::MAX);
    let mut source = words();
    source.reference_increment_address = 0;
    source.reference_exchange_address = 3;
    source.pass_flags = 0;
    let owned = OwnedMainFinish::decode(1, 2, &bytes, source).unwrap();
    let required = owned.terminal_requirements().unwrap();
    assert_eq!(
        required,
        vec![
            FinishRequirement::ResetVertexArrays,
            FinishRequirement::Continuation {
                user_offset: 0x80,
                target: 0x10200
            },
            FinishRequirement::Continuation {
                user_offset: 0x8c,
                target: 0x10230
            },
            FinishRequirement::Continuation {
                user_offset: 0x84,
                target: 0x10210
            },
            FinishRequirement::Continuation {
                user_offset: 0x88,
                target: 0x10220
            },
            FinishRequirement::ResetMaterialState,
            FinishRequirement::FinalProducerPublication,
        ]
    );
}
#[test]
fn actual_optional_order_and_shared_word_alias_are_not_old_worker_restrictions() {
    let bytes = user(true);
    let mut source = words();
    source.reference_exchange_address = source.reference_increment_address;
    source.pass_flags = 0;
    let owned = OwnedMainFinish::decode(1, 2, &bytes, source).unwrap();
    let required = owned.terminal_requirements().unwrap();
    let calls: Vec<_> = required
        .iter()
        .filter_map(|r| match r {
            FinishRequirement::Continuation { user_offset, .. } => Some(*user_offset),
            _ => None,
        })
        .collect();
    assert_eq!(
        calls,
        [0x70, 0x74, 0x78, 0x7c, 0x80, 0x8c, 0x84, 0x90, 0x88]
    );
}
#[test]
fn missing_spans_and_active_invalid_effects_refuse_without_a_completion_receipt() {
    assert_eq!(
        OwnedMainFinish::decode(1, 2, &[0; 159], words()),
        Err(FinishError::UserExtent)
    );
    let mut bytes = user(true);
    let mut source = words();
    source.reference_increment_address = 3;
    assert_eq!(
        OwnedMainFinish::decode(1, 2, &bytes, source)
            .unwrap()
            .terminal_requirements(),
        Err(FinishError::ReferenceAddress(3))
    );
    word(&mut bytes, 0x88, 0);
    assert_eq!(
        OwnedMainFinish::decode(1, 2, &bytes, words())
            .unwrap()
            .terminal_requirements(),
        Err(FinishError::Continuation {
            user_offset: 0x88,
            target: 0
        })
    );
}
