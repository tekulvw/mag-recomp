//! Pointer-free ABI: handles are non-reused registry tokens, never addresses.
use std::{
    collections::BTreeMap,
    mem::size_of,
    panic::{AssertUnwindSafe, catch_unwind},
    sync::{Mutex, OnceLock},
    thread::{self, ThreadId},
};

pub const ABI_VERSION: u32 = 1;
pub const OK: u32 = 0;
pub const INVALID_VERSION: u32 = 1;
pub const INVALID_SIZE: u32 = 2;
pub const INVALID_HANDLE: u32 = 3;
pub const WRONG_THREAD: u32 = 4;
pub const STALE_GENERATION: u32 = 5;
pub const INVALID_LENGTH: u32 = 6;
pub const INTERNAL_ERROR: u32 = 7;
pub const INVALID_CONFIG: u32 = 8;
#[repr(C)]
#[derive(Clone, Copy)]
pub struct MagRenderConfig {
    pub abi_version: u32,
    pub struct_size: u32,
    pub reserved: u64,
}
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct MagRenderCreateResult {
    pub abi_version: u32,
    pub struct_size: u32,
    pub status: u32,
    pub reserved: u32,
    pub handle: u64,
    pub callback_generation: u64,
}
fn result(status: u32, handle: u64) -> MagRenderCreateResult {
    MagRenderCreateResult {
        abi_version: ABI_VERSION,
        struct_size: size_of::<MagRenderCreateResult>() as u32,
        status,
        reserved: 0,
        handle,
        callback_generation: handle,
    }
}
#[unsafe(no_mangle)]
pub extern "C" fn mag_render_abi_version() -> u32 {
    1
}
struct Registry {
    next: u64,
    owners: BTreeMap<u64, ThreadId>,
}
static REGISTRY: OnceLock<Mutex<Registry>> = OnceLock::new();
fn registry() -> &'static Mutex<Registry> {
    REGISTRY.get_or_init(|| {
        Mutex::new(Registry {
            next: 1,
            owners: BTreeMap::new(),
        })
    })
}
fn boundary(action: impl FnOnce() -> u32) -> u32 {
    catch_unwind(AssertUnwindSafe(action)).unwrap_or(INTERNAL_ERROR)
}
/// Create a lifetime token only. No rendering or observer success is implied.
#[unsafe(no_mangle)]
pub extern "C" fn mag_render_create(config: MagRenderConfig) -> MagRenderCreateResult {
    catch_unwind(AssertUnwindSafe(|| {
        if config.abi_version != ABI_VERSION {
            return result(INVALID_VERSION, 0);
        }
        if config.struct_size as usize != size_of::<MagRenderConfig>() {
            return result(INVALID_SIZE, 0);
        }
        if config.reserved != 0 {
            return result(INVALID_CONFIG, 0);
        }
        let Ok(mut registry) = registry().lock() else {
            return result(INTERNAL_ERROR, 0);
        };
        // Bound leaked handles and never wrap/reuse a retired generation.
        if registry.owners.len() >= 4096 {
            return result(INTERNAL_ERROR, 0);
        }
        let handle = registry.next;
        let Some(next) = handle.checked_add(1) else {
            return result(INTERNAL_ERROR, 0);
        };
        registry.next = next;
        registry.owners.insert(handle, thread::current().id());
        result(OK, handle)
    }))
    .unwrap_or_else(|_| result(INTERNAL_ERROR, 0))
}
#[unsafe(no_mangle)]
pub extern "C" fn mag_render_destroy(handle: u64) -> u32 {
    boundary(|| {
        let Ok(mut registry) = registry().lock() else {
            return INTERNAL_ERROR;
        };
        match registry.owners.get(&handle) {
            None => INVALID_HANDLE,
            Some(owner) if *owner != thread::current().id() => WRONG_THREAD,
            Some(_) => {
                registry.owners.remove(&handle);
                OK
            }
        }
    })
}
/// Validate delivery before any future callback adapter touches its resources.
/// No callback function pointer or caller memory is accepted by this skeleton.
#[unsafe(no_mangle)]
pub extern "C" fn mag_render_validate_callback(handle: u64, generation: u64) -> u32 {
    boundary(|| {
        let Ok(registry) = registry().lock() else {
            return INTERNAL_ERROR;
        };
        match registry.owners.get(&handle) {
            None => INVALID_HANDLE,
            Some(owner) if *owner != thread::current().id() => WRONG_THREAD,
            Some(_) if generation != handle => STALE_GENERATION,
            Some(_) => OK,
        }
    })
}
/// Validate a nonempty counted byte span without accepting/dereferencing memory.
/// Future data-bearing adapters must additionally validate their actual memory.
#[unsafe(no_mangle)]
pub extern "C" fn mag_render_validate_span(
    element_count: u64,
    element_size: u64,
    byte_length: u64,
) -> u32 {
    boundary(|| {
        if element_count == 0
            || element_size == 0
            || byte_length > isize::MAX as u64
            || element_count.checked_mul(element_size) != Some(byte_length)
        {
            INVALID_LENGTH
        } else {
            OK
        }
    })
}
