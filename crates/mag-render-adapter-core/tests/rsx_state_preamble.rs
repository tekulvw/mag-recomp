use mag_render_adapter_core::rsx_state_preamble::*;

fn w(method: u32, value: u32) -> MethodWrite {
    MethodWrite {
        subchannel: 0,
        method,
        value,
    }
}

#[test]
fn work_and_no_operations_are_dropped_and_state_keeps_source_order() {
    let writes = [
        w(0x1d6c, 0x490),   // semaphore offset: synchronisation
        w(0x0a74, 1),       // depth test: raster
        w(0x1d94, 0xf3),    // clear: work
        w(0x0100, 0),       // no-operation
        w(0x1808, 5),       // begin: work
        w(0x1814, 0x30000), // draw arrays: work
        w(0x1808, 0),       // end: work
        w(0x0a70, 0),       // depth write: raster
    ];
    let p = encode_state_preamble(&writes, ClassSet::all_state(), 64).unwrap();
    assert_eq!(p.words, [0x0004_0a74, 1, 0x0004_0a70, 0, RETURN]);
    assert_eq!((p.kept, p.dropped), (2, 6));
}

#[test]
fn a_program_upload_keeps_its_slot_data_and_start_order_in_one_packet_per_run() {
    let mut writes = vec![w(0x1e9c, 7)];
    for i in 0..8 {
        writes.push(w(0x0b80 + 4 * i, 0x100 + i));
    }
    writes.push(w(0x1ea0, 7));
    writes.push(w(0x08e4, 0x0003_0321));
    writes.push(w(0x1d60, 0x0200_0400));
    let p =
        encode_state_preamble(&writes, ClassSet::empty().with(MethodClass::Program), 64).unwrap();
    let mut expected = vec![0x0004_1e9c, 7, (8 << 18) | 0x0b80];
    expected.extend((0..8).map(|i| 0x100 + i));
    expected.extend([
        0x0004_1ea0,
        7,
        0x0004_08e4,
        0x0003_0321,
        0x0004_1d60,
        0x0200_0400,
        RETURN,
    ]);
    assert_eq!(p.words, expected);
}

#[test]
fn only_requested_classes_are_kept() {
    let writes = [
        w(0x1738, 0x100),  // vertex base: vertex input
        w(0x1a00, 0x4000), // texture 0 offset: texture binding
        w(0x0210, 0x2000), // colour offset: attachment
        w(0x1efc, 3),      // constant slot: constants
        w(0x1f00, 1),
    ];
    let keep = ClassSet::empty()
        .with(MethodClass::VertexInput)
        .with(MethodClass::VertexConstants);
    let p = encode_state_preamble(&writes, keep, 64).unwrap();
    // The constant slot and its first data word are consecutive methods, so
    // they share one incrementing packet.
    assert_eq!(
        p.words,
        [0x0004_1738, 0x100, (2 << 18) | 0x1efc, 3, 1, RETURN]
    );
    assert_eq!((p.kept, p.dropped), (3, 2));
}

#[test]
fn repeated_writes_to_one_method_stay_separate_writes() {
    let writes = [w(0x0324, 0x0101_0101), w(0x0324, 0)];
    let p = encode_state_preamble(&writes, ClassSet::all_state(), 64).unwrap();
    assert_eq!(p.words, [0x0004_0324, 0x0101_0101, 0x0004_0324, 0, RETURN]);
}

#[test]
fn refusals_happen_before_any_output() {
    assert_eq!(
        encode_state_preamble(&[w(0x0a74, 1), w(0x0004, 1)], ClassSet::all_state(), 64),
        Err(PreambleError::Unclassified {
            index: 1,
            method: 4
        })
    );
    assert_eq!(
        encode_state_preamble(&[w(0x0a76, 1)], ClassSet::all_state(), 64),
        Err(PreambleError::Unclassified {
            index: 0,
            method: 0x0a76
        })
    );
    let other = MethodWrite {
        subchannel: 3,
        method: 0x0180,
        value: 0,
    };
    assert_eq!(
        encode_state_preamble(&[other], ClassSet::all_state(), 64),
        Err(PreambleError::Subchannel {
            index: 0,
            subchannel: 3
        })
    );
    assert_eq!(
        encode_state_preamble(&[], ClassSet::all_state().with(MethodClass::Draw), 64),
        Err(PreambleError::WorkClass(MethodClass::Draw))
    );
    let many: Vec<_> = (0..10).map(|i| w(0x0324, i)).collect();
    assert_eq!(
        encode_state_preamble(&many, ClassSet::all_state(), 20),
        Err(PreambleError::Capacity { words: 21 })
    );
}

#[test]
fn an_empty_replay_is_a_bare_return() {
    let p = encode_state_preamble(&[w(0x1808, 5)], ClassSet::all_state(), 1).unwrap();
    assert_eq!(p.words, [RETURN]);
}

#[test]
fn long_incrementing_runs_split_at_the_packet_limit() {
    let writes: Vec<_> = (0..0x800u32).map(|i| w(0x0b80 + 4 * (i % 32), i)).collect();
    // Every 32 writes the method wraps back to 0x0b80, so runs are 32 long.
    let p = encode_state_preamble(&writes, ClassSet::all_state(), 0x900).unwrap();
    assert_eq!(p.words[0], (32 << 18) | 0x0b80);
    assert_eq!(p.words.len(), 0x800 + 64 + 1);
}

#[test]
fn classes_of_representative_methods() {
    use MethodClass as C;
    for (m, c) in [
        (0x1d74, C::Synchronization),
        (0x0a00, C::Attachment),
        (0x08c0, C::Attachment),
        (0x0a68, C::Raster),
        (0x1d8c, C::Raster),
        (0x1fec, C::Raster),
        (0x181c, C::VertexInput),
        (0x1fc0, C::VertexInput),
        (0x1840, C::TextureBinding),
        (0x0b00, C::TextureBinding),
        (0x1d00, C::TextureBinding),
        (0x1ef8, C::Program),
    ] {
        assert_eq!(classify(m), Some(c), "{m:#x}");
        assert!(c.is_state() != matches!(c, C::Synchronization));
    }
    assert!(!C::Draw.is_state() && !C::Clear.is_state() && !C::NoOperation.is_state());
}
