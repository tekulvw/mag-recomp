use mag_input::delivery::Delivery;

#[test]
fn a_held_action_remains_down_without_new_edges() {
    let mut input = Delivery::default();
    input.push(1, true, 10).unwrap();
    input.advance(20).unwrap();
    let first = input.offer(1).unwrap();
    assert!(first.down);
    input.observe(1, first.pulse_id, first.down).unwrap();
    input.advance(100).unwrap();
    let held = input.offer(1).unwrap();
    assert!(held.down);
    assert_eq!(held.physical_duration_ns, 90);
    input.push(1, false, 110).unwrap();
    input.advance(120).unwrap();
    assert!(!input.offer(1).unwrap().down);
}

#[test]
fn a_completed_click_requires_down_then_up_observations() {
    let mut input = Delivery::default();
    input.push(1, true, 10).unwrap();
    input.push(1, false, 15).unwrap();
    input.advance(100).unwrap();
    let down = input.offer(1).unwrap();
    assert!(down.down);
    assert_eq!(down.physical_duration_ns, 5);
    input.advance(200).unwrap();
    assert_eq!(input.offer(1).unwrap(), down);
    input.observe(1, down.pulse_id, down.down).unwrap();
    input.advance(300).unwrap();
    let up = input.offer(1).unwrap();
    assert!(!up.down);
    assert_eq!(up.physical_duration_ns, 5);
    input.observe(1, up.pulse_id, up.down).unwrap();
    input.advance(400).unwrap();
    assert!(input.offer(1).is_none());
}

#[test]
fn two_short_clicks_are_not_collapsed() {
    let mut input = Delivery::default();
    for (down, time) in [(true, 10), (false, 15), (true, 20), (false, 25)] {
        input.push(1, down, time).unwrap();
    }
    let mut ids = Vec::new();
    for (now, expected) in [(100, true), (200, false), (300, true), (400, false)] {
        input.advance(now).unwrap();
        let offer = input.offer(1).unwrap();
        assert_eq!(offer.down, expected);
        ids.push(offer.pulse_id);
        input.observe(1, offer.pulse_id, offer.down).unwrap();
    }
    assert_eq!(ids[0], ids[1]);
    assert_eq!(ids[2], ids[3]);
    assert_ne!(ids[0], ids[2]);
}

#[test]
fn cancellation_discards_pending_input_and_stale_acknowledgments() {
    let mut input = Delivery::default();
    input.push(1, true, 10).unwrap();
    input.advance(20).unwrap();
    let old = input.offer(1).unwrap();
    input.cancel();
    input.push(1, true, 30).unwrap();
    input.advance(40).unwrap();
    assert!(input.observe(1, old.pulse_id, old.down).is_err());
    let current = input.offer(1).unwrap();
    assert!(current.down);
    assert_ne!(current.pulse_id, old.pulse_id);
}

#[test]
fn malformed_transitions_are_rejected_without_losing_the_valid_hold() {
    let mut input = Delivery::default();
    assert!(input.push(0, true, 1).is_err());
    input.push(1, true, 10).unwrap();
    assert!(input.push(1, true, 11).is_err());
    assert!(input.push(1, false, 9).is_err());
    input.advance(20).unwrap();
    assert!(input.offer(1).unwrap().down);
}

#[test]
fn a_delayed_down_acknowledgment_cannot_acknowledge_release() {
    let mut input = Delivery::default();
    input.push(1, true, 10).unwrap();
    input.push(1, false, 15).unwrap();
    input.advance(20).unwrap();
    let down = input.offer(1).unwrap();
    input.observe(1, down.pulse_id, down.down).unwrap();
    input.advance(30).unwrap();
    assert!(!input.offer(1).unwrap().down);
    assert!(input.observe(1, down.pulse_id, down.down).is_err());
}
