//! Main producer: quincunx resolve, group-15 slot 0 (`main-quincunx.md`).
//!
//! Known deviation, as the specification requires it to stay labelled: native backends have no
//! RSX convolution filter, so the draw samples the nearest texel instead of the quincunx blend.

use crate::effects::{
    EffectsError, LoadOp, ModuleConstants, ProducerPass, ProducerResult, ProducerTexture,
    ProducerViewport, SurfaceBinding, consistent_binding, fullscreen_triangle, nearest,
    producer_raster, producer_viewport,
};
use crate::producer::ProgramUsage;
use crate::producers::RendererObject;
use crate::setup::PipelineState;
use crate::surfaces::{SelectedSurface, select, table_position};
use crate::texture::{DecodedUnit, TextureUnitRecord, decode, native_sampler, view_parameters};

/// The carried source descriptor of step 7.
pub fn source_descriptor(descriptor: &[u32; 8], srgb: bool) -> [u32; 8] {
    let mut d = *descriptor;
    d[5] = (d[5] & !0x0f0f_e000) | 0x0407_2000;
    d[2] &= !0x20;
    d[3] &= !0x70;
    if srgb {
        d[2] = (d[2] & !0x00f0_0000) | 0x0070_0000;
    }
    d
}

/// The compatibility descriptor used only for translation and sampler creation.
pub fn compatibility_descriptor(source: &[u32; 8]) -> Result<[u32; 8], EffectsError> {
    if source[5] & 0x0f0f_e000 != 0x0407_2000 || source[5] & 0x1fff != 0 {
        return Err(EffectsError::Unsupported("quincunx source filter"));
    }
    let mut d = *source;
    d[5] = (d[5] & !0x0f0f_0000) | 0x0103_0000;
    Ok(d)
}

/// The result of the quincunx producer: the producer result and the new current surface.
#[derive(Clone, Debug, PartialEq)]
pub struct QuincunxResult {
    pub result: ProducerResult,
    pub current: Option<(SelectedSurface, ProducerViewport)>,
    /// The compatibility descriptor the draw was compiled for.
    pub compatibility: Option<[u32; 8]>,
}

/// Runs the quincunx resolve.
#[allow(clippy::too_many_arguments)]
pub fn quincunx(
    renderer: &RendererObject,
    programs_present: bool,
    usage: &ProgramUsage,
    current: &SelectedSurface,
    bindings: &[SurfaceBinding],
    selector: Option<u32>,
    constants: &ModuleConstants,
    state: &PipelineState,
    units: &[TextureUnitRecord; 16],
) -> Result<QuincunxResult, EffectsError> {
    let no_op = || QuincunxResult {
        result: ProducerResult::no_op(state, units),
        current: None,
        compatibility: None,
    };
    if !renderer.bank_zero_enabled() || current.role != 1 || !programs_present {
        return Ok(no_op());
    }
    let s = current;
    let d = select(renderer, s.index ^ 1, 2).map_err(|_| EffectsError::Association)?;
    let s_binding = consistent_binding(bindings, s)?;
    let d_binding = consistent_binding(bindings, &d)?;
    let s_colour = s_binding
        .colour
        .ok_or(EffectsError::Target("source colour"))?;
    let d_colour = d_binding
        .colour
        .filter(|c| c.load == LoadOp::Load)
        .ok_or(EffectsError::Target("target colour load"))?;
    if (d_binding.depth.is_some() && !d_binding.depth_initialised)
        || (d_binding.stencil.is_some() && !d_binding.stencil_initialised)
    {
        return Err(EffectsError::Target("target depth or stencil"));
    }
    match selector {
        None => return Err(EffectsError::ColourTarget),
        Some(1) => {}
        Some(_) => return Err(EffectsError::Unsupported("colour target")),
    }
    if d_colour.image == s_colour.image {
        return Err(EffectsError::Alias);
    }
    let srgb = renderer.srgb != 0;
    let base = select(renderer, s.index, 1).map_err(|_| EffectsError::Association)?;
    let source = source_descriptor(
        &renderer.surface_descriptors[table_position(base.role, base.index)].value,
        srgb,
    );
    let mut units = *units;
    units[0].apply_compact(&source);
    if let Some(unit) = (1..16).find(|u| usage.samples(*u)) {
        return Err(EffectsError::SampledUnit(unit));
    }
    let compatibility = compatibility_descriptor(&source)?;
    let mut textures = Vec::new();
    if usage.samples(0) {
        let record = TextureUnitRecord {
            compact: Some(compatibility),
            ..units[0]
        };
        let controls = match decode(&record).map_err(|_| EffectsError::SampledImage)? {
            DecodedUnit::Enabled(c) => c,
            DecodedUnit::Disabled(_) => return Err(EffectsError::SampledImage),
        };
        textures.push(ProducerTexture {
            unit: 0,
            image: s_colour.image,
            format: s_binding.colour_format,
            view: view_parameters(&controls),
            sampler: native_sampler(&controls).map_err(|_| EffectsError::SampledImage)?,
        });
    }
    let mut draw_state = PipelineState {
        depth_test: Some(false),
        depth_write: Some(false),
        alpha_test: Some(false),
        blend_enable: Some(false),
        colour_masks: Some([15; 4]),
        cull_enable: Some(false),
        ..*state
    };
    if srgb {
        draw_state.srgb_output = Some(true);
    }
    producer_raster(&draw_state, None)?;
    let geometry = fullscreen_triangle(constants.l, constants.h, constants.f, constants.g)?;
    let viewport = producer_viewport(
        &d.record,
        d_binding.physical,
        f32::from_bits(constants.zmin),
        f32::from_bits(constants.zmax),
    )?;
    let pass = ProducerPass {
        target: d.address,
        colour: Some(d_colour),
        depth: d_binding.depth,
        stencil: d_binding.stencil,
        colour_format: d_binding.colour_format,
        depth_format: d_binding.depth_format,
        viewport,
        geometry: geometry.clone(),
        state: draw_state,
        textures,
    };
    // Footer.
    let mut after = source;
    after[5] = nearest(after[5]);
    units[0].apply_compact(&after);
    for unit in 0..3 {
        if units[unit].compact.is_none() {
            return Err(EffectsError::UnknownUnit(unit as u32));
        }
        units[unit].disable_if_known();
    }
    Ok(QuincunxResult {
        result: ProducerResult {
            pass: Some(pass),
            state: draw_state,
            units,
            attribute0: Some(geometry.attribute0),
            attribute8: Some(geometry.attribute8),
        },
        current: Some((d, viewport)),
        compatibility: Some(compatibility),
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::effects::AttachmentImage;
    use crate::effects::tests::{all_bindings, constants, producer_state, renderer};

    const DESCRIPTOR: [u32; 8] = [
        0x4000,
        0x0001_8529,
        0x0003_0321,
        0x8000_0078,
        0x0000_aae4,
        0x0606_4000,
        0x0280_00b4,
        0,
    ];

    fn fixture() -> (
        RendererObject,
        Vec<SurfaceBinding>,
        SelectedSurface,
        [TextureUnitRecord; 16],
    ) {
        let mut r = renderer(true);
        r.aa_enable = 1;
        let s_position = table_position(1, 0);
        r.surface_table[s_position].value.words[3] |= 3 << 12;
        r.surface_descriptors[s_position].value = DESCRIPTOR;
        let s = select(&r, 0, 1).unwrap();
        let mut bindings = all_bindings(&r);
        bindings
            .iter_mut()
            .find(|b| b.address == s.address)
            .unwrap()
            .physical = (640, 180);
        let mut units: [TextureUnitRecord; 16] = Default::default();
        units[1].compact = Some([1, 2, 3, 0x8000_0000, 5, 6, 7, 8]);
        units[2].compact = Some([9, 10, 11, 0x8000_0000, 13, 14, 15, 16]);
        (r, bindings, s, units)
    }

    const UNIT0: ProgramUsage = ProgramUsage {
        sampled_units: 1,
        discards: false,
    };

    fn run(
        r: &RendererObject,
        bindings: &[SurfaceBinding],
        s: &SelectedSurface,
        units: &[TextureUnitRecord; 16],
    ) -> Result<QuincunxResult, EffectsError> {
        quincunx(
            r,
            true,
            &UNIT0,
            s,
            bindings,
            Some(1),
            &constants(),
            &producer_state(),
            units,
        )
    }

    #[test]
    fn v1_v2_source_descriptor() {
        let d = source_descriptor(&DESCRIPTOR, false);
        assert_eq!((d[2], d[3], d[5]), (0x0003_0301, 0x8000_0008, 0x0407_2000));
        assert_eq!(&d[..2], &DESCRIPTOR[..2]);
        assert_eq!(source_descriptor(&DESCRIPTOR, true)[2], 0x0073_0301);
        let (mut r, bindings, s, units) = fixture();
        r.srgb = 1;
        let out = run(&r, &bindings, &s, &units).unwrap();
        assert_eq!(out.result.pass.unwrap().state.srgb_output, Some(true));
    }

    #[test]
    fn v3_v4_compatibility_and_footer() {
        let (r, bindings, s, units) = fixture();
        let out = run(&r, &bindings, &s, &units).unwrap();
        assert_eq!(out.compatibility.unwrap()[5], 0x0103_2000);
        let after = out.result.units;
        assert_eq!(
            (after[0].compact.unwrap()[5], after[0].compact.unwrap()[3]),
            (0x0101_2000, 0)
        );
        assert_eq!(after[1].compact, Some([1, 2, 3, 0, 5, 6, 7, 8]));
        assert_eq!(after[2].compact, Some([9, 10, 11, 0, 13, 14, 15, 16]));
        assert_eq!(
            out.result.attribute0.unwrap().map(f32::from_bits),
            [-0.75, 2.5, -0.75, 1.0]
        );
    }

    #[test]
    fn v5_pass_and_current_surface() {
        let (r, bindings, s, units) = fixture();
        let out = run(&r, &bindings, &s, &units).unwrap();
        let pass = out.result.pass.unwrap();
        let d = select(&r, 1, 2).unwrap();
        assert_eq!(d.address, r.address + 0x2c10 + 0x150 + 0x54);
        assert_eq!(pass.target, d.address);
        assert_eq!(
            pass.colour.unwrap().image,
            bindings
                .iter()
                .find(|b| b.address == d.address)
                .unwrap()
                .colour
                .unwrap()
                .image
        );
        let v = pass.viewport.native;
        assert_eq!((v.x, v.y, v.width, v.height), (0.0, 0.0, 320.0, 180.0));
        assert_eq!(pass.geometry.indices.len(), 3);
        assert_eq!(pass.viewport.clip, (320, 180));
        assert_eq!(pass.textures[0].sampler.min_filter, 0);
        let (current, viewport) = out.current.unwrap();
        assert_eq!(current.address, d.address);
        assert_eq!((viewport.native.x, viewport.native.y), (0.0, 0.0));
    }

    #[test]
    fn v6_to_v9_refusals() {
        let (mut r, bindings, s, units) = fixture();
        r.surface_descriptors[table_position(1, 0)].value[5] = 0x0606_4001;
        assert_eq!(
            source_descriptor(&r.surface_descriptors[table_position(1, 0)].value, false)[5],
            0x0407_2001
        );
        assert_eq!(
            run(&r, &bindings, &s, &units),
            Err(EffectsError::Unsupported("quincunx source filter"))
        );
        let (r, bindings, _, units) = fixture();
        let role2 = select(&r, 0, 2).unwrap();
        let out = run(&r, &bindings, &role2, &units).unwrap();
        assert!(out.result.pass.is_none() && out.current.is_none());
        let (r, mut bindings, s, units) = fixture();
        let d = select(&r, 1, 2).unwrap();
        let s_image = bindings
            .iter()
            .find(|b| b.address == s.address)
            .unwrap()
            .colour
            .unwrap()
            .image;
        bindings
            .iter_mut()
            .find(|b| b.address == d.address)
            .unwrap()
            .colour = Some(AttachmentImage {
            image: s_image,
            load: LoadOp::Load,
        });
        assert_eq!(run(&r, &bindings, &s, &units), Err(EffectsError::Alias));
        let (r, bindings, s, mut units) = fixture();
        units[1].compact = None;
        assert_eq!(
            run(&r, &bindings, &s, &units),
            Err(EffectsError::UnknownUnit(1))
        );
        let (r, bindings, s, units) = fixture();
        assert_eq!(
            quincunx(
                &r,
                true,
                &UNIT0,
                &s,
                &bindings,
                Some(2),
                &constants(),
                &producer_state(),
                &units
            ),
            Err(EffectsError::Unsupported("colour target"))
        );
        assert!(
            quincunx(
                &r,
                false,
                &UNIT0,
                &s,
                &bindings,
                Some(1),
                &constants(),
                &producer_state(),
                &units
            )
            .unwrap()
            .result
            .pass
            .is_none()
        );
    }
}
