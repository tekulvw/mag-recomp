use mag_input::engine::{
    Behavior, Binding, Capabilities, Capture, Context, ContextKey, Control, Engine, Event,
    FrameKey, Source,
};

const MOVE: u32 = 1;
const INTERACT: u32 = 2;

fn binding(code: u32, action: u32) -> Binding {
    Binding {
        control: Control {
            source: Source::Keyboard,
            code,
        },
        action,
        contexts: Context::OnFoot.bit(),
        behavior: Behavior::Hold,
    }
}

fn setup(bindings: Vec<Binding>) -> Engine {
    let mut engine = Engine::new(7, bindings).unwrap();
    engine
        .set_capture(Capture {
            focused: true,
            supported_context: true,
            mouse_captured: true,
            controller_connected: false,
        })
        .unwrap();
    engine
}

fn event(engine: &mut Engine, device: u64, code: u32, down: bool, time_ns: u64) {
    engine
        .push(Event {
            device,
            control: Control {
                source: Source::Keyboard,
                code,
            },
            down,
            time_ns,
            generation: engine.generation(),
        })
        .unwrap();
}

fn key(sequence: u64) -> FrameKey {
    FrameKey {
        session: 7,
        sequence,
        player_slot: 0,
    }
}
fn context(owner: u64) -> ContextKey {
    ContextKey {
        owner,
        context: Context::OnFoot,
    }
}

#[test]
fn capability_changes_preserve_held_movement_and_interaction() {
    let mut engine = setup(vec![binding(26, MOVE), binding(8, INTERACT)]);
    event(&mut engine, 1, 26, true, 10);
    event(&mut engine, 1, 8, true, 11);
    let generation = engine.generation();
    for sequence in 1..=5 {
        let frame = engine
            .seal(key(sequence), 20 * sequence, context(99))
            .unwrap();
        assert!(frame.action(MOVE).unwrap().down);
        assert!(frame.action(INTERACT).unwrap().down);
        assert!(frame.look(Capabilities { look: false }).is_none());
        assert_eq!(engine.generation(), generation);
    }
}

#[test]
fn releasing_one_device_does_not_release_another_device() {
    let mut engine = setup(vec![binding(26, MOVE)]);
    event(&mut engine, 1, 26, true, 10);
    event(&mut engine, 2, 26, true, 11);
    let frame = engine.seal(key(1), 20, context(99)).unwrap();
    engine
        .observe(key(1), MOVE, frame.action(MOVE).unwrap())
        .unwrap();
    event(&mut engine, 1, 26, false, 30);
    assert!(
        engine
            .seal(key(2), 40, context(99))
            .unwrap()
            .action(MOVE)
            .unwrap()
            .down
    );
    event(&mut engine, 2, 26, false, 50);
    assert!(
        !engine
            .seal(key(3), 60, context(99))
            .unwrap()
            .action(MOVE)
            .unwrap()
            .down
    );
}

#[test]
fn keyboard_repeat_does_not_toggle_twice() {
    let mut toggle = binding(8, INTERACT);
    toggle.behavior = Behavior::Toggle;
    let mut engine = setup(vec![toggle]);
    event(&mut engine, 1, 8, true, 10);
    event(&mut engine, 1, 8, true, 11);
    let first = engine.seal(key(1), 20, context(99)).unwrap();
    assert!(first.action(INTERACT).unwrap().down);
    engine
        .observe(key(1), INTERACT, first.action(INTERACT).unwrap())
        .unwrap();
    event(&mut engine, 1, 8, false, 30);
    assert!(
        engine
            .seal(key(2), 40, context(99))
            .unwrap()
            .action(INTERACT)
            .unwrap()
            .down
    );
    event(&mut engine, 1, 8, true, 50);
    assert!(
        !engine
            .seal(key(3), 60, context(99))
            .unwrap()
            .action(INTERACT)
            .unwrap()
            .down
    );
}

#[test]
fn focus_loss_requires_release_then_a_fresh_press() {
    let mut engine = setup(vec![binding(26, MOVE)]);
    event(&mut engine, 1, 26, true, 10);
    let old = engine.seal(key(1), 20, context(99)).unwrap();
    engine.set_capture(Capture::default()).unwrap();
    engine
        .set_capture(Capture {
            focused: true,
            supported_context: true,
            mouse_captured: true,
            controller_connected: false,
        })
        .unwrap();
    assert!(
        engine
            .observe(key(1), MOVE, old.action(MOVE).unwrap())
            .is_err()
    );
    event(&mut engine, 1, 26, true, 30);
    assert!(
        engine
            .seal(key(2), 40, context(99))
            .unwrap()
            .action(MOVE)
            .is_none()
    );
    event(&mut engine, 1, 26, false, 50);
    event(&mut engine, 1, 26, true, 60);
    assert!(
        engine
            .seal(key(3), 70, context(99))
            .unwrap()
            .action(MOVE)
            .unwrap()
            .down
    );
}

#[test]
fn sealing_freezes_one_frame_and_keeps_later_events_for_the_next() {
    let mut engine = setup(vec![binding(26, MOVE)]);
    let first = engine.seal(key(1), 20, context(99)).unwrap();
    event(&mut engine, 1, 26, true, 30);
    assert_eq!(engine.seal(key(1), 40, context(99)).unwrap(), first);
    let next = engine.seal(key(2), 50, context(99)).unwrap();
    assert!(next.action(MOVE).unwrap().down);
    assert!(engine.seal(key(1), 60, context(99)).is_err());
}

#[test]
fn a_new_player_owner_cannot_inherit_a_held_action() {
    let mut engine = setup(vec![binding(26, MOVE)]);
    event(&mut engine, 1, 26, true, 10);
    assert!(
        engine
            .seal(key(1), 20, context(99))
            .unwrap()
            .action(MOVE)
            .is_some()
    );
    assert!(
        engine
            .seal(key(2), 30, context(100))
            .unwrap()
            .action(MOVE)
            .is_none()
    );
    event(&mut engine, 1, 26, false, 40);
    event(&mut engine, 1, 26, true, 50);
    assert!(
        engine
            .seal(key(3), 60, context(100))
            .unwrap()
            .action(MOVE)
            .unwrap()
            .down
    );
}

#[test]
fn stale_capture_events_are_rejected() {
    let mut engine = setup(vec![binding(26, MOVE)]);
    let stale = engine.generation();
    engine.set_capture(Capture::default()).unwrap();
    assert!(
        engine
            .push(Event {
                device: 1,
                control: Control {
                    source: Source::Keyboard,
                    code: 26
                },
                down: true,
                time_ns: 10,
                generation: stale
            })
            .is_err()
    );
}

#[test]
fn overlapping_physical_bindings_and_third_alternates_are_rejected() {
    assert!(Engine::new(7, vec![binding(26, MOVE), binding(26, INTERACT)]).is_err());
    assert!(
        Engine::new(
            7,
            vec![binding(26, MOVE), binding(27, MOVE), binding(28, MOVE)]
        )
        .is_err()
    );
}

fn mouse_binding(action: u32) -> Binding {
    Binding {
        control: Control {
            source: Source::Mouse,
            code: 0,
        },
        action,
        contexts: Context::OnFoot.bit(),
        behavior: Behavior::Hold,
    }
}

fn mouse_edge(engine: &mut Engine, down: bool, time_ns: u64) {
    engine
        .push(Event {
            device: 2,
            control: Control {
                source: Source::Mouse,
                code: 0,
            },
            down,
            time_ns,
            generation: engine.generation(),
        })
        .unwrap();
}

#[test]
fn losing_mouse_capture_cancels_a_pending_click_without_cancelling_keyboard_hold() {
    let mut engine = setup(vec![binding(26, MOVE), mouse_binding(3)]);
    mouse_edge(&mut engine, true, 10);
    mouse_edge(&mut engine, false, 11);
    event(&mut engine, 1, 26, true, 12);
    let first = engine.seal(key(1), 20, context(99)).unwrap();
    assert!(first.action(3).unwrap().down);
    engine
        .observe(key(1), MOVE, first.action(MOVE).unwrap())
        .unwrap();
    engine
        .set_capture(Capture {
            focused: true,
            supported_context: true,
            mouse_captured: false,
            controller_connected: false,
        })
        .unwrap();
    let next = engine.seal(key(2), 40, context(99)).unwrap();
    assert!(next.action(3).is_none());
    assert!(next.action(MOVE).unwrap().down);
}

#[test]
fn revoking_mouse_preserves_a_queued_keyboard_click_for_the_same_action() {
    let mut engine = setup(vec![binding(8, INTERACT), mouse_binding(INTERACT)]);
    mouse_edge(&mut engine, true, 10);
    mouse_edge(&mut engine, false, 11);
    event(&mut engine, 1, 8, true, 12);
    event(&mut engine, 1, 8, false, 13);
    let first = engine.seal(key(1), 20, context(99)).unwrap();
    let mouse = first.action(INTERACT).unwrap();
    engine
        .set_capture(Capture {
            focused: true,
            supported_context: true,
            mouse_captured: false,
            controller_connected: false,
        })
        .unwrap();
    let next = engine
        .seal(key(2), 40, context(99))
        .unwrap()
        .action(INTERACT)
        .unwrap();
    assert!(next.down);
    assert_ne!(next.pulse_id, mouse.pulse_id);
    assert_eq!(next.physical_duration_ns, 1);
}

#[test]
fn disconnecting_one_keyboard_device_preserves_another_device() {
    let mut engine = setup(vec![binding(26, MOVE)]);
    event(&mut engine, 1, 26, true, 10);
    event(&mut engine, 2, 26, true, 11);
    engine.seal(key(1), 20, context(99)).unwrap();
    engine.disconnect(Source::Keyboard, 1).unwrap();
    assert!(
        engine
            .seal(key(2), 30, context(99))
            .unwrap()
            .action(MOVE)
            .unwrap()
            .down
    );
    engine.disconnect(Source::Keyboard, 2).unwrap();
    assert!(
        engine
            .seal(key(3), 40, context(99))
            .unwrap()
            .action(MOVE)
            .is_none()
    );
}

#[test]
fn retiring_a_frame_invalidates_receipts_without_dropping_an_unobserved_click() {
    let mut engine = setup(vec![binding(8, INTERACT)]);
    event(&mut engine, 1, 8, true, 10);
    event(&mut engine, 1, 8, false, 15);
    let first = engine.seal(key(1), 20, context(99)).unwrap();
    engine.retire(key(1)).unwrap();
    assert!(
        engine
            .observe(key(1), INTERACT, first.action(INTERACT).unwrap())
            .is_err()
    );
    assert!(engine.seal(key(1), 30, context(99)).is_err());
    let next = engine.seal(key(2), 40, context(99)).unwrap();
    assert_eq!(next.action(INTERACT), first.action(INTERACT));
}

#[test]
fn rebinding_is_validated_before_reset_and_requires_a_fresh_activation() {
    let mut engine = setup(vec![binding(26, MOVE)]);
    event(&mut engine, 1, 26, true, 10);
    engine.seal(key(1), 20, context(99)).unwrap();
    assert!(
        engine
            .configure(vec![binding(26, MOVE), binding(26, INTERACT)])
            .is_err()
    );
    assert!(
        engine
            .seal(key(2), 30, context(99))
            .unwrap()
            .action(MOVE)
            .unwrap()
            .down
    );
    engine.configure(vec![binding(26, INTERACT)]).unwrap();
    assert!(
        engine
            .seal(key(3), 40, context(99))
            .unwrap()
            .action(INTERACT)
            .is_none()
    );
    event(&mut engine, 1, 26, false, 50);
    event(&mut engine, 1, 26, true, 60);
    assert!(
        engine
            .seal(key(4), 70, context(99))
            .unwrap()
            .action(INTERACT)
            .unwrap()
            .down
    );
}

#[test]
fn a_mouse_sample_is_fractional_finite_and_not_replayed() {
    let mut engine = setup(vec![]);
    engine
        .push_mouse(engine.generation(), 0.25, -0.125)
        .unwrap();
    assert!(
        engine
            .push_mouse(engine.generation(), f64::NAN, 0.0)
            .is_err()
    );
    let first = engine.seal(key(1), 20, context(99)).unwrap();
    assert_eq!(first.look(Capabilities { look: true }).unwrap().dx, 0.25);
    engine.push_mouse(engine.generation(), 0.5, 0.25).unwrap();
    assert_eq!(engine.seal(key(1), 30, context(99)).unwrap(), first);
    let second = engine.seal(key(2), 40, context(99)).unwrap();
    assert_eq!(second.mouse.dx, 0.5);
    assert_eq!(engine.seal(key(3), 50, context(99)).unwrap().mouse.dx, 0.0);
}

#[test]
fn physical_sample_excludes_events_arriving_before_later_interpretation() {
    let mut engine = setup(vec![binding(26, MOVE), binding(8, INTERACT)]);
    event(&mut engine, 1, 26, true, 10);
    engine.sample(key(1)).unwrap();
    event(&mut engine, 1, 8, true, 15);
    let first = engine.seal(key(1), 20, context(99)).unwrap();
    assert!(first.action(MOVE).unwrap().down);
    assert!(first.action(INTERACT).is_none());
    let next = engine.seal(key(2), 30, context(99)).unwrap();
    assert!(next.action(INTERACT).unwrap().down);
}

#[test]
fn retiring_an_uninterpreted_sample_blocks_its_held_controls() {
    let mut engine = setup(vec![binding(26, MOVE)]);
    event(&mut engine, 1, 26, true, 10);
    engine.sample(key(1)).unwrap();
    engine.retire(key(1)).unwrap();
    assert!(engine.seal(key(1), 20, context(99)).is_err());
    assert!(
        engine
            .seal(key(2), 30, context(99))
            .unwrap()
            .action(MOVE)
            .is_none()
    );
}
