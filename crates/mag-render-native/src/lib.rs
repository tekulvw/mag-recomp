//! Frontend-owned native context. No SDK headers, discovery or shader compiler
//! dependency enters the core crate. The host supplies the one existing owner.
use mag_render::sdk_compute::{NativeLimits, PipelineDesc, ProbeReport, SdkApi, SdkComputeDevice};
use std::{
    ffi::{c_char, c_void},
    marker::PhantomData,
    mem::{MaybeUninit, size_of},
    ptr::NonNull,
    rc::Rc,
};
pub type Proc = Option<unsafe extern "C" fn()>;
#[repr(C)]
#[derive(Clone, Copy)]
pub struct HostOwner {
    pub abi_version: u32,
    pub struct_size: u32,
    pub backend: u32,
    pub reserved: u32,
    pub lifetime: u64,
    pub device: *mut c_void,
    pub queue: *mut c_void,
    pub userdata: *mut c_void,
    pub queue_family: u32,
    pub reserved1: u32,
    pub instance: *mut c_void,
    pub physical_device: *mut c_void,
    pub get_instance_proc: Option<unsafe extern "C" fn(*mut c_void, *const c_char) -> Proc>,
    pub get_device_proc: Option<unsafe extern "C" fn(*mut c_void, *const c_char) -> Proc>,
    pub retain: Option<unsafe extern "C" fn(*mut c_void, u64) -> i32>,
    pub release: Option<unsafe extern "C" fn(*mut c_void, u64)>,
    pub try_begin: Option<unsafe extern "C" fn(*mut c_void, u64) -> i32>,
    pub end: Option<unsafe extern "C" fn(*mut c_void, u64)>,
}
#[repr(C)]
#[derive(Clone, Copy)]
pub struct HostOwnerV2 {
    pub abi_version: u32,
    pub struct_size: u32,
    pub base: HostOwner,
    pub known_features: u64,
    pub enabled_features: u64,
}
pub const FEATURE_BC: u64 = 1;
pub const FEATURE_ANISOTROPY: u64 = 2;
pub const FEATURE_DEPTH_BOUNDS: u64 = 4;
unsafe extern "C" {
    fn mag_native_create_v2(owner: *const HostOwnerV2, out: *mut *mut c_void) -> i32;
    fn mag_native_owner_v2_layout(out: *mut u32, words: u32) -> i32;
    fn mag_native_create(owner: *const HostOwner, out: *mut *mut c_void) -> i32;
    fn mag_native_close(device: *mut c_void) -> i32;
    fn mag_render_native_get_api(device: *mut c_void, api: *mut SdkApi) -> i32;
}
/// Owns native backend resources and a retained host lease, never a new device.
/// Construction pins the provider before touching borrowed native handles.
/// !Send/!Sync; the host contract and compute API are serialized/thread-affine.
pub struct NativeContext {
    raw: Option<NonNull<c_void>>,
    _thread: PhantomData<Rc<()>>,
}
impl NativeContext {
    /// # Safety
    /// Host callbacks/userdata must be genuine and process-lived. They pin all
    /// borrowed owner state, reject stale snapshots and provide actual original
    /// queue/encoding serialization. No native callback may unwind.
    pub unsafe fn from_host(owner: &HostOwner) -> Result<Self, i32> {
        let mut raw = std::ptr::null_mut();
        let status = unsafe { mag_native_create(owner, &mut raw) };
        if status != 0 {
            return Err(status);
        }
        let raw = NonNull::new(raw).ok_or(-6)?;
        Ok(Self {
            raw: Some(raw),
            _thread: PhantomData,
        })
    }
    /// # Safety
    /// Same genuine retained owner contract as from_host. Feature facts refer
    /// to enabled device features captured under this exact owner generation.
    pub unsafe fn from_host_v2(owner: &HostOwnerV2) -> Result<Self, i32> {
        use std::mem::{align_of, offset_of};
        let expected = [
            size_of::<HostOwnerV2>() as u32,
            align_of::<HostOwnerV2>() as u32,
            offset_of!(HostOwnerV2, base) as u32,
            offset_of!(HostOwnerV2, known_features) as u32,
            offset_of!(HostOwnerV2, enabled_features) as u32,
            FEATURE_BC as u32,
            FEATURE_ANISOTROPY as u32,
            FEATURE_DEPTH_BOUNDS as u32,
        ];
        let mut actual = [0; 8];
        let status = unsafe { mag_native_owner_v2_layout(actual.as_mut_ptr(), 8) };
        if status != 0 {
            return Err(status);
        }
        if actual != expected
            || owner.abi_version != 2
            || owner.struct_size as usize != size_of::<HostOwnerV2>()
            || owner.known_features & !7 != 0
            || owner.enabled_features & !owner.known_features != 0
        {
            return Err(-1);
        }
        let mut raw = std::ptr::null_mut();
        let status = unsafe { mag_native_create_v2(owner, &mut raw) };
        if status != 0 {
            return Err(status);
        }
        Ok(Self {
            raw: Some(NonNull::new(raw).ok_or(-6)?),
            _thread: PhantomData,
        })
    }
    fn api(&mut self) -> Result<SdkApi, i32> {
        let raw = self.raw.ok_or(-3)?;
        // All-zero nullable function pointers/raw pointers/integer fields are
        // valid representations; C fills the checked table before it is used.
        let mut api = unsafe { MaybeUninit::<SdkApi>::zeroed().assume_init() };
        api.abi_version = 1;
        api.struct_size = size_of::<SdkApi>() as u32;
        let status = unsafe { mag_render_native_get_api(raw.as_ptr(), &mut api) };
        if status == 0 { Ok(api) } else { Err(status) }
    }
    pub fn compute(&mut self, limits: NativeLimits) -> Result<SdkComputeDevice<'_>, i32> {
        let api = self.api()?;
        // The exclusive context borrow prevents context close/another driver
        // until this driver drops; context retains the actual host owner.
        unsafe { SdkComputeDevice::from_api(api, limits) }
    }
    /// BUSY preserves context and host lifetime; no implicit GPU wait occurs.
    pub fn close(&mut self) -> Result<(), i32> {
        let raw = self.raw.ok_or(-3)?;
        let status = unsafe { mag_native_close(raw.as_ptr()) };
        if status == 0 {
            self.raw = None;
            Ok(())
        } else {
            Err(status)
        }
    }
}
impl Drop for NativeContext {
    fn drop(&mut self) {
        if self.raw.is_some() {
            let _ = self.close();
        }
    }
}
/// # Safety
/// Caller provides valid descriptor/report ranges and the HostOwner contract
/// above. A pending/error result forbids host teardown; unknown work is retained.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mag_render_native_probe_owner(
    owner: *const HostOwner,
    desc: *const PipelineDesc,
    signal: Option<unsafe extern "C" fn(*mut c_void)>,
    user: *mut c_void,
    report: *mut ProbeReport,
) -> i32 {
    if owner.is_null() || desc.is_null() || report.is_null() {
        return -1;
    }
    let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        let mut context = unsafe { NativeContext::from_host(&*owner) }?;
        let api = context.api()?;
        let status = unsafe {
            mag_render::sdk_compute::mag_render_sdk_compute_probe(&api, desc, signal, user, report)
        };
        if status != 0 {
            return Err(status);
        }
        context.close()?;
        Ok(())
    }));
    let status = match result {
        Ok(Ok(())) => 0,
        Ok(Err(code)) => code,
        Err(_) => -6,
    };
    unsafe {
        (*report).status = status;
    }
    status
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::{AtomicU32, Ordering};
    static RETAINS: AtomicU32 = AtomicU32::new(0);
    unsafe extern "C" fn stale(_: *mut c_void, _: u64) -> i32 {
        RETAINS.fetch_add(1, Ordering::SeqCst);
        -3
    }
    unsafe extern "C" fn release(_: *mut c_void, _: u64) {
        panic!("rejected lifetime must never be released");
    }
    unsafe extern "C" fn begin(_: *mut c_void, _: u64) -> i32 {
        -4
    }
    unsafe extern "C" fn end(_: *mut c_void, _: u64) {}
    fn owner() -> HostOwner {
        HostOwner {
            abi_version: 1,
            struct_size: size_of::<HostOwner>() as u32,
            backend: 1,
            reserved: 0,
            lifetime: 7,
            device: std::ptr::NonNull::<u8>::dangling().as_ptr().cast(),
            queue: std::ptr::NonNull::<u8>::dangling().as_ptr().cast(),
            userdata: std::ptr::null_mut(),
            queue_family: 0,
            reserved1: 0,
            instance: std::ptr::null_mut(),
            physical_device: std::ptr::null_mut(),
            get_instance_proc: None,
            get_device_proc: None,
            retain: Some(stale),
            release: Some(release),
            try_begin: Some(begin),
            end: Some(end),
        }
    }
    #[test]
    fn unknown_enabled_features_are_rejected_before_native_access() {
        let host = HostOwnerV2 {
            abi_version: 2,
            struct_size: size_of::<HostOwnerV2>() as u32,
            base: owner(),
            known_features: 0,
            enabled_features: FEATURE_BC,
        };
        assert!(matches!(
            unsafe { NativeContext::from_host_v2(&host) },
            Err(-1)
        ));
    }
    #[test]
    #[cfg(all(feature = "metal", target_os = "macos"))]
    fn owner_v2_preserves_retain_before_device_access() {
        unsafe extern "C" fn reject(_: *mut c_void, _: u64) -> i32 {
            -3
        }
        let mut base = owner();
        base.retain = Some(reject);
        let host = HostOwnerV2 {
            abi_version: 2,
            struct_size: size_of::<HostOwnerV2>() as u32,
            base,
            known_features: 7,
            enabled_features: 1,
        };
        assert!(matches!(
            unsafe { NativeContext::from_host_v2(&host) },
            Err(-3)
        ));
    }
    #[test]
    fn unsupported_or_bad_owner_never_dereferences_native_pointers() {
        let mut bad = owner();
        bad.abi_version = 2;
        assert!(matches!(unsafe { NativeContext::from_host(&bad) }, Err(-1)));
        bad = owner();
        bad.backend = 99;
        assert!(matches!(unsafe { NativeContext::from_host(&bad) }, Err(-2)));
    }
    #[test]
    #[cfg(all(feature = "metal", target_os = "macos"))]
    fn stale_lifetime_is_rejected_before_metal_device_dereference() {
        RETAINS.store(0, Ordering::SeqCst);
        assert!(matches!(
            unsafe { NativeContext::from_host(&owner()) },
            Err(-3)
        ));
        assert_eq!(RETAINS.load(Ordering::SeqCst), 1);
    }
}

pub mod batch_device;

mod draw_probe;

mod scene_probe;

pub mod host_image;

/// Version of the frontend-owned native implementation linked into a client.
#[unsafe(no_mangle)]
pub extern "C" fn mag_render_native_abi_version() -> u32 {
    1
}

mod client;

pub mod host_frame;

pub mod thread_stack;

pub mod worker;
