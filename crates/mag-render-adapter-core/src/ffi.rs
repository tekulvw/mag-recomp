//! Pointer-free, versioned ABI for source-bound observations.
//! No Rust allocation/enum/reference crosses C. Checked failures return status.
//! Unwinding profiles contain panics; release panic=abort is process-fatal.
//!
//! Every entry point here first checks the master switch in
//! [`crate::runtime`]. While it is off, each returns its "not ready" result
//! (`ConfigStatus::NotReady`, an absent snapshot, or shadow-call status 3),
//! records nothing and does no other work.
use crate::{
    configuration::{ConfigEvent, ConfigStatus, ConfigurationObserver},
    runtime,
};
use std::{
    panic::catch_unwind,
    sync::{Mutex, OnceLock},
};
static OBSERVER: OnceLock<Mutex<ConfigurationObserver>> = OnceLock::new();
fn observer() -> &'static Mutex<ConfigurationObserver> {
    OBSERVER.get_or_init(|| Mutex::new(ConfigurationObserver::new(64)))
}
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct ConfigSnapshot {
    pub status: u32,
    pub reserved0: u32,
    pub vm: u64,
    pub initialization: u64,
    pub manager: u32,
    pub state_address: u32,
    pub parameters: [u32; 3],
    pub reserved1: u32,
}
#[unsafe(no_mangle)]
pub extern "C" fn mag_render_adapter_config_event(event: ConfigEvent) -> u32 {
    if !runtime::enabled() {
        return ConfigStatus::NotReady as u32;
    }
    catch_unwind(|| match observer().lock() {
        Ok(mut state) => state.observe(event) as u32,
        Err(_) => ConfigStatus::Invalid as u32,
    })
    .unwrap_or(ConfigStatus::Invalid as u32)
}
#[unsafe(no_mangle)]
pub extern "C" fn mag_render_adapter_config_snapshot(vm: u64, manager: u32) -> ConfigSnapshot {
    let absent = ConfigSnapshot {
        status: ConfigStatus::NotReady as u32,
        ..ConfigSnapshot::default()
    };
    if !runtime::enabled() {
        return absent;
    }
    catch_unwind(|| {
        let Ok(state) = observer().lock() else {
            return absent;
        };
        let Some(value) = state.configuration(vm, manager) else {
            return absent;
        };
        ConfigSnapshot {
            status: ConfigStatus::Accepted as u32,
            reserved0: 0,
            vm: value.vm_observation,
            initialization: value.initialization_invocation,
            manager: value.manager,
            state_address: value.state_address,
            parameters: value.parameters,
            reserved1: 0,
        }
    })
    .unwrap_or(absent)
}

use crate::shadow_call::{ShadowCallError, ShadowCallInput, ShadowCallObserver};
static CALL_OBSERVER: OnceLock<Mutex<ShadowCallObserver>> = OnceLock::new();
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct ShadowCallResult {
    pub status: u32,
    pub reserved: u32,
    pub vectors: [[u32; 4]; 14],
}
#[unsafe(no_mangle)]
pub extern "C" fn mag_render_adapter_shadow_call(input: ShadowCallInput) -> ShadowCallResult {
    let absent = |status| ShadowCallResult {
        status,
        reserved: 0,
        vectors: [[0; 4]; 14],
    };
    if !runtime::enabled() {
        return absent(3);
    }
    catch_unwind(|| {
        let Ok(configurations) = observer().lock() else {
            return absent(1);
        };
        let Ok(mut calls) = CALL_OBSERVER
            .get_or_init(|| Mutex::new(ShadowCallObserver::default()))
            .lock()
        else {
            return absent(1);
        };
        match calls.prepare(input, &configurations) {
            Ok(call) => ShadowCallResult {
                status: 0,
                reserved: 0,
                vectors: *call.output().vectors(),
            },
            Err(error) => absent(match error {
                ShadowCallError::Invalid => 1,
                ShadowCallError::Stale => 2,
                ShadowCallError::NotReady => 3,
                ShadowCallError::Numeric(_) => 4,
            }),
        }
    })
    .unwrap_or_else(|_| absent(1))
}
