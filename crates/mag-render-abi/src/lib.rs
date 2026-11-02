//! Shared transport records only: no native backend, shader compiler,
//! runtime device, exported C symbol or build script is linked by this crate.
#![forbid(unsafe_code)]
use core::{
    ffi::{c_char, c_void},
    mem::size_of,
};

#[repr(C)]
#[derive(Clone, Copy)]
pub struct ShaderStage {
    pub source: *const c_void,
    pub source_length: u64,
    pub entry: *const c_char,
    pub entry_length: u32,
    pub language: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct BindingLayout {
    pub slot: u32,
    pub access: u32,
    pub minimum_bytes: u64,
    pub runtime_array_stride: u32,
    pub runtime_size_index: u32,
    pub alignment_bytes: u32,
    pub binding_kind: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct VertexAttribute {
    pub location: u32,
    pub format: u32,
    pub offset: u32,
    pub reserved: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct TextureLayout {
    pub stage: u32,
    pub texture_slot: u32,
    pub sampler_slot: u32,
    pub format: u32,
    pub comparison: u32,
    pub reserved: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct Source {
    pub engine_update: u64,
    pub cohort: u64,
    pub publication: u64,
    pub pass_occurrence: u64,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct HostImage {
    pub abi_version: u32,
    pub struct_size: u32,
    pub backend: u32,
    pub format: u32,
    pub width: u32,
    pub height: u32,
    pub reserved: u32,
    pub reserved1: u32,
    pub owner_lifetime: u64,
    pub resource_id: u64,
    pub resource_generation: u64,
    pub image: *mut c_void,
    pub userdata: *mut c_void,
    pub retain: Option<unsafe extern "C" fn(*mut c_void, u64, u64, u64) -> i32>,
    pub release: Option<unsafe extern "C" fn(*mut c_void, u64, u64, u64)>,
    pub validate: Option<unsafe extern "C" fn(*mut c_void, u64, u64, u64) -> i32>,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct HostFrame {
    pub abi_version: u32,
    pub struct_size: u32,
    pub owner_lifetime: u64,
    pub reservation: u64,
    pub userdata: *mut c_void,
    pub preflight: Option<unsafe extern "C" fn(*mut c_void, u64, u64, u64, u64) -> i32>,
    pub committed: Option<unsafe extern "C" fn(*mut c_void, u64, u64, u64, u64, u32) -> i32>,
    pub cancel: Option<unsafe extern "C" fn(*mut c_void, u64, u64) -> i32>,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct Attachment {
    pub image: u32,
    pub load: u32,
    pub store: u32,
    pub reserved: u32,
    pub clear: [f32; 4],
}

impl HostFrame {
    /// Validate representation only. A caller still must supply genuine,
    /// process-lived callbacks from the same target/owner reservation provider.
    pub fn valid(&self) -> bool {
        self.abi_version == 1
            && self.struct_size == size_of::<Self>() as u32
            && self.owner_lifetime != 0
            && self.reservation != 0
            && self.preflight.is_some()
            && self.committed.is_some()
            && self.cancel.is_some()
    }
}

pub mod packet;
