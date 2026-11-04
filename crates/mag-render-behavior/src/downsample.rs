//! Main producer: depth downsample, group-15 slot 1 (`main-downsample.md`).

use crate::effects::{
    AttachmentImage, EffectsError, LoadOp, ModuleConstants, PacketImageInfo, ProducerPass,
    ProducerResult, ProducerTexture, ProducerTextureBinding, SurfaceBinding, consistent_binding,
    fullscreen_triangle, nearest, producer_raster, producer_viewport,
};
use crate::producer::ProgramUsage;
use crate::producers::RendererObject;
use crate::setup::{Comparison, PipelineState};
use crate::surfaces::{
    SelectedSurface, SourceViewport, ViewportConstants, select, source_viewport,
};
use crate::texture::{DecodedUnit, TextureUnitRecord, decode, native_sampler, view_parameters};

/// Packet metadata of the target's depth image.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct DepthImageInfo {
    pub format: u32,
    pub width: u32,
    pub height: u32,
    pub dimension: u32,
    pub samples: u32,
}

#[derive(Clone, Debug, PartialEq)]
pub struct DownsampleResult {
    pub result: ProducerResult,
    /// The restored current surface and its jittered surface viewport.
    pub current: Option<(SelectedSurface, SourceViewport)>,
    /// Incremented when the draw is omitted.
    pub omitted: u32,
}

/// Packet formats that are depth formats.
fn depth_packet_format(format: u32) -> bool {
    matches!(format, 2 | 15 | 16)
}

/// Runs the depth downsample. `omit_draw` is the opt-in diagnostic omission mode.
#[allow(clippy::too_many_arguments)]
pub fn downsample(
    renderer: &RendererObject,
    programs_present: bool,
    usage: &ProgramUsage,
    current_index: u32,
    bindings: &[SurfaceBinding],
    depth_image: Option<DepthImageInfo>,
    producer_bindings: &[ProducerTextureBinding],
    images: &[PacketImageInfo],
    constants: &ModuleConstants,
    viewport_constants: &ViewportConstants,
    state: &PipelineState,
    units: &[TextureUnitRecord; 16],
    omit_draw: bool,
) -> Result<DownsampleResult, EffectsError> {
    if !renderer.bank_zero_enabled() || !programs_present {
        return Ok(DownsampleResult {
            result: ProducerResult::no_op(state, units),
            current: None,
            omitted: 0,
        });
    }
    let q = renderer.downsample_record;
    if q.address != renderer.address + 0x2bac {
        return Err(EffectsError::Association);
    }
    let i0 = current_index;
    let dsrc = renderer.lighting_source_bank_zero;
    let mut unit0 = dsrc.value;
    unit0[5] = nearest(unit0[5]);
    let geometry = fullscreen_triangle(constants.l, constants.h, constants.f, constants.g)?;
    let restore = || -> Result<(SelectedSurface, SourceViewport), EffectsError> {
        let surface = select(renderer, i0, 2).map_err(|_| EffectsError::Association)?;
        consistent_binding(bindings, &surface)?;
        let viewport =
            source_viewport(&surface, viewport_constants).map_err(|_| EffectsError::Viewport)?;
        Ok((surface, viewport))
    };

    if omit_draw {
        let mut units = *units;
        units[0].apply_compact(&unit0);
        units[0].disable_if_known();
        let after = PipelineState {
            depth_test: Some(true),
            depth_write: Some(true),
            depth_comparison: Some(Comparison::LessOrEqual),
            alpha_test: Some(false),
            blend_enable: Some(false),
            cull_enable: Some(false),
            colour_masks: Some([0; 4]),
            ..*state
        };
        let current = restore()?;
        return Ok(DownsampleResult {
            result: ProducerResult {
                pass: None,
                state: after,
                units,
                attribute0: Some(geometry.attribute0),
                attribute8: Some(geometry.attribute8),
            },
            current: Some(current),
            omitted: 1,
        });
    }

    let q_surface = SelectedSurface {
        index: 0,
        role: 0,
        address: q.address,
        record: q.value,
    };
    let binding = consistent_binding(bindings, &q_surface)?;
    let depth = binding
        .depth
        .filter(|d| d.load == LoadOp::Load && binding.depth_initialised)
        .ok_or(EffectsError::Target("downsample depth"))?;
    if binding.stencil.is_some() && !binding.stencil_initialised {
        return Err(EffectsError::Target("downsample stencil"));
    }
    let info = depth_image.ok_or(EffectsError::Target("depth image metadata"))?;
    if info.format != binding.depth_format
        || info.dimension != 2
        || info.samples != 1
        || info.width < binding.physical.0
        || info.height < binding.physical.1
    {
        return Err(EffectsError::Target("depth image metadata"));
    }
    let source = producer_bindings
        .iter()
        .find(|b| b.address == dsrc.address && b.words == dsrc.value)
        .ok_or(EffectsError::Association)?;
    let image = images
        .get(source.image as usize)
        .ok_or(EffectsError::Association)?;
    if image.format != source.format
        || (image.width, image.height) != (source.width, source.height)
        || image.dimension != 2
        || image.samples != 1
        || image.layers != 1
        || depth_packet_format(image.format)
    {
        return Err(EffectsError::Unsupported("depth-format downsample source"));
    }
    let mut units = *units;
    units[0].apply_compact(&unit0);
    if let Some(unit) = (1..16).find(|u| usage.samples(*u)) {
        return Err(EffectsError::SampledUnit(unit));
    }
    let mut textures = Vec::new();
    if usage.samples(0) {
        let controls = match decode(&units[0]).map_err(|_| EffectsError::SampledImage)? {
            DecodedUnit::Enabled(c) => c,
            DecodedUnit::Disabled(_) => return Err(EffectsError::SampledImage),
        };
        textures.push(ProducerTexture {
            unit: 0,
            image: source.image,
            format: source.format,
            view: view_parameters(&controls),
            sampler: native_sampler(&controls).map_err(|_| EffectsError::SampledImage)?,
        });
    }
    let draw_state = PipelineState {
        depth_test: Some(true),
        depth_write: Some(true),
        alpha_test: Some(false),
        blend_enable: Some(false),
        cull_enable: Some(false),
        colour_masks: Some([0; 4]),
        ..*state
    };
    producer_raster(&draw_state, Some(0))?;
    let viewport = producer_viewport(
        &q.value,
        binding.physical,
        f32::from_bits(constants.zmin),
        f32::from_bits(constants.zmax),
    )?;
    let pass = ProducerPass {
        target: q.address,
        colour: None,
        depth: Some(AttachmentImage {
            image: depth.image,
            load: depth.load,
        }),
        stencil: binding.stencil,
        colour_format: 0,
        depth_format: binding.depth_format,
        viewport,
        geometry: geometry.clone(),
        state: draw_state,
        textures,
    };
    let after = PipelineState {
        depth_test: Some(true),
        depth_comparison: Some(Comparison::LessOrEqual),
        ..draw_state
    };
    units[0].disable_if_known();
    let current = restore()?;
    Ok(DownsampleResult {
        result: ProducerResult {
            pass: Some(pass),
            state: after,
            units,
            attribute0: Some(geometry.attribute0),
            attribute8: Some(geometry.attribute8),
        },
        current: Some(current),
        omitted: 0,
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::effects::tests::{all_bindings, constants, producer_state, renderer};
    use crate::surfaces::tests::record;

    const DSRC: [u32; 8] = [
        0x2000,
        0x0001_a522,
        0x0003_0303,
        0x8000_0000,
        0x0000_aae4,
        0x0202_0000,
        0x00a0_005a,
        0,
    ];

    struct Fixture {
        renderer: RendererObject,
        bindings: Vec<SurfaceBinding>,
        depth: Option<DepthImageInfo>,
        producer: Vec<ProducerTextureBinding>,
        images: Vec<PacketImageInfo>,
    }

    fn fixture() -> Fixture {
        let mut r = renderer(true);
        r.downsample_record.value = record(160, 90, 8, 2, 0);
        r.lighting_source_bank_zero.value = DSRC;
        let mut bindings = all_bindings(&r);
        bindings.push(SurfaceBinding {
            address: r.downsample_record.address,
            physical: (160, 90),
            colour_format: 0,
            depth_format: 15,
            colour: None,
            depth: Some(AttachmentImage {
                image: 77,
                load: LoadOp::Load,
            }),
            stencil: None,
            depth_initialised: true,
            stencil_initialised: false,
        });
        Fixture {
            producer: vec![ProducerTextureBinding {
                address: r.lighting_source_bank_zero.address,
                words: DSRC,
                image: 0,
                format: 13,
                width: 160,
                height: 90,
            }],
            renderer: r,
            bindings,
            depth: Some(DepthImageInfo {
                format: 15,
                width: 160,
                height: 90,
                dimension: 2,
                samples: 1,
            }),
            images: vec![PacketImageInfo {
                format: 13,
                width: 160,
                height: 90,
                dimension: 2,
                samples: 1,
                layers: 1,
            }],
        }
    }

    const UNIT0: ProgramUsage = ProgramUsage {
        sampled_units: 1,
        discards: false,
    };

    fn vc() -> ViewportConstants {
        ViewportConstants {
            vp_hx: 0.5,
            vp_hy: -0.5,
            clip_min: 0.0,
            clip_max: 1.0,
            jitter: [0.0, 0.0],
        }
    }

    fn run(
        f: &Fixture,
        state: &PipelineState,
        omit: bool,
    ) -> Result<DownsampleResult, EffectsError> {
        downsample(
            &f.renderer,
            true,
            &UNIT0,
            1,
            &f.bindings,
            f.depth,
            &f.producer,
            &f.images,
            &constants(),
            &vc(),
            state,
            &Default::default(),
            omit,
        )
    }

    #[test]
    fn v1_v2_normal_draw() {
        let f = fixture();
        let out = run(&f, &producer_state(), false).unwrap();
        let pass = out.result.pass.as_ref().unwrap();
        assert_eq!((pass.colour, pass.depth.unwrap().image), (None, 77));
        let v = pass.viewport;
        assert_eq!(
            (v.native.x, v.native.y, v.native.width, v.native.height),
            (0.0, 0.0, 160.0, 90.0)
        );
        assert_eq!(v.native.scissor, [0, 0, 160, 90]);
        assert_eq!((v.shader_scale[2], v.shader_offset[2]), (0.5, 0.5));
        assert_eq!(pass.geometry.indices.len(), 3);
        assert_eq!(pass.target, f.renderer.address + 0x2bac);
        assert_eq!(pass.colour_format, 0);
        assert_eq!(out.result.units[0].compact.unwrap()[5], 0x0101_0000);
        assert_eq!(out.result.units[0].compact.unwrap()[3], 0);
        assert_eq!(
            out.result.attribute0.unwrap().map(f32::from_bits),
            [-0.75, 2.5, -0.75, 1.0]
        );
        assert_eq!(
            out.result.attribute8.unwrap().map(f32::from_bits),
            [0.0, -0.75, 0.0, 1.0]
        );
        assert_eq!(
            out.result.state.depth_comparison,
            Some(Comparison::LessOrEqual)
        );
        let (current, _) = out.current.unwrap();
        assert_eq!(current.address, f.renderer.address + 0x2c10 + 0x150 + 0x54);
    }

    #[test]
    fn v3_inherited_comparison() {
        let f = fixture();
        let state = PipelineState {
            depth_comparison: Some(Comparison::Greater),
            ..producer_state()
        };
        let out = run(&f, &state, false).unwrap();
        assert_eq!(
            out.result.pass.unwrap().state.depth_comparison,
            Some(Comparison::Greater)
        );
        assert_eq!(
            out.result.state.depth_comparison,
            Some(Comparison::LessOrEqual)
        );
    }

    #[test]
    fn v4_to_v7_refusals_and_no_ops() {
        let mut f = fixture();
        f.images[0].format = 15;
        f.producer[0].format = 15;
        assert_eq!(
            run(&f, &producer_state(), false),
            Err(EffectsError::Unsupported("depth-format downsample source"))
        );
        let mut f = fixture();
        f.bindings.last_mut().unwrap().depth_initialised = false;
        assert_eq!(
            run(&f, &producer_state(), false),
            Err(EffectsError::Target("downsample depth"))
        );
        let mut f = fixture();
        f.renderer.stencil_branch = 0;
        let out = run(&f, &producer_state(), false).unwrap();
        assert!(out.result.pass.is_none() && out.current.is_none());
        let f = fixture();
        let out = downsample(
            &f.renderer,
            false,
            &UNIT0,
            1,
            &f.bindings,
            f.depth,
            &f.producer,
            &f.images,
            &constants(),
            &vc(),
            &producer_state(),
            &Default::default(),
            false,
        )
        .unwrap();
        assert!(out.result.pass.is_none() && out.current.is_none());
        assert_eq!(out.result.state, producer_state());
        let mut f = fixture();
        f.producer.clear();
        assert_eq!(
            run(&f, &producer_state(), false),
            Err(EffectsError::Association)
        );
        let mut f = fixture();
        f.depth = Some(DepthImageInfo {
            width: 100,
            ..f.depth.unwrap()
        });
        assert_eq!(
            run(&f, &producer_state(), false),
            Err(EffectsError::Target("depth image metadata"))
        );
    }

    #[test]
    fn v8_omission_mode() {
        let f = fixture();
        let out = run(&f, &producer_state(), true).unwrap();
        assert!(out.result.pass.is_none());
        assert_eq!(out.omitted, 1);
        let unit0 = out.result.units[0].compact.unwrap();
        assert_eq!((unit0[5], unit0[3]), (0x0101_0000, 0));
        assert_eq!(
            (out.result.state.depth_write, out.result.state.colour_masks),
            (Some(true), Some([0; 4]))
        );
        assert!(out.current.is_some());
    }
}
