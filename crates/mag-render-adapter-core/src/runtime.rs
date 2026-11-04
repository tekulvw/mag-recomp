//! Master runtime switch for the adapter's per-frame work.
//!
//! The switch is off by default. A process turns it on either by setting
//! `MAG_RENDER_ADAPTER=1` in its environment before the first query, or by
//! calling [`enable`] (C: `mag_render_adapter_enable`) before its first frame.
//! Once on, it stays on for the life of the process.
//!
//! While it is off, every per-frame entry point in this crate returns at once
//! with its documented "disabled" result, leaves its outputs untouched and does
//! no other work. A build that links this crate without opting in therefore
//! behaves like one that does not link it. Hosts should also query
//! [`enabled`] (C: `mag_render_adapter_enabled`) before doing their own
//! per-frame preparation for the adapter.
use std::sync::atomic::{AtomicU8, Ordering};

/// Environment variable that turns the switch on when it is exactly `1`.
pub const ENVIRONMENT: &str = "MAG_RENDER_ADAPTER";

const UNKNOWN: u8 = 0;
const OFF: u8 = 1;
const ON: u8 = 2;
static STATE: AtomicU8 = AtomicU8::new(UNKNOWN);

fn from_environment(value: Option<std::ffi::OsString>) -> u8 {
    if value.is_some_and(|value| value == "1") {
        ON
    } else {
        OFF
    }
}

/// Whether the adapter may do per-frame work in this process.
#[inline]
pub fn enabled() -> bool {
    match STATE.load(Ordering::Acquire) {
        ON => true,
        OFF => false,
        _ => {
            let read = from_environment(std::env::var_os(ENVIRONMENT));
            // An earlier `enable` wins over the environment.
            match STATE.compare_exchange(UNKNOWN, read, Ordering::AcqRel, Ordering::Acquire) {
                Ok(_) => read == ON,
                Err(current) => current == ON,
            }
        }
    }
}

/// Turn the switch on for the rest of the process.
pub fn enable() {
    STATE.store(ON, Ordering::Release);
}

/// C form of [`enabled`]: 1 when on, 0 when off.
#[unsafe(no_mangle)]
pub extern "C" fn mag_render_adapter_enabled() -> u32 {
    u32::from(enabled())
}

/// C form of [`enable`].
#[unsafe(no_mangle)]
pub extern "C" fn mag_render_adapter_enable() {
    enable();
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn only_an_exact_one_turns_the_environment_switch_on() {
        assert_eq!(from_environment(None), OFF);
        assert_eq!(from_environment(Some("0".into())), OFF);
        assert_eq!(from_environment(Some("true".into())), OFF);
        assert_eq!(from_environment(Some(" 1".into())), OFF);
        assert_eq!(from_environment(Some("1".into())), ON);
    }

    #[test]
    fn enable_is_sticky() {
        enable();
        assert!(enabled());
        assert_eq!(mag_render_adapter_enabled(), 1);
        mag_render_adapter_enable();
        assert!(enabled());
    }
}
