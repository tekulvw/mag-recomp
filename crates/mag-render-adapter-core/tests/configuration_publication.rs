use mag_render_adapter_core::configuration::*;
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
fn real_return_protocol_publishes_owned_configuration_only_after_begin() {
    let mut observer = ConfigurationObserver::new(1);
    observer.observe(event(0));
    assert_eq!(observer.observe(event(3)), ConfigStatus::NotReady);
    assert!(observer.configuration(1, 0x10000).is_none());
    assert_eq!(observer.observe(event(2)), ConfigStatus::Accepted);
    assert!(observer.configuration(1, 0x10000).is_none());
    assert_eq!(observer.observe(event(3)), ConfigStatus::Accepted);
    assert_eq!(
        observer.configuration(1, 0x10000).unwrap().parameters,
        [11, 12, 13]
    );
}
#[test]
fn teardown_revokes_before_original_destroy_but_keeps_owned_snapshot_alive() {
    let mut observer = ConfigurationObserver::new(1);
    observer.observe(event(0));
    observer.observe(event(2));
    observer.observe(event(3));
    let first = observer.configuration(1, 0x10000).unwrap();
    observer.observe(event(4));
    assert!(observer.configuration(1, 0x10000).is_none());
    assert_eq!(first.parameters, [11, 12, 13]);
}
#[test]
fn reinitialization_revokes_old_ready_configuration_before_writes() {
    let mut observer = ConfigurationObserver::new(1);
    observer.observe(event(0));
    observer.observe(event(2));
    observer.observe(event(3));
    let old = observer.configuration(1, 0x10000).unwrap();
    let mut next = event(2);
    next.invocation = 5;
    observer.observe(next);
    assert!(observer.configuration(1, 0x10000).is_none());
    next.kind = 3;
    next.parameters = [21, 22, 23];
    observer.observe(next);
    assert_ne!(
        observer
            .configuration(1, 0x10000)
            .unwrap()
            .initialization_invocation,
        old.initialization_invocation
    );
    assert_eq!(old.parameters, [11, 12, 13]);
}
#[test]
fn interrupted_or_not_ready_initializer_never_publishes() {
    let mut observer = ConfigurationObserver::new(1);
    observer.observe(event(0));
    observer.observe(event(2));
    observer.observe(event(5));
    assert_eq!(observer.observe(event(3)), ConfigStatus::NotReady);
    observer.observe(event(2));
    let mut result = event(3);
    result.state_value = 1;
    assert_eq!(observer.observe(result), ConfigStatus::NotReady);
    assert!(observer.configuration(1, 0x10000).is_none());
}
#[test]
fn foreign_owner_or_context_cannot_finish_pending_publication() {
    for field in 0..3 {
        let mut observer = ConfigurationObserver::new(1);
        observer.observe(event(0));
        observer.observe(event(2));
        let mut result = event(3);
        match field {
            0 => result.owner += 1,
            1 => result.context += 1,
            _ => result.invocation += 1,
        }
        assert_eq!(observer.observe(result), ConfigStatus::Stale);
        assert!(observer.configuration(1, 0x10000).is_none());
    }
}
#[test]
fn vm_replacement_and_end_revoke_configuration_identity() {
    let mut observer = ConfigurationObserver::new(1);
    observer.observe(event(0));
    observer.observe(event(2));
    observer.observe(event(3));
    let mut start = event(0);
    start.vm = 2;
    observer.observe(start);
    assert!(observer.configuration(1, 0x10000).is_none());
    assert!(observer.configuration(2, 0x10000).is_none());
    assert_eq!(observer.observe(event(3)), ConfigStatus::Stale);
    start.kind = 1;
    observer.observe(start);
    assert!(observer.configuration(2, 0x10000).is_none());
}
#[test]
fn pointer_free_ffi_roundtrip_validates_header_and_owns_words() {
    use mag_render_adapter_core::ffi::*;
    mag_render_adapter_core::runtime::enable();
    assert_eq!(size_of::<ConfigEvent>(), 80);
    assert_eq!(size_of::<ConfigSnapshot>(), 48);
    assert_eq!(mag_render_adapter_config_event(event(0)), 0);
    assert_eq!(mag_render_adapter_config_event(event(2)), 0);
    assert_eq!(mag_render_adapter_config_event(event(3)), 0);
    let captured = mag_render_adapter_config_snapshot(1, 0x10000);
    assert_eq!(captured.status, 0);
    assert_eq!(captured.parameters, [11, 12, 13]);
    let mut malformed = event(0);
    malformed.abi_version = 2;
    assert_eq!(
        mag_render_adapter_config_event(malformed),
        ConfigStatus::Invalid as u32
    );
    assert_eq!(mag_render_adapter_config_event(event(1)), 0);
    assert_eq!(
        mag_render_adapter_config_snapshot(1, 0x10000).status,
        ConfigStatus::NotReady as u32
    );
    assert_eq!(captured.parameters, [11, 12, 13]);
}
#[test]
fn replayed_vm_start_cannot_restore_old_lifetime() {
    let mut observer = ConfigurationObserver::new(1);
    observer.observe(event(0));
    let mut next = event(0);
    next.vm = 2;
    observer.observe(next);
    assert_eq!(observer.observe(event(0)), ConfigStatus::Stale);
    assert_eq!(observer.observe(event(2)), ConfigStatus::Stale);
}
#[test]
fn initialization_identity_cannot_be_replayed_after_teardown() {
    let mut observer = ConfigurationObserver::new(1);
    observer.observe(event(0));
    observer.observe(event(2));
    observer.observe(event(3));
    observer.observe(event(4));
    assert_eq!(observer.observe(event(2)), ConfigStatus::Stale);
    assert_eq!(observer.observe(event(3)), ConfigStatus::NotReady);
    assert!(observer.configuration(1, 0x10000).is_none());
}
