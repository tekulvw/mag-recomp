//! Round trips of packets built by the owned-arena builder through the real native packet
//! validator (`mag_render_native::validate_scene_packet_v3`), which copies and validates exactly
//! as scene preparation does. Every input is synthetic.
#![allow(unsafe_code)]

use mag_render_abi::packet::{ImageView, Viewport};
use mag_render_abi::{Source, TextureLayout, VertexAttribute};
use mag_render_behavior::effects::{AttachmentImage, LoadOp, SurfaceBinding};
use mag_render_behavior::program_compilation::{
    EffectiveValues, ProgramCompiler, TechniqueSource, Variant,
};
use mag_render_behavior::raster::{BlendConstant, RasterDrawInputs, Topology, translate};
use mag_render_behavior::scene_plan::{
    DrawSources, PlanStep, SetupReferences, TraversalItem, flat_controls, traverse,
};
use mag_render_behavior::setup::{Comparison, CullFace, FrontFace, PipelineState};
use mag_render_behavior::surfaces::{explicit_viewport, map_viewport};
use mag_render_behavior::texture::{
    DecodedUnit, TextureUnitRecord, decode, native_sampler, view_parameters,
};
use mag_render_behavior::vertex_inputs::{
    AttributeList, AuxiliarySet, Prepared, VertexBinding, VertexDraw, pack_vertex_inputs,
};
use mag_render_behavior_glue::abi::*;
use mag_render_behavior_glue::packet::{CallerWork, PacketAssembler};
use mag_render_behavior_glue::programs::RsxTranslator;

fn validate(builder: &SceneBuilder) -> Result<(), i32> {
    let view = builder.view();
    // SAFETY: every range in the descriptor points into `builder` or `view`, both alive and
    // unchanged for the synchronous call.
    unsafe { mag_render_native::validate_scene_packet_v3(view.desc()) }
}

const SOURCE: Source = Source {
    engine_update: 1,
    cohort: 2,
    publication: 3,
    pass_occurrence: 4,
};

/// A fully known pipeline state: no cull, depth off, blend off, all colour, stencil off, no
/// bounds, no offset.
fn state() -> PipelineState {
    PipelineState {
        srgb_output: Some(false),
        alpha_test: Some(false),
        alpha_comparison: Some(Comparison::Always),
        alpha_reference: Some(0),
        cull_enable: Some(false),
        cull_face: Some(CullFace::Back),
        front_face: Some(FrontFace::CounterClockwise),
        blend_enable: Some(false),
        blend_factors: None,
        blend_equations: None,
        colour_masks: Some([15; 4]),
        depth_test: Some(false),
        depth_write: Some(false),
        depth_comparison: None,
        depth_bounds_enable: Some(false),
        depth_bounds: None,
        polygon_offset_fill: Some(false),
        offset_factor: None,
        offset_units: None,
        stencil_enable: Some(false),
        two_sided_stencil: None,
        front_stencil: None,
        back_stencil: None,
    }
}

fn raster() -> mag_render_abi::packet::RasterState {
    translate(
        &state(),
        &RasterDrawInputs {
            topology: Some(Topology::Triangles),
            samples: Some(1),
            colour_attachment: 0,
            blend_constant: BlendConstant::Unobserved,
            bias_clamp: None,
            obligations: 0,
        },
    )
    .unwrap()
}

fn viewport(width: u32, height: u32) -> Viewport {
    let source = explicit_viewport(width, height, 0.0, 1.0);
    packet_viewport(&map_viewport(&source, (width, height), (1, 1)).unwrap())
}

fn colour_attachment(image: u32) -> AttachmentImage {
    AttachmentImage {
        image,
        load: LoadOp::Load,
    }
}

fn main_binding(colour_image: u32) -> SurfaceBinding {
    SurfaceBinding {
        address: 0x1000,
        physical: (64, 64),
        colour_format: 1,
        depth_format: 0,
        colour: Some(colour_attachment(colour_image)),
        depth: None,
        stencil: None,
        depth_initialised: false,
        stencil_initialised: false,
    }
}

fn ne_indices(indices: &[u32]) -> Vec<u8> {
    indices.iter().flat_map(|i| i.to_ne_bytes()).collect()
}

/// A one-instruction vertex program that moves input 0 to the position output.
fn mov() -> [u32; 4] {
    let src = 0x1b02;
    [
        (1 << 30) | (63 << 15),
        (1 << 22) | (src >> 9),
        (src & 511) << 23,
        (15 << 13) | 1,
    ]
}

fn encoded(words: &[u32]) -> Vec<u8> {
    words
        .iter()
        .flat_map(|w| w.rotate_left(16).to_be_bytes())
        .collect()
}

/// A fragment program with one inline constant row.
fn constant_fragment() -> Vec<u8> {
    encoded(&[
        (1 << 24) | (15 << 9) | 1,
        2 | 0x1c800 | (7 << 18),
        0x1c800,
        0x1c800,
        1f32.to_bits(),
        0,
        0,
        1f32.to_bits(),
    ])
}

/// A fragment program that samples unit 0.
fn texture_fragment() -> Vec<u8> {
    encoded(&[
        (0x17 << 24) | (15 << 9) | (4 << 13) | 1,
        1 | 0x1c800 | (7 << 18),
        0x1c800,
        0x1c800,
    ])
}

fn directory() -> Vec<u8> {
    let mut d = vec![0u8; 28];
    d[24..28].copy_from_slice(&28u32.to_be_bytes());
    d.extend(1u16.to_be_bytes());
    d.extend(1u16.to_be_bytes());
    d
}

fn technique(fragment: Vec<u8>) -> TechniqueSource {
    let length = fragment.len();
    TechniqueSource {
        vertex_instructions: vec![mov()],
        load_slot: 0,
        declared_input_mask: 1,
        declared_output_mask: 1,
        transform_word: 0,
        fragment_bytes: fragment,
        semantic_bytes: length,
        transfer_bytes: length,
        shader_control: 0x40,
        texcoord_controls: [0; 10],
        directory: if length == 32 {
            directory()
        } else {
            vec![0; 24]
        },
    }
}

fn values() -> EffectiveValues {
    EffectiveValues {
        vertex_bank: vec![Some([0; 4]); 512],
        viewport_scale: [1f32.to_bits(), 1f32.to_bits(), 0.5f32.to_bits(), 0],
        viewport_offset: [0, 0, 0.5f32.to_bits(), 0],
        current: [None; 16],
        fragment_writes: vec![(0, [1f32.to_bits(); 4])],
        alpha_window: [0; 4],
    }
}

/// Adds the compiled programs and a pipeline for them; returns the pipeline index and the two
/// uniform blocks.
fn add_compiled(
    builder: &mut SceneBuilder,
    fragment: Vec<u8>,
    with_texture: bool,
) -> (u32, Vec<u8>, Vec<u8>) {
    let mut compiler = ProgramCompiler::new(RsxTranslator, false);
    let mut controls = flat_controls((1, 1));
    controls.enabled_arrays = 1;
    let mut v = values();
    if with_texture {
        v.fragment_writes.clear();
    }
    let pair = compiler
        .compile(&technique(fragment), Variant::Source, &controls, &v)
        .unwrap();
    assert_eq!(pair.required_arrays(1), Ok(1));
    let vertex = builder.add_program(OwnedProgram {
        stage: 0,
        flags: 0,
        language: LANGUAGE_HLSL,
        source: pair.vertex.bytes.clone(),
        entry: b"main".to_vec(),
        buffers: vec![BindingLayoutRecord::uniform(
            0,
            pair.vertex_uniform.len() as u64,
        )],
        textures: Vec::new(),
        samplers: Vec::new(),
        vertex_locations: pair.vertex.vertex_input_locations.clone(),
        threads: [0; 3],
    });
    let fragment = builder.add_program(OwnedProgram {
        stage: 1,
        flags: 0,
        language: LANGUAGE_HLSL,
        source: pair.fragment.bytes.clone(),
        entry: b"main".to_vec(),
        buffers: vec![BindingLayoutRecord::uniform(
            1,
            pair.fragment_uniform.len() as u64,
        )],
        textures: if with_texture {
            vec![TextureSlotRecord {
                slot: 0,
                dimension: DIMENSION_2D,
                sample_kind: 0,
            }]
        } else {
            Vec::new()
        },
        samplers: if with_texture {
            vec![(0, 0)]
        } else {
            Vec::new()
        },
        vertex_locations: Vec::new(),
        threads: [0; 3],
    });
    let pipeline = builder.add_pipeline(OwnedPipeline {
        record: PipelineRecord {
            vertex,
            fragment,
            stride: 16,
            colour_format: 1,
            depth_format: 0,
            binding_model: 1,
            state: raster(),
        },
        attributes: vec![VertexAttribute {
            location: 0,
            format: 4,
            offset: 0,
            reserved: 0,
        }],
        textures: if with_texture {
            vec![TextureLayout {
                stage: 1,
                texture_slot: 0,
                sampler_slot: 0,
                format: 1,
                comparison: 0,
                reserved: 0,
            }]
        } else {
            Vec::new()
        },
    });
    (pipeline, pair.vertex_uniform, pair.fragment_uniform)
}

fn item(group: u32, first: u32) -> TraversalItem {
    let mut mbi = [0u8; 32];
    mbi[0] = (group << 4) as u8;
    mbi[15] = 1;
    let mut transform = [0u8; 112];
    transform[..4].copy_from_slice(&first.to_be_bytes());
    TraversalItem {
        mbi,
        transform: Some(transform),
        material: Some(Some(DrawSources {
            group,
            transform,
            pass_control: 3,
        })),
    }
}

const POSITIONS: [[u32; 4]; 3] = [
    [0xbf80_0000, 0xbf80_0000, 0, 0x3f80_0000],
    [0x4040_0000, 0xbf80_0000, 0, 0x3f80_0000],
    [0xbf80_0000, 0x4040_0000, 0, 0x3f80_0000],
];

/// A clean-room plan, end to end: traversal, pass assembly, compiled programs, packed vertex
/// inputs and uniforms, then the native validator.
#[test]
fn clean_room_plan_round_trips_through_the_validator() {
    let mut builder = SceneBuilder::new(SOURCE);
    let target = builder.add_image(OwnedImage::attachment(1, 64, 64));
    builder.output_image = target;
    let (pipeline, vertex_uniform, fragment_uniform) =
        add_compiled(&mut builder, constant_fragment(), false);

    let setup = SetupReferences {
        lists_enabled: true,
        initial: [0; 3],
        group_thirteen: 0,
        group_fifteen: [0; 2],
    };
    let items = [
        item(2, 0x3f80_0000),
        item(2, 0x3f80_0000),
        item(3, 0x4000_0000),
    ];
    let (steps, stats) = traverse(&items, &setup, 3).unwrap();
    assert_eq!((stats.emitted_items, stats.repeated), (2, 1));

    let draw = VertexDraw {
        output_bindings: Some(vec![VertexBinding {
            declaration: 0,
            semantic: 1,
            slot: 0,
            format: 2,
            components: 4,
        }]),
        auxiliary: Some(AuxiliarySet::default()),
        primary: AttributeList::Absent,
        secondary: AttributeList::Absent,
        positions: &POSITIONS,
        raw_indices: &[0, 1, 2],
        direct: None,
        group_zero: Prepared::Absent,
        non_instanced: Prepared::Absent,
    };
    let mut assembler = PacketAssembler::<OwnedDraw>::new();
    for step in steps {
        let Some(work) = assembler
            .apply(step, &main_binding(target), Some(1))
            .unwrap()
        else {
            continue;
        };
        match work {
            CallerWork::Prepare { item, .. } => {
                let inputs = pack_vertex_inputs(&draw, 1, 1 << 20).unwrap();
                let vertices = builder.add_buffer(OwnedBuffer::Bytes(inputs.vertices.clone()));
                let indices = builder.add_buffer(OwnedBuffer::Bytes(ne_indices(&inputs.indices)));
                let vb = builder.add_buffer(OwnedBuffer::Bytes(vertex_uniform.clone()));
                let fb = builder.add_buffer(OwnedBuffer::Bytes(fragment_uniform.clone()));
                assembler
                    .add_draw(OwnedDraw {
                        source_item: item as u64,
                        source_partition: 0,
                        obligations: 0,
                        pipeline,
                        index_count: inputs.indices.len() as u32,
                        vertices: (vertices, 0, inputs.vertices.len() as u64),
                        indices: (indices, 0, 4 * inputs.indices.len() as u64),
                        buffers: vec![
                            (0, 0, vb, 0, vertex_uniform.len() as u64),
                            (1, 1, fb, 0, fragment_uniform.len() as u64),
                        ],
                        textures: Vec::new(),
                    })
                    .unwrap();
            }
            CallerWork::Step(PlanStep::Transition { .. }) | CallerWork::Step(_) => {}
        }
    }
    let passes = assembler.passes.len();
    builder
        .add_assembled(assembler, &vec![viewport(64, 64); passes])
        .unwrap();
    assert_eq!(builder.items.len(), 3);
    assert_eq!(validate(&builder), Ok(()));

    // The validator really checks the content: an index past the vertices is refused.
    let mut broken = builder.clone();
    let index_buffer = broken.passes[0].draws[0].indices.0 as usize;
    broken.buffers[index_buffer] = OwnedBuffer::Bytes(ne_indices(&[0, 1, 9]));
    assert!(validate(&broken).is_err());
    // And so is an item that disagrees with the intended count.
    let mut missing_item = builder.clone();
    missing_item.items.pop();
    assert!(validate(&missing_item).is_err());
}

/// A textured draw: a 2×2 RGBA8 image, a view and sampler from decoded texture-unit words.
#[test]
fn textured_draw_round_trips_through_the_validator() {
    let mut builder = SceneBuilder::new(SOURCE);
    let target = builder.add_image(OwnedImage::attachment(1, 64, 64));
    builder.output_image = target;
    let (pipeline, vertex_uniform, fragment_uniform) =
        add_compiled(&mut builder, texture_fragment(), true);
    let texels = vec![255u8; 16];
    let image = builder.add_image(OwnedImage {
        subresources: vec![OwnedSubresource {
            mip: 0,
            layer: 0,
            width: 2,
            height: 2,
            depth: 1,
            row_bytes: 8,
            bytes: texels,
        }],
        ..OwnedImage::attachment(1, 2, 2)
    });
    let unit = TextureUnitRecord {
        compact: Some([
            0,
            0x0001_8521,
            0x0003_0303,
            0x8000_0000,
            0xaae4,
            0x0202_0000,
            0x0002_0002,
            8,
        ]),
        ..Default::default()
    };
    let DecodedUnit::Enabled(controls) = decode(&unit).unwrap() else {
        panic!("enabled unit")
    };
    let view = builder.add_view(view_parameters(&controls).to_packet(image), DIMENSION_2D);
    let sampler = builder.add_sampler(native_sampler(&controls).unwrap());
    // De-duplication.
    assert_eq!(
        builder.add_view(view_parameters(&controls).to_packet(image), DIMENSION_2D),
        view
    );
    assert_eq!(
        builder.add_sampler(native_sampler(&controls).unwrap()),
        sampler
    );
    let vertices: Vec<u8> = POSITIONS
        .iter()
        .flatten()
        .flat_map(|w| w.to_le_bytes())
        .collect();
    let vb = builder.add_buffer(OwnedBuffer::Bytes(vertices));
    let ib = builder.add_buffer(OwnedBuffer::Bytes(ne_indices(&[0, 1, 2])));
    let vu = builder.add_buffer(OwnedBuffer::Bytes(vertex_uniform.clone()));
    let fu = builder.add_buffer(OwnedBuffer::Bytes(fragment_uniform.clone()));
    let mut assembler = PacketAssembler::<OwnedDraw>::new();
    assembler
        .apply(PlanStep::OpenPass, &main_binding(target), Some(1))
        .unwrap();
    assembler
        .apply(
            PlanStep::Prepare { item: 0, group: 2 },
            &main_binding(target),
            Some(1),
        )
        .unwrap();
    assembler
        .add_draw(OwnedDraw {
            source_item: 0,
            source_partition: 0,
            obligations: 0,
            pipeline,
            index_count: 3,
            vertices: (vb, 0, 48),
            indices: (ib, 0, 12),
            buffers: vec![
                (0, 0, vu, 0, vertex_uniform.len() as u64),
                (1, 1, fu, 0, fragment_uniform.len() as u64),
            ],
            textures: vec![(1, 0, 0, view, sampler)],
        })
        .unwrap();
    builder
        .add_assembled(assembler, &[viewport(64, 64)])
        .unwrap();
    assert_eq!(validate(&builder), Ok(()));
    // A view whose remap code is outside the packet encoding is refused.
    let mut bad = builder.clone();
    bad.views[0].0 = ImageView {
        remap: [9, 3, 4, 5],
        ..bad.views[0].0
    };
    assert!(validate(&bad).is_err());
}

/// A producer pass with a depth/stencil attachment and the stencil table.
#[test]
fn producer_stencil_pass_round_trips_through_the_validator() {
    let mut builder = SceneBuilder::new(SOURCE);
    let colour = builder.add_image(OwnedImage::attachment(1, 64, 64));
    let depth = builder.add_image(OwnedImage::attachment(15, 64, 64));
    builder.output_image = colour;
    let (pipeline, vertex_uniform, fragment_uniform) =
        add_compiled(&mut builder, constant_fragment(), false);
    let vertices: Vec<u8> = POSITIONS
        .iter()
        .flatten()
        .flat_map(|w| w.to_le_bytes())
        .collect();
    let vb = builder.add_buffer(OwnedBuffer::Bytes(vertices));
    let ib = builder.add_buffer(OwnedBuffer::Bytes(ne_indices(&[0, 1, 2])));
    let vu = builder.add_buffer(OwnedBuffer::Bytes(vertex_uniform.clone()));
    let fu = builder.add_buffer(OwnedBuffer::Bytes(fragment_uniform.clone()));
    let pass = mag_render_behavior::effects::ProducerPass {
        target: 0x2000,
        colour: Some(colour_attachment(colour)),
        depth: Some(AttachmentImage {
            image: depth,
            load: LoadOp::DontCare,
        }),
        stencil: Some(AttachmentImage {
            image: depth,
            load: LoadOp::DontCare,
        }),
        colour_format: 1,
        depth_format: 15,
        viewport: mag_render_behavior::effects::producer_viewport(
            &{
                let mut words = [0u32; 21];
                words[1] = 64 << 16;
                words[2] = 64 << 16;
                words[3] = 8 | (2 << 5);
                mag_render_behavior::surfaces::SurfaceRecord { words }
            },
            (64, 64),
            0.0,
            1.0,
        )
        .unwrap(),
        geometry: mag_render_behavior::effects::fullscreen_triangle(
            (-1f32).to_bits(),
            1f32.to_bits(),
            3f32.to_bits(),
            2f32.to_bits(),
        )
        .unwrap(),
        state: state(),
        textures: Vec::new(),
    };
    let mut assembler = PacketAssembler::<OwnedDraw>::new();
    let identity = assembler.add_producer_pass(
        &pass,
        OwnedDraw {
            source_item: 0,
            source_partition: 0,
            obligations: 0,
            pipeline,
            index_count: 3,
            vertices: (vb, 0, 48),
            indices: (ib, 0, 12),
            buffers: vec![
                (0, 0, vu, 0, vertex_uniform.len() as u64),
                (1, 1, fu, 0, fragment_uniform.len() as u64),
            ],
            textures: Vec::new(),
        },
    );
    // The draw belongs to the producer item.
    assembler.passes[0].draws[0].source_item = identity;
    builder
        .add_assembled(assembler, &[packet_viewport(&pass.viewport.native)])
        .unwrap();
    // The pipeline must declare the depth format of the pass it draws into.
    builder.pipelines[pipeline as usize].record.depth_format = 15;
    assert_eq!(validate(&builder), Ok(()));
    assert_eq!(builder.passes[0].stencil.image, depth);
    // A stencil record that does not name the combined depth image is refused.
    let mut bad = builder.clone();
    bad.passes[0].stencil = NO_STENCIL;
    assert!(validate(&bad).is_err());
}
