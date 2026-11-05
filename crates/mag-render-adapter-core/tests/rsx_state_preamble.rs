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

#[test]
fn context_dma_bindings_are_state_except_notify_and_synchronisation() {
    for (method, class) in [
        (0x0194, MethodClass::Attachment), // colour A
        (0x018c, MethodClass::Attachment), // colour B
        (0x01b4, MethodClass::Attachment), // colour C
        (0x01b8, MethodClass::Attachment), // colour D
        (0x0198, MethodClass::Attachment), // zeta
        (0x0184, MethodClass::TextureBinding),
        (0x0188, MethodClass::TextureBinding),
        (0x019c, MethodClass::VertexInput),
        (0x01a0, MethodClass::VertexInput),
    ] {
        assert_eq!(classify(method), Some(class), "{method:#06x}");
    }
    // The other context DMAs are bindings too.
    for method in [0x0180, 0x0190, 0x01a4, 0x01a8, 0x01ac, 0x01b0] {
        assert_eq!(
            classify(method),
            Some(MethodClass::Attachment),
            "{method:#06x}"
        );
    }
    // A surface setup replays its context DMAs with the surface words.
    let writes = [
        w(0x0194, 0xfeed_0000),
        w(0x0198, 0xfeed_0001),
        w(0x0200, 0x0101),
    ];
    let p = encode_state_preamble(&writes, ClassSet::all_state(), 64).unwrap();
    assert_eq!(
        p.words,
        [
            (2 << 18) | 0x0194,
            0xfeed_0000,
            0xfeed_0001,
            0x0004_0200,
            0x0101,
            RETURN
        ]
    );
}

#[test]
fn the_method_table_classes_and_its_array_ends() {
    for (method, class) in [
        (0x0b40, MethodClass::Raster),      // texture coordinate control 0
        (0x0b7c, MethodClass::Raster),      // its last element
        (0x08d4, MethodClass::Raster),      // fog parameter 1
        (0x1480, MethodClass::Raster),      // polygon stipple pattern
        (0x1ea4, MethodClass::Raster),      // zcull control 0
        (0x0a80, MethodClass::VertexInput), // scaled immediate data, attribute 0
        (0x1cfc, MethodClass::VertexInput), // 4f immediate data, attribute 15
        (0x1e7c, MethodClass::VertexInput), // 1f immediate data, attribute 15
        (0x1db0, MethodClass::VertexInput), // restart index
        (0x097c, MethodClass::TextureBinding),
        (0x03c0, MethodClass::TextureBinding),
        (0x1e94, MethodClass::Program),
        (0x1fd4, MethodClass::Program),
        (0x1d80, MethodClass::Attachment),
        (0x1824, MethodClass::Draw),
        (0x1438, MethodClass::Clear),
        (0x0110, MethodClass::Synchronization),
        (0x1800, MethodClass::Synchronization),
    ] {
        assert_eq!(classify(method), Some(class), "{method:#06x}");
    }
    // Outside the table.
    for method in [0x0004, 0x03a4, 0x0400, 0x2000] {
        assert_eq!(classify(method), None, "{method:#06x}");
    }
}

#[test]
fn every_unclassified_method_is_listed_once_with_its_count() {
    let writes = [
        w(0x0004, 0),
        w(0x0a74, 1),
        w(0x03a4, 0),
        w(0x0004, 1),
        MethodWrite {
            subchannel: 3,
            method: 0x0004,
            value: 0,
        },
    ];
    assert_eq!(unclassified_methods(&writes), [(0x0004, 2), (0x03a4, 1)]);
    assert!(unclassified_methods(&[w(0x0a74, 1)]).is_empty());
}
