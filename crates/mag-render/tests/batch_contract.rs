use mag_render::{batch::*, resources::*};
struct Catalog {
    owner: u64,
    readwrite: [BindingRequirement; 2],
}
impl Catalog {
    fn new(owner: u64) -> Self {
        Self {
            owner,
            readwrite: [
                BindingRequirement {
                    slot: 0,
                    access: Access::Read,
                    minimum_bytes: 16,
                    alignment: 16,
                },
                BindingRequirement {
                    slot: 1,
                    access: Access::ReadWrite,
                    minimum_bytes: 16,
                    alignment: 16,
                },
            ],
        }
    }
}
impl PipelineCatalog for Catalog {
    fn owner_generation(&self) -> u64 {
        self.owner
    }
    fn bindings(&self, id: u64) -> Option<&[BindingRequirement]> {
        if id == 7 { Some(&self.readwrite) } else { None }
    }
}
fn limits() -> BatchLimits {
    BatchLimits {
        max_commands: 8,
        max_gpu_buffers: 4,
        max_gpu_bytes: 128,
        max_uploads: 4,
    }
}
fn builder() -> BatchBuilder {
    BatchBuilder::new(1, limits()).unwrap()
}
fn cpu() -> ResourceLease {
    let mut store = ResourceStore::new(ResourceLimits {
        max_resources: 1,
        max_bytes: 32,
    })
    .unwrap();
    let v = store
        .apply(ResourceDelta::Create { bytes: vec![1; 32] })
        .unwrap();
    store.lease(v).unwrap()
}
fn range() -> ByteRange {
    ByteRange {
        offset: 0,
        length: 32,
    }
}
fn command(source: ReadSource, buffer: GpuBuffer) -> Vec<Binding> {
    vec![
        Binding::Read {
            slot: 0,
            source,
            range: range(),
        },
        Binding::Write {
            slot: 1,
            buffer,
            range: range(),
        },
    ]
}
#[test]
fn producer_consumer_requires_no_cpu_materialization() {
    let mut b = builder();
    let upload = b.upload(cpu()).unwrap();
    let first = b.allocate(32).unwrap();
    let second = b.allocate(32).unwrap();
    let values = b
        .dispatch(7, command(ReadSource::Upload(upload), first), [1, 1, 1])
        .unwrap();
    let output = b
        .dispatch(
            7,
            command(ReadSource::Produced(values[0]), second),
            [1, 1, 1],
        )
        .unwrap();
    assert_eq!(output[0].producer(), 1);
    let prepared = b.prepare(&Catalog::new(1)).unwrap();
    assert_eq!(prepared.uploads().len(), 1);
    assert_eq!(prepared.dependencies().len(), 1);
    assert_eq!(prepared.dependencies()[0].producer, 0);
    assert_eq!(prepared.dependencies()[0].consumer, 1);
    assert_eq!(prepared.buffers().len(), 2);
    assert!(prepared.buffers().iter().all(|b| b.zero_initialize));
}
#[test]
fn read_before_write_and_foreign_batch_values_decline() {
    let mut a = builder();
    let output = a.allocate(32).unwrap();
    assert_eq!(a.value(output), Err(BatchError::ReadBeforeWrite));
    let input = a.upload(cpu()).unwrap();
    let value = a
        .dispatch(7, command(ReadSource::Upload(input), output), [1, 1, 1])
        .unwrap()[0];
    let mut b = builder();
    let target = b.allocate(32).unwrap();
    assert_eq!(
        b.dispatch(7, command(ReadSource::Produced(value), target), [1, 1, 1]),
        Err(BatchError::WrongBatch)
    );
}
#[test]
fn second_writer_and_read_write_alias_decline_before_commit() {
    let mut b = builder();
    let input = b.upload(cpu()).unwrap();
    let output = b.allocate(32).unwrap();
    let value = b
        .dispatch(7, command(ReadSource::Upload(input), output), [1, 1, 1])
        .unwrap()[0];
    assert_eq!(
        b.dispatch(7, command(ReadSource::Upload(input), output), [1, 1, 1]),
        Err(BatchError::MultipleWriters)
    );
    assert_eq!(
        b.dispatch(7, command(ReadSource::Produced(value), output), [1, 1, 1]),
        Err(BatchError::Alias)
    );
}
#[test]
fn range_overflow_and_failed_dispatch_do_not_claim_output() {
    let mut b = builder();
    let input = b.upload(cpu()).unwrap();
    let output = b.allocate(32).unwrap();
    let mut bindings = command(ReadSource::Upload(input), output);
    if let Binding::Write { range, .. } = &mut bindings[1] {
        *range = ByteRange {
            offset: u64::MAX,
            length: 32,
        };
    }
    assert_eq!(
        b.dispatch(7, bindings, [1, 1, 1]),
        Err(BatchError::InvalidRange)
    );
    assert_eq!(b.value(output), Err(BatchError::ReadBeforeWrite));
    b.dispatch(7, command(ReadSource::Upload(input), output), [1, 1, 1])
        .unwrap();
    assert!(b.prepare(&Catalog::new(1)).is_ok());
}
#[test]
fn budgets_include_dependencies_and_fail_atomically() {
    let mut l = limits();
    l.max_commands = 2;
    l.max_gpu_bytes = 64;
    let mut b = BatchBuilder::new(1, l).unwrap();
    let input = b.upload(cpu()).unwrap();
    let a = b.allocate(32).unwrap();
    let c = b.allocate(32).unwrap();
    assert_eq!(b.allocate(1), Err(BatchError::Capacity));
    let value = b
        .dispatch(7, command(ReadSource::Upload(input), a), [1, 1, 1])
        .unwrap()[0];
    assert_eq!(
        b.dispatch(7, command(ReadSource::Produced(value), c), [1, 1, 1]),
        Err(BatchError::Capacity)
    );
    assert_eq!(b.value(c), Err(BatchError::ReadBeforeWrite));
    assert_eq!(b.prepare(&Catalog::new(1)).unwrap().dispatches().len(), 1);
}
#[test]
fn native_catalog_checks_owner_pipeline_layout_and_alignment() {
    for mode in 0..4 {
        let mut b = builder();
        let input = b.upload(cpu()).unwrap();
        let output = b.allocate(32).unwrap();
        let mut bindings = command(ReadSource::Upload(input), output);
        if mode == 2
            && let Binding::Read { slot, .. } = &mut bindings[0]
        {
            *slot = 9;
        }
        if mode == 3
            && let Binding::Read { range, .. } = &mut bindings[0]
        {
            *range = ByteRange {
                offset: 4,
                length: 16,
            };
        }
        b.dispatch(if mode == 1 { 99 } else { 7 }, bindings, [1, 1, 1])
            .unwrap();
        let expected = match mode {
            0 => BatchError::WrongOwner,
            1 => BatchError::UnknownPipeline,
            _ => BatchError::InvalidBindings,
        };
        assert!(matches!(b.prepare(&Catalog::new(if mode==0 {2}else{1})),Err(e) if e==expected));
    }
}
#[test]
fn unchanged_upload_identity_is_shared_and_empty_batch_cannot_pass() {
    let mut b = builder();
    let lease = cpu();
    let a = b.upload(lease.clone()).unwrap();
    let second = b.upload(lease).unwrap();
    assert_eq!(a, second);
    assert!(matches!(
        b.prepare(&Catalog::new(1)),
        Err(BatchError::Empty)
    ));
}

#[test]
fn draw_consumes_produced_vertex_and_index_values_and_counts_attachment_bytes() {
    struct Graphics(Catalog);
    impl PipelineCatalog for Graphics {
        fn owner_generation(&self) -> u64 {
            self.0.owner_generation()
        }
        fn bindings(&self, id: u64) -> Option<&[BindingRequirement]> {
            self.0.bindings(id)
        }
        fn graphics(&self, id: u64) -> Option<GraphicsRequirement> {
            if id == 9 {
                Some(GraphicsRequirement { vertex_stride: 16 })
            } else {
                None
            }
        }
    }
    let mut b = BatchBuilder::new(
        1,
        BatchLimits {
            max_commands: 16,
            max_gpu_buffers: 4,
            max_gpu_bytes: 1024,
            max_uploads: 4,
        },
    )
    .unwrap();
    let input = b.upload(cpu()).unwrap();
    let vertices = b.allocate(32).unwrap();
    let indices = b.allocate(32).unwrap();
    let v = b
        .dispatch(7, command(ReadSource::Upload(input), vertices), [1, 1, 1])
        .unwrap()[0];
    let i = b
        .dispatch(7, command(ReadSource::Upload(input), indices), [1, 1, 1])
        .unwrap()[0];
    b.draw_indexed(IndexedDraw {
        pipeline: 9,
        vertices: v,
        indices: i,
        vertex_range: range(),
        index_range: ByteRange {
            offset: 0,
            length: 12,
        },
        index_count: 3,
        target: DrawTarget {
            width: 8,
            height: 8,
            clear_color: [0.0; 4],
            clear_depth: 1.0,
        },
    })
    .unwrap();
    let after = b.allocate(16).unwrap();
    assert_eq!(
        b.dispatch(7, command(ReadSource::Upload(input), after), [1, 1, 1]),
        Err(BatchError::InvalidOrder)
    );
    let p = b.prepare(&Graphics(Catalog::new(1))).unwrap();
    assert_eq!(p.draw().unwrap().index_count, 3);
    assert_eq!(
        p.dependencies().iter().map(|d| d.usage).collect::<Vec<_>>(),
        vec![DependencyUsage::VertexRead, DependencyUsage::IndexRead]
    );
    let (commands, leases) = p.into_submission();
    assert_eq!(commands.draw().unwrap().pipeline, 9);
    assert_eq!(leases.len(), 1);
}

#[test]
fn draw_validation_rejects_ranges_aliases_nonfinite_clear_and_capacity_atomically() {
    let mut b = BatchBuilder::new(
        1,
        BatchLimits {
            max_commands: 8,
            max_gpu_buffers: 4,
            max_gpu_bytes: 128,
            max_uploads: 4,
        },
    )
    .unwrap();
    let upload = b.upload(cpu()).unwrap();
    let a = b.allocate(32).unwrap();
    let z = b.allocate(32).unwrap();
    let v = b
        .dispatch(7, command(ReadSource::Upload(upload), a), [1, 1, 1])
        .unwrap()[0];
    let i = b
        .dispatch(7, command(ReadSource::Upload(upload), z), [1, 1, 1])
        .unwrap()[0];
    let mut draw = IndexedDraw {
        pipeline: 9,
        vertices: v,
        indices: i,
        vertex_range: range(),
        index_range: ByteRange {
            offset: 0,
            length: 12,
        },
        index_count: 3,
        target: DrawTarget {
            width: 8,
            height: 8,
            clear_color: [0.0; 4],
            clear_depth: 1.0,
        },
    };
    assert_eq!(b.draw_indexed(draw), Err(BatchError::Capacity));
    draw.target.width = 1;
    draw.target.height = 1;
    draw.target.clear_depth = f32::NAN;
    assert_eq!(b.draw_indexed(draw), Err(BatchError::InvalidRange));
    draw.target.clear_depth = 1.0;
    draw.indices = v;
    assert_eq!(b.draw_indexed(draw), Err(BatchError::Alias));
    draw.indices = i;
    draw.index_range.offset = u64::MAX;
    assert_eq!(b.draw_indexed(draw), Err(BatchError::InvalidRange));
    draw.index_range.offset = 0;
    draw.index_count = 9;
    assert_eq!(b.draw_indexed(draw), Err(BatchError::InvalidRange));
    draw.index_count = 3;
    b.draw_indexed(draw).unwrap();
    // Failed admissions inserted no dependencies and did not seal the builder.
    assert!(matches!(
        b.prepare(&Catalog::new(1)),
        Err(BatchError::UnknownPipeline)
    ));
}

#[test]
fn moved_uploads_are_authenticated_by_namespace_and_content_identity() {
    let mut b = builder();
    let original = cpu();
    let upload = b.upload(original.clone()).unwrap();
    let buffer = b.allocate(32).unwrap();
    b.dispatch(7, command(ReadSource::Upload(upload), buffer), [1, 1, 1])
        .unwrap();
    let (commands, leases) = b.prepare(&Catalog::new(1)).unwrap().into_submission();
    assert!(commands.validate_uploads(&leases).is_ok());
    // Different stores deliberately collide in slot/generation/content version.
    let other = cpu();
    assert_eq!(other.version(), original.version());
    assert_eq!(
        commands.validate_uploads(&[other]),
        Err(BatchError::InvalidBindings)
    );
    assert_eq!(
        commands.validate_uploads(&[]),
        Err(BatchError::InvalidBindings)
    );
}
