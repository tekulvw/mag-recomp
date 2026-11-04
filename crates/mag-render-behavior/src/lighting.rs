//! Main lighting producer, initial slot 1 (`main-lighting.md`).

use crate::producer::{
    ProducerDraw, ProducerError, ProducerResources, ProgramUsage, TextureBinding, attributes_after,
    bounds_words, check_draw_state, check_fragment_program, check_sampled_image, check_target,
    fullscreen_state, fullscreen_triangle, set_stencil_test,
};
use crate::producers::ProducerInputs;
use crate::setup::{Comparison, PipelineState};
use crate::surfaces::{
    DepthClass, LightingSources, ViewportConstants, classify_depth, explicit_viewport, select,
    source_viewport, surface_target_viewport,
};
use crate::texture::TextureUnitRecord;

/// The numeric inputs of the lighting constants, as binary32 values.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct LightingMathInputs {
    /// Row-major camera matrix `M`.
    pub camera: [f32; 16],
    pub p_scale: f32,
    pub p_far: f32,
    pub p_slope: f32,
    pub p_aspect: f32,
    pub r_a: f32,
    pub r_b: f32,
    pub transform0: [f32; 16],
    pub transform1: [f32; 16],
    pub v_one: [f32; 4],
    pub s_depth: f32,
    pub a: f32,
    pub b: f32,
    pub c: f32,
    pub z0: f32,
    pub zh: f32,
    pub pa: f32,
    pub pb: f32,
    pub pc: f32,
    pub pd: f32,
    pub loff: f32,
}

/// The computed lighting constants.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct LightingConstants {
    /// Fragment rows 0–3 are `F_0`, rows 4–7 are `F_1`.
    pub fragment_rows: [[f32; 4]; 8],
    /// Vertex constant row 4: `(width, height, +0, +0)`.
    pub vertex_row4: [f32; 4],
    /// `[Z0, upper]`.
    pub bounds: [f32; 2],
}

type Matrix = [[f64; 4]; 4];

fn matrix(values: &[f32; 16]) -> Matrix {
    std::array::from_fn(|row| std::array::from_fn(|column| values[4 * row + column] as f64))
}

fn multiply(left: &Matrix, right: &Matrix) -> Matrix {
    std::array::from_fn(|row| {
        std::array::from_fn(|column| (0..4).map(|k| left[row][k] * right[k][column]).sum())
    })
}

/// The camera inverse `Inv`: the affine inverse of `M` (column 3 treated as `(0, 0, 0, 1)`)
/// with column `j` scaled by `v_one[j]`.
fn affine_inverse(camera: &[f32; 16], v_one: &[f32; 4]) -> Result<Matrix, ProducerError> {
    let m = matrix(camera);
    let a = |r: usize, c: usize| m[r][c];
    let cofactor = |r: usize, c: usize| {
        let (r0, r1) = ((r + 1) % 3, (r + 2) % 3);
        let (c0, c1) = ((c + 1) % 3, (c + 2) % 3);
        a(r0, c0) * a(r1, c1) - a(r0, c1) * a(r1, c0)
    };
    let det = (0..3).map(|c| a(0, c) * cofactor(0, c)).sum::<f64>();
    if det == 0.0 || !det.is_finite() {
        return Err(ProducerError::SingularCamera);
    }
    let inverse3: [[f64; 3]; 3] =
        std::array::from_fn(|r| std::array::from_fn(|c| cofactor(c, r) / det));
    let t = [m[3][0], m[3][1], m[3][2]];
    let o = v_one.map(|v| v as f64);
    let mut inv: Matrix = [[0.0; 4]; 4];
    for r in 0..3 {
        for c in 0..3 {
            inv[r][c] = inverse3[r][c] * o[c];
        }
    }
    for c in 0..3 {
        inv[3][c] = -(0..3).map(|k| t[k] * inverse3[k][c]).sum::<f64>() * o[c];
    }
    inv[3][3] = 1.0;
    if inv.iter().flatten().any(|v| !(*v as f32).is_finite()) {
        return Err(ProducerError::SingularCamera);
    }
    Ok(inv)
}

/// The camera inverse rounded to binary32, for inspection. Inputs are not checked for finiteness
/// here; [`lighting_constants`] does that.
pub fn camera_inverse(
    camera: &[f32; 16],
    v_one: &[f32; 4],
) -> Result<[[f32; 4]; 4], ProducerError> {
    Ok(affine_inverse(camera, v_one)?.map(|row| row.map(|v| v as f32)))
}

/// The window-depth map `D(z) = ZH·((PA / z + PB + PC) / PD) + ZH`.
pub fn window_depth(z: f64, zh: f64, pa: f64, pb: f64, pc: f64, pd: f64) -> f64 {
    zh * ((pa / z + pb + pc) / pd) + zh
}

/// Computes the lighting constants. Evaluates in binary64 and rounds once to binary32.
pub fn lighting_constants(i: &LightingMathInputs) -> Result<LightingConstants, ProducerError> {
    // Column 3 of M above row 3 is never read, so it is not checked. Lanes 1–3 of the basis
    // vectors are not part of these inputs.
    let camera_checked = (0..16).filter(|index| !(index % 4 == 3 && index / 4 < 3));
    if camera_checked
        .map(|index| i.camera[index])
        .any(|v| !v.is_finite())
    {
        return Err(ProducerError::NonFinite("camera matrix"));
    }
    if i.transform0
        .iter()
        .chain(&i.transform1)
        .any(|v| !v.is_finite())
    {
        return Err(ProducerError::NonFinite("parameter transform"));
    }
    if i.v_one.iter().any(|v| !v.is_finite()) {
        return Err(ProducerError::NonFinite("V_ONE"));
    }
    let inv = affine_inverse(&i.camera, &i.v_one)?;

    let (pa_, pb_, pc_) = (i.a as f64, i.b as f64, i.c as f64);
    let mut fragment_rows = [[0.0f32; 4]; 8];
    for (k, (transform, e)) in [(&i.transform0, pc_), (&i.transform1, 0.0)]
        .into_iter()
        .enumerate()
    {
        let texture: Matrix = [
            [pc_, 0.0, 0.0, 0.0],
            [0.0, pb_, 0.0, 0.0],
            [0.0, 0.0, pa_, 0.0],
            [e, pa_, pa_, 1.0],
        ];
        let f = multiply(&multiply(&inv, &matrix(transform)), &texture);
        for r in 0..4 {
            fragment_rows[4 * k + r] = f[r].map(|v| v as f32);
        }
    }
    if fragment_rows.iter().flatten().any(|v| !v.is_finite()) {
        return Err(ProducerError::NonFinite("fragment constant"));
    }

    let height = i.p_far as f64 * i.p_slope as f64;
    let width = height * i.p_aspect as f64;
    let vertex_row4 = [width as f32, height as f32, 0.0, 0.0];
    if !vertex_row4[0].is_finite() || !vertex_row4[1].is_finite() {
        return Err(ProducerError::NonFinite("reconstruction extent"));
    }

    let distance = (i.r_a as f64 + i.r_b as f64) / (i.p_scale as f64 * i.s_depth as f64);
    let upper = window_depth(
        distance + i.loff as f64,
        i.zh as f64,
        i.pa as f64,
        i.pb as f64,
        i.pc as f64,
        i.pd as f64,
    ) as f32;
    let bounds = [i.z0, upper];
    if !bounds[0].is_finite() || !bounds[1].is_finite() {
        return Err(ProducerError::NonFinite("depth bound"));
    }
    if bounds[0] > bounds[1] {
        return Err(ProducerError::InvertedBounds);
    }
    Ok(LightingConstants {
        fragment_rows,
        vertex_row4,
        bounds,
    })
}

/// Everything the lighting producer leaves behind.
#[derive(Clone, Debug, PartialEq)]
pub struct LightingOutput {
    pub constants: LightingConstants,
    /// Vertex constant row 4. The lighting vertex program's static constants are applied after
    /// it and win where they also write row 4.
    pub vertex_row4: [u32; 4],
    /// Guest address of the lighting vertex program whose static constants follow row 4.
    pub vertex_program: u32,
    /// Texture units 0 and 1 after the producer. Any image previously associated with them is
    /// forgotten; the draws carry the bindings.
    pub units: [TextureUnitRecord; 2],
    pub draws: Vec<ProducerDraw>,
    pub state_after: PipelineState,
    pub attribute0: [u32; 4],
    pub attribute8: [u32; 4],
    /// Read by the fog producer.
    pub lighting_ran: bool,
}

fn vector_lane0(
    vector: &crate::producers::VectorConstant,
    what: &'static str,
) -> Result<f32, ProducerError> {
    match vector {
        crate::producers::VectorConstant::Read(tagged) => Ok(f32::from_bits(tagged.value[0])),
        crate::producers::VectorConstant::Misaligned(address) => Err(ProducerError::Misaligned {
            what,
            address: *address,
        }),
    }
}

/// Gathers the numeric inputs from the acquired input set, declining on misaligned sources.
pub fn lighting_math_inputs(inputs: &ProducerInputs) -> Result<LightingMathInputs, ProducerError> {
    let renderer = &inputs.renderer;
    let projection = &renderer.projection;
    if !projection.address.is_multiple_of(16) {
        return Err(ProducerError::Misaligned {
            what: "projection block",
            address: projection.address,
        });
    }
    if !inputs.params.address.is_multiple_of(16) {
        return Err(ProducerError::Misaligned {
            what: "PARAMS",
            address: inputs.params.address,
        });
    }
    let v_one = match &inputs.v_one {
        crate::producers::VectorConstant::Read(tagged) => tagged.value.map(f32::from_bits),
        crate::producers::VectorConstant::Misaligned(address) => {
            return Err(ProducerError::Misaligned {
                what: "V_ONE",
                address: *address,
            });
        }
    };
    let a = vector_lane0(&inputs.basis_a, "BASIS_A")?;
    let b = vector_lane0(&inputs.basis_b, "BASIS_B")?;
    let c = vector_lane0(&inputs.basis_c, "BASIS_C")?;
    let f = f32::from_bits;
    let s = &inputs.scalars;
    Ok(LightingMathInputs {
        camera: projection.matrix.map(f),
        p_scale: f(projection.scale),
        p_far: f(projection.far),
        p_slope: f(projection.slope),
        p_aspect: f(projection.aspect),
        r_a: f(renderer.depth_distance_a.value),
        r_b: f(renderer.depth_distance_b.value),
        transform0: inputs.params.transform0.value.map(f),
        transform1: inputs.params.transform1.value.map(f),
        v_one,
        s_depth: f(inputs.s_depth.value),
        a,
        b,
        c,
        z0: f(s.z0.value),
        zh: f(s.zh.value),
        pa: f(s.pa.value),
        pb: f(s.pb.value),
        pc: f(s.pc.value),
        pd: f(s.pd.value),
        loff: f(s.loff.value),
    })
}

/// Runs the lighting producer.
///
/// `usage` describes the lighting fragment program (program decode contract) and `state` is
/// the fixed-function state inherited at this point of Main.
pub fn run_lighting<R: ProducerResources + ?Sized>(
    inputs: &ProducerInputs,
    usage: Option<ProgramUsage>,
    state: &PipelineState,
    resources: &R,
) -> Result<LightingOutput, ProducerError> {
    let math = lighting_math_inputs(inputs)?;
    let programs = &inputs.programs.lighting;
    let (Some(vertex), Some(fragment)) = (&programs.vertex, &programs.fragment) else {
        return Err(ProducerError::ProgramAbsent);
    };
    let usage = usage.ok_or(ProducerError::ProgramUnparsed)?;
    check_fragment_program(fragment.control_flags, &usage)?;

    let constants = lighting_constants(&math)?;
    let s = &inputs.scalars;
    let triangle = fullscreen_triangle(s.t_lo.value, s.t_hi.value, s.t_far.value, s.t_uvfar.value)?;

    // Stencil precondition: a depth-bounds rejection must not be able to change stencil.
    match state.stencil_enable {
        None => return Err(ProducerError::UnknownState("stencil enable")),
        Some(true) => {
            let front = state
                .front_stencil
                .ok_or(ProducerError::UnknownState("front stencil"))?;
            let back = state
                .back_stencil
                .ok_or(ProducerError::UnknownState("back stencil"))?;
            if front.write_mask != 0 || back.write_mask != 0 {
                return Err(ProducerError::StencilWrites);
            }
        }
        Some(false) => {}
    }

    // Textures.
    let sources = LightingSources::of(inputs);
    let mut unit0 = TextureUnitRecord::default();
    unit0.apply_compact(&sources.unit0.value);
    let mut unit1 = TextureUnitRecord::default();
    unit1.apply_full(&sources.unit1.value);
    unit1.border = Some(0xffff_ffff);
    let mut textures = Vec::new();
    if usage.samples(0) {
        let tagged = sources.unit0;
        let binding = match classify_depth(&inputs.renderer, &tagged.value, tagged.address) {
            DepthClass::Unsupported => {
                return Err(ProducerError::UnsupportedDepthSource { unit: 0 });
            }
            DepthClass::Depth(source) => {
                let image = resources
                    .depth_plane(&source)
                    .ok_or(ProducerError::MissingImage { unit: 0 })?;
                if !image.depth {
                    return Err(ProducerError::MismatchedImage { unit: 0 });
                }
                check_sampled_image(0, &image, &tagged.value)?;
                TextureBinding {
                    unit: 0,
                    source_address: tagged.address,
                    descriptor: tagged.value,
                    border: None,
                    image,
                    depth_source: Some(source),
                }
            }
            DepthClass::NotDepth => {
                let image = resources
                    .image(tagged.address, &tagged.value)
                    .ok_or(ProducerError::MissingImage { unit: 0 })?;
                if image.depth {
                    return Err(ProducerError::MismatchedImage { unit: 0 });
                }
                check_sampled_image(0, &image, &tagged.value)?;
                TextureBinding {
                    unit: 0,
                    source_address: tagged.address,
                    descriptor: tagged.value,
                    border: None,
                    image,
                    depth_source: None,
                }
            }
        };
        textures.push(binding);
    }
    if usage.samples(1) {
        let compact = sources.compact(1);
        let image = resources
            .image(compact.address, &compact.value)
            .ok_or(ProducerError::MissingImage { unit: 1 })?;
        let depth_format = (0x90..=0x93).contains(&((compact.value[1] >> 8) & 0x9f));
        if image.depth && !depth_format {
            return Err(ProducerError::MismatchedImage { unit: 1 });
        }
        check_sampled_image(1, &image, &compact.value)?;
        textures.push(TextureBinding {
            unit: 1,
            source_address: compact.address,
            descriptor: compact.value,
            border: unit1.border,
            image,
            depth_source: None,
        });
    }

    // Draw 0.
    let fragment_rows: Vec<[u32; 4]> = constants
        .fragment_rows
        .iter()
        .map(|row| row.map(f32::to_bits))
        .collect();
    let first_state = PipelineState {
        depth_bounds_enable: Some(true),
        depth_bounds: Some(bounds_words(constants.bounds)),
        srgb_output: Some(false),
        ..fullscreen_state(state, 8)
    };
    check_draw_state(&first_state)?;
    let renderer = &inputs.renderer;
    let mut draws = Vec::new();
    let target0 = select(renderer, 1, 1)?;
    let info0 = check_target(resources, &target0)?;
    let explicit = explicit_viewport(
        renderer.explicit_width,
        renderer.explicit_height,
        f32::from_bits(s.z0.value),
        f32::from_bits(s.z1.value),
    );
    draws.push(ProducerDraw {
        target: target0,
        viewport: surface_target_viewport(&target0, &explicit, info0.physical)?,
        state: first_state,
        vertices: triangle,
        fragment_rows: fragment_rows.clone(),
        textures: textures.clone(),
    });

    // Draw 1.
    let mut last_state = first_state;
    if renderer.bank_zero_enabled() {
        let second_state = PipelineState {
            depth_bounds_enable: Some(false),
            front_stencil: set_stencil_test(
                first_state.front_stencil,
                Comparison::Less,
                0x80,
                0xff,
            ),
            back_stencil: set_stencil_test(first_state.back_stencil, Comparison::Less, 0x80, 0xff),
            ..first_state
        };
        check_draw_state(&second_state)?;
        let target1 = select(renderer, 1, 0)?;
        let info1 = check_target(resources, &target1)?;
        let source = source_viewport(&target1, &ViewportConstants::of(inputs))?;
        draws.push(ProducerDraw {
            target: target1,
            viewport: surface_target_viewport(&target1, &source, info1.physical)?,
            state: second_state,
            vertices: triangle,
            fragment_rows,
            textures,
        });
        last_state = second_state;
    }

    let state_after = PipelineState {
        depth_bounds_enable: Some(false),
        two_sided_stencil: Some(false),
        stencil_enable: Some(false),
        ..last_state
    };
    let (attribute0, attribute8) = attributes_after(&triangle);
    Ok(LightingOutput {
        vertex_row4: constants.vertex_row4.map(f32::to_bits),
        constants,
        vertex_program: vertex.address,
        units: [unit0, unit1],
        draws,
        state_after,
        attribute0,
        attribute8,
        lighting_ran: true,
    })
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;
    use crate::producer::tests::{Resources, face, image_for, known_state};
    use crate::producer::{ImageInfo, TargetInfo};
    use crate::producers::{
        FragmentProgramFile, Tagged, VectorConstant, VertexProgramObject, acquire, tests as p,
    };
    use crate::surfaces::tests::record;
    use crate::testutil::Rng;

    const IDENTITY: [f32; 16] = [
        1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0,
    ];
    const L1_CAMERA: [f32; 16] = [
        0.0, 2.0, 0.0, 0.0, -4.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.5, 0.0, 1.0, 2.0, 3.0, 1.0,
    ];
    const L1_P1: [f32; 16] = [
        2.0, 0.0, 0.0, 0.0, 0.0, 2.0, 0.0, 0.0, 0.0, 0.0, 2.0, 0.0, 0.25, 0.5, 0.75, 1.0,
    ];

    pub(crate) fn math() -> LightingMathInputs {
        LightingMathInputs {
            camera: L1_CAMERA,
            p_scale: 2.0,
            p_far: 100.0,
            p_slope: 0.5,
            p_aspect: 1.75,
            r_a: 3.0,
            r_b: 5.0,
            transform0: IDENTITY,
            transform1: L1_P1,
            v_one: [1.0; 4],
            s_depth: 1.0,
            a: 0.5,
            b: -0.5,
            c: 0.5,
            z0: 0.0,
            zh: 0.5,
            pa: -2.0,
            pb: 1.0,
            pc: 0.0,
            pd: 1.0,
            loff: 0.0,
        }
    }

    #[test]
    fn l1_inverse_and_transforms() {
        assert_eq!(
            camera_inverse(&L1_CAMERA, &[1.0; 4]).unwrap(),
            [
                [0.0, -0.25, 0.0, 0.0],
                [0.5, 0.0, 0.0, 0.0],
                [0.0, 0.0, 2.0, 0.0],
                [-1.0, 0.25, -6.0, 1.0]
            ]
        );
        let k = lighting_constants(&math()).unwrap();
        let expected = [
            [0.0, 0.125, 0.0, 0.0],
            [0.25, 0.0, 0.0, 0.0],
            [0.0, 0.0, 1.0, 0.0],
            [0.0, 0.375, -2.5, 1.0],
            [0.0, 0.25, 0.0, 0.0],
            [0.5, 0.0, 0.0, 0.0],
            [0.0, 0.0, 2.0, 0.0],
            [-0.875, 0.0, -41.0 / 8.0, 1.0],
        ];
        assert_eq!(k.fragment_rows, expected);
    }

    #[test]
    fn l2_lane_scale() {
        let inv = camera_inverse(&L1_CAMERA, &[2.0, 1.0, 1.0, 1.0]).unwrap();
        assert_eq!(
            inv,
            [
                [0.0, -0.25, 0.0, 0.0],
                [1.0, 0.0, 0.0, 0.0],
                [0.0, 0.0, 2.0, 0.0],
                [-2.0, 0.25, -6.0, 1.0]
            ]
        );
        let mut inputs = math();
        inputs.v_one = [2.0, 1.0, 1.0, 1.0];
        let scaled = lighting_constants(&inputs).unwrap();
        let expected = [
            [0.0, 0.125, 0.0, 0.0],
            [0.5, 0.0, 0.0, 0.0],
            [0.0, 0.0, 1.0, 0.0],
            [-0.5, 0.375, -2.5, 1.0],
        ];
        assert_eq!(&scaled.fragment_rows[..4], &expected);
    }

    #[test]
    fn l3_singular_camera() {
        let mut inputs = math();
        inputs.camera[8..12].fill(0.0);
        assert_eq!(
            lighting_constants(&inputs),
            Err(ProducerError::SingularCamera)
        );
    }

    #[test]
    fn l4_ignored_lane() {
        let mut inputs = math();
        inputs.camera[3] = f32::NAN;
        assert_eq!(lighting_constants(&inputs), lighting_constants(&math()));
        // Row 3, column 3 is not exempt.
        let mut inputs = math();
        inputs.camera[15] = f32::INFINITY;
        assert_eq!(
            lighting_constants(&inputs),
            Err(ProducerError::NonFinite("camera matrix"))
        );
        let mut inputs = math();
        inputs.transform1[7] = f32::NAN;
        assert_eq!(
            lighting_constants(&inputs),
            Err(ProducerError::NonFinite("parameter transform"))
        );
        let mut inputs = math();
        inputs.v_one[3] = f32::NAN;
        assert_eq!(
            lighting_constants(&inputs),
            Err(ProducerError::NonFinite("V_ONE"))
        );
    }

    #[test]
    fn l5_extent() {
        assert_eq!(
            lighting_constants(&math()).unwrap().vertex_row4,
            [87.5, 50.0, 0.0, 0.0]
        );
        assert_eq!(
            lighting_constants(&math()).unwrap().vertex_row4[2].to_bits(),
            0
        );
    }

    #[test]
    fn l6_depth_bound() {
        let rows = [
            (3.0, 5.0, 2.0, 1.0, 0.0, 0x3f40_0000),
            (1.5, 1.5, 0.5, 3.0, 0.0, 0x3f00_0000),
            (1.5, 1.5, 0.5, 3.0, 2.0, 0x3f40_0000),
        ];
        for (r_a, r_b, p_scale, s_depth, loff, upper) in rows {
            let inputs = LightingMathInputs {
                r_a,
                r_b,
                p_scale,
                s_depth,
                loff,
                ..math()
            };
            let k = lighting_constants(&inputs).unwrap();
            assert_eq!(k.bounds[1].to_bits(), upper);
            assert_eq!(k.bounds[0].to_bits(), 0);
        }
    }

    #[test]
    fn l7_inverted_bounds() {
        let inputs = LightingMathInputs { z0: 0.9, ..math() };
        assert_eq!(
            lighting_constants(&inputs),
            Err(ProducerError::InvertedBounds)
        );
    }

    #[test]
    fn l8_zero_distance() {
        let inputs = LightingMathInputs {
            r_a: 0.0,
            r_b: 0.0,
            ..math()
        };
        assert_eq!(
            lighting_constants(&inputs),
            Err(ProducerError::NonFinite("depth bound"))
        );
    }

    /// Property: random well-conditioned cameras and transforms meet the fragment-row tolerance
    /// against an independent reference (the general 4×4 inverse by Gauss–Jordan elimination).
    #[test]
    fn property_fragment_rows_within_tolerance() {
        fn invert(m: &Matrix) -> Matrix {
            let mut a = *m;
            let mut inv: Matrix =
                std::array::from_fn(|r| std::array::from_fn(|c| f64::from(u8::from(r == c))));
            for col in 0..4 {
                let pivot = (col..4)
                    .max_by(|x, y| a[*x][col].abs().total_cmp(&a[*y][col].abs()))
                    .unwrap();
                a.swap(col, pivot);
                inv.swap(col, pivot);
                let p = a[col][col];
                for c in 0..4 {
                    a[col][c] /= p;
                    inv[col][c] /= p;
                }
                for r in 0..4 {
                    if r != col {
                        let factor = a[r][col];
                        for c in 0..4 {
                            a[r][c] -= factor * a[col][c];
                            inv[r][c] -= factor * inv[col][c];
                        }
                    }
                }
            }
            inv
        }
        let mut rng = Rng::new(11);
        for _ in 0..2_000 {
            let mut inputs = math();
            for (index, value) in inputs.camera.iter_mut().enumerate() {
                *value = if index % 4 == 3 {
                    if index == 15 { 1.0 } else { 0.0 }
                } else {
                    rng.range_f32(-4.0, 4.0)
                };
            }
            for value in inputs
                .transform0
                .iter_mut()
                .chain(inputs.transform1.iter_mut())
            {
                *value = rng.range_f32(-8.0, 8.0);
            }
            inputs.a = rng.range_f32(0.1, 1.0);
            inputs.b = rng.range_f32(-1.0, -0.1);
            inputs.c = rng.range_f32(0.1, 1.0);
            let camera = matrix(&inputs.camera);
            let det3 = {
                let a = |r: usize, c: usize| camera[r][c];
                a(0, 0) * (a(1, 1) * a(2, 2) - a(1, 2) * a(2, 1))
                    - a(0, 1) * (a(1, 0) * a(2, 2) - a(1, 2) * a(2, 0))
                    + a(0, 2) * (a(1, 0) * a(2, 1) - a(1, 1) * a(2, 0))
            };
            if det3.abs() < 0.5 {
                continue;
            }
            let Ok(k) = lighting_constants(&inputs) else {
                panic!("well-conditioned camera declined")
            };
            let inv = invert(&camera);
            let (a, b, c) = (inputs.a as f64, inputs.b as f64, inputs.c as f64);
            for (index, (transform, e)) in [(inputs.transform0, c), (inputs.transform1, 0.0)]
                .into_iter()
                .enumerate()
            {
                let t: Matrix = [
                    [c, 0.0, 0.0, 0.0],
                    [0.0, b, 0.0, 0.0],
                    [0.0, 0.0, a, 0.0],
                    [e, a, a, 1.0],
                ];
                let reference = multiply(&multiply(&inv, &matrix(&transform)), &t);
                let maxabs = reference
                    .iter()
                    .flatten()
                    .fold(0.0f64, |m, v| m.max(v.abs()));
                let allowed = 2f64.powi(-18) * maxabs.max(1.0);
                for r in 0..4 {
                    for col in 0..4 {
                        let got = k.fragment_rows[4 * index + r][col] as f64;
                        assert!(
                            (got - reference[r][col]).abs() <= allowed,
                            "row {r} col {col}"
                        );
                    }
                }
            }
        }
    }

    /// Property: the upper bound is within 2^-22 of the exact value.
    #[test]
    fn property_upper_bound_within_tolerance() {
        let mut rng = Rng::new(12);
        for _ in 0..10_000 {
            let inputs = LightingMathInputs {
                r_a: rng.range_f32(0.1, 100.0),
                r_b: rng.range_f32(0.1, 100.0),
                p_scale: rng.range_f32(0.5, 4.0),
                s_depth: rng.range_f32(0.5, 4.0),
                loff: rng.range_f32(0.0, 10.0),
                pa: rng.range_f32(-1.0, -0.01),
                ..math()
            };
            let Ok(k) = lighting_constants(&inputs) else {
                continue;
            };
            let distance = (inputs.r_a as f64 + inputs.r_b as f64)
                / (inputs.p_scale as f64 * inputs.s_depth as f64);
            let exact = window_depth(
                distance + inputs.loff as f64,
                0.5,
                inputs.pa as f64,
                1.0,
                0.0,
                1.0,
            );
            assert!((k.bounds[1] as f64 - exact).abs() <= 2f64.powi(-22));
        }
    }

    // --- Producer-level tests -------------------------------------------------------------

    pub(crate) fn producer_inputs(bank_zero: bool) -> ProducerInputs {
        let mut inputs = acquire(&p::image(), p::TOC).unwrap();
        let f = f32::to_bits;
        let m = math();
        let r = &mut inputs.renderer;
        r.stencil_branch = u8::from(bank_zero);
        r.surface_branch = u32::from(bank_zero);
        r.explicit_width = 1280;
        r.explicit_height = 720;
        r.projection.matrix = m.camera.map(f);
        r.projection.scale = f(m.p_scale);
        r.projection.far = f(m.p_far);
        r.projection.slope = f(m.p_slope);
        r.projection.aspect = f(m.p_aspect);
        r.depth_distance_a.value = f(m.r_a);
        r.depth_distance_b.value = f(m.r_b);
        for (position, entry) in r.surface_table.iter_mut().enumerate() {
            entry.value = record(1280, 720, 8, 2, 0);
            entry.value.words[8] = 0x10_0000 * (position as u32 + 1);
            entry.value.words[4] = 5120;
        }
        // Lighting sources: a colour image on unit 0 and a full descriptor on unit 1.
        let unit0 = [
            0x900_0000,
            0x0001_9a21,
            0,
            0x8000_0000,
            0xaae4,
            0x0202_0000,
            (1280 << 16) | 720,
            5120,
        ];
        r.lighting_source_bank_zero.value = unit0;
        r.lighting_source_no_bank_zero.value = unit0;
        inputs.params.transform0.value = m.transform0.map(f);
        inputs.params.transform1.value = m.transform1.map(f);
        inputs.params.lighting_unit1.value = [
            0xa00_0000,
            0x0001_8521,
            0,
            0x8000_0000,
            0xaae4,
            0x0202_0000,
            (512 << 16) | 512,
            2048,
            0x1234,
            0,
            0,
            0,
        ];
        let vector = |value: [f32; 4]| {
            VectorConstant::Read(Tagged {
                address: 0x40_0000,
                value: value.map(f),
            })
        };
        inputs.v_one = vector([1.0; 4]);
        inputs.basis_a = vector([0.5, f32::NAN, 0.0, 0.0]);
        inputs.basis_b = vector([-0.5, 0.0, 0.0, 0.0]);
        inputs.basis_c = vector([0.5, 0.0, 0.0, 0.0]);
        inputs.s_depth.value = f(1.0);
        let s = &mut inputs.scalars;
        for (field, value) in [
            (&mut s.z0, 0.0),
            (&mut s.z1, 1.0),
            (&mut s.zh, 0.5),
            (&mut s.pa, -2.0),
            (&mut s.pb, 1.0),
            (&mut s.pc, 0.0),
            (&mut s.pd, 1.0),
            (&mut s.loff, 0.0),
            (&mut s.ftop, 1.0),
            (&mut s.t_lo, -1.0),
            (&mut s.t_hi, 1.0),
            (&mut s.t_far, 3.0),
            (&mut s.t_uvfar, 2.0),
            (&mut s.vp_hx, 0.5),
            (&mut s.vp_hy, -0.5),
            (&mut s.clip_min, 0.0),
            (&mut s.clip_max, 1.0),
        ] {
            field.value = f(value);
        }
        inputs.programs.lighting.vertex = Some(VertexProgramObject {
            registry_index: 0x12,
            address: 0x50_0000,
            instruction_count: 0,
            instruction_offset: 0,
            constant_count: 0,
            bytes: vec![0; 24],
        });
        inputs.programs.lighting.fragment = Some(FragmentProgramFile {
            registry_index: 0x44,
            address: 0x51_0000,
            microcode_length: 16,
            control_flags: 0,
            storage_word: 0,
            microcode_offset: 24,
            bytes: vec![0; 40],
        });
        inputs
    }

    pub(crate) fn resources_for(inputs: &ProducerInputs) -> Resources {
        let mut resources = Resources::default();
        for entry in &inputs.renderer.surface_table {
            resources.targets.insert(
                entry.address,
                TargetInfo {
                    physical: (1280, 720),
                    colour_format: 8,
                    colour_image: Some(ImageInfo {
                        id: entry.address,
                        width: 1280,
                        height: 720,
                        mip_count: 1,
                        samples: 1,
                        layers: 1,
                        two_dimensional: true,
                        depth: false,
                    }),
                    colour_initialised: true,
                    depth_format: 2,
                    depth_initialised: true,
                    stencil_initialised: true,
                },
            );
        }
        let sources = LightingSources::of(inputs);
        for unit in 0..2 {
            let compact = sources.compact(unit);
            resources.images.insert(
                (compact.address, compact.value),
                image_for(&compact.value, 100 + unit),
            );
        }
        resources
    }

    const BOTH: ProgramUsage = ProgramUsage {
        sampled_units: 3,
        discards: false,
    };

    #[test]
    fn l10_draw_count_without_bank_zero() {
        let inputs = producer_inputs(false);
        let out =
            run_lighting(&inputs, Some(BOTH), &known_state(), &resources_for(&inputs)).unwrap();
        assert_eq!(out.draws.len(), 1);
        let draw = &out.draws[0];
        assert_eq!(draw.target.role, 2);
        assert_eq!(
            draw.target.address,
            select(&inputs.renderer, 1, 1).unwrap().address
        );
        assert_eq!(draw.state.depth_bounds_enable, Some(true));
        assert_eq!(draw.state.depth_bounds, Some([0, 0x3f40_0000]));
        assert_eq!(draw.state.colour_masks, Some([8; 4]));
        assert_eq!(
            (
                draw.state.depth_test,
                draw.state.depth_write,
                draw.state.alpha_test,
                draw.state.blend_enable,
                draw.state.cull_enable,
                draw.state.srgb_output
            ),
            (
                Some(false),
                Some(false),
                Some(false),
                Some(false),
                Some(false),
                Some(false)
            )
        );
        assert_eq!((draw.viewport.width, draw.viewport.height), (1280.0, 720.0));
        assert_eq!(draw.fragment_rows.len(), 8);
        assert_eq!(draw.textures.len(), 2);
        assert_eq!(out.state_after.stencil_enable, Some(false));
        assert_eq!(out.state_after.two_sided_stencil, Some(false));
        assert_eq!(out.state_after.depth_bounds_enable, Some(false));
        assert_eq!(out.units[1].border, Some(0xffff_ffff));
        assert_eq!(out.units[1].compact.unwrap()[7], 0x1234);
        assert!(out.lighting_ran);
        assert_eq!(out.vertex_row4, [87.5f32.to_bits(), 50f32.to_bits(), 0, 0]);
    }

    #[test]
    fn l10_draw_count_with_bank_zero() {
        let inputs = producer_inputs(true);
        let mut state = known_state();
        state.stencil_enable = Some(true);
        state.front_stencil = Some(face(0));
        state.back_stencil = Some(face(0));
        let out = run_lighting(&inputs, Some(BOTH), &state, &resources_for(&inputs)).unwrap();
        assert_eq!(out.draws.len(), 2);
        let second = &out.draws[1];
        assert_eq!(
            second.target.address,
            select(&inputs.renderer, 1, 0).unwrap().address
        );
        assert_eq!(second.state.depth_bounds_enable, Some(false));
        for face in [
            second.state.front_stencil.unwrap(),
            second.state.back_stencil.unwrap(),
        ] {
            assert_eq!(
                (face.comparison, face.reference, face.read_mask),
                (Comparison::Less, 0x80, 0xff)
            );
            assert_eq!(face.write_mask, 0);
        }
        // The surface viewport of select(1, 0) carries no jitter here.
        assert_eq!((second.viewport.x, second.viewport.y), (0.0, 0.0));
        assert_eq!(out.state_after.stencil_enable, Some(false));
    }

    #[test]
    fn l9_stencil_precondition() {
        let inputs = producer_inputs(false);
        let resources = resources_for(&inputs);
        let mut state = known_state();
        state.stencil_enable = Some(true);
        state.front_stencil = Some(face(0));
        state.back_stencil = Some(face(0xff));
        assert_eq!(
            run_lighting(&inputs, Some(BOTH), &state, &resources),
            Err(ProducerError::StencilWrites)
        );
        state.back_stencil = Some(face(0));
        assert!(run_lighting(&inputs, Some(BOTH), &state, &resources).is_ok());
        state.stencil_enable = None;
        assert_eq!(
            run_lighting(&inputs, Some(BOTH), &state, &resources),
            Err(ProducerError::UnknownState("stencil enable"))
        );
    }

    #[test]
    fn l11_triangle_and_attributes() {
        let inputs = producer_inputs(false);
        let out =
            run_lighting(&inputs, Some(BOTH), &known_state(), &resources_for(&inputs)).unwrap();
        let vertices = out.draws[0].vertices.map(|v| v.map(f32::from_bits));
        assert_eq!(vertices[0], [-1.0, -1.0, -1.0, 1.0, 0.0, 1.0, 0.0, 1.0]);
        assert_eq!(vertices[1], [3.0, -1.0, -1.0, 1.0, 2.0, 1.0, 0.0, 1.0]);
        assert_eq!(vertices[2], [-1.0, 3.0, -1.0, 1.0, 0.0, -1.0, 0.0, 1.0]);
        assert_eq!(out.attribute0.map(f32::from_bits), [-1.0, 3.0, -1.0, 1.0]);
        assert_eq!(out.attribute8.map(f32::from_bits), [0.0, -1.0, 0.0, 1.0]);
    }

    #[test]
    fn declines() {
        let base = producer_inputs(false);
        let state = known_state();
        let resources = resources_for(&base);
        let run = |inputs: &ProducerInputs, usage| run_lighting(inputs, usage, &state, &resources);

        let mut inputs = base.clone();
        inputs.basis_a = VectorConstant::Misaligned(0x40_0008);
        assert_eq!(
            run(&inputs, Some(BOTH)),
            Err(ProducerError::Misaligned {
                what: "BASIS_A",
                address: 0x40_0008
            })
        );
        let mut inputs = base.clone();
        inputs.params.address += 4;
        assert!(matches!(
            run(&inputs, Some(BOTH)),
            Err(ProducerError::Misaligned { what: "PARAMS", .. })
        ));
        let mut inputs = base.clone();
        inputs.programs.lighting.fragment = None;
        assert_eq!(run(&inputs, Some(BOTH)), Err(ProducerError::ProgramAbsent));
        assert_eq!(run(&base, None), Err(ProducerError::ProgramUnparsed));
        assert_eq!(
            run(
                &base,
                Some(ProgramUsage {
                    sampled_units: 1,
                    discards: true
                })
            ),
            Err(ProducerError::Discards)
        );
        assert_eq!(
            run(
                &base,
                Some(ProgramUsage {
                    sampled_units: 4,
                    discards: false
                })
            ),
            Err(ProducerError::SampledUnit(2))
        );
        let mut inputs = base.clone();
        inputs
            .programs
            .lighting
            .fragment
            .as_mut()
            .unwrap()
            .control_flags = 8;
        assert_eq!(
            run(&inputs, Some(BOTH)),
            Err(ProducerError::UnsupportedOutputs)
        );
        let mut inputs = base.clone();
        inputs.scalars.t_far.value = f32::INFINITY.to_bits();
        assert_eq!(
            run(&inputs, Some(BOTH)),
            Err(ProducerError::NonFinite("triangle constant"))
        );

        // Missing and mismatched images.
        let mut missing = resources_for(&base);
        missing.images.clear();
        assert_eq!(
            run_lighting(&base, Some(BOTH), &state, &missing),
            Err(ProducerError::MissingImage { unit: 0 })
        );
        // Unit 1 only: sampling only unit 1 does not need unit 0's image.
        assert_eq!(
            run_lighting(
                &base,
                Some(ProgramUsage {
                    sampled_units: 2,
                    discards: false
                }),
                &state,
                &missing
            ),
            Err(ProducerError::MissingImage { unit: 1 })
        );
        let mut multisampled = resources_for(&base);
        for image in multisampled.images.values_mut() {
            image.samples = 4;
        }
        assert_eq!(
            run_lighting(&base, Some(BOTH), &state, &multisampled),
            Err(ProducerError::MismatchedImage { unit: 0 })
        );
        let mut depth_as_colour = resources_for(&base);
        for image in depth_as_colour.images.values_mut() {
            image.depth = true;
        }
        assert_eq!(
            run_lighting(&base, Some(BOTH), &state, &depth_as_colour),
            Err(ProducerError::MismatchedImage { unit: 0 })
        );

        // Targets.
        let target = select(&base.renderer, 1, 1).unwrap().address;
        let mut uninitialised = resources_for(&base);
        uninitialised
            .targets
            .get_mut(&target)
            .unwrap()
            .depth_initialised = false;
        assert_eq!(
            run_lighting(&base, Some(BOTH), &state, &uninitialised),
            Err(ProducerError::UninitialisedTarget { address: target })
        );
        let mut absent = resources_for(&base);
        absent.targets.remove(&target);
        assert_eq!(
            run_lighting(&base, Some(BOTH), &state, &absent),
            Err(ProducerError::MissingTarget { address: target })
        );
        let mut wrong = resources_for(&base);
        wrong.targets.get_mut(&target).unwrap().colour_format = 11;
        assert_eq!(
            run_lighting(&base, Some(BOTH), &state, &wrong),
            Err(ProducerError::MismatchedTarget { address: target })
        );

        // Unknown inherited state the draw depends on.
        let unknown = PipelineState {
            srgb_output: None,
            ..state
        };
        // sRGB output is set by the producer, so it is not needed.
        assert!(run_lighting(&base, Some(BOTH), &unknown, &resources).is_ok());
        let unknown = PipelineState {
            stencil_enable: None,
            ..state
        };
        assert_eq!(
            run_lighting(&base, Some(BOTH), &unknown, &resources),
            Err(ProducerError::UnknownState("stencil enable"))
        );
    }

    #[test]
    fn depth_sources_on_unit_zero() {
        let mut inputs = producer_inputs(false);
        let state = known_state();
        // Native depth through the explicit classification.
        inputs.renderer.lighting_source_no_bank_zero.value = [
            0x40_0000,
            0x0001_9021,
            0,
            0x8000_0000,
            0xaae4,
            0x0202_0000,
            (1280 << 16) | 720,
            5120,
        ];
        let mut resources = resources_for(&inputs);
        assert_eq!(
            run_lighting(&inputs, Some(BOTH), &state, &resources),
            Err(ProducerError::MissingImage { unit: 0 })
        );
        resources.depth = Some(ImageInfo {
            depth: true,
            ..image_for(&inputs.renderer.lighting_source_no_bank_zero.value, 7)
        });
        let out = run_lighting(&inputs, Some(BOTH), &state, &resources).unwrap();
        assert!(out.draws[0].textures[0].depth_source.is_some());
        // A packed alias: A8R8G8B8 at +0x3140 naming the downsample record's Z24S8 plane.
        let mut alias = inputs.clone();
        let mut owner = crate::surfaces::tests::record(1280, 720, 8, 2, 0);
        owner.words[16] = 0x40_0000;
        owner.words[17] = 5120;
        alias.renderer.downsample_record.value = owner;
        alias.renderer.lighting_source_no_bank_zero.value[1] = 0x0001_8521;
        let out = run_lighting(&alias, Some(BOTH), &state, &resources).unwrap();
        assert_eq!(
            out.draws[0].textures[0].depth_source.unwrap().kind,
            crate::surfaces::DepthPlaneKind::PackedAlias
        );
        // Float depth is unsupported.
        inputs.renderer.lighting_source_no_bank_zero.value[1] = 0x0001_9121;
        assert_eq!(
            run_lighting(&inputs, Some(BOTH), &state, &resources),
            Err(ProducerError::UnsupportedDepthSource { unit: 0 })
        );
    }
}
