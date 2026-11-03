use mag_render::{batch::*, resources::*, scene::*};
struct Catalog;
impl PipelineCatalog for Catalog {
    fn owner_generation(&self) -> u64 {
        1
    }
    fn bindings(&self, id: u64) -> Option<&[BindingRequirement]> {
        static B: [BindingRequirement; 3] = [
            BindingRequirement {
                slot: 0,
                access: Access::Read,
                minimum_bytes: 4,
                alignment: 4,
            },
            BindingRequirement {
                slot: 1,
                access: Access::ReadWrite,
                minimum_bytes: 48,
                alignment: 4,
            },
            BindingRequirement {
                slot: 2,
                access: Access::ReadWrite,
                minimum_bytes: 12,
                alignment: 4,
            },
        ];
        (id == 1).then_some(&B)
    }
}
impl SceneCatalog for Catalog {
    fn scene_graphics(&self, id: u64) -> Option<GraphicsLayout> {
        (id == 2).then_some(GraphicsLayout {
            vertex_stride: 16,
            color_format: Some(ImageFormat::Rgba8),
            fragment_stage: true,
            depth_attachment: true,
            depth: DepthState {
                compare: DepthCompare::Less,
                write: true,
            },
            blend: Blend::Opaque,
            buffers: vec![],
            textures: vec![],
        })
    }
}
fn source(bytes: Vec<u8>) -> ResourceLease {
    let mut s = ResourceStore::new(ResourceLimits {
        max_resources: 1,
        max_bytes: bytes.len(),
    })
    .unwrap();
    let v = s.apply(ResourceDelta::Create { bytes }).unwrap();
    s.lease(v).unwrap()
}
fn builder() -> (SceneBuilder, GpuValue, GpuValue) {
    let mut b = BatchBuilder::new(
        1,
        BatchLimits {
            max_commands: 8,
            max_gpu_buffers: 4,
            max_gpu_bytes: 1024,
            max_uploads: 4,
        },
    )
    .unwrap();
    let u = b.upload(source(vec![0; 64])).unwrap();
    let v = b.allocate(48).unwrap();
    let i = b.allocate(12).unwrap();
    b.dispatch(
        1,
        vec![
            Binding::Read {
                slot: 0,
                source: ReadSource::Upload(u),
                range: ByteRange {
                    offset: 0,
                    length: 64,
                },
            },
            Binding::Write {
                slot: 1,
                buffer: v,
                range: ByteRange {
                    offset: 0,
                    length: 48,
                },
            },
            Binding::Write {
                slot: 2,
                buffer: i,
                range: ByteRange {
                    offset: 0,
                    length: 12,
                },
            },
        ],
        [1, 1, 1],
    )
    .unwrap();
    let vs = b.value(v).unwrap();
    let ix = b.value(i).unwrap();
    (
        SceneBuilder::new(
            b.prepare(&Catalog).unwrap(),
            SceneLimits {
                max_images: 8,
                max_passes: 8,
                max_draws: 32,
                max_bytes: 65536,
            },
        )
        .unwrap(),
        vs,
        ix,
    )
}
fn draw(v: GpuValue, i: GpuValue) -> SceneDraw {
    SceneDraw {
        pipeline: 2,
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
        buffers: vec![],
        textures: vec![],
    }
}
fn pass(color: Image, depth: Image, v: GpuValue, i: GpuValue) -> RenderPass {
    RenderPass {
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
        draws: vec![draw(v, i)],
    }
}
#[test]
fn multiple_passes_share_initialized_color_and_depth() {
    let (mut s, v, i) = builder();
    let c = s
        .image(ImageDesc {
            width: 8,
            height: 8,
            format: ImageFormat::Rgba8,
        })
        .unwrap();
    let d = s
        .image(ImageDesc {
            width: 8,
            height: 8,
            format: ImageFormat::Depth32,
        })
        .unwrap();
    let first = s.render_pass(pass(c, d, v, i)).unwrap();
    let mut second = pass(c, d, v, i);
    second.color.as_mut().unwrap().load = Load::Load(first[0]);
    second.depth.as_mut().unwrap().load = Load::Load(first[1]);
    let final_values = s.render_pass(second).unwrap();
    s.output(final_values[0], false).unwrap();
    let ready = s.prepare(&Catalog).unwrap();
    assert_eq!(ready.passes().len(), 2);
    assert_eq!(ready.images().len(), 2);
    assert!(!ready.output().unwrap().diagnostic_readback);
}
#[test]
fn load_before_clear_is_rejected_without_consuming_pass_capacity() {
    let (mut s, v, i) = builder();
    let c = s
        .image(ImageDesc {
            width: 8,
            height: 8,
            format: ImageFormat::Rgba8,
        })
        .unwrap();
    let d = s
        .image(ImageDesc {
            width: 8,
            height: 8,
            format: ImageFormat::Depth32,
        })
        .unwrap();
    assert_eq!(s.current(c), Err(SceneError::Uninitialized));
    assert!(s.render_pass(pass(c, d, v, i)).is_ok());
}
#[test]
fn sampling_active_attachment_is_rejected_and_clear_remains_usable() {
    let (mut s, v, i) = builder();
    let c = s
        .image(ImageDesc {
            width: 8,
            height: 8,
            format: ImageFormat::Rgba8,
        })
        .unwrap();
    let d = s
        .image(ImageDesc {
            width: 8,
            height: 8,
            format: ImageFormat::Depth32,
        })
        .unwrap();
    let first = s.render_pass(pass(c, d, v, i)).unwrap();
    let mut bad = pass(c, d, v, i);
    bad.draws[0].textures.push(SampledImage {
        stage: Stage::Fragment,
        texture_slot: 0,
        sampler_slot: 0,
        image: first[0],
        sampler: Sampler::NearestClamp,
    });
    assert_eq!(s.render_pass(bad), Err(SceneError::Alias));
    assert_eq!(s.current(c).unwrap(), first[0]);
    assert!(s.render_pass(pass(c, d, v, i)).is_ok());
}
#[test]
fn stale_loaded_or_sampled_epoch_does_not_read_a_rewritten_target() {
    let (mut s, v, i) = builder();
    let c = s
        .image(ImageDesc {
            width: 8,
            height: 8,
            format: ImageFormat::Rgba8,
        })
        .unwrap();
    let d = s
        .image(ImageDesc {
            width: 8,
            height: 8,
            format: ImageFormat::Depth32,
        })
        .unwrap();
    let first = s.render_pass(pass(c, d, v, i)).unwrap();
    let newer = s.render_pass(pass(c, d, v, i)).unwrap();
    let mut bad = pass(c, d, v, i);
    bad.color.as_mut().unwrap().load = Load::Load(first[0]);
    assert_eq!(s.render_pass(bad), Err(SceneError::StaleImage));
    assert_eq!(s.current(c).unwrap(), newer[0]);
}
#[test]
fn discarded_target_cannot_be_loaded_or_output() {
    let (mut s, v, i) = builder();
    let c = s
        .image(ImageDesc {
            width: 8,
            height: 8,
            format: ImageFormat::Rgba8,
        })
        .unwrap();
    let d = s
        .image(ImageDesc {
            width: 8,
            height: 8,
            format: ImageFormat::Depth32,
        })
        .unwrap();
    let before = s.render_pass(pass(c, d, v, i)).unwrap();
    let mut p = pass(c, d, v, i);
    p.color.as_mut().unwrap().store = Store::Discard;
    s.render_pass(p).unwrap();
    assert_eq!(s.current(c), Err(SceneError::Uninitialized));
    assert_eq!(s.output(before[0], true), Err(SceneError::Uninitialized));
}
#[test]
fn images_and_geometry_from_another_scene_are_rejected() {
    let (mut a, av, ai) = builder();
    let (mut b, bv, bi) = builder();
    let c = a
        .image(ImageDesc {
            width: 8,
            height: 8,
            format: ImageFormat::Rgba8,
        })
        .unwrap();
    let d = a
        .image(ImageDesc {
            width: 8,
            height: 8,
            format: ImageFormat::Depth32,
        })
        .unwrap();
    assert_eq!(
        b.render_pass(pass(c, d, bv, bi)),
        Err(SceneError::WrongScene)
    );
    assert_eq!(
        a.render_pass(pass(c, d, bv, bi)),
        Err(SceneError::Batch(BatchError::WrongBatch))
    );
    assert!(a.render_pass(pass(c, d, av, ai)).is_ok());
}
#[test]
fn texture_upload_shape_and_identity_are_checked_and_leased_through_submission() {
    let (mut s, v, i) = builder();
    let desc = ImageDesc {
        width: 2,
        height: 2,
        format: ImageFormat::Rgba8,
    };
    assert_eq!(
        s.texture(desc, source(vec![0; 15])),
        Err(SceneError::InvalidBindings)
    );
    let tex = source(vec![7; 16]);
    let key = tex.cache_key();
    let img = s.texture(desc, tex).unwrap();
    assert_eq!(img.image().index(), 0);
    let c = s
        .image(ImageDesc {
            width: 8,
            height: 8,
            format: ImageFormat::Rgba8,
        })
        .unwrap();
    let d = s
        .image(ImageDesc {
            width: 8,
            height: 8,
            format: ImageFormat::Depth32,
        })
        .unwrap();
    s.render_pass(pass(c, d, v, i)).unwrap();
    let (cmd, mut leases) = s.prepare(&Catalog).unwrap().into_submission();
    assert!(cmd.validate_uploads(&leases).is_ok());
    assert_eq!(leases.last().unwrap().cache_key(), key);
    *leases.last_mut().unwrap() = source(vec![7; 16]);
    assert_eq!(
        cmd.validate_uploads(&leases),
        Err(SceneError::InvalidBindings)
    );
}
#[test]
fn attachment_extent_and_format_are_explicit() {
    let (mut s, v, i) = builder();
    let c = s
        .image(ImageDesc {
            width: 8,
            height: 8,
            format: ImageFormat::Rgba8,
        })
        .unwrap();
    let d = s
        .image(ImageDesc {
            width: 4,
            height: 8,
            format: ImageFormat::Depth32,
        })
        .unwrap();
    assert_eq!(
        s.render_pass(pass(c, d, v, i)),
        Err(SceneError::InvalidPass)
    );
    assert_eq!(s.current(c), Err(SceneError::Uninitialized));
    let mut wrong = pass(c, d, v, i);
    wrong.depth.as_mut().unwrap().image = c;
    assert_eq!(s.render_pass(wrong), Err(SceneError::InvalidPass));
}
#[test]
fn unknown_graphics_pipeline_is_not_prepared() {
    let (mut s, v, i) = builder();
    let c = s
        .image(ImageDesc {
            width: 8,
            height: 8,
            format: ImageFormat::Rgba8,
        })
        .unwrap();
    let d = s
        .image(ImageDesc {
            width: 8,
            height: 8,
            format: ImageFormat::Depth32,
        })
        .unwrap();
    let mut p = pass(c, d, v, i);
    p.draws[0].pipeline = 99;
    s.render_pass(p).unwrap();
    assert!(matches!(
        s.prepare(&Catalog),
        Err(SceneError::UnknownPipeline)
    ));
}
#[test]
fn empty_scene_does_not_succeed() {
    let (s, _, _) = builder();
    assert!(matches!(s.prepare(&Catalog), Err(SceneError::Empty)));
}

#[test]
fn clear_only_scene_has_real_attachment_effects_without_buffers_or_compute() {
    let b = BatchBuilder::new(
        1,
        BatchLimits {
            max_commands: 1,
            max_gpu_buffers: 1,
            max_gpu_bytes: 1,
            max_uploads: 1,
        },
    )
    .unwrap();
    let base = b.prepare_scene_prefix(&Catalog).unwrap();
    let mut s = SceneBuilder::new(
        base,
        SceneLimits {
            max_images: 2,
            max_passes: 2,
            max_draws: 1,
            max_bytes: 1024,
        },
    )
    .unwrap();
    let d = s
        .image(ImageDesc {
            width: 8,
            height: 8,
            format: ImageFormat::Depth32,
        })
        .unwrap();
    let out = s
        .render_pass(RenderPass {
            color: None,
            depth: Some(DepthAttachment {
                image: d,
                load: Load::Clear(1.),
                store: Store::Store,
            }),
            draws: vec![],
        })
        .unwrap();
    s.render_pass(RenderPass {
        color: None,
        depth: Some(DepthAttachment {
            image: d,
            load: Load::Load(out[0]),
            store: Store::Store,
        }),
        draws: vec![],
    })
    .unwrap();
    let scene = s.prepare(&Catalog).unwrap();
    assert!(scene.compute().dispatches().is_empty());
    assert!(scene.compute().uploads().is_empty());
    assert_eq!(scene.passes().len(), 2);
}
#[test]
fn missing_stage_binding_and_wrong_registry_owner_are_rejected() {
    struct Other {
        owner: u64,
        requires_uniform: bool,
    }
    impl PipelineCatalog for Other {
        fn owner_generation(&self) -> u64 {
            self.owner
        }
        fn bindings(&self, id: u64) -> Option<&[BindingRequirement]> {
            Catalog.bindings(id)
        }
    }
    impl SceneCatalog for Other {
        fn scene_graphics(&self, id: u64) -> Option<GraphicsLayout> {
            let mut g = Catalog.scene_graphics(id)?;
            if self.requires_uniform {
                g.buffers.push(GraphicsBufferLayout {
                    stage: Stage::Fragment,
                    binding: BindingRequirement {
                        slot: 0,
                        access: Access::Read,
                        minimum_bytes: 32,
                        alignment: 16,
                    },
                });
            }
            Some(g)
        }
    }
    for (owner, requires_uniform, expected) in [
        (2, false, SceneError::Batch(BatchError::WrongOwner)),
        (1, true, SceneError::InvalidBindings),
    ] {
        let (mut s, v, i) = builder();
        let c = s
            .image(ImageDesc {
                width: 8,
                height: 8,
                format: ImageFormat::Rgba8,
            })
            .unwrap();
        let d = s
            .image(ImageDesc {
                width: 8,
                height: 8,
                format: ImageFormat::Depth32,
            })
            .unwrap();
        s.render_pass(pass(c, d, v, i)).unwrap();
        assert!(matches!(s.prepare(&Other{owner,requires_uniform}),Err(e) if e==expected));
    }
}
#[test]
fn disabled_depth_cannot_write_and_failed_image_admission_is_atomic() {
    struct BadDepth;
    impl PipelineCatalog for BadDepth {
        fn owner_generation(&self) -> u64 {
            1
        }
        fn bindings(&self, id: u64) -> Option<&[BindingRequirement]> {
            Catalog.bindings(id)
        }
    }
    impl SceneCatalog for BadDepth {
        fn scene_graphics(&self, id: u64) -> Option<GraphicsLayout> {
            let mut g = Catalog.scene_graphics(id)?;
            g.depth.compare = DepthCompare::Disabled;
            Some(g)
        }
    }
    let (mut s, v, i) = builder();
    assert_eq!(
        s.image(ImageDesc {
            width: 8192,
            height: 8192,
            format: ImageFormat::Rgba8
        }),
        Err(SceneError::Capacity)
    );
    let c = s
        .image(ImageDesc {
            width: 8,
            height: 8,
            format: ImageFormat::Rgba8,
        })
        .unwrap();
    assert_eq!(c.index(), 0);
    let d = s
        .image(ImageDesc {
            width: 8,
            height: 8,
            format: ImageFormat::Depth32,
        })
        .unwrap();
    s.render_pass(pass(c, d, v, i)).unwrap();
    assert!(matches!(
        s.prepare(&BadDepth),
        Err(SceneError::InvalidBindings)
    ));
}

#[test]
fn explicit_interleaved_stride_accepts_float3_uv_color_and_rejects_torn_vertex() {
    struct Interleaved(u32);
    impl PipelineCatalog for Interleaved {
        fn owner_generation(&self) -> u64 {
            1
        }
        fn bindings(&self, _: u64) -> Option<&[BindingRequirement]> {
            None
        }
    }
    impl SceneCatalog for Interleaved {
        fn scene_graphics(&self, id: u64) -> Option<GraphicsLayout> {
            (id == 2).then_some(GraphicsLayout {
                vertex_stride: self.0,
                color_format: Some(ImageFormat::Rgba8),
                fragment_stage: true,
                depth_attachment: false,
                depth: DepthState {
                    compare: DepthCompare::Disabled,
                    write: false,
                },
                blend: Blend::Opaque,
                buffers: vec![],
                textures: vec![],
            })
        }
    }
    fn make(stride: u32) -> Result<PreparedScene, SceneError> {
        let catalog = Interleaved(stride);
        let mut batch = BatchBuilder::new(
            1,
            BatchLimits {
                max_commands: 1,
                max_gpu_buffers: 1,
                max_gpu_bytes: 1024,
                max_uploads: 2,
            },
        )
        .unwrap();
        // Four bytes of allocator padding, then3 prepared24-byte vertices.
        let vertices = batch.upload(source(vec![0; 76])).unwrap();
        let indices = batch.upload(source(vec![0; 12])).unwrap();
        let base = batch.prepare_scene_prefix(&catalog).unwrap();
        let mut scene = SceneBuilder::new(
            base,
            SceneLimits {
                max_images: 1,
                max_passes: 1,
                max_draws: 1,
                max_bytes: 1024,
            },
        )
        .unwrap();
        let color = scene
            .image(ImageDesc {
                width: 8,
                height: 8,
                format: ImageFormat::Rgba8,
            })
            .unwrap();
        scene
            .render_pass(RenderPass {
                color: Some(ColorAttachment {
                    image: color,
                    load: Load::Clear([0.; 4]),
                    store: Store::Store,
                }),
                depth: None,
                draws: vec![SceneDraw {
                    pipeline: 2,
                    vertices: ReadSource::Upload(vertices),
                    indices: ReadSource::Upload(indices),
                    vertex_range: ByteRange {
                        offset: 4,
                        length: 72,
                    },
                    index_range: ByteRange {
                        offset: 0,
                        length: 12,
                    },
                    index_count: 3,
                    buffers: vec![],
                    textures: vec![],
                }],
            })
            .unwrap();
        scene.prepare(&catalog)
    }
    assert!(make(24).is_ok());
    assert!(matches!(make(32), Err(SceneError::InvalidBindings)));
    assert!(matches!(make(0), Err(SceneError::InvalidBindings)));
}

#[test]
fn bgra_target_preserves_declared_format_and_requires_matching_pipeline() {
    struct ColorCatalog(ImageFormat);
    impl PipelineCatalog for ColorCatalog {
        fn owner_generation(&self) -> u64 {
            1
        }
        fn bindings(&self, _: u64) -> Option<&[BindingRequirement]> {
            None
        }
    }
    impl SceneCatalog for ColorCatalog {
        fn scene_graphics(&self, _: u64) -> Option<GraphicsLayout> {
            Some(GraphicsLayout {
                vertex_stride: 16,
                color_format: Some(self.0),
                fragment_stage: true,
                depth_attachment: false,
                depth: DepthState {
                    compare: DepthCompare::Disabled,
                    write: false,
                },
                blend: Blend::Opaque,
                buffers: vec![],
                textures: vec![],
            })
        }
    }
    fn make(pipeline: ImageFormat) -> Result<PreparedScene, SceneError> {
        let catalog = ColorCatalog(pipeline);
        let mut batch = BatchBuilder::new(
            1,
            BatchLimits {
                max_commands: 1,
                max_gpu_buffers: 1,
                max_gpu_bytes: 1024,
                max_uploads: 2,
            },
        )
        .unwrap();
        let vertices = batch.upload(source(vec![0; 48])).unwrap();
        let indices = batch.upload(source(vec![0; 12])).unwrap();
        let base = batch.prepare_scene_prefix(&catalog).unwrap();
        let mut scene = SceneBuilder::new(
            base,
            SceneLimits {
                max_images: 1,
                max_passes: 1,
                max_draws: 1,
                max_bytes: 1024,
            },
        )
        .unwrap();
        let color = scene
            .image(ImageDesc {
                width: 8,
                height: 8,
                format: ImageFormat::Bgra8,
            })
            .unwrap();
        let values = scene
            .render_pass(RenderPass {
                color: Some(ColorAttachment {
                    image: color,
                    load: Load::Clear([1., 0., 0., 1.]),
                    store: Store::Store,
                }),
                depth: None,
                draws: vec![SceneDraw {
                    pipeline: 1,
                    vertices: ReadSource::Upload(vertices),
                    indices: ReadSource::Upload(indices),
                    vertex_range: ByteRange {
                        offset: 0,
                        length: 48,
                    },
                    index_range: ByteRange {
                        offset: 0,
                        length: 12,
                    },
                    index_count: 3,
                    buffers: vec![],
                    textures: vec![],
                }],
            })
            .unwrap();
        scene.output(values[0], false).unwrap();
        scene.prepare(&catalog)
    }
    let scene = make(ImageFormat::Bgra8).unwrap();
    assert_eq!(scene.images()[0].desc.format, ImageFormat::Bgra8);
    assert!(!scene.output().unwrap().diagnostic_readback);
    assert!(matches!(
        make(ImageFormat::Rgba8),
        Err(SceneError::InvalidBindings)
    ));
}
