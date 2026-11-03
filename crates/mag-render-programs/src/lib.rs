//! Owned NV40 program translation, independent of original renderer/SPU state.
//! The retained standalone ISA emitter has isolated symbols and is serialized
//! here: its legacy mutable branch base can never race the original renderer.
use std::{
    ffi::{CStr, c_char},
    sync::Mutex,
};
static COMPILER: Mutex<()> = Mutex::new(());
const OUTPUT_BYTES: usize = 1024 * 1024;
#[derive(Debug, PartialEq, Eq)]
pub enum ProgramError {
    Invalid,
    Unsupported,
    Capacity,
    Compiler,
}
pub struct VertexProgram<'a> {
    pub words: &'a [u32],
    pub start_slot: u32,
    pub vertex_textures: u32,
    pub enabled_arrays: u16,
    pub known_current_attributes: u16,
}
#[derive(Debug)]
pub struct HlslProgram {
    pub source: String,
    pub instructions: u32,
    pub constant_slots: u32,
    pub input_mask: u32,
    pub current_attribute_mask: u32,
    /// Exact emitted uniform block extent, including viewport/current or alpha tail.
    pub constant_buffer_bytes: u32,
}
#[repr(C)]
struct InputAnalysis {
    mask: u32,
    exact: i32,
}
unsafe extern "C" {
    fn mrf_rsx_vp_set_branch_base(start: u32);
    fn mrf_rsx_vp_analyze_inputs(
        bytes: *const u8,
        length: u32,
        analysis: *mut InputAnalysis,
    ) -> i32;
    fn mrf_rsx_vp_decompile_compact_ex(
        bytes: *const u8,
        length: u32,
        textures: u32,
        mask: u32,
        out: *mut c_char,
        capacity: u32,
    ) -> i32;
}
fn output(buffer: &[u8]) -> Result<String, ProgramError> {
    let text = CStr::from_bytes_until_nul(buffer)
        .map_err(|_| ProgramError::Capacity)?
        .to_str()
        .map_err(|_| ProgramError::Compiler)?;
    if text.contains("TODO:") {
        return Err(ProgramError::Unsupported);
    }
    Ok(text.to_owned())
}
pub fn vertex(program: VertexProgram<'_>) -> Result<HlslProgram, ProgramError> {
    if program.words.is_empty()
        || !program.words.len().is_multiple_of(4)
        || program.words.len() > 512 * 4
        || program.start_slot >= 512
        || program.vertex_textures & !15 != 0
    {
        return Err(ProgramError::Invalid);
    }
    let instructions = program
        .words
        .as_chunks::<4>()
        .0
        .iter()
        .position(|w| w[3] & 1 != 0)
        .ok_or(ProgramError::Invalid)?
        + 1;
    if program.start_slot as usize + instructions > 512 {
        return Err(ProgramError::Invalid);
    }
    for words in program.words[..instructions * 4].as_chunks::<4>().0 {
        let vector = (words[1] >> 22) & 31;
        if vector == 25 && program.vertex_textures & (1 << ((words[2] >> 8) & 3)) == 0 {
            return Err(ProgramError::Unsupported);
        }
    }
    let bytes: Vec<_> = program.words[..instructions * 4]
        .iter()
        .flat_map(|x| x.to_le_bytes())
        .collect();
    let mut buffer = vec![0; OUTPUT_BYTES];
    let _guard = COMPILER.lock().map_err(|_| ProgramError::Compiler)?;
    let mut analysis = InputAnalysis { mask: 0, exact: 0 };
    // SAFETY: complete bounded owned bytecode/output ranges; isolated compiler
    // functions run under one lock including branch-base setup and source emission.
    let count = unsafe {
        if mrf_rsx_vp_analyze_inputs(bytes.as_ptr(), bytes.len() as u32, &mut analysis) < 0 {
            return Err(ProgramError::Invalid);
        }
        mrf_rsx_vp_set_branch_base(program.start_slot);
        mrf_rsx_vp_decompile_compact_ex(
            bytes.as_ptr(),
            bytes.len() as u32,
            program.vertex_textures,
            analysis.mask,
            buffer.as_mut_ptr().cast(),
            buffer.len() as u32,
        )
    };
    if count < 0 {
        return Err(ProgramError::Compiler);
    }
    let current = analysis.mask & !u32::from(program.enabled_arrays);
    if current & !u32::from(program.known_current_attributes) != 0 {
        return Err(ProgramError::Invalid);
    }
    let mut source = output(&buffer)?;
    let start = source
        .find("struct VSInput {\n")
        .ok_or(ProgramError::Compiler)?;
    let end = start + source[start..].find("};\n").ok_or(ProgramError::Compiler)? + 3;
    let mut declaration = String::from("struct VSInput {\n");
    let enabled = analysis.mask & u32::from(program.enabled_arrays);
    for i in 0..16 {
        if enabled & (1 << i) != 0 {
            declaration.push_str(&format!("    [[vk::location({i})]] float4 a{i}:LOC{i};\n"));
        }
    }
    if enabled == 0 {
        declaration.push_str("    uint vertex_id:SV_VertexID;\n");
    }
    declaration.push_str("};\n");
    source.replace_range(start..end, &declaration);
    if current != 0 {
        source = source.replace(
            "    float4 vp_posoffset;\n",
            "    float4 vp_posoffset;\n    float4 vp_current[16];\n",
        );
        for i in 0..16 {
            if current & (1 << i) != 0 {
                source = source.replace(&format!("input.a{i};"), &format!("vp_current[{i}];"));
            }
        }
    }
    for unit in 0..4 {
        source = source.replace(
            &format!("register(t{})", 16 + unit),
            &format!("register(t{unit})"),
        );
    }
    Ok(HlslProgram {
        source,
        instructions: count as u32,
        constant_slots: 512,
        input_mask: analysis.mask,
        current_attribute_mask: current,
        constant_buffer_bytes: (514 + if current != 0 { 16 } else { 0 }) * 16,
    })
}
/// Conservative reads of the actual controlled emitter's VP constant bank.
/// Indexed addressing requires all rows; static reads need only listed rows.
#[derive(Debug, PartialEq, Eq)]
pub struct VertexConstantReads {
    pub rows: Vec<u16>,
    pub indexed: bool,
}
pub fn vertex_constant_reads(program: &HlslProgram) -> Result<VertexConstantReads, ProgramError> {
    let start = program
        .source
        .find("VSOutput main(")
        .ok_or(ProgramError::Invalid)?;
    let mut body = &program.source[start..];
    let mut rows = std::collections::BTreeSet::new();
    let mut indexed = false;
    while let Some(at) = body.find("vp_c[") {
        body = &body[at + 5..];
        if body.starts_with('(') {
            indexed = true;
        } else {
            let end = body.find(']').ok_or(ProgramError::Compiler)?;
            let row = body[..end]
                .parse::<u16>()
                .map_err(|_| ProgramError::Compiler)?;
            if row >= 512 {
                return Err(ProgramError::Compiler);
            }
            rows.insert(row);
        }
    }
    Ok(VertexConstantReads {
        rows: rows.into_iter().collect(),
        indexed,
    })
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct ShadowTexture {
    pub enabled: u8,
    pub function: u8,
    pub linear: u8,
    pub unnormalized: u8,
}
pub struct FragmentProgram<'a> {
    pub bytes: &'a [u8],
    pub shader_control: u32,
    pub cube_mask: u32,
    pub volume_mask: u32,
    pub shadow: [ShadowTexture; 16],
    pub gamma: [u8; 16],
    pub expand: [u8; 16],
    pub alpha_function: Option<u32>,
    pub srgb_output: bool,
    pub sample_grid: [u32; 2],
    pub unnormalized_mask: u32,
    pub texture_dimensions: [[u32; 2]; 16],
}
unsafe extern "C" {
    fn mrf_rsx_fp_texture_units(code: *const u8, size: u32) -> u32;
    fn mrf_rsx_fp_opcode_name(opcode: u32) -> *const c_char;
    fn mrf_rsx_fp_alpha_ref(raw: u32, surface_color_format: u32) -> f32;
    fn mrf_rsx_fp_decompile_buffered_shadow_color_expand_vol_ex(
        code: *const u8,
        size: u32,
        control: u32,
        cube: u32,
        volume: u32,
        shadow: *const ShadowTexture,
        gamma: *const u8,
        expand: *const u8,
        out: *mut c_char,
        capacity: u32,
        constants: *mut u32,
    ) -> i32;
    fn mrf_rsx_fp_apply_alpha_test_buffered(out: *mut c_char, capacity: u32, function: u32) -> i32;
    fn mrf_rsx_fp_apply_srgb_output(out: *mut c_char, capacity: u32) -> i32;
    fn mrf_rsx_fp_apply_wpos_scale(out: *mut c_char, capacity: u32, x: u32, y: u32) -> i32;
    fn mrf_rsx_fp_apply_unnorm_scale(
        out: *mut c_char,
        capacity: u32,
        mask: u32,
        dimensions: *const [u32; 2],
        cube: u32,
    ) -> i32;
}
fn texture_coordinate_controls(
    mut source: String,
    controls: [u32; 10],
) -> Result<String, ProgramError> {
    let mut initializers = String::new();
    for (unit, control) in controls.into_iter().enumerate() {
        if control & 0x10 != 0 {
            let declaration = format!("float4 tc{unit}:TEXCOORD{unit}");
            source = source.replace(&declaration, &format!("centroid {declaration}"));
        }
        let input = format!("input.tc{unit}");
        if control & 1 != 0 && source.contains(&input) {
            // Apply source z/w before swizzle, abs and negate. Perspective
            // correction has an exact resulting w=1, avoiding rcp(w)*w error.
            source = source.replace(
                &format!("({input} * _rsx_inv_w(input.position))"),
                &format!("float4({input}.xy * _rsx_inv_w(input.position), 0.0, 1.0)"),
            );
            source = source.replace(&input, &format!("mrf_tc{unit}"));
            initializers.push_str(&format!(
                "    float4 mrf_tc{unit} = float4({input}.xy, 0.0, mrf_clip_w(input.position));\n"
            ));
        }
    }
    if !initializers.is_empty() {
        let main = source
            .find("main(PSInput input)")
            .ok_or(ProgramError::Compiler)?;
        let at = main + source[main..].find('{').ok_or(ProgramError::Compiler)? + 1;
        source.insert_str(at, &format!("\n{initializers}"));
        source.insert_str(0, "#ifdef RSX_FRAGCOORD_W_IS_RCP\n#define mrf_clip_w(p) (1.0 / (p).w)\n#else\n#define mrf_clip_w(p) ((p).w)\n#endif\n");
    }
    Ok(source)
}
fn fragment_word(bytes: &[u8]) -> u32 {
    u32::from_be_bytes(bytes[..4].try_into().expect("checked complete word")).rotate_left(16)
}
struct FragmentStream {
    end: usize,
    instructions: u32,
    constants: u32,
}
// One shared source-stream admission for translation and usage reflection.
// Literal slots are never interpreted as instructions by either caller.
fn fragment_stream(
    bytes: &[u8],
    mut visit: impl FnMut([u32; 4]) -> Result<(), ProgramError>,
) -> Result<FragmentStream, ProgramError> {
    if bytes.is_empty() || bytes.len() > OUTPUT_BYTES || !bytes.len().is_multiple_of(16) {
        return Err(ProgramError::Invalid);
    }
    let mut offset = 0;
    let mut instructions = 0;
    let mut constants = 0;
    loop {
        let slot = bytes
            .get(offset..offset + 16)
            .ok_or(ProgramError::Invalid)?;
        let words = core::array::from_fn::<_, 4, _>(|i| fragment_word(&slot[i * 4..]));
        // Existing constant collector and branch decoder disagree about encoded
        // operand fields on flow instructions. Preserve translation's refusal.
        if words[2] & (1 << 31) != 0 {
            return Err(ProgramError::Unsupported);
        }
        visit(words)?;
        offset += 16;
        instructions += 1;
        if words[1..].iter().any(|word| word & 3 == 2) {
            constants += 1;
            offset += 16;
            if offset > bytes.len() {
                return Err(ProgramError::Invalid);
            }
        }
        if words[0] & 1 != 0 {
            break;
        }
    }
    if constants > 2048 {
        return Err(ProgramError::Capacity);
    }
    Ok(FragmentStream {
        end: offset,
        instructions,
        constants,
    })
}
/// Actual sampled-unit mask from the isolated retained ISA parser, after the
/// same bounded/terminated/flow admission used for translation. This reports
/// source reads only; it does not certify shader support, texture bindings or
/// native execution. State is required only for units the program can observe.
pub fn fragment_texture_units(bytes: &[u8]) -> Result<u16, ProgramError> {
    let stream = fragment_stream(bytes, |_| Ok(()))?;
    let _guard = COMPILER.lock().map_err(|_| ProgramError::Compiler)?;
    // SAFETY: the shared admission bounds the complete owned instruction/literal
    // span. The existing isolated parser only reads that span under our lock.
    let mask = unsafe { mrf_rsx_fp_texture_units(bytes.as_ptr(), stream.end as u32) };
    u16::try_from(mask).map_err(|_| ProgramError::Compiler)
}
/// Conservatively reflect source KIL instructions through the same admitted
/// instruction/literal stream as translation. Predicated KIL is may-discard;
/// constant literals and bytes after PROGRAM_END are not executable slots.
/// Unknown retained opcode metadata and unadmitted flow fail closed. This says
/// nothing about injected alpha/texture tests, shader validity, vertex coverage
/// or a native whole-attachment write; those remain separate obligations.
pub fn fragment_may_discard(bytes: &[u8]) -> Result<bool, ProgramError> {
    let mut discard = false;
    fragment_stream(bytes, |words| {
        // Same opcode field used by translation; the retained parser supplies
        // the operation identity instead of another authored opcode table.
        // SAFETY: this pure switch returns process-lived static C strings for
        // every u32. It reads/writes no shared compiler state.
        let name = unsafe { mrf_rsx_fp_opcode_name((words[0] >> 24) & 63) };
        if name.is_null() {
            return Err(ProgramError::Compiler);
        }
        // SAFETY: the isolated helper's result is a static NUL-terminated name.
        let name = unsafe { CStr::from_ptr(name) }.to_bytes();
        if name == b"?" {
            return Err(ProgramError::Unsupported);
        }
        discard |= name == b"KIL";
        Ok(())
    })?;
    Ok(discard)
}
/// Alpha reference conversion from the source NV4097 surface-color code
/// (low five bits only), not the native attachment-format enum. The isolated
/// retained helper is pure and preserves float/half semantics without an SDK
/// state read. Raw upper format bits and unknown format codes are rejected.
pub fn fragment_alpha_reference(raw: u32, source_color_format: u32) -> Result<u32, ProgramError> {
    if !matches!(source_color_format, 1..=5 | 8..=16) {
        return Err(ProgramError::Invalid);
    }
    // SAFETY: pure scalar conversion, no pointers, mutable globals or callbacks.
    Ok(unsafe { mrf_rsx_fp_alpha_ref(raw, source_color_format) }.to_bits())
}
pub fn fragment(program: FragmentProgram<'_>) -> Result<HlslProgram, ProgramError> {
    fragment_with_texcoords(program, [0; 10])
}
/// Source TEX_COORD_CONTROL: bit0 supplies 2-D z/w, bit4 selects centroid.
/// Remaining bits are unsupported. The source caller supplies all ten values.
pub fn fragment_with_texcoords(
    program: FragmentProgram<'_>,
    controls: [u32; 10],
) -> Result<HlslProgram, ProgramError> {
    if controls.iter().any(|control| control & !0x11 != 0) {
        return Err(ProgramError::Unsupported);
    }
    if program.bytes.is_empty()
        || program.bytes.len() > 1024 * 1024
        || !program.bytes.len().is_multiple_of(16)
        || (program.cube_mask | program.volume_mask | program.unnormalized_mask) & !0xffff != 0
        || program.cube_mask & program.volume_mask != 0
        || program.gamma.iter().any(|x| *x > 15)
        || program.expand.iter().any(|x| *x > 31)
        || program
            .alpha_function
            .is_some_and(|f| !(0x200..=0x207).contains(&f))
        || program.sample_grid.iter().any(|x| *x != 1 && *x != 2)
        || program
            .shadow
            .iter()
            .any(|s| s.enabled > 2 || s.function > 7 || s.linear > 1 || s.unnormalized > 1)
    {
        return Err(ProgramError::Invalid);
    }
    let stream = fragment_stream(program.bytes, |words| {
        let opcode = (words[0] >> 24) & 63;
        let unit = ((words[0] >> 17) & 15) as usize;
        if matches!(opcode, 0x19 | 0x33 | 0x34)
            && (program.gamma[unit] != 0 || program.expand[unit] != 0 || words[0] & (1 << 21) != 0)
        {
            return Err(ProgramError::Unsupported);
        }
        if program.unnormalized_mask & (1 << unit) != 0
            && matches!(opcode, 0x17 | 0x18 | 0x19 | 0x2f | 0x31 | 0x33 | 0x34)
        {
            if !matches!(opcode, 0x17 | 0x18)
                || (program.cube_mask | program.volume_mask) & (1 << unit) != 0
            {
                return Err(ProgramError::Unsupported);
            }
            if program.texture_dimensions[unit].contains(&0) {
                return Err(ProgramError::Invalid);
            }
        }
        Ok(())
    })?;
    let (offset, instructions, constants) = (stream.end, stream.instructions, stream.constants);
    let mut buffer = vec![0; OUTPUT_BYTES];
    let mut actual = 0;
    let _guard = COMPILER.lock().map_err(|_| ProgramError::Compiler)?;
    // SAFETY: fixed-size metadata arrays and bounded owned output/code slices;
    // the isolated emitter and all transformations remain serialized together.
    let count = unsafe {
        mrf_rsx_fp_decompile_buffered_shadow_color_expand_vol_ex(
            program.bytes.as_ptr(),
            offset as u32,
            program.shader_control,
            program.cube_mask,
            program.volume_mask,
            program.shadow.as_ptr(),
            program.gamma.as_ptr(),
            program.expand.as_ptr(),
            buffer.as_mut_ptr().cast(),
            buffer.len() as u32,
            &mut actual,
        )
    };
    if count < 0 || actual != constants {
        return Err(ProgramError::Compiler);
    }
    // Inspect unsupported emitter paths before any shader can be admitted.
    let source = texture_coordinate_controls(output(&buffer)?, controls)?;
    if source.len() >= buffer.len() {
        return Err(ProgramError::Capacity);
    }
    buffer[..source.len()].copy_from_slice(source.as_bytes());
    buffer[source.len()] = 0;
    // SAFETY: every helper receives this same bounded, NUL-terminated output;
    // inputs have been validated against each helper's supported domain above.
    unsafe {
        if program.srgb_output
            && mrf_rsx_fp_apply_srgb_output(buffer.as_mut_ptr().cast(), buffer.len() as u32) < 0
        {
            return Err(ProgramError::Compiler);
        }
        if let Some(function) = program.alpha_function
            && mrf_rsx_fp_apply_alpha_test_buffered(
                buffer.as_mut_ptr().cast(),
                buffer.len() as u32,
                function,
            ) < 0
        {
            return Err(ProgramError::Compiler);
        }
        if mrf_rsx_fp_apply_wpos_scale(
            buffer.as_mut_ptr().cast(),
            buffer.len() as u32,
            program.sample_grid[0],
            program.sample_grid[1],
        ) < 0
        {
            return Err(ProgramError::Compiler);
        }
        if program.unnormalized_mask != 0
            && mrf_rsx_fp_apply_unnorm_scale(
                buffer.as_mut_ptr().cast(),
                buffer.len() as u32,
                program.unnormalized_mask,
                program.texture_dimensions.as_ptr(),
                program.cube_mask,
            ) < 0
        {
            return Err(ProgramError::Compiler);
        }
    }
    let mut source = output(&buffer)?;
    // These are exact declarations emitted by the isolated compiler above, not
    // arbitrary caller shader text. All texture indices are ISA immediates.
    let texture_decls = (0..16)
        .map(|i| format!("Texture2D rsx_tex{i} : register(t{i});\n"))
        .collect::<String>();
    let sampler_decls = (0..16)
        .map(|i| format!("SamplerState rsx_samp{i} : register(s{i});\n"))
        .collect::<String>();
    source = source
        .replace("Texture2D    rsx_tex[16] : register(t0);\n", &texture_decls)
        .replace(
            "SamplerState rsx_samp[16] : register(s0);\n",
            &sampler_decls,
        );
    for i in 0..16 {
        source = source
            .replace(&format!("rsx_tex[{i}]"), &format!("rsx_tex{i}"))
            .replace(&format!("rsx_samp[{i}]"), &format!("rsx_samp{i}"));
    }
    if source.contains("rsx_tex[") || source.contains("rsx_samp[") {
        return Err(ProgramError::Unsupported);
    }
    Ok(HlslProgram {
        source,
        instructions,
        constant_slots: actual,
        input_mask: 0,
        current_attribute_mask: 0,
        constant_buffer_bytes: (actual.max(1) + 1) * 16,
    })
}

#[cfg(feature = "native-compiler")]
pub mod native_compile;

mod position_poc;
pub use position_poc::vertex_position_only_poc;
