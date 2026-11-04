//! Pointer-free by-value bridge for actual synchronous cohort source callbacks.
//! This owns observed bytes, not a complete FrameInput or asset-ready capability.
//! Every entry point returns 1 and records nothing while the adapter core's
//! master switch is off.
use crate::cohort::{CohortBuilder, CohortHeader, OwnedVisibilityCohort};
use std::{
    panic::catch_unwind,
    sync::{Arc, Mutex, OnceLock},
};
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct CohortBegin {
    pub abi_version: u32,
    pub struct_bytes: u32,
    pub camera: u32,
    pub reserved: u32,
    pub vm: u64,
    pub invocation: u64,
    pub owner: u64,
    pub pass_occurrence: u64,
    pub execution_update: u64,
    pub group_counts: [[u32; 16]; 4],
}
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct CohortItemInput {
    pub vm: u64,
    pub invocation: u64,
    pub shard: u32,
    pub source_index: u32,
    pub has_transform: u32,
    pub reserved: u32,
    pub mbi: [u32; 8],
    pub transform: [u32; 28],
}
#[derive(Default)]
struct Mailbox {
    vm: u64,
    last_invocation: u64,
    building: Option<(u64, CohortBuilder)>,
    latest: Option<Arc<OwnedVisibilityCohort>>,
}
static COHORTS: OnceLock<Mutex<Mailbox>> = OnceLock::new();
fn mailbox() -> &'static Mutex<Mailbox> {
    COHORTS.get_or_init(|| Mutex::new(Mailbox::default()))
}
fn bytes<const N: usize, const B: usize>(words: [u32; N]) -> [u8; B] {
    let mut out = [0; B];
    for (chunk, word) in out.as_chunks_mut::<4>().0.iter_mut().zip(words) {
        *chunk = word.to_be_bytes();
    }
    out
}
/// The real VM lifecycle bridge revokes outstanding capture before reuse.
#[unsafe(no_mangle)]
pub extern "C" fn mag_render_adapter_cohort_vm(vm: u64) -> u32 {
    if !mag_render_adapter_core::runtime::enabled() {
        return 1;
    }
    catch_unwind(|| {
        let Ok(mut state) = mailbox().lock() else {
            return 1;
        };
        crate::cohort_resource_ffi::vm_changed(vm);
        *state = Mailbox {
            vm,
            ..Mailbox::default()
        };
        0
    })
    .unwrap_or(1)
}
/// A real source arrival that declines qualification revokes an older mailbox
/// result. This does not authorize, create or advance a producer publication.
#[unsafe(no_mangle)]
pub extern "C" fn mag_render_adapter_cohort_decline(vm: u64) -> u32 {
    if !mag_render_adapter_core::runtime::enabled() {
        return 1;
    }
    catch_unwind(|| {
        let Ok(mut state) = mailbox().lock() else {
            return 1;
        };
        if vm == 0 || vm != state.vm {
            return 1;
        }
        crate::cohort_resource_ffi::decline(vm);
        state.latest = None;
        state.building = None;
        0
    })
    .unwrap_or(1)
}
#[unsafe(no_mangle)]
pub extern "C" fn mag_render_adapter_cohort_begin(input: CohortBegin) -> u32 {
    if !mag_render_adapter_core::runtime::enabled() {
        return 1;
    }
    catch_unwind(|| {
        let Ok(mut state) = mailbox().lock() else {
            return 1;
        };
        state.latest = None;
        state.building = None;
        if input.abi_version != 1
            || input.struct_bytes as usize != size_of::<CohortBegin>()
            || input.reserved != 0
            || input.vm == 0
            || input.vm != state.vm
            || input.invocation <= state.last_invocation
        {
            crate::cohort_resource_ffi::decline(state.vm);
            return 1;
        }
        crate::cohort_resource_ffi::begin_copy(state.vm);
        state.last_invocation = input.invocation;
        let header = CohortHeader {
            vm: input.vm,
            source_invocation: input.invocation,
            ppu_owner: input.owner,
            pass_occurrence: input.pass_occurrence,
            execution_update: input.execution_update,
            camera: input.camera,
            group_counts: input.group_counts,
        };
        let Ok(builder) = CohortBuilder::new(header, 262144, 128 * 1024 * 1024) else {
            return 1;
        };
        state.building = Some((input.invocation, builder));
        0
    })
    .unwrap_or(1)
}
#[unsafe(no_mangle)]
pub extern "C" fn mag_render_adapter_cohort_item(input: CohortItemInput) -> u32 {
    if !mag_render_adapter_core::runtime::enabled() {
        return 1;
    }
    catch_unwind(|| {
        let Ok(mut state) = mailbox().lock() else {
            return 1;
        };
        if input.vm == 0
            || input.vm != state.vm
            || input.reserved != 0
            || input.has_transform > 1
            || input.shard > 3
            || input.source_index > 65535
        {
            state.building = None;
            return 1;
        }
        let Some((invocation, builder)) = state.building.as_mut() else {
            return 1;
        };
        if *invocation != input.invocation {
            state.building = None;
            return 1;
        }
        let transform = (input.has_transform == 1).then(|| bytes(input.transform));
        if builder
            .push(
                input.shard as u8,
                input.source_index as u16,
                bytes(input.mbi),
                transform,
            )
            .is_err()
        {
            state.building = None;
            return 1;
        }
        0
    })
    .unwrap_or(1)
}
#[unsafe(no_mangle)]
pub extern "C" fn mag_render_adapter_cohort_finish(vm: u64, invocation: u64) -> u32 {
    if !mag_render_adapter_core::runtime::enabled() {
        return 1;
    }
    catch_unwind(|| {
        let Ok(mut state) = mailbox().lock() else {
            return 1;
        };
        state.latest = None;
        let Some((actual, builder)) = state.building.take() else {
            return 1;
        };
        if vm == 0 || vm != state.vm || actual != invocation {
            return 1;
        }
        let Ok(cohort) = builder.finish() else {
            return 1;
        };
        state.latest = Some(Arc::new(cohort));
        0
    })
    .unwrap_or(1)
}
/// Only an owned immutable cohort crosses into a later native scene consumer.
/// The consumer must bind it to its real pass/target and satisfy asset/writer
/// and complete geometry/material obligations before admitting any draw.
pub fn take_latest_cohort() -> Option<Arc<OwnedVisibilityCohort>> {
    mailbox().lock().ok()?.latest.take()
}

/// Internal synchronous raw-asset consumer keeps the actual owned source
/// population alive. Matching identifiers select existing observations only.
pub(crate) fn retain_latest_cohort(vm: u64, invocation: u64) -> Option<Arc<OwnedVisibilityCohort>> {
    let state = mailbox().lock().ok()?;
    let cohort = state.latest.as_ref()?;
    (cohort.header().vm == vm && cohort.header().source_invocation == invocation)
        .then(|| Arc::clone(cohort))
}
