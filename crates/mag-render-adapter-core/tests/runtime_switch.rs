//! The master switch gates every C entry point. This file is its own test
//! process, so the switch starts in its default state here.
use mag_render_adapter_core::{
    configuration::{ConfigEvent, ConfigStatus},
    ffi::*,
    runtime,
    shadow_call::ShadowCallInput,
};

fn event(kind: u32) -> ConfigEvent {
    ConfigEvent {
        abi_version: 1,
        struct_bytes: size_of::<ConfigEvent>() as u32,
        kind,
        reserved0: 0,
        vm: 1,
        context: 2,
        owner: 3,
        invocation: 4,
        manager: 0x10000,
        state_address: 0x20000,
        state_value: 2,
        reserved1: 0,
        parameters: [11, 12, 13],
        reserved2: 0,
    }
}

#[test]
fn entry_points_do_nothing_until_the_switch_is_on() {
    if std::env::var_os(runtime::ENVIRONMENT).is_some_and(|v| v == "1") {
        return; // The environment opted in; the default-off case cannot be observed.
    }
    assert!(!runtime::enabled());
    assert_eq!(runtime::mag_render_adapter_enabled(), 0);
    for kind in [0, 2, 3] {
        assert_eq!(
            mag_render_adapter_config_event(event(kind)),
            ConfigStatus::NotReady as u32
        );
    }
    let snapshot = mag_render_adapter_config_snapshot(1, 0x10000);
    assert_eq!(snapshot.status, ConfigStatus::NotReady as u32);
    assert_eq!(snapshot.parameters, [0; 3]);
    let call = mag_render_adapter_shadow_call(ShadowCallInput {
        abi_version: 1,
        struct_bytes: size_of::<ShadowCallInput>() as u32,
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
        pose_rows: [[0; 4]; 4],
        view_scalar_60: 0.5f32.to_bits(),
        view_scalar_a24: 2.0f32.to_bits(),
        reserved_tail: 0,
    });
    assert_eq!(call.status, 3);
    assert_eq!(call.vectors, [[0; 4]; 14]);

    // Nothing was recorded while off: turning the switch on starts from an
    // empty observer, and the same events now publish.
    runtime::mag_render_adapter_enable();
    assert!(runtime::enabled());
    assert_eq!(
        mag_render_adapter_config_snapshot(1, 0x10000).status,
        ConfigStatus::NotReady as u32
    );
    for kind in [0, 2, 3] {
        assert_eq!(mag_render_adapter_config_event(event(kind)), 0);
    }
    let snapshot = mag_render_adapter_config_snapshot(1, 0x10000);
    assert_eq!(snapshot.status, ConfigStatus::Accepted as u32);
    assert_eq!(snapshot.parameters, [11, 12, 13]);
}
