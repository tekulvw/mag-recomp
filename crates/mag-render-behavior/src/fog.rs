//! Main fog producer, group 13 (`main-fog.md`).

use crate::fog_values::{FogCamera, FogValueInputs, PreparedFogRows, fog_values};
use crate::producer::{
    ProducerDraw, ProducerError, ProducerResources, ProgramUsage, TextureBinding, attributes_after,
    check_draw_state, check_fragment_program, check_target, fullscreen_state, fullscreen_triangle,
    set_stencil_test,
};
use crate::producers::ProducerInputs;
use crate::setup::{Comparison, PipelineState};
use crate::surfaces::{
    SelectedSurface, SelectionControls, SourceViewport, ViewportConstants, associate_colour,
    effective_role, explicit_viewport, select, source_viewport, surface_target_viewport,
    table_position,
};
use crate::texture::TextureUnitRecord;

/// The surface Main is drawing into when group 13 is reached.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct CurrentSurface {
    /// The current surface record, its index and effective role.
    pub surface: SelectedSurface,
    /// The role originally requested for it.
    pub requested_role: u32,
}

/// Inputs of the fog producer besides the acquired set.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct FogProducerInputs {
    /// Read through `TOC−0x223C`; only needed once the fog values are computed.
    pub camera: Option<FogCamera>,
    /// The game-prepared rows; only needed once the fog values are computed.
    pub prepared: Option<PreparedFogRows>,
    pub current: CurrentSurface,
    /// The lighting producer ran earlier in this Main invocation.
    pub lighting_ran: bool,
    /// Program decode results for fragment programs 3 (untinted) and 4 (tinted).
    pub usage: [Option<ProgramUsage>; 2],
}

/// Why the producer stopped without drawing.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum FogStop {
    RoleForcing,
    ProgramsAbsent,
    VariantAbsent,
}

#[derive(Clone, Debug, PartialEq)]
pub struct FogOutput {
    /// `None` when the draws ran.
    pub stopped: Option<FogStop>,
    pub units: [TextureUnitRecord; 16],
    pub state: PipelineState,
    pub draws: Vec<ProducerDraw>,
    pub tinted: Option<bool>,
    /// Fog vertex rows 0 and 1, written after the fog vertex program's static constants.
    pub vertex_rows: Option<[[u32; 4]; 2]>,
    pub vertex_program: Option<u32>,
    /// The current surface and its source viewport after the restore.
    pub current: Option<(SelectedSurface, SourceViewport)>,
    pub attribute0: Option<[u32; 4]>,
    pub attribute8: Option<[u32; 4]>,
}

const FOG_CONTROL_KEEP_BOUNDS: u32 = 0x400;

fn stencil_footer(state: &PipelineState) -> PipelineState {
    PipelineState {
        two_sided_stencil: Some(false),
        stencil_enable: Some(false),
        ..*state
    }
}

/// Runs the fog producer. A decline (`Err`) has no visible effect; a stop returns the state
/// changes made before it.
pub fn run_fog<R: ProducerResources + ?Sized>(
    inputs: &ProducerInputs,
    fog: &FogProducerInputs,
    units: &[TextureUnitRecord; 16],
    state: &PipelineState,
    resources: &R,
) -> Result<FogOutput, ProducerError> {
    let renderer = &inputs.renderer;
    let mut units = *units;
    units[11].disable_if_known();
    units[15].disable_if_known();
    let stop = |reason, units, state| FogOutput {
        stopped: Some(reason),
        units,
        state,
        draws: Vec::new(),
        tinted: None,
        vertex_rows: None,
        vertex_program: None,
        current: None,
        attribute0: None,
        attribute8: None,
    };
    if renderer.role_forcing() {
        return Ok(stop(FogStop::RoleForcing, units, *state));
    }
    let programs = &inputs.programs.fog;
    let Some(vertex) = &programs.vertex else {
        return Ok(stop(FogStop::ProgramsAbsent, units, stencil_footer(state)));
    };
    if programs.untinted.is_none() && programs.tinted.is_none() {
        return Ok(stop(FogStop::ProgramsAbsent, units, stencil_footer(state)));
    }

    let camera = fog
        .camera
        .ok_or(ProducerError::UnknownState("fog camera"))?;
    let prepared = fog
        .prepared
        .ok_or(ProducerError::UnknownState("prepared fog rows"))?;
    let values = fog_values(&FogValueInputs::of(inputs, &camera, prepared))
        .map_err(ProducerError::FogValues)?;

    let (selected, usage) = if values.tinted {
        (&programs.tinted, fog.usage[1])
    } else {
        (&programs.untinted, fog.usage[0])
    };
    let Some(fragment) = selected else {
        return Ok(stop(FogStop::VariantAbsent, units, stencil_footer(state)));
    };
    let usage = usage.ok_or(ProducerError::ProgramUnparsed)?;
    check_fragment_program(fragment.control_flags, &usage)?;

    // Unit 0: the scene colour of the current surface.
    let current = fog.current;
    let surface = current.surface;
    let role = effective_role(current.requested_role, SelectionControls::of(renderer));
    let descriptor_tagged = renderer.surface_descriptors[table_position(role, surface.index)];
    let mut descriptor0 = descriptor_tagged.value;
    if associate_colour(&descriptor0, &[(surface.address, surface.record)])?.is_none() {
        return Err(ProducerError::MismatchedImage { unit: 0 });
    }
    let current_target = resources
        .target(surface.address)
        .ok_or(ProducerError::MissingTarget {
            address: surface.address,
        })?;
    let scene_image = current_target
        .colour_image
        .ok_or(ProducerError::MissingImage { unit: 0 })?;
    let srgb = renderer.srgb != 0;
    if srgb {
        descriptor0[2] = (descriptor0[2] & !0x00f0_0000) | 0x0070_0000;
    }
    units[0].apply_compact(&descriptor0);

    // Unit 1: the lighting source.
    let lighting = renderer.lighting_source();
    let mut descriptor1 = lighting.value;
    if fog.lighting_ran {
        descriptor1[5] = (descriptor1[5] & !0x0f0f_0000) | 0x0101_0000;
    }
    let lighting_image = resources
        .image(lighting.address, &lighting.value)
        .ok_or(ProducerError::MissingImage { unit: 1 })?;
    units[1].apply_compact(&descriptor1);

    let mut textures = Vec::new();
    if usage.samples(0) {
        textures.push(TextureBinding {
            unit: 0,
            source_address: descriptor_tagged.address,
            descriptor: descriptor0,
            border: units[0].border,
            image: scene_image,
            depth_source: None,
        });
    }
    if usage.samples(1) {
        textures.push(TextureBinding {
            unit: 1,
            source_address: lighting.address,
            descriptor: descriptor1,
            border: units[1].border,
            image: lighting_image,
            depth_source: None,
        });
    }

    // Depth bounds.
    let bounds_enable = if renderer.fog_controls & FOG_CONTROL_KEEP_BOUNDS == 0 {
        true
    } else {
        state
            .depth_bounds_enable
            .ok_or(ProducerError::UnknownState("depth-bounds enable"))?
    };
    let mut base = PipelineState {
        depth_bounds_enable: Some(bounds_enable),
        depth_bounds: Some(values.bounds),
        ..fullscreen_state(state, 15)
    };
    if srgb {
        base.srgb_output = Some(true);
    }

    let s = &inputs.scalars;
    let triangle = fullscreen_triangle(s.t_lo.value, s.t_hi.value, s.t_far.value, s.t_uvfar.value)?;
    let two_draws = renderer.bank_zero_enabled();
    let with_stencil = |state: &PipelineState, comparison| PipelineState {
        stencil_enable: Some(true),
        two_sided_stencil: Some(true),
        front_stencil: set_stencil_test(state.front_stencil, comparison, 128, 0xff),
        back_stencil: set_stencil_test(state.back_stencil, comparison, 128, 0xff),
        ..*state
    };

    let mut targets = vec![surface];
    if two_draws {
        targets.push(select(renderer, surface.index, 0)?);
    }
    let mut draws = Vec::new();
    let mut last = base;
    for (number, target) in targets.into_iter().enumerate() {
        let mut draw_state = last;
        if two_draws {
            let comparison = if number == 0 {
                Comparison::GreaterOrEqual
            } else {
                Comparison::Less
            };
            draw_state = with_stencil(&draw_state, comparison);
        }
        if number == 1 {
            draw_state.depth_bounds_enable = Some(false);
        }
        check_draw_state(&draw_state)?;
        let info = check_target(resources, &target)?;
        if usage.samples(0) && info.colour_image.map(|image| image.id) == Some(scene_image.id) {
            return Err(ProducerError::Feedback {
                address: target.address,
            });
        }
        let source = if number == 0 {
            explicit_viewport(
                renderer.explicit_width,
                renderer.explicit_height,
                f32::from_bits(s.z0.value),
                f32::from_bits(s.z1.value),
            )
        } else {
            source_viewport(&target, &ViewportConstants::of(inputs))?
        };
        draws.push(ProducerDraw {
            target,
            viewport: surface_target_viewport(&target, &source, info.physical)?,
            state: draw_state,
            vertices: triangle,
            fragment_rows: values.fragment_rows.clone(),
            textures: textures.clone(),
        });
        last = draw_state;
    }

    let after = stencil_footer(&PipelineState {
        depth_bounds_enable: Some(false),
        ..last
    });
    let restored = select(renderer, surface.index, 1)?;
    let restored_viewport = source_viewport(&restored, &ViewportConstants::of(inputs))?;
    let (attribute0, attribute8) = attributes_after(&triangle);
    Ok(FogOutput {
        stopped: None,
        units,
        state: after,
        draws,
        tinted: Some(values.tinted),
        vertex_rows: Some(values.vertex_rows),
        vertex_program: Some(vertex.address),
        current: Some((restored, restored_viewport)),
        attribute0: Some(attribute0),
        attribute8: Some(attribute8),
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::fog_values::tests::{F1, F2, inputs as value_inputs};
    use crate::lighting::tests::{producer_inputs, resources_for};
    use crate::producer::ImageInfo;
    use crate::producer::tests::{Resources, face, known_state};
    use crate::producers::{FragmentProgramFile, VertexProgramObject};

    fn fragment(index: u32) -> FragmentProgramFile {
        FragmentProgramFile {
            registry_index: index,
            address: 0x60_0000 + 0x100 * index,
            microcode_length: 16,
            control_flags: 0,
            storage_word: 0,
            microcode_offset: 24,
            bytes: vec![0; 40],
        }
    }

    /// Common setup: bank zero enabled, role forcing off, both fragment programs present, the
    /// current surface is (i = 0, requested role 1), and lighting ran.
    fn setup(bank_zero: bool, tint: f32) -> (ProducerInputs, FogProducerInputs, Resources) {
        let mut inputs = producer_inputs(bank_zero);
        let values = value_inputs(if tint > 0.0 { F2 } else { F1 });
        let values = FogValueInputs {
            prepared: PreparedFogRows {
                fragment: {
                    let mut rows = values.prepared.fragment;
                    rows[0][3] = tint.to_bits();
                    rows
                },
                ..values.prepared
            },
            ..values
        };
        inputs.renderer.fog_block.value = values.block;
        inputs.programs.fog.vertex = Some(VertexProgramObject {
            registry_index: 0x0d,
            address: 0x61_0000,
            instruction_count: 0,
            instruction_offset: 0,
            constant_count: 0,
            bytes: vec![0; 24],
        });
        inputs.programs.fog.untinted = Some(fragment(3));
        inputs.programs.fog.tinted = Some(fragment(4));
        let current = select(&inputs.renderer, 0, 1).unwrap();
        let position = table_position(current.role, 0);
        // The current surface's descriptor associates with its record.
        let record = current.record;
        inputs.renderer.surface_descriptors[position].value = [
            record.colour_offset(),
            0x0001_8521,
            0x0003_0303,
            0x8000_0000,
            0xaae4,
            0x0202_0000,
            (1280 << 16) | 720,
            5120,
        ];
        inputs.renderer.lighting_source_bank_zero.value[5] = 0x0202_2000;
        inputs.renderer.lighting_source_no_bank_zero.value[5] = 0x0202_2000;
        let mut resources = resources_for(&inputs);
        let lighting = inputs.renderer.lighting_source();
        resources.images.insert(
            (lighting.address, lighting.value),
            ImageInfo {
                id: 900,
                width: 1280,
                height: 720,
                mip_count: 1,
                samples: 1,
                layers: 1,
                two_dimensional: true,
                depth: false,
            },
        );
        let fog = FogProducerInputs {
            camera: Some(FogCamera {
                address: 0x70_0000,
                slope: values.camera_slope,
                aspect: values.camera_aspect,
            }),
            prepared: Some(values.prepared),
            current: CurrentSurface {
                surface: current,
                requested_role: 1,
            },
            lighting_ran: true,
            usage: [Some(UNIT1); 2],
        };
        (inputs, fog, resources)
    }

    const UNIT1: ProgramUsage = ProgramUsage {
        sampled_units: 2,
        discards: false,
    };
    const BOTH: ProgramUsage = ProgramUsage {
        sampled_units: 3,
        discards: false,
    };

    fn stencil_state() -> PipelineState {
        PipelineState {
            front_stencil: Some(face(0)),
            back_stencil: Some(face(0)),
            ..known_state()
        }
    }

    #[test]
    fn vector_1_role_forcing() {
        let (mut inputs, fog, resources) = setup(true, 0.0);
        inputs.renderer.selection_controls[0] = 1;
        let mut units: [TextureUnitRecord; 16] = Default::default();
        units[11].compact = Some([0, 0, 0, 0x8000_0000, 0, 0, 0, 0]);
        let state = PipelineState {
            stencil_enable: Some(true),
            ..stencil_state()
        };
        let out = run_fog(&inputs, &fog, &units, &state, &resources).unwrap();
        assert_eq!(out.stopped, Some(FogStop::RoleForcing));
        assert_eq!(out.units[11].compact.unwrap()[3], 0);
        assert_eq!(out.units[15].compact, None);
        assert!(out.draws.is_empty());
        assert_eq!(out.state, state);
    }

    #[test]
    fn vector_2_no_fragment_programs() {
        let (mut inputs, fog, resources) = setup(true, 0.0);
        inputs.programs.fog.untinted = None;
        inputs.programs.fog.tinted = None;
        let mut units: [TextureUnitRecord; 16] = Default::default();
        units[15].compact = Some([0, 0, 0, 0x8000_0000, 0, 0, 0, 0]);
        let state = PipelineState {
            stencil_enable: Some(true),
            two_sided_stencil: Some(true),
            ..stencil_state()
        };
        let out = run_fog(&inputs, &fog, &units, &state, &resources).unwrap();
        assert_eq!(out.stopped, Some(FogStop::ProgramsAbsent));
        assert_eq!(out.units[15].compact.unwrap()[3], 0);
        assert_eq!(
            (out.state.stencil_enable, out.state.two_sided_stencil),
            (Some(false), Some(false))
        );
        assert!(out.draws.is_empty());
        // An absent vertex program stops the same way.
        let (mut inputs, fog, resources) = setup(true, 0.0);
        inputs.programs.fog.vertex = None;
        assert_eq!(
            run_fog(&inputs, &fog, &Default::default(), &state, &resources)
                .unwrap()
                .stopped,
            Some(FogStop::ProgramsAbsent)
        );
        // The selected variant absent.
        let (mut inputs, fog, resources) = setup(true, 0.0);
        inputs.programs.fog.untinted = None;
        let out = run_fog(&inputs, &fog, &Default::default(), &state, &resources).unwrap();
        assert_eq!(out.stopped, Some(FogStop::VariantAbsent));
        assert_eq!(out.state.stencil_enable, Some(false));
    }

    #[test]
    fn vector_3_variant() {
        let (inputs, fog, resources) = setup(true, 0.25);
        let out = run_fog(
            &inputs,
            &fog,
            &Default::default(),
            &stencil_state(),
            &resources,
        )
        .unwrap();
        assert_eq!(out.tinted, Some(true));
        assert_eq!(out.draws[0].fragment_rows.len(), 8);
        let (inputs, fog, resources) = setup(true, 0.0);
        let out = run_fog(
            &inputs,
            &fog,
            &Default::default(),
            &stencil_state(),
            &resources,
        )
        .unwrap();
        assert_eq!(out.tinted, Some(false));
        assert_eq!(out.draws[0].fragment_rows.len(), 7);
    }

    #[test]
    fn vector_4_srgb() {
        let (mut inputs, fog, resources) = setup(true, 0.0);
        inputs.renderer.srgb = 1;
        let out = run_fog(
            &inputs,
            &fog,
            &Default::default(),
            &stencil_state(),
            &resources,
        )
        .unwrap();
        assert_eq!(out.units[0].compact.unwrap()[2], 0x0073_0303);
        assert!(out.draws.iter().all(|d| d.state.srgb_output == Some(true)));
        let (inputs, fog, resources) = setup(true, 0.0);
        let out = run_fog(
            &inputs,
            &fog,
            &Default::default(),
            &stencil_state(),
            &resources,
        )
        .unwrap();
        assert_eq!(out.units[0].compact.unwrap()[2], 0x0003_0303);
    }

    #[test]
    fn vector_5_filter_override() {
        let (inputs, fog, resources) = setup(true, 0.0);
        let out = run_fog(
            &inputs,
            &fog,
            &Default::default(),
            &stencil_state(),
            &resources,
        )
        .unwrap();
        assert_eq!(out.units[1].compact.unwrap()[5], 0x0101_2000);
        assert_eq!(out.draws[0].textures[0].descriptor[5], 0x0101_2000);
        let fog = FogProducerInputs {
            lighting_ran: false,
            ..fog
        };
        let out = run_fog(
            &inputs,
            &fog,
            &Default::default(),
            &stencil_state(),
            &resources,
        )
        .unwrap();
        assert_eq!(out.units[1].compact.unwrap()[5], 0x0202_2000);
        // The image is found by the unmodified descriptor; without it the producer declines.
        let mut missing = resources;
        missing.images.clear();
        let fog = FogProducerInputs {
            lighting_ran: true,
            ..fog
        };
        assert_eq!(
            run_fog(
                &inputs,
                &fog,
                &Default::default(),
                &stencil_state(),
                &missing
            ),
            Err(ProducerError::MissingImage { unit: 1 })
        );
    }

    #[test]
    fn vector_6_two_draws() {
        let (inputs, fog, resources) = setup(true, 0.0);
        let out = run_fog(
            &inputs,
            &fog,
            &Default::default(),
            &stencil_state(),
            &resources,
        )
        .unwrap();
        assert_eq!(out.draws.len(), 2);
        let (d0, d1) = (&out.draws[0], &out.draws[1]);
        for (draw, comparison) in [(d0, Comparison::GreaterOrEqual), (d1, Comparison::Less)] {
            assert_eq!(
                (draw.state.stencil_enable, draw.state.two_sided_stencil),
                (Some(true), Some(true))
            );
            for face in [
                draw.state.front_stencil.unwrap(),
                draw.state.back_stencil.unwrap(),
            ] {
                assert_eq!(
                    (face.comparison, face.reference, face.read_mask),
                    (comparison, 128, 0xff)
                );
            }
            assert_eq!(draw.state.colour_masks, Some([15; 4]));
            assert_eq!(
                (
                    draw.state.depth_test,
                    draw.state.depth_write,
                    draw.state.cull_enable
                ),
                (Some(false), Some(false), Some(false))
            );
        }
        assert_eq!(d0.state.depth_bounds_enable, Some(true));
        assert_eq!(d1.state.depth_bounds_enable, Some(false));
        assert_eq!(d0.target, fog.current.surface);
        assert_eq!(d1.target, select(&inputs.renderer, 0, 0).unwrap());
        assert_eq!(d0.state.depth_bounds.unwrap()[0], 0x3f00_0000);
    }

    #[test]
    fn vector_7_inherited_bounds() {
        let (mut inputs, fog, resources) = setup(true, 0.0);
        inputs.renderer.fog_controls = 0x400;
        let state = PipelineState {
            depth_bounds_enable: Some(false),
            ..stencil_state()
        };
        let out = run_fog(&inputs, &fog, &Default::default(), &state, &resources).unwrap();
        assert_eq!(out.draws[0].state.depth_bounds_enable, Some(false));
        assert_eq!(out.draws[0].state.depth_bounds.unwrap()[0], 0x3f00_0000);
        let state = PipelineState {
            depth_bounds_enable: None,
            ..stencil_state()
        };
        assert_eq!(
            run_fog(&inputs, &fog, &Default::default(), &state, &resources),
            Err(ProducerError::UnknownState("depth-bounds enable"))
        );
    }

    #[test]
    fn vector_8_feedback() {
        let (inputs, mut fog, resources) = setup(true, 0.0);
        fog.usage = [Some(BOTH); 2];
        assert_eq!(
            run_fog(
                &inputs,
                &fog,
                &Default::default(),
                &stencil_state(),
                &resources
            ),
            Err(ProducerError::Feedback {
                address: fog.current.surface.address
            })
        );
    }

    #[test]
    fn vector_9_restore() {
        let (inputs, fog, resources) = setup(true, 0.0);
        let out = run_fog(
            &inputs,
            &fog,
            &Default::default(),
            &stencil_state(),
            &resources,
        )
        .unwrap();
        let (surface, viewport) = out.current.unwrap();
        assert_eq!(surface, select(&inputs.renderer, 0, 1).unwrap());
        assert_eq!(viewport.scissor, [0, 0, 1280, 720]);
        assert_eq!(
            (
                out.state.depth_bounds_enable,
                out.state.stencil_enable,
                out.state.two_sided_stencil
            ),
            (Some(false), Some(false), Some(false))
        );
        assert_eq!(
            out.attribute0.unwrap().map(f32::from_bits),
            [-1.0, 3.0, -1.0, 1.0]
        );
        assert_eq!(
            out.attribute8.unwrap().map(f32::from_bits),
            [0.0, -1.0, 0.0, 1.0]
        );
        assert_eq!(out.vertex_rows.unwrap(), fog.prepared.unwrap().vertex);
        assert_eq!(out.vertex_program, Some(0x61_0000));
    }

    #[test]
    fn vector_10_single_draw() {
        let (inputs, fog, resources) = setup(false, 0.0);
        let state = PipelineState {
            stencil_enable: Some(true),
            two_sided_stencil: Some(false),
            ..stencil_state()
        };
        let fog = FogProducerInputs {
            current: CurrentSurface {
                surface: select(&inputs.renderer, 0, 1).unwrap(),
                requested_role: 1,
            },
            ..fog
        };
        let mut resources = resources;
        let lighting = inputs.renderer.lighting_source();
        resources.images.insert(
            (lighting.address, lighting.value),
            crate::producer::tests::image_for(&lighting.value, 901),
        );
        let out = run_fog(&inputs, &fog, &Default::default(), &state, &resources).unwrap();
        assert_eq!(out.draws.len(), 1);
        let draw = &out.draws[0];
        assert_eq!(
            (draw.state.stencil_enable, draw.state.two_sided_stencil),
            (Some(true), Some(false))
        );
        assert_eq!(draw.state.front_stencil, Some(face(0)));
        assert_eq!(out.state.stencil_enable, Some(false));
        // With stencil writes enabled, the depth-bounds draw declines.
        let writes = PipelineState {
            front_stencil: Some(face(0xff)),
            ..state
        };
        assert_eq!(
            run_fog(&inputs, &fog, &Default::default(), &writes, &resources),
            Err(ProducerError::StencilWrites)
        );
    }

    #[test]
    fn declines() {
        let (inputs, fog, resources) = setup(true, 0.0);
        let state = stencil_state();
        let units = Default::default();
        // Fog values decline.
        let mut zero_range = inputs.clone();
        zero_range.renderer.fog_block.value[5] = 0;
        assert!(matches!(
            run_fog(&zero_range, &fog, &units, &state, &resources),
            Err(ProducerError::FogValues(_))
        ));
        // Unparsed, discarding or depth-exporting program.
        let unparsed = FogProducerInputs {
            usage: [None; 2],
            ..fog
        };
        assert_eq!(
            run_fog(&inputs, &unparsed, &units, &state, &resources),
            Err(ProducerError::ProgramUnparsed)
        );
        let discards = FogProducerInputs {
            usage: [Some(ProgramUsage {
                sampled_units: 2,
                discards: true,
            }); 2],
            ..fog
        };
        assert_eq!(
            run_fog(&inputs, &discards, &units, &state, &resources),
            Err(ProducerError::Discards)
        );
        let other_unit = FogProducerInputs {
            usage: [Some(ProgramUsage {
                sampled_units: 1 << 3,
                discards: false,
            }); 2],
            ..fog
        };
        assert_eq!(
            run_fog(&inputs, &other_unit, &units, &state, &resources),
            Err(ProducerError::SampledUnit(3))
        );
        let mut exports = inputs.clone();
        exports
            .programs
            .fog
            .untinted
            .as_mut()
            .unwrap()
            .control_flags = 2;
        assert_eq!(
            run_fog(&exports, &fog, &units, &state, &resources),
            Err(ProducerError::UnsupportedOutputs)
        );
        // The unit-0 descriptor does not associate with the current surface.
        let mut unassociated = inputs.clone();
        let position = table_position(fog.current.surface.role, 0);
        unassociated.renderer.surface_descriptors[position].value[7] = 2560;
        assert_eq!(
            run_fog(&unassociated, &fog, &units, &state, &resources),
            Err(ProducerError::MismatchedImage { unit: 0 })
        );
        // Unknown stencil faces needed by the two-draw stencil state.
        let unknown = PipelineState {
            front_stencil: None,
            ..state
        };
        assert_eq!(
            run_fog(&inputs, &fog, &units, &unknown, &resources),
            Err(ProducerError::UnknownState("front stencil"))
        );
        // Missing camera or prepared rows.
        let no_camera = FogProducerInputs {
            camera: None,
            ..fog
        };
        assert_eq!(
            run_fog(&inputs, &no_camera, &units, &state, &resources),
            Err(ProducerError::UnknownState("fog camera"))
        );
        // An uninitialised target.
        let mut uninitialised = resources_for(&inputs);
        let lighting = inputs.renderer.lighting_source();
        uninitialised.images.insert(
            (lighting.address, lighting.value),
            crate::producer::tests::image_for(&lighting.value, 5),
        );
        uninitialised
            .targets
            .get_mut(&fog.current.surface.address)
            .unwrap()
            .stencil_initialised = false;
        assert_eq!(
            run_fog(&inputs, &fog, &units, &state, &uninitialised),
            Err(ProducerError::UninitialisedTarget {
                address: fog.current.surface.address
            })
        );
    }
}
