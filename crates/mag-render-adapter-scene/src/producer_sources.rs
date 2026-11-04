//! Exact pre-generator source observation, independently owned from later Main
//! captured resources. A source-window token is not an asset/GPU generation.
//!
//! The producer inputs and the fog camera are acquired by the clean-room
//! behaviour crate (`material-producers.md`, `main-fog-values.md`) through the
//! bridge's synchronous copy. The entry point returns its refused result at
//! once, and records nothing, while the adapter core's master switch is off.
use core::ffi::c_void;
use mag_render_behavior::{
    fog_values::{FogCamera, acquire_fog_camera},
    guest::{GuestMemory, SourceError},
    producers::{AcquisitionError, ProducerInputs, acquire_with_budget},
};
use std::cell::Cell;
use std::{
    panic::{AssertUnwindSafe, catch_unwind},
    sync::{Arc, Mutex, OnceLock},
};
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct ProducerSourceInput {
    pub abi_version: u32,
    pub struct_bytes: u32,
    pub vm: u64,
    pub source_window: u64,
    pub ppu_owner: u64,
    pub execution_update: u64,
    pub source_toc_address: u32,
    pub renderer_address: u32,
    pub bank_arena: u32,
    pub bank_index: u32,
    pub group_flags: u32,
    pub group_limit: i32,
    pub clip_min: u32,
    pub clip_max: u32,
}
/// What one producer-source call acquired, and what it cost.
pub struct ProducerCapture {
    pub inputs: Result<ProducerInputs, AcquisitionError>,
    /// The fog pass's camera fields, read through the same copy.
    pub fog_camera: Result<FogCamera, AcquisitionError>,
    /// Successful guest copies and their bytes.
    pub reads: usize,
    pub copied_bytes: usize,
}
pub struct ProducerObservation {
    pub source: ProducerSourceInput,
    pub capture: ProducerCapture,
}
/// Byte and read budgets of one producer-source call.
const BYTE_BUDGET: usize = 512 * 1024;
const READ_BUDGET: usize = 256;
/// The bridge's copy as a behaviour [`GuestMemory`], counted and bounded.
struct CopyMemory {
    copy: ProducerCopy,
    opaque: *mut c_void,
    reads: Cell<usize>,
    bytes: Cell<usize>,
}
impl GuestMemory for CopyMemory {
    fn read(&self, address: u32, length: u32) -> Result<Vec<u8>, SourceError> {
        let refuse = |reason: &str| SourceError {
            address,
            length,
            reason: reason.into(),
        };
        let bytes = length as usize;
        if self.reads.get() >= READ_BUDGET || self.bytes.get() + bytes > BYTE_BUDGET {
            return Err(refuse("producer source budget"));
        }
        let mut out = Vec::new();
        out.try_reserve_exact(bytes)
            .map_err(|_| refuse("allocation"))?;
        out.resize(bytes, 0);
        // SAFETY: the live owned output covers the exact synchronous range.
        if unsafe { (self.copy)(self.opaque, address, length, out.as_mut_ptr()) } != 0 {
            return Err(refuse("outside the source window"));
        }
        self.reads.set(self.reads.get() + 1);
        self.bytes.set(self.bytes.get() + bytes);
        Ok(out)
    }
}
#[derive(Default)]
struct Registry {
    vm: u64,
    last_window: u64,
    records: Vec<Arc<ProducerObservation>>,
}
static SOURCES: OnceLock<Mutex<Registry>> = OnceLock::new();
fn registry() -> &'static Mutex<Registry> {
    SOURCES.get_or_init(|| Mutex::new(Registry::default()))
}
pub(crate) fn vm_changed(vm: u64) {
    if let Ok(mut s) = registry().lock() {
        *s = Registry {
            vm,
            ..Registry::default()
        };
    }
}
/// Lookup is exact. Main's provider must also validate its genuine Regular
/// scope relation to the observation; the identifier alone grants no readiness.
pub fn retain(
    vm: u64,
    window: u64,
    toc: u32,
    owner: u64,
    update: u64,
) -> Option<Arc<ProducerObservation>> {
    let s = registry().lock().ok()?;
    if s.vm != vm {
        return None;
    }
    s.records
        .iter()
        .find(|r| {
            let i = r.source;
            i.source_window == window
                && i.source_toc_address == toc
                && i.ppu_owner == owner
                && i.execution_update == update
        })
        .cloned()
}
pub type ProducerCopy = unsafe extern "C" fn(*mut c_void, u32, u32, *mut u8) -> i32;
/// # Safety
/// Callback/opaque and complete source read window must remain valid throughout
/// the synchronous call. Callback initializes exactly the requested bytes on
/// success. No callback/opaque/guest mapping is retained. Production provider
/// verifies actual VM/context/host/Regular scope before every read.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mag_render_adapter_producer_sources(
    input: ProducerSourceInput,
    copy: Option<ProducerCopy>,
    opaque: *mut c_void,
) -> u32 {
    if !mag_render_adapter_core::runtime::enabled() {
        return 1;
    }
    catch_unwind(AssertUnwindSafe(|| {
        if input.abi_version != 1
            || input.struct_bytes as usize != size_of::<ProducerSourceInput>()
            || input.vm == 0
            || input.source_window == 0
            || input.ppu_owner == 0
            || input.execution_update == 0
            || input.source_toc_address < 0x27dc
            || !input.source_toc_address.is_multiple_of(4)
            || input.renderer_address == 0
            || input.renderer_address.checked_add(0x31d0) != Some(input.bank_arena)
            || input.bank_index > 1
        {
            return 1;
        }
        let Some(copy) = copy else { return 1 };
        {
            let Ok(s) = registry().lock() else { return 1 };
            if s.vm != input.vm || input.source_window <= s.last_window {
                return 1;
            }
        }
        let memory = CopyMemory {
            copy,
            opaque,
            reads: Cell::new(0),
            bytes: Cell::new(0),
        };
        let inputs = acquire_with_budget(&memory, input.source_toc_address, BYTE_BUDGET as u64);
        let fog_camera = acquire_fog_camera(&memory, input.source_toc_address);
        let capture = ProducerCapture {
            inputs,
            fog_camera,
            reads: memory.reads.get(),
            copied_bytes: memory.bytes.get(),
        };
        if capture
            .inputs
            .as_ref()
            .is_ok_and(|p| p.renderer.address != input.renderer_address)
        {
            return 1;
        }
        let Ok(mut s) = registry().lock() else {
            return 1;
        };
        if s.vm != input.vm || input.source_window <= s.last_window {
            return 1;
        }
        // Only two pending observations remain addressable. Already-owned Main
        // records pin their exact immutable Arc independently of this registry.
        if s.records.len() == 2 {
            s.records.remove(0);
        }
        s.last_window = input.source_window;
        s.records.push(Arc::new(ProducerObservation {
            source: input,
            capture,
        }));
        0
    }))
    .unwrap_or(1)
}
