use mag_input::policy::{Mode, Policy};
#[test]
fn accepted_kbm_disables_assist_until_an_explicit_controller_exception() {
    let mut policy = Policy::default();
    assert!(policy.assist_allowed());
    policy.accept_sources(1);
    assert!(!policy.assist_allowed());
    policy.set_mode(Mode::Controller);
    assert!(policy.assist_allowed());
    policy.set_mode(Mode::Auto);
    assert!(!policy.assist_allowed());
}
#[test]
fn mouse_ownership_requires_a_fresh_rearmed_stick_to_switch_back() {
    let mut policy = Policy::default();
    policy.update_capture(true, true, false, false);
    policy.accept_mouse(false);
    assert!(policy.mouse_owner);
    policy.update_capture(true, true, false, true);
    assert!(policy.mouse_owner);
    policy.update_capture(true, true, true, false);
    policy.update_capture(true, true, false, true);
    assert!(!policy.mouse_owner);
}
