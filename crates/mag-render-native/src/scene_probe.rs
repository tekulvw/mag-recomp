//! Authored connected M2 scene diagnostic. It does not certify original-work
//! removal, complete game materials, physical presentation or match throughput.
use crate::{
    HostOwner, NativeContext,
    batch_device::{BatchDeviceLimits, scene_device::*},
    draw_probe::{compute_prefix, parameter_bytes},
};
use mag_render::{
    backend::*, batch::*, resources::*, scene::*, sdk_compute::PipelineDesc, types::*,
};
use std::{
    ffi::c_void,
    mem::size_of,
    time::{Duration, Instant},
};
#[repr(C)]
#[derive(Default)]
pub struct SceneProbeReport {
    pub abi_version: u32,
    pub struct_size: u32,
    pub status: i32,
    pub submitted: u32,
    pub completed: u32,
    pub pending: u32,
    pub retained_before_terminal: u64,
    pub retained_after_terminal: u64,
    pub center_rgba: u32,
    pub ui_rgba: u32,
    pub shadow_rgba: u32,
    pub outside_rgba: u32,
    pub pass_count: u32,
    pub draw_count: u32,
}
fn words(data: &[f32]) -> Vec<u8> {
    data.iter().flat_map(|n| n.to_ne_bytes()).collect()
}
fn make_scene(
    device: &SceneDevice<'_>,
    pipes: [u64; 7],
    leases: &[ResourceLease],
    diagnostic: bool,
) -> Result<PreparedScene, i32> {
    let (mut b, v, i) = compute_prefix(
        device.owner_generation(),
        [pipes[0], pipes[1]],
        &leases[..3],
    )?;
    let constants: Vec<_> = leases[3..14]
        .iter()
        .map(|l| b.upload(l.clone()).map_err(|_| -5))
        .collect::<Result<_, _>>()?;
    let mut s = SceneBuilder::new(
        b.prepare(device).map_err(|_| -1)?,
        SceneLimits {
            max_images: 8,
            max_passes: 8,
            max_draws: 16,
            max_bytes: 1024 * 1024,
        },
    )
    .map_err(|_| -1)?;
    let shadow_a = s
        .image(ImageDesc {
            width: 64,
            height: 64,
            format: ImageFormat::Depth32,
        })
        .map_err(|_| -1)?;
    let shadow_b = s
        .image(ImageDesc {
            width: 64,
            height: 64,
            format: ImageFormat::Depth32,
        })
        .map_err(|_| -1)?;
    let color = s
        .image(ImageDesc {
            width: 64,
            height: 64,
            format: ImageFormat::Rgba8,
        })
        .map_err(|_| -1)?;
    let depth = s
        .image(ImageDesc {
            width: 64,
            height: 64,
            format: ImageFormat::Depth32,
        })
        .map_err(|_| -1)?;
    let albedo = s
        .texture(
            ImageDesc {
                width: 2,
                height: 2,
                format: ImageFormat::Rgba8,
            },
            leases[14].clone(),
        )
        .map_err(|_| -1)?;
    let alpha = s
        .texture(
            ImageDesc {
                width: 2,
                height: 2,
                format: ImageFormat::Rgba8,
            },
            leases[15].clone(),
        )
        .map_err(|_| -1)?;
    let cutout = s
        .texture(
            ImageDesc {
                width: 2,
                height: 2,
                format: ImageFormat::Rgba8,
            },
            leases[16].clone(),
        )
        .map_err(|_| -1)?;
    let base = |pipeline: u64, view: usize| SceneDraw {
        pipeline,
        vertices: ReadSource::Produced(v),
        indices: ReadSource::Produced(i),
        vertex_range: ByteRange {
            offset: 0,
            length: 48,
        },
        index_range: ByteRange {
            offset: 0,
            length: 12,
        },
        index_count: 3,
        buffers: vec![GraphicsBuffer {
            stage: Stage::Vertex,
            slot: 0,
            source: ReadSource::Upload(constants[view]),
            range: ByteRange {
                offset: 0,
                length: 32,
            },
        }],
        textures: vec![],
    };
    // Empty shadow views must still initialize depth. The next pass explicitly
    // loads that clear and emits opaque occluders without a host boundary.
    let clear = s
        .render_pass(RenderPass {
            color: None,
            depth: Some(DepthAttachment {
                image: shadow_a,
                load: Load::Clear(1.),
                store: Store::Store,
            }),
            draws: vec![],
        })
        .map_err(|_| -1)?;
    let a = s
        .render_pass(RenderPass {
            color: None,
            depth: Some(DepthAttachment {
                image: shadow_a,
                load: Load::Load(clear[0]),
                store: Store::Store,
            }),
            draws: vec![base(pipes[2], 0)],
        })
        .map_err(|_| -1)?[0];
    let mut cutout_draw = base(pipes[6], 1);
    cutout_draw.textures.push(SampledImage {
        stage: Stage::Fragment,
        texture_slot: 1,
        sampler_slot: 2,
        image: cutout,
        sampler: Sampler::NearestClamp,
    });
    let b = s
        .render_pass(RenderPass {
            color: None,
            depth: Some(DepthAttachment {
                image: shadow_b,
                load: Load::Clear(1.),
                store: Store::Store,
            }),
            draws: vec![cutout_draw],
        })
        .map_err(|_| -1)?[0];
    let shadows = [a, b];
    let textured = |pipeline, view, material, texture| {
        let mut d = base(pipeline, view);
        d.buffers.push(GraphicsBuffer {
            stage: Stage::Fragment,
            slot: 0,
            source: ReadSource::Upload(constants[material]),
            range: ByteRange {
                offset: 0,
                length: 32,
            },
        });
        d.textures = vec![
            SampledImage {
                stage: Stage::Fragment,
                texture_slot: 1,
                sampler_slot: 2,
                image: texture,
                sampler: Sampler::NearestClamp,
            },
            SampledImage {
                stage: Stage::Fragment,
                texture_slot: 3,
                sampler_slot: 5,
                image: shadows[0],
                sampler: Sampler::CompareLessEqual,
            },
            SampledImage {
                stage: Stage::Fragment,
                texture_slot: 4,
                sampler_slot: 5,
                image: shadows[1],
                sampler: Sampler::CompareLessEqual,
            },
        ];
        d
    };
    let out = s
        .render_pass(RenderPass {
            color: Some(ColorAttachment {
                image: color,
                load: Load::Clear([0.; 4]),
                store: Store::Store,
            }),
            depth: Some(DepthAttachment {
                image: depth,
                load: Load::Clear(1.),
                store: Store::Store,
            }),
            draws: vec![
                textured(pipes[3], 2, 6, albedo),
                textured(pipes[3], 3, 7, albedo),
                textured(pipes[3], 4, 8, alpha),
            ],
        })
        .map_err(|_| -1)?;
    let load_pass = |previous: &[ImageValue], draw| RenderPass {
        color: Some(ColorAttachment {
            image: color,
            load: Load::Load(previous[0]),
            store: Store::Store,
        }),
        depth: Some(DepthAttachment {
            image: depth,
            load: Load::Load(previous[1]),
            store: Store::Store,
        }),
        draws: vec![draw],
    };
    let out = s
        .render_pass(load_pass(&out, textured(pipes[4], 4, 9, albedo)))
        .map_err(|_| -1)?;
    let out = s
        .render_pass(load_pass(&out, textured(pipes[5], 5, 10, albedo)))
        .map_err(|_| -1)?;
    s.output(out[0], diagnostic).map_err(|_| -1)?;
    s.prepare(device).map_err(|_| -1)
}
/// # Safety
/// Genuine retained host descriptor and synchronous readable native pipeline
/// descriptors/report. Signal must release only this fixture's actual queue gate.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mag_render_native_scene_probe(
    owner: *const HostOwner,
    first: *const PipelineDesc,
    second: *const PipelineDesc,
    graphics: *const SceneGraphicsDesc,
    diagnostic: u32,
    signal: Option<unsafe extern "C" fn(*mut c_void)>,
    user: *mut c_void,
    report: *mut SceneProbeReport,
) -> i32 {
    if owner.is_null()
        || first.is_null()
        || second.is_null()
        || graphics.is_null()
        || report.is_null()
        || diagnostic > 1
        || unsafe {
            (*report).abi_version != 1
                || (*report).struct_size != size_of::<SceneProbeReport>() as u32
        }
    {
        return -1;
    }
    let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| -> Result<(), i32> {
        let mut context = unsafe { NativeContext::from_host(&*owner) }?;
        let mut device = context.scene(BatchDeviceLimits {
            max_bytes: 1024 * 1024,
            max_pipelines: 8,
            diagnostic_readback: diagnostic == 1,
        })?;
        let descriptors = unsafe { std::slice::from_raw_parts(graphics, 5) };
        let mut pipes = [0u64; 7];
        pipes[0] = unsafe { device.compute_pipeline(&*first) }?;
        pipes[1] = unsafe { device.compute_pipeline(&*second) }?;
        for (i, d) in descriptors.iter().enumerate() {
            pipes[i + 2] = unsafe { device.graphics_pipeline(d) }?;
        }
        let mut data = vec![
            words(&[-2.8, -0.8, 0.5, 1., -1.2, -0.8, 0.5, 1., -2., 0.8, 0.5, 1.]),
            parameter_bytes(1., 2.),
            parameter_bytes(0.75, 0.),
        ];
        for (scale, x, y, z) in [
            (1., 0., 0., -0.125),
            (1., 0., 0., -0.025),
            (1., 0., 0., -0.175),
            (1., 0., 0., 0.425),
            (1., 0., 0., -0.275),
            (0.25, 0.65, 0.65, -0.375),
        ] {
            data.push(words(&[scale, scale, 1., 1., x, y, z, 0.]));
        }
        for (tint, cutoff, shadow) in [
            ([0., 1., 0., 1.], 0., 1.),
            ([1., 0., 0., 1.], 0., 0.),
            ([1., 0., 1., 1.], 0.5, 0.),
            ([0., 0., 1., 0.5], 0., 0.),
            ([1., 0., 0., 1.], 0., 0.),
        ] {
            let mut material = tint.to_vec();
            material.extend([cutoff, 0.5, shadow, 0.]);
            data.push(words(&material));
        }
        data.push([128, 255, 128, 255].repeat(4));
        data.push(vec![0; 16]);
        data.push([255, 255, 255, 255, 255, 255, 255, 0].repeat(2));
        let mut store = ResourceStore::new(ResourceLimits {
            max_resources: 17,
            max_bytes: 608,
        })
        .map_err(|_| -5)?;
        let versions: Vec<_> = data
            .into_iter()
            .map(|bytes| store.apply(ResourceDelta::Create { bytes }).map_err(|_| -5))
            .collect::<Result<_, _>>()?;
        let leases: Vec<_> = versions
            .iter()
            .map(|v| store.lease(*v).map_err(|_| -1))
            .collect::<Result<_, _>>()?;
        let first = make_scene(&device, pipes, &leases, diagnostic == 1)?;
        let second = make_scene(&device, pipes, &leases, diagnostic == 1)?;
        drop(leases);
        let mut backend = RenderBackend::new(
            device,
            BackendLimits {
                max_leases_per_submission: 17,
                max_bytes_per_submission: 608,
            },
        )
        .map_err(|_| -1)?;
        let mut tickets = Vec::new();
        for (index, scene) in [first, second].into_iter().enumerate() {
            let (commands, leases) = scene.into_submission();
            let frame = FrameIdentity {
                frame_id: FrameId(index as u64 + 1),
                world_update_id: WorldUpdateId(1),
                source_publication: 1,
                camera_publication: 1,
            };
            tickets.push(backend.submit(frame, commands, leases).map_err(|_| -6)?);
            unsafe {
                (*report).submitted += 1;
                (*report).pending = backend.pending_count() as u32;
            }
        }
        for v in versions {
            store.retire(v).map_err(|_| -1)?;
        }
        unsafe {
            (*report).retained_before_terminal = store.stats().resident_bytes as u64;
            (*report).pass_count = 6;
            (*report).draw_count = 7;
        }
        let early = backend.poll().map_err(|_| -6)?;
        if !early.events.is_empty() || !early.errors.is_empty() || backend.pending_count() != 2 {
            return Err(-6);
        }
        unsafe { signal.ok_or(-1)?(user) };
        let deadline = Instant::now() + Duration::from_secs(5);
        while backend.pending_count() > 0 {
            if Instant::now() > deadline {
                return Err(-4);
            }
            let poll = backend.poll().map_err(|_| -6)?;
            if !poll.errors.is_empty() {
                return Err(-6);
            }
            for e in poll.events {
                if let BackendEvent::Terminal { status, .. } = e {
                    if status != TerminalStatus::Succeeded {
                        return Err(-6);
                    }
                    unsafe {
                        (*report).completed += 1;
                    }
                }
            }
            std::thread::sleep(Duration::from_millis(1));
        }
        unsafe {
            (*report).pending = 0;
            (*report).retained_after_terminal = store.stats().resident_bytes as u64;
        }
        if diagnostic == 1 {
            for ticket in tickets {
                let pixels = backend.device().readback(ticket)?;
                let get = |x: usize, y: usize| -> Result<[u8; 4], i32> {
                    pixels[(y * 64 + x) * 4..(y * 64 + x) * 4 + 4]
                        .try_into()
                        .map_err(|_| -1)
                };
                let center = get(32, 32)?;
                let ui = get(52, 11)?;
                let shadow = get(24, 32)?;
                let outside = get(0, 0)?;
                unsafe {
                    (*report).center_rgba = u32::from_le_bytes(center);
                    (*report).ui_rgba = u32::from_le_bytes(ui);
                    (*report).shadow_rgba = u32::from_le_bytes(shadow);
                    (*report).outside_rgba = u32::from_le_bytes(outside);
                }
                if shadow[0] > 2
                    || shadow[1] > 2
                    || !(62..=66).contains(&shadow[2])
                    || shadow[3] != 255
                    || center[0] > 2
                    || !(62..=66).contains(&center[1])
                    || !(62..=66).contains(&center[2])
                    || center[3] != 255
                    || !(126..=130).contains(&ui[0])
                    || ui[1] > 2
                    || ui[2] > 2
                    || ui[3] != 255
                    || outside != [0; 4]
                {
                    return Err(-6);
                }
            }
        }
        if store.stats().resident_bytes != 0 {
            return Err(-6);
        }
        backend.try_shutdown().map_err(|_| -4)?;
        drop(backend);
        context.close()?;
        Ok(())
    }));
    let status = match result {
        Ok(Ok(())) => 0,
        Ok(Err(c)) => c,
        Err(_) => -6,
    };
    unsafe {
        if let Some(signal) = signal {
            signal(user)
        }
        (*report).status = status;
    }
    status
}
