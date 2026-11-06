//! CPU-only recording owner. No shader/GPU execution or presentation claim.
//! The actual ScenePacket/SceneBuilder/SceneDevice allocation and descriptor
//! conversion run; the final callback deeply copies values before returning.
use super::*;
use crate::client::scene_packet::{self, packet_tests};
use mag_render::scene::SceneCatalog;
use std::{collections::BTreeMap, mem::MaybeUninit};
#[derive(Default)]
struct Recorded {
    writes: Vec<(u64, u64, u64)>,
    vertices: Vec<(u64, u64, u64)>,
    groups: Vec<[u32; 3]>,
}
#[derive(Default)]
struct Owner {
    next_buffer: u64,
    buffers: BTreeMap<u64, Vec<u8>>,
    schemas: BTreeMap<u64, Vec<BindingLayout>>,
    records: Vec<Recorded>,
    full: Vec<Vec<u64>>,
    released: usize,
    reads: usize,
    done: bool,
    sent: bool,
    fail: bool,
}
unsafe fn owner<'a>(p: *mut c_void) -> &'a mut Owner {
    unsafe { &mut *p.cast::<Owner>() }
}
unsafe extern "C" fn caps(_p: *mut c_void, out: *mut Caps) -> i32 {
    unsafe {
        *out = Caps {
            struct_size: size_of::<Caps>() as u32,
            abi_version: 1,
            device_generation: 42,
            compute_supported: 1,
            draws_supported: 1,
            max_pending: 2,
            max_bindings: 16,
            runtime_sizes_slot: 30,
            reserved: 0,
        }
    };
    0
}
unsafe extern "C" fn buffer_create(p: *mut c_void, d: *const BufferDesc, out: *mut Buffer) -> i32 {
    let d = unsafe { &*d };
    let Ok(n) = usize::try_from(d.byte_length) else {
        return -1;
    };
    if n > 1 << 20 {
        return -5;
    }
    let bytes = if d.initial_bytes.is_null() {
        vec![0; n]
    } else {
        unsafe { std::slice::from_raw_parts(d.initial_bytes.cast::<u8>(), n) }.to_vec()
    };
    let s = unsafe { owner(p) };
    s.next_buffer += 1;
    let id = s.next_buffer;
    s.buffers.insert(id, bytes);
    unsafe {
        *out = Buffer {
            device_generation: 42,
            id,
            version: 1,
        }
    };
    0
}
unsafe extern "C" fn buffer_release(p: *mut c_void, b: Buffer) -> i32 {
    let s = unsafe { owner(p) };
    if s.buffers.remove(&b.id).is_none() {
        return -3;
    }
    s.released += 1;
    0
}
unsafe extern "C" fn buffer_read(
    p: *mut c_void,
    _b: Buffer,
    _a: u64,
    _o: *mut c_void,
    _n: u64,
) -> i32 {
    unsafe { owner(p) }.reads += 1;
    -2
}
unsafe extern "C" fn pipeline_create(
    p: *mut c_void,
    d: *const PipelineDesc,
    out: *mut Pipeline,
) -> i32 {
    let d = unsafe { &*d };
    if d.binding_count > 16 || d.threads_per_group.contains(&0) {
        return -1;
    }
    let schema = unsafe { std::slice::from_raw_parts(d.bindings, d.binding_count as usize) };
    if schema
        .iter()
        .any(|b| b.binding_kind == 1 && (b.access != 1 || b.runtime_array_stride != 0))
    {
        return -1;
    }
    let s = unsafe { owner(p) };
    s.schemas.insert(
        2,
        unsafe { std::slice::from_raw_parts(d.bindings, d.binding_count as usize) }.to_vec(),
    );
    unsafe {
        *out = Pipeline {
            device_generation: 42,
            id: 2,
        }
    };
    0
}
unsafe extern "C" fn pipeline_release(_p: *mut c_void, _v: Pipeline) -> i32 {
    0
}
unsafe extern "C" fn frame_submit(_p: *mut c_void, _f: *const Frame, _t: *mut Ticket) -> i32 {
    -2
}
unsafe extern "C" fn poll(p: *mut c_void, out: *mut Event, capacity: u32, count: *mut u32) -> i32 {
    let s = unsafe { owner(p) };
    unsafe { *count = 0 };
    if s.done && !s.sent && capacity != 0 {
        unsafe {
            *out = Event {
                struct_size: size_of::<Event>() as u32,
                abi_version: 1,
                ticket: Ticket {
                    device_generation: 42,
                    submission_id: s.records.len() as u64,
                },
                succeeded: 1,
                no_further_gpu_access: 1,
                native_status: 0,
                reserved: 0,
            };
            *count = 1
        };
        s.sent = true;
    }
    0
}
unsafe extern "C" fn record(p: *mut c_void, input: *const Scene, out: *mut Ticket) -> i32 {
    let scene = unsafe { &*input };
    let s = unsafe { owner(p) };
    let mut saved = Recorded::default();
    for dispatch in
        unsafe { std::slice::from_raw_parts(scene.dispatches, scene.dispatch_count as usize) }
    {
        saved.groups.push(dispatch.groups);
        let Some(schema) = s.schemas.get(&dispatch.pipeline.id) else {
            return -1;
        };
        for b in unsafe {
            std::slice::from_raw_parts(dispatch.bindings, dispatch.binding_count as usize)
        } {
            if !s.buffers.contains_key(&b.buffer.id) {
                return -1;
            }
            if schema.iter().any(|q| q.slot == b.slot && q.access == 2) {
                saved.writes.push((b.buffer.id, b.offset, b.byte_length));
            }
        }
    }
    for pass in unsafe { std::slice::from_raw_parts(scene.passes, scene.pass_count as usize) } {
        for draw in unsafe { std::slice::from_raw_parts(pass.draws, pass.draw_count as usize) } {
            saved
                .vertices
                .push((draw.vertices.id, draw.vertex_offset, draw.vertex_length));
        }
    }
    s.records.push(saved);
    if s.fail {
        return -1;
    }
    unsafe {
        s.sent = false;
        *out = Ticket {
            device_generation: 42,
            submission_id: s.records.len() as u64,
        }
    };
    0
}
/// Records every field the native encoder reads, following each pointer,
/// with buffer ids renumbered in first-use order so that two submissions of
/// the same packet compare equal whatever fresh buffers they were given.
unsafe extern "C" fn record_full(p: *mut c_void, input: *const Scene, out: *mut Ticket) -> i32 {
    let scene = unsafe { &*input };
    let s = unsafe { owner(p) };
    let mut ids: Vec<u64> = Vec::new();
    let mut w: Vec<u64> = Vec::new();
    let mut buffer = |w: &mut Vec<u64>, b: Buffer| {
        let n = ids.iter().position(|&i| i == b.id).unwrap_or_else(|| {
            ids.push(b.id);
            ids.len() - 1
        });
        w.extend([n as u64, b.device_generation, b.version]);
    };
    let binding = |w: &mut Vec<u64>, b: &Binding, buffer: &mut dyn FnMut(&mut Vec<u64>, Buffer)| {
        w.extend([b.slot as u64, b.reserved as u64, b.offset, b.byte_length]);
        buffer(w, b.buffer);
    };
    w.extend([
        scene.resource_count as u64,
        scene.image_count as u64,
        scene.output_image as u64,
    ]);
    for r in unsafe { std::slice::from_raw_parts(scene.resources, scene.resource_count as usize) } {
        w.extend([r.role as u64]);
        buffer(&mut w, r.buffer);
    }
    for im in unsafe { std::slice::from_raw_parts(scene.images, scene.image_count as usize) } {
        w.extend([im.format as u64, im.width as u64, im.height as u64]);
        buffer(&mut w, im.upload);
    }
    for f in unsafe { std::slice::from_raw_parts(scene.dispatches, scene.dispatch_count as usize) }
    {
        w.extend([
            f.pipeline.id,
            f.binding_count as u64,
            f.groups[0] as u64,
            f.groups[1] as u64,
            f.groups[2] as u64,
        ]);
        for b in unsafe { std::slice::from_raw_parts(f.bindings, f.binding_count as usize) } {
            binding(&mut w, b, &mut buffer);
        }
    }
    for pass in unsafe { std::slice::from_raw_parts(scene.passes, scene.pass_count as usize) } {
        for a in [pass.color, pass.depth] {
            w.extend([a.image as u64, a.load as u64, a.store as u64]);
            w.extend(a.clear.map(|c| c.to_bits() as u64));
        }
        w.push(pass.draw_count as u64);
        for d in unsafe { std::slice::from_raw_parts(pass.draws, pass.draw_count as usize) } {
            w.extend([
                d.pipeline.id,
                d.vertex_offset,
                d.vertex_length,
                d.index_offset,
                d.index_length,
            ]);
            w.extend([
                d.index_count as u64,
                d.buffer_count as u64,
                d.texture_count as u64,
            ]);
            buffer(&mut w, d.vertices);
            buffer(&mut w, d.indices);
            for b in unsafe { std::slice::from_raw_parts(d.buffers, d.buffer_count as usize) } {
                w.extend([b.stage as u64]);
                binding(&mut w, &b.binding, &mut buffer);
            }
            for t in unsafe { std::slice::from_raw_parts(d.textures, d.texture_count as usize) } {
                w.extend(
                    [t.stage, t.texture_slot, t.sampler_slot, t.image, t.sampler].map(u64::from),
                );
            }
        }
    }
    s.full.push(w);
    unsafe { record(p, input, out) }
}
fn api(state: &mut Owner) -> SdkApi {
    let mut api = unsafe { MaybeUninit::<SdkApi>::zeroed().assume_init() };
    api.abi_version = 1;
    api.struct_size = size_of::<SdkApi>() as u32;
    // This function fills only ABI metadata/function addresses; it does not
    // dereference or treat this CPU recorder as a native GPU owner.
    assert_eq!(
        unsafe { crate::mag_render_native_get_api((state as *mut Owner).cast(), &mut api) },
        0
    );
    api.caps = Some(caps);
    api.buffer_create = Some(buffer_create);
    api.buffer_release = Some(buffer_release);
    api.buffer_read = Some(buffer_read);
    api.pipeline_create = Some(pipeline_create);
    api.pipeline_release = Some(pipeline_release);
    api.submit = Some(frame_submit);
    api.poll = Some(poll);
    api
}
fn setup(state: &mut Owner) -> (SceneDevice<'static>, scene_packet::ScenePacket) {
    setup_with(state, false)
}
fn setup_with(
    state: &mut Owner,
    textures: bool,
) -> (SceneDevice<'static>, scene_packet::ScenePacket) {
    let packet = scene_packet::compute::tests::copied_fixture_with_textures(textures);
    let inner = BatchDevice::from_api_with_byte_cap(
        api(state),
        BatchDeviceLimits {
            max_bytes: 1 << 20,
            max_pipelines: 16,
            diagnostic_readback: false,
        },
        None,
        MAX_SCENE_BYTES,
    )
    .unwrap();
    let mut device = SceneDevice {
        submit_recorder: Some(record),
        scratch: SubmitScratch::default(),
        samplers: scene_sampler::Cache::default(),
        frame: None,
        target: None,
        imports: vec![],
        inner,
        graphics: BTreeMap::new(),
        rasters: BTreeMap::new(),
    };
    let p = &packet.programs[2];
    let shader = ShaderStage {
        source: p.source.as_ptr().cast(),
        source_length: p.source.len() as u64,
        entry: p.entry.as_ptr().cast(),
        entry_length: p.entry.len() as u32,
        language: p.language,
    };
    let desc = PipelineDesc {
        struct_size: size_of::<PipelineDesc>() as u32,
        abi_version: 1,
        source_bytes: shader.source,
        source_length: shader.source_length,
        entry_point: shader.entry,
        entry_point_length: shader.entry_length,
        binding_count: p.buffers.len() as u32,
        bindings: p.buffers.as_ptr(),
        threads_per_group: p.threads,
        shader_language: p.language,
    };
    assert_eq!(unsafe { device.compute_pipeline(&desc) }, Ok(2));
    device.inner.pipelines.insert(
        1,
        Definition {
            handle: Pipeline {
                device_generation: 42,
                id: 1,
            },
            compute: None,
            graphics: true,
        },
    );
    device
        .graphics
        .insert(1, packet_tests::Catalog(&packet).scene_graphics(1).unwrap());
    device.rasters.insert(1, packet.pipelines[0].state);
    (device, packet)
}
#[test]
fn packet_reaches_real_scene_device_encoder_and_retains_to_recorded_terminal() {
    let mut state = Box::<Owner>::default();
    let (mut device, packet) = setup(&mut state);
    // This test covers single-use uploads: pin resident uploads off.
    device.set_resident_uploads(false);
    let prepared = packet.prepare(&device, &[1, 2], 32, 1 << 20).unwrap();
    let (commands, leases) = prepared.into_submission();
    let mut backend = RenderBackend::new(
        device,
        BackendLimits {
            max_leases_per_submission: 16,
            max_bytes_per_submission: 1 << 20,
        },
    )
    .unwrap();
    let ticket = backend.submit_diagnostic(3, commands, leases).unwrap();
    assert_eq!(backend.pending_count(), 1);
    assert_eq!(state.records.len(), 1);
    let r = &state.records[0];
    assert_eq!(r.groups, vec![[1, 1, 1]]);
    assert_eq!(r.writes.len(), 1);
    assert_eq!(r.vertices, r.writes);
    assert_eq!(r.writes[0].2, 72);
    assert!(!state.buffers.is_empty());
    assert_eq!(state.released, 0);
    assert_eq!(state.reads, 0);
    backend.poll().unwrap();
    assert_eq!(backend.pending_count(), 1);
    assert!(!state.buffers.is_empty());
    state.done = true;
    backend.poll().unwrap();
    assert_eq!(backend.pending_count(), 0);
    assert!(state.buffers.is_empty());
    assert!(state.released > 0);
    assert_eq!(state.reads, 0);
    assert_eq!(
        backend.state(ticket).unwrap().gpu_terminal,
        Some(TerminalStatus::Succeeded)
    );
    backend.try_shutdown().unwrap();
    drop(backend);
}
#[test]
fn rejected_native_encoding_releases_allocations_without_terminal_or_pending_lease() {
    let mut state = Box::<Owner>::default();
    state.fail = true;
    let (mut device, packet) = setup(&mut state);
    device.set_resident_uploads(false);
    let prepared = packet.prepare(&device, &[1, 2], 32, 1 << 20).unwrap();
    let (commands, leases) = prepared.into_submission();
    let mut backend = RenderBackend::new(
        device,
        BackendLimits {
            max_leases_per_submission: 16,
            max_bytes_per_submission: 1 << 20,
        },
    )
    .unwrap();
    assert!(matches!(
        backend.submit_diagnostic(3, commands, leases),
        Err(BackendError::DeviceRejected { .. })
    ));
    assert_eq!(backend.pending_count(), 0);
    assert_eq!(state.records.len(), 1);
    assert!(state.buffers.is_empty());
    assert!(state.released > 0);
    assert_eq!(state.reads, 0);
    backend.try_shutdown().unwrap();
    drop(backend);
}

#[test]
fn resident_texture_uploads_survive_terminal_until_device_drop() {
    let mut state = Box::<Owner>::default();
    let (mut device, packet) = setup(&mut state);
    device.set_resident_uploads(true);
    let mut backend = RenderBackend::new(
        device,
        BackendLimits {
            max_leases_per_submission: 16,
            max_bytes_per_submission: 1 << 20,
        },
    )
    .unwrap();
    let mut cache = crate::client::scene_upload_cache::UploadCache::new(16, 1 << 20).unwrap();
    for round in 0..2u64 {
        let prepared = packet
            .prepare_with(backend.device(), &[1, 2], 32, 1 << 20, Some(&mut cache))
            .unwrap();
        let (commands, leases) = prepared.into_submission();
        backend
            .submit_diagnostic(3 + round, commands, leases)
            .unwrap();
        state.done = true;
        backend.poll().unwrap();
        assert_eq!(backend.pending_count(), 0);
        state.done = false;
        let stats = backend.device().resident_buffer_stats();
        assert!(stats.entries > 0, "{stats:?}");
        assert_eq!(stats.pinned_bytes, 0); // terminal unpinned every use
        // Only resident texture uploads outlive the terminal.
        assert_eq!(state.buffers.len(), stats.entries);
        assert_eq!((stats.hits > 0), round == 1, "{stats:?}");
    }
    assert!(cache.stats().hits > 0);
    backend.try_shutdown().unwrap();
    drop(backend);
    assert!(state.buffers.is_empty());
}

#[test]
fn reused_submit_scratch_describes_the_same_scene_every_time() {
    // Texture bindings need the native sampler registry, which this
    // CPU-only owner does not provide; the sampler tests cover their content.
    submit_three_times_and_compare(false);
}
fn submit_three_times_and_compare(textures: bool) {
    let mut state = Box::<Owner>::default();
    let (mut device, packet) = setup_with(&mut state, textures);
    device.submit_recorder = Some(record_full);
    device.set_resident_uploads(false);
    let mut backend = RenderBackend::new(
        device,
        BackendLimits {
            max_leases_per_submission: 16,
            max_bytes_per_submission: 1 << 20,
        },
    )
    .unwrap();
    for round in 0..3u64 {
        let prepared = packet
            .prepare(backend.device(), &[1, 2], 32, 1 << 20)
            .unwrap();
        let (commands, leases) = prepared.into_submission();
        backend
            .submit_diagnostic(3 + round, commands, leases)
            .unwrap();
        state.done = true;
        backend.poll().unwrap();
        assert_eq!(backend.pending_count(), 0);
        state.done = false;
    }
    assert_eq!(state.full.len(), 3);
    assert!(!state.full[0].is_empty());
    // The first submission built every array from empty; the next two reused them.
    assert_eq!(state.full[1], state.full[0]);
    assert_eq!(state.full[2], state.full[0]);
    if std::env::var_os("SCENE_SNAPSHOT_PRINT").is_some() {
        println!("snapshot textures={textures}: {:?}", state.full[0]);
    }
    backend.try_shutdown().unwrap();
    drop(backend);
}
