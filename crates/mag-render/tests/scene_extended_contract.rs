use mag_render::{batch::*, resources::*, scene::*};
struct Catalog;
impl PipelineCatalog for Catalog {
    fn owner_generation(&self) -> u64 {
        1
    }
    fn bindings(&self, _: u64) -> Option<&[BindingRequirement]> {
        None
    }
}
impl SceneCatalog for Catalog {
    fn scene_graphics(&self, _: u64) -> Option<GraphicsLayout> {
        None
    }
}
fn builder(bytes: u64) -> SceneBuilder {
    let prefix = BatchBuilder::new(
        1,
        BatchLimits {
            max_commands: 1,
            max_gpu_buffers: 1,
            max_gpu_bytes: bytes,
            max_uploads: 1,
        },
    )
    .unwrap()
    .prepare_scene_prefix(&Catalog)
    .unwrap();
    SceneBuilder::new(
        prefix,
        SceneLimits {
            max_images: 8,
            max_passes: 4,
            max_draws: 4,
            max_bytes: bytes,
        },
    )
    .unwrap()
}
fn lease(bytes: Vec<u8>) -> ResourceLease {
    let mut s = ResourceStore::new(ResourceLimits {
        max_resources: 1,
        max_bytes: bytes.len(),
    })
    .unwrap();
    let v = s.apply(ResourceDelta::Create { bytes }).unwrap();
    s.lease(v).unwrap()
}
fn cube() -> ImageLayout {
    let mut l = ImageLayout {
        dimension: Dimension::Cube,
        layers: 6,
        mip_count: 3,
        ..ImageLayout::default()
    };
    for layer in 0..6 {
        for mip in 0..3 {
            l.subresources.push(Subresource {
                mip,
                layer,
                width: 4 >> mip,
                height: 4 >> mip,
                depth: 1,
                row_bytes: 8,
                offset: u64::from(layer * 3 + mip) * 16,
                length: 8,
            })
        }
    }
    l
}
fn viewport() -> Viewport {
    Viewport {
        x: -0.25,
        y: 0.5,
        width: 4.,
        height: 4.,
        min_depth: 0.,
        max_depth: 1.,
        scissor_x: 0,
        scissor_y: 0,
        scissor_width: 4,
        scissor_height: 4,
        known_fields: 7,
        reserved: 0,
    }
}
#[test]
fn complete_cube_payload_and_view_survive_into_submission() {
    let mut s = builder(2048);
    let value = s
        .texture_with_layout(
            ImageDesc {
                width: 4,
                height: 4,
                format: ImageFormat::Bc1,
            },
            cube(),
            lease(vec![7; 288]),
        )
        .unwrap();
    let view = s
        .view(
            value,
            ViewDesc {
                dimension: Dimension::Cube,
                base_mip: 1,
                mip_count: 2,
                base_layer: 0,
                layer_count: 6,
                remap: [0, 1, 2, 5],
            },
        )
        .unwrap();
    assert_eq!(view.view().unwrap().base_mip, 1);
    let color = s
        .image(ImageDesc {
            width: 4,
            height: 4,
            format: ImageFormat::Rgba8,
        })
        .unwrap();
    let out = s
        .render_pass_with_viewport(
            RenderPass {
                color: Some(ColorAttachment {
                    image: color,
                    load: Load::Clear([0.; 4]),
                    store: Store::Store,
                }),
                depth: None,
                draws: vec![],
            },
            viewport(),
        )
        .unwrap();
    s.output(out[0], false).unwrap();
    let (c, l) = s.prepare(&Catalog).unwrap().into_submission();
    assert_eq!(l[0].bytes().len(), 288);
    assert_eq!(
        c.images()[0].layout.as_ref().unwrap().subresources.len(),
        18
    );
    assert_eq!(c.viewports()[0].unwrap().x, -0.25);
}
#[test]
fn missing_mips_and_overlapping_ranges_refuse_without_allocating_image() {
    let mut s = builder(2048);
    let desc = ImageDesc {
        width: 4,
        height: 4,
        format: ImageFormat::Bc1,
    };
    let mut bad = cube();
    bad.subresources.pop();
    assert!(
        s.texture_with_layout(desc, bad, lease(vec![0; 288]))
            .is_err()
    );
    let value = s
        .texture_with_layout(desc, cube(), lease(vec![0; 288]))
        .unwrap();
    assert_eq!(value.image().index(), 0);
    assert!(
        s.view(
            value,
            ViewDesc {
                dimension: Dimension::Cube,
                base_mip: 0,
                mip_count: 3,
                base_layer: 0,
                layer_count: 5,
                remap: [0, 1, 2, 3]
            }
        )
        .is_err()
    );
}
#[test]
fn fresh_cube_clear_cannot_initialize_unwritten_faces() {
    let mut s = builder(2048);
    let l = ImageLayout {
        dimension: Dimension::Cube,
        layers: 6,
        ..ImageLayout::default()
    };
    assert!(
        s.image_with_layout(
            ImageDesc {
                width: 4,
                height: 4,
                format: ImageFormat::Rgba8
            },
            l
        )
        .is_err()
    );
}
#[test]
fn hdr_clear_preserves_finite_outside_unorm_range() {
    let mut s = builder(2048);
    let color = s
        .image(ImageDesc {
            width: 4,
            height: 4,
            format: ImageFormat::Rgba16Float,
        })
        .unwrap();
    let result = s.render_pass(RenderPass {
        color: Some(ColorAttachment {
            image: color,
            load: Load::Clear([2., -0.5, 0., 1.]),
            store: Store::Store,
        }),
        depth: None,
        draws: vec![],
    });
    assert!(result.is_ok());
}
