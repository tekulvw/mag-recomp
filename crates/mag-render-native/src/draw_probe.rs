//! Bounded authored integration diagnostic, not a scene/removal certificate.
use crate::{HostOwner, NativeContext, batch_device::*};
use mag_render::{backend::*, batch::*, resources::*, sdk_compute::PipelineDesc, types::*};
use std::{
    ffi::c_void,
    mem::size_of,
    time::{Duration, Instant},
};
#[repr(C)]
#[derive(Default)]
pub struct DrawProbeReport {
    pub abi_version: u32,
    pub struct_size: u32,
    pub status: i32,
    pub submitted: u32,
    pub completed: u32,
    pub pending: u32,
    pub retained_before_terminal: u64,
    pub retained_after_terminal: u64,
    pub center_rgba: u32,
    pub outside_rgba: u32,
}
fn resource(store: &mut ResourceStore, bytes: Vec<u8>) -> Result<ResourceVersion, i32> {
    store.apply(ResourceDelta::Create { bytes }).map_err(|_| -5)
}
fn parameter_bytes(scale: f32, translate: f32) -> Vec<u8> {
    let mut words = [0u32; 20];
    for i in 0..4 {
        words[i * 5] = if i == 3 {
            1.0f32.to_bits()
        } else {
            scale.to_bits()
        };
    }
    words[12] = translate.to_bits();
    words[16] = 3;
    words.iter().flat_map(|n| n.to_ne_bytes()).collect()
}
fn make_batch(
    device: &BatchDevice<'_>,
    pipelines: [u64; 3],
    leases: &[ResourceLease],
) -> Result<PreparedBatch, i32> {
    let mut b = BatchBuilder::new(
        device.owner_generation(),
        BatchLimits {
            max_commands: 16,
            max_gpu_buffers: 4,
            max_gpu_bytes: 1024 * 1024,
            max_uploads: 4,
        },
    )
    .map_err(|_| -1)?;
    let source = b.upload(leases[0].clone()).map_err(|_| -5)?;
    let translation = b.upload(leases[1].clone()).map_err(|_| -5)?;
    let scale = b.upload(leases[2].clone()).map_err(|_| -5)?;
    let intermediate = b.allocate(48).map_err(|_| -5)?;
    let vertices = b.allocate(48).map_err(|_| -5)?;
    let indices = b.allocate(12).map_err(|_| -5)?;
    let full = ByteRange {
        offset: 0,
        length: 48,
    };
    let params = ByteRange {
        offset: 0,
        length: 80,
    };
    let first = b
        .dispatch(
            pipelines[0],
            vec![
                Binding::Read {
                    slot: 0,
                    source: ReadSource::Upload(source),
                    range: full,
                },
                Binding::Write {
                    slot: 1,
                    buffer: intermediate,
                    range: full,
                },
                Binding::Read {
                    slot: 2,
                    source: ReadSource::Upload(translation),
                    range: params,
                },
            ],
            [1, 1, 1],
        )
        .map_err(|_| -1)?[0];
    b.dispatch(
        pipelines[1],
        vec![
            Binding::Read {
                slot: 0,
                source: ReadSource::Produced(first),
                range: full,
            },
            Binding::Write {
                slot: 1,
                buffer: vertices,
                range: full,
            },
            Binding::Write {
                slot: 2,
                buffer: indices,
                range: ByteRange {
                    offset: 0,
                    length: 12,
                },
            },
            Binding::Read {
                slot: 3,
                source: ReadSource::Upload(scale),
                range: params,
            },
        ],
        [1, 1, 1],
    )
    .map_err(|_| -1)?;
    b.draw_indexed(IndexedDraw {
        pipeline: pipelines[2],
        vertices: b.value(vertices).map_err(|_| -1)?,
        indices: b.value(indices).map_err(|_| -1)?,
        vertex_range: full,
        index_range: ByteRange {
            offset: 0,
            length: 12,
        },
        index_count: 3,
        target: DrawTarget {
            width: 64,
            height: 64,
            clear_color: [0.0; 4],
            clear_depth: 1.0,
        },
    })
    .map_err(|_| -1)?;
    b.prepare(device).map_err(|_| -1)
}
/// # Safety
/// Genuine owner and valid descriptor/report ranges for this synchronous call.
/// Signal must unblock only the fixture's owned real queue gate, without unwind.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mag_render_native_draw_probe(
    owner: *const HostOwner,
    first: *const PipelineDesc,
    second: *const PipelineDesc,
    graphics: *const GraphicsDesc,
    diagnostic_readback: u32,
    signal: Option<unsafe extern "C" fn(*mut c_void)>,
    user: *mut c_void,
    report: *mut DrawProbeReport,
) -> i32 {
    if diagnostic_readback > 1
        || owner.is_null()
        || first.is_null()
        || second.is_null()
        || graphics.is_null()
        || report.is_null()
    {
        return -1;
    }
    if unsafe {
        (*report).abi_version != 1 || (*report).struct_size != size_of::<DrawProbeReport>() as u32
    } {
        return -1;
    }
    let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| -> Result<(), i32> {
        let mut context = unsafe { NativeContext::from_host(&*owner) }?;
        let mut device = context.batch(BatchDeviceLimits {
            max_bytes: 1024 * 1024,
            max_pipelines: 4,
            diagnostic_readback: diagnostic_readback == 1,
        })?;
        let pipelines = unsafe {
            [
                device.compute_pipeline(&*first)?,
                device.compute_pipeline(&*second)?,
                device.graphics_pipeline(&*graphics)?,
            ]
        };
        let mut store = ResourceStore::new(ResourceLimits {
            max_resources: 3,
            max_bytes: 208,
        })
        .map_err(|_| -5)?;
        let positions: [f32; 12] = [
            -2.8, -0.8, 0.5, 1.0, -1.2, -0.8, 0.5, 1.0, -2.0, 0.8, 0.5, 1.0,
        ];
        let versions = [
            resource(
                &mut store,
                positions.iter().flat_map(|n| n.to_ne_bytes()).collect(),
            )?,
            resource(&mut store, parameter_bytes(1.0, 2.0))?,
            resource(&mut store, parameter_bytes(0.75, 0.0))?,
        ];
        let leases: Vec<_> = versions
            .iter()
            .map(|v| store.lease(*v).map_err(|_| -1))
            .collect::<Result<_, _>>()?;
        let first = make_batch(&device, pipelines, &leases)?;
        let second = make_batch(&device, pipelines, &leases)?;
        drop(leases);
        let mut backend = RenderBackend::new(
            device,
            BackendLimits {
                max_leases_per_submission: 3,
                max_bytes_per_submission: 208,
            },
        )
        .map_err(|_| -1)?;
        let mut tickets = Vec::new();
        unsafe {
            (*report).pending = 2;
        }
        for (index, batch) in [first, second].into_iter().enumerate() {
            let (commands, leases) = batch.into_submission();
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
        for version in versions {
            store.retire(version).map_err(|_| -1)?;
        }
        unsafe {
            (*report).retained_before_terminal = store.stats().resident_bytes as u64;
        }
        // Genuine queued gate ensures both batches remain unresolved here.
        let early = backend.poll().map_err(|_| -6)?;
        if !early.events.is_empty() || !early.errors.is_empty() || backend.pending_count() != 2 {
            return Err(-6);
        }
        if backend.device().readback(tickets[0]).err()
            != Some(if diagnostic_readback == 1 { -4 } else { -2 })
        {
            return Err(-6);
        }
        if let Some(signal) = signal {
            unsafe { signal(user) }
        } else {
            return Err(-1);
        }
        let deadline = Instant::now() + Duration::from_secs(5);
        while backend.pending_count() > 0 {
            if Instant::now() > deadline {
                return Err(-4);
            }
            let poll = backend.poll().map_err(|_| -6)?;
            if !poll.errors.is_empty() {
                return Err(-6);
            }
            for event in poll.events {
                if let BackendEvent::Terminal { status, .. } = event {
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
            (*report).pending = backend.pending_count() as u32;
        }
        for ticket in tickets.into_iter().filter(|_| diagnostic_readback == 1) {
            let pixels = backend.device().readback(ticket)?;
            let center = u32::from_le_bytes(
                pixels[(32 * 64 + 32) * 4..(32 * 64 + 32) * 4 + 4]
                    .try_into()
                    .map_err(|_| -1)?,
            );
            let outside = u32::from_le_bytes(pixels[0..4].try_into().map_err(|_| -1)?);
            unsafe {
                (*report).center_rgba = center;
                (*report).outside_rgba = outside;
            }
            if center != u32::from_le_bytes([51, 153, 255, 255]) || outside != 0 {
                return Err(-6);
            }
        }
        unsafe {
            (*report).pending = backend.pending_count() as u32;
            (*report).retained_after_terminal = store.stats().resident_bytes as u64;
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
        Ok(Err(code)) => code,
        Err(_) => -6,
    };
    unsafe {
        (*report).status = status;
    }
    status
}
