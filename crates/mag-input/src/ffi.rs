//! Fixed-width C boundary. Callers retain all buffer ownership.
use crate::delivery::Offer;
use crate::engine::{
    Behavior, Binding, Capture, Context, ContextKey, Control, Engine, Event, FrameKey, Source,
};
use crate::policy::{Mode, Policy};
use std::collections::{BTreeMap, BTreeSet};
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::sync::{Mutex, OnceLock};

pub const OK: i32 = 0;
pub const INVALID: i32 = 1;
pub const STALE: i32 = 2;
pub const ABSENT: i32 = 3;
pub const FAULTED: i32 = 4;
pub const ENGINE_ERROR: i32 = 5;
pub const CAPACITY: i32 = 6;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct BindingRecord {
    pub source: u32,
    pub code: u32,
    pub action: u32,
    pub contexts: u32,
    pub behavior: u32,
}
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct FrameKeyRecord {
    pub session: u64,
    pub sequence: u64,
    pub slot: u32,
    pub reserved: u32,
}
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct FrameRecord {
    pub generation: u64,
    pub dx: f64,
    pub dy: f64,
    pub action_count: u32,
    pub reserved: u32,
}
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct OfferRecord {
    pub pulse_id: u64,
    pub duration_ns: u64,
    pub down: u32,
    pub source_mask: u32,
}

#[derive(Default)]
struct RecognitionState {
    recognizer: crate::gesture::Recognizer,
    sources: u8,
    action: u32,
    native_contribution: bool,
}

struct Runtime {
    engine: Engine,
    faulted: bool,
    policy: Policy,
    now_ns: u64,
    capture_flags: u32,
    recognition_generation: u64,
    shared_native_actions: BTreeSet<u32>,
    recognizers: BTreeMap<(u64, u64), RecognitionState>,
}
#[derive(Default)]
struct Registry {
    next: u64,
    engines: BTreeMap<u64, Runtime>,
}
fn registry() -> &'static Mutex<Registry> {
    static REGISTRY: OnceLock<Mutex<Registry>> = OnceLock::new();
    REGISTRY.get_or_init(|| Mutex::new(Registry::default()))
}
fn source(tag: u32) -> Result<Source, i32> {
    match tag {
        0 => Ok(Source::Keyboard),
        1 => Ok(Source::Mouse),
        2 => Ok(Source::Controller),
        _ => Err(INVALID),
    }
}
fn context(tag: u32) -> Result<Context, i32> {
    match tag {
        1 => Ok(Context::OnFoot),
        2 => Ok(Context::Vehicle),
        4 => Ok(Context::Turret),
        8 => Ok(Context::Tactical),
        16 => Ok(Context::Menu),
        32 => Ok(Context::Settings),
        64 => Ok(Context::Rebinding),
        _ => Err(INVALID),
    }
}
fn key(record: FrameKeyRecord) -> Result<FrameKey, i32> {
    if record.reserved != 0 {
        return Err(INVALID);
    }
    Ok(FrameKey {
        session: record.session,
        sequence: record.sequence,
        player_slot: record.slot,
    })
}
fn operate(handle: u64, operation: impl FnOnce(&mut Runtime) -> Result<(), i32>) -> i32 {
    let Ok(mut registry) = registry().lock() else {
        return FAULTED;
    };
    let Some(runtime) = registry.engines.get_mut(&handle) else {
        return STALE;
    };
    if runtime.faulted {
        return FAULTED;
    }
    match catch_unwind(AssertUnwindSafe(|| operation(runtime))) {
        Ok(Ok(())) => OK,
        Ok(Err(status)) => status,
        Err(_) => {
            runtime.faulted = true;
            FAULTED
        }
    }
}
unsafe fn bindings(pointer: *const BindingRecord, count: u32) -> Result<Vec<Binding>, i32> {
    if count > 1024 || (count != 0 && pointer.is_null()) {
        return Err(INVALID);
    }
    let records = if count == 0 {
        &[]
    } else {
        unsafe { std::slice::from_raw_parts(pointer, count as usize) }
    };
    records
        .iter()
        .map(|record| {
            Ok(Binding {
                control: Control {
                    source: source(record.source)?,
                    code: record.code,
                },
                action: record.action,
                contexts: record.contexts,
                behavior: match record.behavior {
                    0 => Behavior::Hold,
                    1 => Behavior::Toggle,
                    _ => return Err(INVALID),
                },
            })
        })
        .collect()
}
fn offer(record: OfferRecord) -> Result<Offer, i32> {
    if record.down > 1 || record.source_mask > 7 {
        return Err(INVALID);
    }
    Ok(Offer {
        pulse_id: record.pulse_id,
        physical_duration_ns: record.duration_ns,
        down: record.down != 0,
        source_mask: record.source_mask as u8,
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn mag_input_abi_version() -> u32 {
    1
}
/// # Safety
/// Bindings must point to `count` readable records; output must point to writable aligned storage.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mag_input_create(
    session: u64,
    pointer: *const BindingRecord,
    count: u32,
    output: *mut u64,
) -> i32 {
    if output.is_null() {
        return INVALID;
    }
    let bindings = match unsafe { bindings(pointer, count) } {
        Ok(value) => value,
        Err(status) => return status,
    };
    let engine = match Engine::new(session, bindings) {
        Ok(value) => value,
        Err(_) => return ENGINE_ERROR,
    };
    let Ok(mut registry) = registry().lock() else {
        return FAULTED;
    };
    if registry.engines.len() >= 64 {
        return CAPACITY;
    }
    let Some(handle) = registry.next.checked_add(1) else {
        return CAPACITY;
    };
    registry.next = handle;
    registry.engines.insert(
        handle,
        Runtime {
            engine,
            faulted: false,
            policy: Policy::default(),
            now_ns: 0,
            capture_flags: 0,
            recognition_generation: 1,
            shared_native_actions: BTreeSet::new(),
            recognizers: BTreeMap::new(),
        },
    );
    unsafe { output.write(handle) };
    OK
}
#[unsafe(no_mangle)]
pub extern "C" fn mag_input_destroy(handle: u64) -> i32 {
    let Ok(mut registry) = registry().lock() else {
        return FAULTED;
    };
    if registry.engines.remove(&handle).is_some() {
        OK
    } else {
        STALE
    }
}
/// # Safety
/// Bindings must point to `count` readable aligned records.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mag_input_configure(
    handle: u64,
    pointer: *const BindingRecord,
    count: u32,
) -> i32 {
    let bindings = match unsafe { bindings(pointer, count) } {
        Ok(value) => value,
        Err(status) => return status,
    };
    operate(handle, |runtime| {
        runtime
            .engine
            .configure(bindings)
            .map_err(|_| ENGINE_ERROR)?;
        runtime.recognizers.clear();
        runtime.recognition_generation = runtime.engine.generation();
        Ok(())
    })
}
/// # Safety
/// Output must point to writable aligned storage for a u64.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mag_input_capture(handle: u64, flags: u32, output: *mut u64) -> i32 {
    if flags & !15 != 0 || output.is_null() {
        return INVALID;
    }
    operate(handle, |runtime| {
        let generation = runtime
            .engine
            .set_capture(Capture {
                focused: flags & 1 != 0,
                supported_context: flags & 2 != 0,
                mouse_captured: flags & 4 != 0,
                controller_connected: flags & 8 != 0,
            })
            .map_err(|_| ENGINE_ERROR)?;
        let lost = runtime.capture_flags & !flags;
        if lost & 3 != 0 {
            runtime.recognizers.clear()
        } else {
            let menu = runtime.engine.menu_context();
            runtime.recognizers.retain(|_, state| {
                !((!menu && lost & 4 != 0 && state.sources & 2 != 0)
                    || (lost & 8 != 0 && state.sources & 4 != 0))
            })
        }
        runtime.capture_flags = flags;
        runtime.recognition_generation = generation;
        unsafe { output.write(generation) };
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub extern "C" fn mag_input_event(
    handle: u64,
    device: u64,
    tag: u32,
    code: u32,
    down: u32,
    time_ns: u64,
    generation: u64,
) -> i32 {
    if down > 1 {
        return INVALID;
    }
    let source = match source(tag) {
        Ok(value) => value,
        Err(status) => return status,
    };
    operate(handle, |runtime| {
        runtime
            .engine
            .push(Event {
                device,
                control: Control { source, code },
                down: down != 0,
                time_ns,
                generation,
            })
            .map_err(|_| ENGINE_ERROR)
    })
}
#[unsafe(no_mangle)]
pub extern "C" fn mag_input_mouse(handle: u64, generation: u64, dx: f64, dy: f64) -> i32 {
    operate(handle, |runtime| {
        runtime
            .engine
            .push_mouse(generation, dx, dy)
            .map_err(|_| ENGINE_ERROR)
    })
}
#[unsafe(no_mangle)]
pub extern "C" fn mag_input_sample(handle: u64, record: FrameKeyRecord) -> i32 {
    operate(handle, |runtime| {
        runtime
            .engine
            .sample(key(record)?)
            .map_err(|_| ENGINE_ERROR)
    })
}
/// # Safety
/// Output must point to writable aligned FrameRecord storage.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mag_input_seal(
    handle: u64,
    record: FrameKeyRecord,
    now_ns: u64,
    owner: u64,
    tag: u32,
    output: *mut FrameRecord,
) -> i32 {
    if output.is_null() {
        return INVALID;
    }
    operate(handle, |runtime| {
        let next = ContextKey {
            owner,
            context: context(tag)?,
        };
        let native_shared = runtime.recognition_generation == runtime.engine.generation()
            && runtime.engine.same_owner_context_change(next);
        let shared = if runtime.recognition_generation == runtime.engine.generation() {
            runtime.engine.shared_context_actions(next)
        } else {
            Default::default()
        };
        let frame = runtime
            .engine
            .seal(key(record)?, now_ns, next)
            .map_err(|_| ENGINE_ERROR)?;
        if runtime.recognition_generation != frame.generation {
            runtime.recognizers.retain(|_, state| {
                shared.contains(&state.action)
                    || (native_shared
                        && state.native_contribution
                        && runtime.shared_native_actions.contains(&state.action))
            });
            runtime.recognition_generation = frame.generation;
        }
        runtime.now_ns = frame.time_ns;
        unsafe {
            output.write(FrameRecord {
                generation: frame.generation,
                dx: frame.mouse.dx,
                dy: frame.mouse.dy,
                action_count: frame.actions.len() as u32,
                reserved: 0,
            })
        };
        Ok(())
    })
}
/// # Safety
/// Output must point to writable aligned OfferRecord storage.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mag_input_offer(
    handle: u64,
    record: FrameKeyRecord,
    action: u32,
    output: *mut OfferRecord,
) -> i32 {
    if output.is_null() {
        return INVALID;
    }
    operate(handle, |runtime| {
        let offer = runtime
            .engine
            .current_frame(key(record)?)
            .map_err(|_| ENGINE_ERROR)?
            .action(action)
            .ok_or(ABSENT)?;
        unsafe {
            output.write(OfferRecord {
                pulse_id: offer.pulse_id,
                duration_ns: offer.physical_duration_ns,
                down: u32::from(offer.down),
                source_mask: u32::from(offer.source_mask),
            })
        };
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub extern "C" fn mag_input_observe(
    handle: u64,
    record: FrameKeyRecord,
    action: u32,
    value: OfferRecord,
) -> i32 {
    operate(handle, |runtime| {
        runtime
            .engine
            .observe(key(record)?, action, offer(value)?)
            .map_err(|_| ENGINE_ERROR)
    })
}
#[unsafe(no_mangle)]
pub extern "C" fn mag_input_retire(handle: u64, record: FrameKeyRecord) -> i32 {
    operate(handle, |runtime| {
        runtime
            .engine
            .retire(key(record)?)
            .map_err(|_| ENGINE_ERROR)
    })
}
/// # Safety
/// Output must point to writable aligned u64 storage. Source 3 revokes all input.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mag_input_reset(handle: u64, tag: u32, output: *mut u64) -> i32 {
    if output.is_null() || tag > 3 {
        return INVALID;
    }
    operate(handle, |runtime| {
        runtime
            .engine
            .reset(if tag == 3 { None } else { Some(source(tag)?) })
            .map_err(|_| ENGINE_ERROR)?;
        if tag == 3 {
            runtime.recognizers.clear()
        } else {
            runtime
                .recognizers
                .retain(|_, state| state.sources & (1 << tag) == 0)
        }
        runtime.recognition_generation = runtime.engine.generation();
        unsafe { output.write(runtime.engine.generation()) };
        Ok(())
    })
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct PolicyRecord {
    pub mode: u32,
    pub mouse_owner: u32,
    pub flags: u32,
    pub reserved: u32,
}
fn mode(tag: u32) -> Result<Mode, i32> {
    match tag {
        0 => Ok(Mode::Auto),
        1 => Ok(Mode::Controller),
        2 => Ok(Mode::Kbm),
        _ => Err(INVALID),
    }
}
fn policy_record(policy: Policy) -> PolicyRecord {
    PolicyRecord {
        mode: policy.mode as u32,
        mouse_owner: u32::from(policy.mouse_owner),
        flags: u32::from(policy.kbm_used)
            | (u32::from(policy.stick_rearmed) << 1)
            | (u32::from(policy.controller_connected) << 2),
        reserved: 0,
    }
}
/// # Safety
/// Output must point to writable aligned PolicyRecord storage. Flags encode active, connected, neutral, fresh.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mag_input_policy(
    handle: u64,
    tag: u32,
    flags: u32,
    output: *mut PolicyRecord,
) -> i32 {
    if output.is_null() || flags & !15 != 0 {
        return INVALID;
    }
    operate(handle, |runtime| {
        runtime.policy.set_mode(mode(tag)?);
        runtime.policy.update_capture(
            flags & 1 != 0,
            flags & 2 != 0,
            flags & 4 != 0,
            flags & 8 != 0,
        );
        unsafe { output.write(policy_record(runtime.policy)) };
        Ok(())
    })
}
/// # Safety
/// Output must point to writable aligned PolicyRecord storage. This operation accepts no gameplay input.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mag_input_policy_read(handle: u64, output: *mut PolicyRecord) -> i32 {
    if output.is_null() {
        return INVALID;
    }
    operate(handle, |runtime| {
        unsafe { output.write(policy_record(runtime.policy)) };
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub extern "C" fn mag_input_accept_action(
    handle: u64,
    record: FrameKeyRecord,
    action: u32,
    value: OfferRecord,
) -> i32 {
    operate(handle, |runtime| {
        let current = runtime
            .engine
            .current_frame(key(record)?)
            .map_err(|_| ENGINE_ERROR)?
            .action(action)
            .ok_or(ABSENT)?;
        if current != offer(value)? {
            return Err(STALE);
        }
        runtime.policy.accept_sources(current.source_mask);
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub extern "C" fn mag_input_accept_mouse(handle: u64, record: FrameKeyRecord, neutral: u32) -> i32 {
    if neutral > 1 {
        return INVALID;
    }
    operate(handle, |runtime| {
        let frame = runtime
            .engine
            .current_frame(key(record)?)
            .map_err(|_| ENGINE_ERROR)?;
        if frame
            .look(crate::engine::Capabilities { look: true })
            .is_none()
            || (frame.mouse.dx == 0.0 && frame.mouse.dy == 0.0)
        {
            return Err(INVALID);
        }
        runtime.policy.accept_mouse(neutral != 0);
        Ok(())
    })
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct RecognitionRecord {
    pub value: f64,
    pub elapsed_ns: u64,
    pub phase: u32,
    pub events: u32,
}
/// # Safety
/// Output must point to writable aligned RecognitionRecord storage.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mag_input_recognize(
    handle: u64,
    record: FrameKeyRecord,
    consumer: u64,
    control: u64,
    action: u32,
    value: f64,
    blocked: u32,
    config: crate::gesture::Config,
    output: *mut RecognitionRecord,
) -> i32 {
    if output.is_null() || consumer == 0 || control == 0 || action == 0 || blocked > 1 {
        return INVALID;
    }
    operate(handle, |runtime| {
        let frame = runtime
            .engine
            .current_frame(key(record)?)
            .map_err(|_| ENGINE_ERROR)?;
        let offer = frame.action(action);
        if runtime.recognizers.len() >= 4096
            && !runtime.recognizers.contains_key(&(consumer, control))
        {
            return Err(CAPACITY);
        }
        let state = runtime.recognizers.entry((consumer, control)).or_default();
        if state.action != 0 && state.action != action {
            return Err(INVALID);
        }
        state.action = action;
        if let Some(offer) = offer {
            state.sources = offer.source_mask;
            state.native_contribution = false;
        } else if value > 0.0 {
            state.sources = 4;
            state.native_contribution = true;
        }
        let output_value = state
            .recognizer
            .update(
                record.sequence,
                runtime.now_ns,
                offer,
                value,
                blocked != 0,
                config,
            )
            .map_err(|_| ENGINE_ERROR)?;
        unsafe {
            output.write(RecognitionRecord {
                value: output_value.value,
                elapsed_ns: output_value.elapsed_ns,
                phase: output_value.phase as u32,
                events: output_value.events,
            })
        };
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub extern "C" fn mag_input_forget_consumer(handle: u64, consumer: u64) -> i32 {
    operate(handle, |runtime| {
        runtime.recognizers.retain(|(id, _), _| *id != consumer);
        Ok(())
    })
}

/// # Safety
/// Output must point to writable aligned PolicyRecord storage. Event flags are active1,connected2,neutral4,fresh8,acceptedKBM16,acceptedMouse32.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mag_input_policy_reduce(
    previous: PolicyRecord,
    requested: u32,
    flags: u32,
    output: *mut PolicyRecord,
) -> i32 {
    if output.is_null()
        || flags & !63 != 0
        || previous.reserved != 0
        || previous.flags & !7 != 0
        || previous.mouse_owner > 1
    {
        return INVALID;
    }
    let mut policy = Policy {
        mode: match mode(previous.mode) {
            Ok(mode) => mode,
            Err(status) => return status,
        },
        mouse_owner: previous.mouse_owner != 0,
        kbm_used: previous.flags & 1 != 0,
        stick_rearmed: previous.flags & 2 != 0,
        controller_connected: previous.flags & 4 != 0,
    };
    if requested != u32::MAX {
        match mode(requested) {
            Ok(mode) => policy.set_mode(mode),
            Err(status) => return status,
        }
    }
    policy.update_capture(
        flags & 1 != 0,
        flags & 2 != 0,
        flags & 4 != 0,
        flags & 8 != 0,
    );
    if flags & 1 != 0 {
        if flags & 16 != 0 {
            policy.accept_sources(3)
        }
        if flags & 32 != 0 {
            policy.accept_mouse(flags & 4 != 0)
        }
    }
    unsafe { output.write(policy_record(policy)) };
    OK
}

/// Edit a copied draft. Conflict status7 returns the conflicting action ID.
/// No buffers are changed on failure except output_count/conflict.
/// # Safety
/// Input records must be readable; output must hold capacity records and both
/// result pointers must be writable. Buffers must not overlap.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mag_input_edit_binding(
    input: *const BindingRecord,
    count: u32,
    request: BindingRecord,
    slot: u32,
    flags: u32,
    output: *mut BindingRecord,
    capacity: u32,
    output_count: *mut u32,
    conflict: *mut u32,
) -> i32 {
    if output_count.is_null()
        || conflict.is_null()
        || flags & !3 != 0
        || capacity > 1024
        || (capacity != 0 && output.is_null())
    {
        return INVALID;
    }
    let original = match unsafe { bindings(input, count) } {
        Ok(b) => b,
        Err(e) => return e,
    };
    let request = match unsafe { bindings(&request, 1) } {
        Ok(b) => b[0],
        Err(e) => return e,
    };
    let result = match crate::editor::edit(&original, request, slot, flags & 1 != 0, flags & 2 != 0)
    {
        Ok(b) => b,
        Err(crate::editor::EditError::Invalid) => return INVALID,
        Err(crate::editor::EditError::Conflict(action)) => {
            unsafe {
                conflict.write(action);
            }
            return 7;
        }
    };
    unsafe {
        output_count.write(result.len() as u32);
        conflict.write(0);
    }
    if result.len() > capacity as usize {
        return CAPACITY;
    }
    for (i, b) in result.iter().enumerate() {
        unsafe {
            output.add(i).write(BindingRecord {
                source: b.control.source as u32,
                code: b.control.code,
                action: b.action,
                contexts: b.contexts,
                behavior: b.behavior as u32,
            });
        }
    }
    OK
}

/// Identify native-controller button recognizers allowed across same-owner
/// context changes. Host pulse continuity still requires shared physical bindings.
/// # Safety
/// IDs must hold count readable u32s; null is valid only for count zero.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mag_input_shared_native_actions(
    handle: u64,
    ids: *const u32,
    count: u32,
) -> i32 {
    if count > 1024 || (count != 0 && ids.is_null()) {
        return INVALID;
    }
    let ids = if count == 0 {
        &[]
    } else {
        unsafe { std::slice::from_raw_parts(ids, count as usize) }
    };
    if ids.contains(&0) {
        return INVALID;
    }
    operate(handle, |runtime| {
        runtime.shared_native_actions = ids.iter().copied().collect();
        Ok(())
    })
}
