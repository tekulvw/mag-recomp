use mag_render::ffi::*;
fn config() -> MagRenderConfig {
    MagRenderConfig {
        abi_version: 1,
        struct_size: 16,
        reserved: 0,
    }
}
#[test]
fn ffi_invalid_inputs() {
    let mut c = config();
    c.abi_version = 2;
    assert_eq!(mag_render_create(c).status, INVALID_VERSION);
    c = config();
    c.struct_size = 0;
    assert_eq!(mag_render_create(c).status, INVALID_SIZE);
    c.struct_size = 17;
    assert_eq!(mag_render_create(c).status, INVALID_SIZE);
    c = config();
    c.reserved = 1;
    assert_eq!(mag_render_create(c).status, INVALID_CONFIG);
    assert_eq!(mag_render_destroy(0), INVALID_HANDLE);
    assert_eq!(mag_render_destroy(u64::MAX), INVALID_HANDLE);
    assert_eq!(
        mag_render_validate_callback(u64::MAX, u64::MAX),
        INVALID_HANDLE
    );
    for (count, size, length) in [
        (0, 1, 0),
        (1, 0, 0),
        (2, 4, 7),
        (u64::MAX, 2, 1),
        (1, u64::MAX, u64::MAX),
    ] {
        assert_eq!(
            mag_render_validate_span(count, size, length),
            INVALID_LENGTH
        );
    }
    assert_eq!(mag_render_validate_span(3, 4, 12), OK);
}
#[test]
fn handle_ownership_and_callback_generation() {
    let first = mag_render_create(config());
    assert_eq!(first.status, OK);
    assert_ne!(first.handle, 0);
    assert_eq!(first.abi_version, 1);
    assert_eq!(first.struct_size, 32);
    assert_eq!(
        mag_render_validate_callback(first.handle, first.callback_generation),
        OK
    );
    assert_eq!(
        mag_render_validate_callback(first.handle, first.callback_generation + 1),
        STALE_GENERATION
    );
    assert_eq!(mag_render_destroy(first.handle), OK);
    assert_eq!(mag_render_destroy(first.handle), INVALID_HANDLE);
    assert_eq!(
        mag_render_validate_callback(first.handle, first.callback_generation),
        INVALID_HANDLE
    );
    let second = mag_render_create(config());
    assert_eq!(second.status, OK);
    assert_ne!(second.handle, first.handle);
    assert_eq!(
        mag_render_validate_callback(second.handle, first.callback_generation),
        STALE_GENERATION
    );
    assert_eq!(mag_render_destroy(second.handle), OK);
}
#[test]
fn callback_and_destroy_are_thread_affine() {
    let created = mag_render_create(config());
    assert_eq!(created.status, OK);
    let (callback, destroy) = std::thread::spawn(move || {
        (
            mag_render_validate_callback(created.handle, created.callback_generation),
            mag_render_destroy(created.handle),
        )
    })
    .join()
    .unwrap();
    assert_eq!(callback, WRONG_THREAD);
    assert_eq!(destroy, WRONG_THREAD);
    assert_eq!(mag_render_destroy(created.handle), OK);
}
