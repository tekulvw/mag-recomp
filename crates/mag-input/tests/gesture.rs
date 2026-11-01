use mag_input::delivery::Offer;
use mag_input::gesture::{Config, Events, Recognizer};
fn pulse(down: bool, duration: u64) -> Option<Offer> {
    Some(Offer {
        pulse_id: 1,
        down,
        physical_duration_ns: duration,
        source_mask: 1,
    })
}
fn config() -> Config {
    Config {
        press_delay_ns: 100,
        double_tap_gap_ns: 200,
        repeat_delay_ns: 150,
        repeat_interval_ns: 50,
        refocus_delay_ns: 0,
        ..Config::default()
    }
}
#[test]
fn a_short_click_keeps_physical_duration_and_delays_single_tap() {
    let mut gesture = Recognizer::default();
    let down = gesture
        .update(1, 1000, pulse(true, 10), 1.0, false, config())
        .unwrap();
    assert_ne!(down.events & Events::DOWN, 0);
    assert_eq!(down.events & Events::PRESS, 0);
    let up = gesture
        .update(2, 1100, pulse(false, 10), 0.0, false, config())
        .unwrap();
    assert_ne!(up.events & Events::TAP, 0);
    assert_eq!(up.events & Events::SINGLE_TAP, 0);
    assert_eq!(up.elapsed_ns, 10);
    let single = gesture.update(3, 1301, None, 0.0, false, config()).unwrap();
    assert_ne!(single.events & Events::SINGLE_TAP, 0);
}
#[test]
fn sustained_input_has_repeatable_reads_and_real_hold_release() {
    let mut gesture = Recognizer::default();
    gesture
        .update(1, 10, pulse(true, 0), 1.0, false, config())
        .unwrap();
    let pressed = gesture
        .update(2, 120, pulse(true, 110), 1.0, false, config())
        .unwrap();
    assert_ne!(pressed.events & Events::PRESS, 0);
    assert_ne!(pressed.events & Events::WHILE_PRESSED, 0);
    assert_eq!(
        gesture
            .update(2, 120, pulse(true, 110), 1.0, false, config())
            .unwrap(),
        pressed
    );
    let up = gesture
        .update(3, 140, pulse(false, 130), 0.0, false, config())
        .unwrap();
    assert_ne!(up.events & Events::RELEASE, 0);
    assert_eq!(up.events & Events::TAP, 0);
}
#[test]
fn a_native_contribution_keeps_an_or_hold_after_host_release() {
    let mut gesture = Recognizer::default();
    gesture
        .update(1, 10, pulse(true, 0), 1.0, false, config())
        .unwrap();
    let held = gesture
        .update(2, 20, pulse(false, 5), 1.0, false, config())
        .unwrap();
    assert_ne!(held.events & Events::WHILE_DOWN, 0);
    assert_eq!(held.events & Events::UP, 0);
    assert_ne!(
        gesture
            .update(3, 30, None, 0.0, false, config())
            .unwrap()
            .events
            & Events::UP,
        0
    );
}
#[test]
fn blocked_input_requires_neutral_before_a_fresh_press() {
    let mut gesture = Recognizer::default();
    gesture
        .update(1, 10, pulse(true, 0), 1.0, true, config())
        .unwrap();
    assert_eq!(
        gesture
            .update(2, 20, pulse(true, 10), 1.0, false, config())
            .unwrap()
            .events,
        0
    );
    gesture.update(3, 30, None, 0.0, false, config()).unwrap();
    assert_ne!(
        gesture
            .update(4, 40, pulse(true, 0), 1.0, false, config())
            .unwrap()
            .events
            & Events::DOWN,
        0
    );
}
