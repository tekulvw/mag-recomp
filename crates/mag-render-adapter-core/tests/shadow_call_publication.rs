use mag_render_adapter_core::{
    configuration::{ConfigEvent, ConfigurationObserver},
    shadow_call::{ShadowCallError, ShadowCallInput, ShadowCallObserver},
};
fn config_event(kind: u32) -> ConfigEvent {
    ConfigEvent {
        abi_version: 1,
        struct_bytes: 80,
        kind,
        reserved0: 0,
        vm: 1,
        context: 8,
        owner: 9,
        invocation: 1,
        manager: 0x10000,
        state_address: 0x20000,
        state_value: 2,
        reserved1: 0,
        parameters: [2.0f32, 4.0, 8.0].map(f32::to_bits),
        reserved2: 0,
    }
}
fn ready() -> ConfigurationObserver {
    let mut c = ConfigurationObserver::new(1);
    for kind in [0, 2, 3] {
        assert_eq!(c.observe(config_event(kind)) as u32, 0);
    }
    c
}
fn input() -> ShadowCallInput {
    ShadowCallInput {
        abi_version: 1,
        struct_bytes: 160,
        reserved: [0; 2],
        vm: 1,
        initialization: 1,
        context: 5,
        owner: 7,
        invocation: 2,
        manager: 0x10000,
        view: 0x30000,
        state_address: 0x20000,
        state_value: 2,
        parameters: [2.0f32, 4.0, 8.0].map(f32::to_bits),
        pose_rows: [
            [1.0f32, 0.0, 0.0, 0.0],
            [0.0, 1.0, 0.0, 0.0],
            [0.0, 0.0, 1.0, 0.0],
            [10.0, 20.0, 30.0, 1.0],
        ]
        .map(|v| v.map(f32::to_bits)),
        view_scalar_60: 0.5f32.to_bits(),
        view_scalar_a24: 2.0f32.to_bits(),
        reserved_tail: 0,
    }
}
#[test]
fn binds_owned_input_to_live_initialization_and_call_identity() {
    let configurations = ready();
    let mut observer = ShadowCallObserver::default();
    let mut event = input();
    let publication = observer.prepare(event, &configurations).unwrap();
    event.pose_rows[3][0] = 0;
    assert_eq!(event.pose_rows[3][0], 0);
    assert_eq!(publication.inputs().pose_rows[3][0], 10.0f32.to_bits());
    assert_eq!(
        publication.output().vectors()[0],
        [12.0f32, 21.0, 28.0, 1.0].map(f32::to_bits)
    );
    assert_eq!(publication.invocation(), 2);
    assert_eq!(size_of::<ShadowCallInput>(), 160);
}
#[test]
fn rejects_replay_and_wrong_configuration_without_publication() {
    let c = ready();
    let mut observer = ShadowCallObserver::default();
    assert!(observer.prepare(input(), &c).is_ok());
    assert_eq!(
        observer.prepare(input(), &c).unwrap_err(),
        ShadowCallError::Stale
    );
    let mut e = input();
    e.invocation = 3;
    e.parameters[0] = 0;
    assert_eq!(
        observer.prepare(e, &c).unwrap_err(),
        ShadowCallError::NotReady
    );
    e = input();
    e.invocation = 3;
    e.initialization = 2;
    assert_eq!(
        observer.prepare(e, &c).unwrap_err(),
        ShadowCallError::NotReady
    );
}
#[test]
fn teardown_rejects_new_calls_but_existing_owned_result_survives() {
    let mut c = ready();
    let mut observer = ShadowCallObserver::default();
    let old = observer.prepare(input(), &c).unwrap();
    c.observe(config_event(4));
    let mut e = input();
    e.invocation = 3;
    assert_eq!(
        observer.prepare(e, &c).unwrap_err(),
        ShadowCallError::NotReady
    );
    assert_eq!(old.inputs().parameters, input().parameters);
}
#[test]
fn malformed_call_and_numeric_refusal_are_explicit() {
    let c = ready();
    let mut observer = ShadowCallObserver::default();
    let mut e = input();
    e.view += 1;
    assert_eq!(
        observer.prepare(e, &c).unwrap_err(),
        ShadowCallError::Invalid
    );
    e = input();
    e.pose_rows[3][0] = f32::INFINITY.to_bits();
    assert!(matches!(
        observer.prepare(e, &c),
        Err(ShadowCallError::Numeric(_))
    ));
    assert_eq!(
        observer.prepare(input(), &c).unwrap_err(),
        ShadowCallError::Stale
    );
}
