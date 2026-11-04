//! `ProgramTranslator` over `mag-render-programs`.
//!
//! Translation produces portable HLSL. Native lowering (the `native-compiler` feature of
//! `mag-render-programs`) belongs to the adapter worker that owns the device, so the "native"
//! programs returned here carry the HLSL text and the vertex input locations.

use mag_render_behavior::producer::ProgramUsage;
use mag_render_behavior::program_compilation::{
    CompileError, ConstantReads, FragmentTranslation, NativeProgram, ProgramTranslator,
    ShaderControls, Variant, VertexTranslation,
};
use mag_render_programs::{
    FragmentProgram, ProgramError, ShadowTexture, VertexProgram, fragment_alpha_reference,
    fragment_may_discard, fragment_texture_units, fragment_with_texcoords, vertex,
    vertex_constant_reads,
};

fn decode_error(stage: &'static str, error: ProgramError) -> CompileError {
    match error {
        ProgramError::Unsupported => CompileError::Unsupported(stage),
        other => CompileError::Decode(format!("{stage}: {other:?}")),
    }
}

/// The producers' view of a fragment program: sampled units and whether it may discard.
pub fn program_usage(fragment_bytes: &[u8]) -> Result<ProgramUsage, CompileError> {
    Ok(ProgramUsage {
        sampled_units: fragment_texture_units(fragment_bytes)
            .map_err(|e| decode_error("fragment units", e))?,
        discards: fragment_may_discard(fragment_bytes)
            .map_err(|e| decode_error("fragment discard", e))?,
    })
}

/// The shader-controls alpha reference conversion.
pub fn alpha_reference(raw: u32, source_colour_format: u32) -> Result<u32, String> {
    fragment_alpha_reference(raw, source_colour_format).map_err(|e| format!("{e:?}"))
}

/// The translator used by `ProgramCompiler`.
#[derive(Clone, Copy, Debug, Default)]
pub struct RsxTranslator;

fn narrow(value: u32, what: &'static str) -> Result<u8, CompileError> {
    u8::try_from(value).map_err(|_| CompileError::Decode(format!("{what} out of range")))
}

impl ProgramTranslator for RsxTranslator {
    fn translate_fragment(
        &self,
        microcode: &[u8],
        shader_control: u32,
        controls: &ShaderControls,
        texcoord_controls: &[u32; 10],
        variant: Variant,
    ) -> Result<FragmentTranslation, CompileError> {
        match variant {
            Variant::Source => {}
            Variant::StoredDepth => {
                return Err(CompileError::Unsupported(
                    "stored-depth lowering needs the native compiler",
                ));
            }
            Variant::BaseColour => {
                return Err(CompileError::Unsupported("base-colour specialisation"));
            }
            Variant::Flat => return Err(CompileError::Unsupported("flat has a fixed fragment")),
        }
        let mut shadow = [ShadowTexture::default(); 16];
        let mut gamma = [0u8; 16];
        let mut expand = [0u8; 16];
        for unit in 0..16 {
            let (enabled, function, linear, unnormalized) = controls.shadow[unit];
            shadow[unit] = ShadowTexture {
                enabled: narrow(enabled, "shadow")?,
                function: narrow(function, "shadow")?,
                linear: narrow(linear, "shadow")?,
                unnormalized: narrow(unnormalized, "shadow")?,
            };
            gamma[unit] = narrow(controls.gamma[unit], "gamma")?;
            expand[unit] = narrow(controls.expand[unit], "expand")?;
        }
        let program = FragmentProgram {
            bytes: microcode,
            shader_control,
            cube_mask: controls.cube_mask as u32,
            volume_mask: controls.volume_mask as u32,
            shadow,
            gamma,
            expand,
            alpha_function: controls.alpha_function,
            srgb_output: controls.srgb_output,
            sample_grid: [controls.sample_grid.0, controls.sample_grid.1],
            unnormalized_mask: controls.unnormalized_mask as u32,
            texture_dimensions: controls.texture_dimensions.map(|(w, h)| [w, h]),
        };
        let hlsl = fragment_with_texcoords(program, *texcoord_controls)
            .map_err(|e| decode_error("fragment", e))?;
        let units =
            fragment_texture_units(microcode).map_err(|e| decode_error("fragment units", e))?;
        Ok(FragmentTranslation {
            program: hlsl.source.into_bytes(),
            constant_slots: hlsl.constant_slots as usize,
            constant_buffer_bytes: hlsl.constant_buffer_bytes as usize,
            textures: (0..16)
                .filter(|u| units & (1 << u) != 0)
                .map(|u| (u, u))
                .collect(),
            needs_stored_depth_arguments: false,
        })
    }

    fn translate_vertex(
        &self,
        instructions: &[[u32; 4]],
        load_slot: u32,
        vertex_texture_mask: u16,
        enabled_arrays: u16,
        known_current: u16,
    ) -> Result<VertexTranslation, CompileError> {
        let words: Vec<u32> = instructions.iter().flatten().copied().collect();
        let request = |known| VertexProgram {
            words: &words,
            start_slot: load_slot,
            vertex_textures: vertex_texture_mask as u32,
            enabled_arrays,
            known_current_attributes: known,
        };
        let hlsl = match vertex(request(known_current)) {
            Ok(hlsl) => hlsl,
            Err(error) => {
                // Name the first input that is neither an enabled array nor a known current
                // value, when that is why translation failed.
                if let Ok(probe) = vertex(request(0xffff)) {
                    let missing = probe.input_mask as u16 & !enabled_arrays & !known_current;
                    if missing != 0 {
                        return Err(CompileError::MissingCurrentAttribute(
                            missing.trailing_zeros(),
                        ));
                    }
                }
                return Err(decode_error("vertex", error));
            }
        };
        let reads = vertex_constant_reads(&hlsl).map_err(|e| decode_error("vertex reads", e))?;
        Ok(VertexTranslation {
            input_mask: hlsl.input_mask as u16,
            current_attribute_mask: hlsl.current_attribute_mask as u16,
            constant_buffer_bytes: hlsl.constant_buffer_bytes as usize,
            reads: if reads.indexed {
                ConstantReads::Indexed
            } else {
                ConstantReads::Rows(reads.rows.iter().map(|r| *r as u32).collect())
            },
            samples_textures: vertex_texture_mask != 0,
            program: hlsl.source.into_bytes(),
        })
    }

    fn lower_vertex(&self, translation: &VertexTranslation) -> Result<NativeProgram, CompileError> {
        let arrays = translation.input_mask & !translation.current_attribute_mask;
        Ok(NativeProgram {
            bytes: translation.program.clone(),
            vertex_input_locations: (0..16).filter(|i| arrays & (1 << i) != 0).collect(),
        })
    }

    fn lower_fragment(
        &self,
        translation: &FragmentTranslation,
        stored_depth: bool,
    ) -> Result<NativeProgram, CompileError> {
        if stored_depth {
            return Err(CompileError::Unsupported(
                "stored-depth lowering needs the native compiler",
            ));
        }
        Ok(NativeProgram {
            bytes: translation.program.clone(),
            vertex_input_locations: Vec::new(),
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use mag_render_behavior::program_compilation::{
        EffectiveValues, ProgramCompiler, TechniqueSource,
    };

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

    /// Half-exchanged fragment words.
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

    fn controls(enabled_arrays: u16) -> ShaderControls {
        mag_render_behavior::program_compilation::ShaderControls {
            enabled_arrays,
            ..mag_render_behavior::scene_plan::flat_controls((1, 1))
        }
    }

    fn directory(slot_values: &[u16]) -> Vec<u8> {
        let mut d = vec![0u8; 28];
        d[24..28].copy_from_slice(&28u32.to_be_bytes());
        d.extend((slot_values.len() as u16).to_be_bytes());
        for v in slot_values {
            d.extend(v.to_be_bytes());
        }
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
            directory: directory(&[1]),
        }
    }

    fn values() -> EffectiveValues {
        EffectiveValues {
            vertex_bank: vec![Some([0; 4]); 512],
            viewport_scale: [0x3f80_0000; 4],
            viewport_offset: [0; 4],
            current: [None; 16],
            fragment_writes: vec![(0, [2f32.to_bits(); 4])],
            alpha_window: [0; 4],
        }
    }

    #[test]
    fn compiles_a_technique_end_to_end() {
        let mut compiler = ProgramCompiler::new(RsxTranslator, true);
        let pair = compiler
            .compile(
                &technique(constant_fragment()),
                Variant::Source,
                &controls(1),
                &values(),
            )
            .unwrap();
        assert_eq!(pair.vertex_uniform.len(), 8224);
        // One inline row (written by slot 0) plus the alpha window.
        assert_eq!(pair.fragment_uniform.len(), 32);
        assert_eq!(&pair.fragment_uniform[..4], &2f32.to_bits().to_le_bytes());
        assert_eq!(pair.required_arrays(1), Ok(1));
        assert!(
            String::from_utf8(pair.vertex.bytes.clone())
                .unwrap()
                .contains("LOC0")
        );
        // Reuse: different values, same translation.
        let mut changed = values();
        changed.fragment_writes = vec![(0, [3f32.to_bits(); 4])];
        let again = compiler
            .compile(
                &technique(constant_fragment()),
                Variant::Source,
                &controls(1),
                &changed,
            )
            .unwrap();
        assert_eq!(&again.fragment_uniform[..4], &3f32.to_bits().to_le_bytes());
    }

    #[test]
    fn missing_current_attribute_is_named() {
        let translator = RsxTranslator;
        assert_eq!(
            translator.translate_vertex(&[mov()], 0, 0, 0, 0),
            Err(CompileError::MissingCurrentAttribute(0))
        );
        let current = translator.translate_vertex(&[mov()], 0, 0, 0, 1).unwrap();
        assert_eq!(current.current_attribute_mask, 1);
        assert!(
            translator
                .lower_vertex(&current)
                .unwrap()
                .vertex_input_locations
                .is_empty()
        );
    }

    #[test]
    fn usage_and_alpha_reference() {
        assert_eq!(
            program_usage(&texture_fragment()),
            Ok(ProgramUsage {
                sampled_units: 1,
                discards: false
            })
        );
        assert_eq!(
            program_usage(&constant_fragment()).unwrap().sampled_units,
            0
        );
        assert!(program_usage(&[0; 16]).is_err());
        assert!(alpha_reference(0x80, 8).is_ok());
        assert!(alpha_reference(0x80, 6).is_err());
        let translation = RsxTranslator
            .translate_fragment(
                &texture_fragment(),
                0x40,
                &controls(1),
                &[0; 10],
                Variant::Source,
            )
            .unwrap();
        assert_eq!(translation.textures, vec![(0, 0)]);
        assert!(matches!(
            RsxTranslator.translate_fragment(
                &texture_fragment(),
                0x40,
                &controls(1),
                &[0; 10],
                Variant::StoredDepth
            ),
            Err(CompileError::Unsupported(_))
        ));
    }
}
