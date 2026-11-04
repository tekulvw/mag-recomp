//! Stack sizes for threads that compile platform pipelines.
//!
//! Driver pipeline compilation and the Vulkan validation layer's SPIR-V checks
//! run on the render client thread and expect about what a native main thread
//! has. On glibc a thread's static TLS and guard page come out of the stack size
//! it is created with, and a host process may carry megabytes of static TLS: a
//! plain size would then leave only a few KiB usable. These sizes add that
//! overhead, so the usable part is what is asked for on every platform.

/// Usable stack for a thread that compiles platform pipelines.
pub const PIPELINE_COMPILER_STACK_BYTES: usize = 16 << 20;

/// Bytes to request so that `usable` bytes of stack remain for the thread's
/// own frames after the C library's per-thread overhead.
pub fn with_overhead(usable: usize) -> usize {
    usable.saturating_add(overhead())
}

#[cfg(all(target_os = "linux", target_env = "gnu"))]
fn overhead() -> usize {
    use std::ffi::{c_char, c_int, c_void};
    unsafe extern "C" {
        fn dlsym(handle: *mut c_void, symbol: *const c_char) -> *mut c_void;
        fn pthread_attr_init(attr: *mut c_void) -> c_int;
        fn pthread_attr_destroy(attr: *mut c_void) -> c_int;
    }
    // glibc's own figure: static TLS, guard and PTHREAD_STACK_MIN together.
    type MinStack = unsafe extern "C" fn(*const c_void) -> usize;
    const PTHREAD_STACK_MIN: usize = 16384;
    // RTLD_DEFAULT is the null handle on glibc.
    let symbol = unsafe { dlsym(std::ptr::null_mut(), c"__pthread_get_minstack".as_ptr()) };
    if symbol.is_null() {
        return 0;
    }
    let min_stack: MinStack = unsafe { std::mem::transmute(symbol) };
    // Larger than pthread_attr_t on every glibc target.
    let mut attr = [0u64; 16];
    if unsafe { pthread_attr_init(attr.as_mut_ptr().cast()) } != 0 {
        return 0;
    }
    let minimum = unsafe { min_stack(attr.as_ptr().cast()) };
    unsafe { pthread_attr_destroy(attr.as_mut_ptr().cast()) };
    minimum.saturating_sub(PTHREAD_STACK_MIN)
}

/// Elsewhere thread-local storage is allocated apart from the stack.
#[cfg(not(all(target_os = "linux", target_env = "gnu")))]
fn overhead() -> usize {
    0
}

#[cfg(test)]
mod tests {
    use super::*;

    // The large-TLS case is tests/thread_stack_tls.rs: a static TLS block
    // there applies to every thread of its binary.
    #[test]
    fn overhead_is_only_ever_added() {
        assert!(with_overhead(PIPELINE_COMPILER_STACK_BYTES) >= PIPELINE_COMPILER_STACK_BYTES);
        assert_eq!(with_overhead(usize::MAX), usize::MAX);
    }
}
