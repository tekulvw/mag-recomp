use mag_input::ffi::*;

#[test]
fn c_boundary_validates_tags_handles_and_output_pointers() {
    unsafe {
        let mut handle = 0;
        assert_eq!(mag_input_create(1, std::ptr::null(), 0, &mut handle), 0);
        assert_ne!(handle, 0);
        assert_ne!(mag_input_capture(handle, 0x100, &mut 0), 0);
        assert_ne!(mag_input_capture(handle, 3, std::ptr::null_mut()), 0);
        assert_eq!(mag_input_destroy(handle), 0);
        assert_ne!(mag_input_capture(handle, 3, &mut 0), 0);
        assert_ne!(mag_input_destroy(handle), 0);
    }
}

#[test]
fn c_boundary_preserves_a_short_click_until_observed() {
    unsafe {
        let binding = BindingRecord {
            source: 0,
            code: 26,
            action: 1,
            contexts: 1,
            behavior: 0,
        };
        let mut handle = 0;
        assert_eq!(mag_input_create(1, &binding, 1, &mut handle), 0);
        let mut generation = 0;
        assert_eq!(mag_input_capture(handle, 7, &mut generation), 0);
        assert_eq!(mag_input_event(handle, 7, 0, 26, 1, 10, generation), 0);
        assert_eq!(mag_input_event(handle, 7, 0, 26, 0, 15, generation), 0);
        let key = FrameKeyRecord {
            session: 1,
            sequence: 1,
            slot: 0,
            reserved: 0,
        };
        let mut frame = FrameRecord::default();
        assert_eq!(mag_input_sample(handle, key), 0);
        assert_eq!(mag_input_seal(handle, key, 20, 99, 1, &mut frame), 0);
        let mut offer = OfferRecord::default();
        assert_eq!(mag_input_offer(handle, key, 1, &mut offer), 0);
        assert_eq!(offer.down, 1);
        assert_eq!(offer.duration_ns, 5);
        assert_eq!(mag_input_observe(handle, key, 1, offer), 0);
        assert_eq!(mag_input_retire(handle, key), 0);
        let key2 = FrameKeyRecord { sequence: 2, ..key };
        assert_eq!(mag_input_seal(handle, key2, 30, 99, 1, &mut frame), 0);
        assert_eq!(mag_input_offer(handle, key2, 1, &mut offer), 0);
        assert_eq!(offer.down, 0);
        assert_eq!(mag_input_destroy(handle), 0);
    }
}

#[test]
fn concurrent_ingress_is_serialized_and_old_handles_never_alias_new_engines() {
    unsafe {
        let binding = BindingRecord {
            source: 0,
            code: 26,
            action: 1,
            contexts: 1,
            behavior: 0,
        };
        let mut handle = 0;
        assert_eq!(mag_input_create(1, &binding, 1, &mut handle), 0);
        let mut generation = 0;
        assert_eq!(mag_input_capture(handle, 7, &mut generation), 0);
        let threads: Vec<_> = (1..=8)
            .map(|device| {
                std::thread::spawn(move || {
                    assert_eq!(mag_input_event(handle, device, 0, 26, 1, 10, generation), 0);
                    assert_eq!(mag_input_event(handle, device, 0, 26, 0, 10, generation), 0);
                })
            })
            .collect();
        for thread in threads {
            thread.join().unwrap();
        }
        let key = FrameKeyRecord {
            session: 1,
            sequence: 1,
            slot: 0,
            reserved: 0,
        };
        assert_eq!(
            mag_input_seal(handle, key, 20, 99, 1, &mut FrameRecord::default()),
            0
        );
        assert_eq!(
            mag_input_offer(handle, key, 1, &mut OfferRecord::default()),
            0
        );
        assert_eq!(mag_input_destroy(handle), 0);
        let mut next = 0;
        assert_eq!(mag_input_create(1, &binding, 1, &mut next), 0);
        assert_ne!(handle, next);
        assert_ne!(mag_input_mouse(handle, generation, 1.0, 0.0), 0);
        assert_eq!(mag_input_destroy(next), 0);
    }
}

#[test]
fn provisional_routing_does_not_accept_kbm_until_an_exact_receipt() {
    unsafe {
        let binding = BindingRecord {
            source: 0,
            code: 26,
            action: 1,
            contexts: 1,
            behavior: 0,
        };
        let mut handle = 0;
        let mut generation = 0;
        assert_eq!(mag_input_create(1, &binding, 1, &mut handle), 0);
        assert_eq!(mag_input_capture(handle, 7, &mut generation), 0);
        assert_eq!(mag_input_event(handle, 1, 0, 26, 1, 10, generation), 0);
        let key = FrameKeyRecord {
            session: 1,
            sequence: 1,
            slot: 0,
            reserved: 0,
        };
        assert_eq!(
            mag_input_seal(handle, key, 20, 99, 1, &mut FrameRecord::default()),
            0
        );
        let mut policy = PolicyRecord::default();
        let mut offer = OfferRecord::default();
        assert_eq!(mag_input_policy_read(handle, &mut policy), 0);
        assert_eq!(policy.flags & 1, 0);
        assert_eq!(mag_input_offer(handle, key, 1, &mut offer), 0);
        assert_eq!(mag_input_accept_action(handle, key, 1, offer), 0);
        assert_eq!(mag_input_policy_read(handle, &mut policy), 0);
        assert_ne!(policy.flags & 1, 0);
        let stale = OfferRecord {
            pulse_id: offer.pulse_id + 1,
            ..offer
        };
        assert_ne!(mag_input_accept_action(handle, key, 1, stale), 0);
        assert_eq!(mag_input_destroy(handle), 0);
    }
}

#[test]
fn resealing_an_existing_frame_cannot_change_its_recognition_time() {
    unsafe {
        let mut handle = 0;
        let mut generation = 0;
        assert_eq!(mag_input_create(1, std::ptr::null(), 0, &mut handle), 0);
        assert_eq!(mag_input_capture(handle, 7, &mut generation), 0);
        let key = FrameKeyRecord {
            session: 1,
            sequence: 1,
            slot: 0,
            reserved: 0,
        };
        assert_eq!(
            mag_input_seal(handle, key, 10, 99, 1, &mut FrameRecord::default()),
            0
        );
        let config = mag_input::gesture::Config {
            press_delay_ns: 100,
            ..Default::default()
        };
        let mut first = RecognitionRecord::default();
        assert_eq!(
            mag_input_recognize(handle, key, 1, 1, 1, 1.0, 0, config, &mut first),
            0
        );
        assert_eq!(
            mag_input_seal(handle, key, 50, 99, 1, &mut FrameRecord::default()),
            0
        );
        let mut again = RecognitionRecord::default();
        assert_eq!(
            mag_input_recognize(handle, key, 1, 1, 1, 1.0, 0, config, &mut again),
            0
        );
        assert_eq!(first.elapsed_ns, again.elapsed_ns);
        assert_eq!(first.events, again.events);
        assert_eq!(mag_input_destroy(handle), 0);
    }
}

#[test]
fn binding_editor_boundary_keeps_output_unchanged_on_conflict_and_capacity_failure() {
    unsafe {
        let original = BindingRecord {
            source: 0,
            code: 26,
            action: 1,
            contexts: 1,
            behavior: 0,
        };
        let request = BindingRecord {
            action: 2,
            ..original
        };
        let mut output = original;
        let mut count = 0;
        let mut conflict = 0;
        assert_eq!(
            mag_input_edit_binding(
                &original,
                1,
                request,
                0,
                0,
                &mut output,
                1,
                &mut count,
                &mut conflict
            ),
            7
        );
        assert_eq!(conflict, 1);
        assert_eq!(output.action, 1);
        assert_eq!(
            mag_input_edit_binding(
                &original,
                1,
                request,
                0,
                2,
                &mut output,
                0,
                &mut count,
                &mut conflict
            ),
            CAPACITY
        );
        assert_eq!(count, 1);
        assert_eq!(output.action, 1);
        assert_eq!(
            mag_input_edit_binding(
                &original,
                1,
                request,
                0,
                2,
                &mut output,
                1,
                &mut count,
                &mut conflict
            ),
            OK
        );
        assert_eq!(output.action, 2);
        assert_eq!(
            mag_input_edit_binding(
                std::ptr::null(),
                1,
                request,
                0,
                0,
                &mut output,
                1,
                &mut count,
                &mut conflict
            ),
            INVALID
        );
    }
}
