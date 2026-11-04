//! Synchronous source claim to the public client ingress. Callback borrows
//! owned native arrays only for this call and returns a queue-commit decision.
use crate::{
    client_scene::*,
    cohort_geometry::MetalViewportDepth,
    cohort_resource_ffi::{MainSourceKey, reserve_main_sources},
};
use core::ffi::c_void;
use std::panic::{AssertUnwindSafe, catch_unwind};
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct SceneRequest {
    pub abi_version: u32,
    pub struct_size: u32,
    pub vm: u64,
    pub invocation: u64,
    pub owner: u64,
    pub pass_occurrence: u64,
    pub engine_update: u64,
    pub publication: u64,
    pub camera: u32,
    pub viewport_scale: u32,
    pub viewport_offset: u32,
    pub reserved: u32,
    pub max_draws: u32,
    pub reserved1: u32,
    pub max_bytes: u64,
}
#[repr(C)]
pub struct ClientDraw {
    pub abi_version: u32,
    pub struct_size: u32,
    pub source_item: u64,
    pub source_partition: u64,
    pub positions: *const [f32; 4],
    pub indices: *const u16,
    pub vertex_count: u32,
    pub index_count: u32,
    pub position_space: u32,
    pub reserved: u32,
    pub unresolved_obligations: u64,
}
#[repr(C)]
pub struct SceneItem {
    pub source_item: u64,
    pub unresolved_obligations: u64,
    pub admitted_draws: u32,
    pub projection_failures: u32,
}
#[repr(C)]
pub struct SceneView {
    pub abi_version: u32,
    pub struct_size: u32,
    pub engine_update: u64,
    pub cohort: u64,
    pub publication: u64,
    pub pass_occurrence: u64,
    pub intended_items: u64,
    pub draws: *const ClientDraw,
    pub draw_count: u32,
    pub reserved: u32,
    pub items: *const SceneItem,
    pub item_count: u32,
    pub reserved1: u32,
}
pub type SceneSubmit = unsafe extern "C" fn(*mut c_void, *const SceneView) -> i32;
/// # Safety
/// Callback and opaque must be valid for the synchronous call, must not unwind,
/// and must copy any needed input before returning. Negative means definitely
/// uncommitted. Zero/positive means committed or indeterminate and is never
/// replayable. This entrypoint must run only after real UI/source/target ordering
/// qualification, using the Main viewport; it does not establish those facts.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mag_render_adapter_submit_scene(
    request: SceneRequest,
    submit: Option<SceneSubmit>,
    opaque: *mut c_void,
) -> i32 {
    // Definitely uncommitted while the master switch is off.
    if !mag_render_adapter_core::runtime::enabled() {
        return -1;
    }
    let mut effects_possible = false;
    let result = catch_unwind(AssertUnwindSafe(|| {
        if request.abi_version != 1
            || request.struct_size as usize != size_of::<SceneRequest>()
            || request.reserved != 0
            || request.reserved1 != 0
            || request.vm == 0
            || request.invocation == 0
            || request.owner == 0
            || request.pass_occurrence == 0
            || request.engine_update == 0
            || request.publication == 0
            || request.max_draws == 0
            || request.max_draws > 16384
            || request.max_bytes == 0
            || request.max_bytes > 64 * 1024 * 1024
        {
            return -1;
        }
        let Some(submit) = submit else {
            return -1;
        };
        let Some(claim) = reserve_main_sources(MainSourceKey {
            vm: request.vm,
            invocation: request.invocation,
            owner: request.owner,
            pass_occurrence: request.pass_occurrence,
            execution_update: request.engine_update,
            camera: request.camera,
        }) else {
            return -3;
        };
        let Ok(scene) = prepare_metal_scene(
            &claim.source.cohort,
            &claim.source.geometry,
            MetalViewportDepth {
                source_scale: request.viewport_scale,
                source_offset: request.viewport_offset,
            },
            SceneBudget {
                draws: request.max_draws as usize,
                bytes: request.max_bytes as usize,
            },
        ) else {
            return -5;
        };
        let mut draws = Vec::new();
        let mut items = Vec::new();
        if draws.try_reserve_exact(scene.draws.len()).is_err()
            || items.try_reserve_exact(scene.items.len()).is_err()
        {
            return -5;
        }
        for item in &scene.items {
            items.push(SceneItem {
                source_item: item.ordinal as u64,
                unresolved_obligations: obligations(&item.omissions)
                    | if item.projection_failures.is_empty() {
                        0
                    } else {
                        OMIT_PROJECTION
                    },
                admitted_draws: 0,
                projection_failures: item.projection_failures.len() as u32,
            });
        }
        for draw in &scene.draws {
            let (Ok(vertex_count), Ok(index_count)) = (
                u32::try_from(draw.positions.len()),
                u32::try_from(draw.indices.len()),
            ) else {
                return -5;
            };
            items[draw.source_item as usize].admitted_draws += 1;
            draws.push(ClientDraw {
                abi_version: 1,
                struct_size: size_of::<ClientDraw>() as u32,
                source_item: draw.source_item,
                source_partition: draw.source_partition,
                positions: draw.positions.as_ptr(),
                indices: draw.indices.as_ptr(),
                vertex_count,
                index_count,
                position_space: 1,
                reserved: 0,
                unresolved_obligations: draw.unresolved_obligations,
            });
        }
        let view = SceneView {
            abi_version: 1,
            struct_size: size_of::<SceneView>() as u32,
            engine_update: scene.source.execution_update,
            cohort: scene.source.source_invocation,
            publication: request.publication,
            pass_occurrence: scene.source.pass_occurrence,
            intended_items: scene.intended_items,
            draws: draws.as_ptr(),
            draw_count: draws.len() as u32,
            reserved: 0,
            items: items.as_ptr(),
            item_count: items.len() as u32,
            reserved1: 0,
        };
        // SAFETY: callback contract above; all pointers reference owned stable arrays
        // whose lifetimes span this synchronous call, and no guest pointers escape.
        let status = unsafe { submit(opaque, &view) };
        if status >= 0 {
            effects_possible = true;
            claim.consume();
            if status == 0 { 0 } else { 1 }
        } else {
            status
        }
    }));
    result.unwrap_or(if effects_possible { 1 } else { -6 })
}
