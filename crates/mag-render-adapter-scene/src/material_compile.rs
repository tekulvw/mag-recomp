//! Owned source programs and explicit final draw values to portable targets.
//! No original worker output, RSX command construction or mutable SDK state.
//! Callers must resolve the full material writer order before supplying values.
use crate::{
    cohort_geometry::DiagnosticMeshDraw,
    material_constants::{ConstantBudget, ConstantError, ConstantRowWrite, FragmentConstantLayout},
    material_programs::TechniquePrograms,
    material_values::{KnownVertexBank, ValueError},
    materials::OwnedMaterialSources,
    vertex_inputs::{self, NativeVertexInputs, VertexInputError},
};
use mag_render_programs::{
    self as compiler,
    native_compile::{self, NativeProgram, Stage},
};

pub use compiler::ShadowTexture;
pub use compiler::native_compile::{CacheLimits, CacheStats};
#[path = "material_fragment_cache.rs"]
mod fragment_cache;
#[path = "material_plan_reuse.rs"]
mod plan_reuse;
#[path = "material_unlit_color.rs"]
mod unlit_color;
#[path = "material_vertex_cache.rs"]
mod vertex_cache;
pub use fragment_cache::FragmentDecodeStats;
use fragment_cache::{DecodedFragment, FragmentDecodeCache};
pub use plan_reuse::{PlanReuseStats, ReusedTechnique};
use std::sync::Arc;
use vertex_cache::{DecodedVertex, VertexDecodeCache, VertexPolicy};
pub use vertex_cache::{NativeCompilerCache, VertexDecodeStats};
/// No Default: every control must come from the effective source material/pass.
#[derive(Clone)]
pub struct EffectiveShaderControls {
    pub vertex_texture_mask: u32,
    pub enabled_arrays: u16,
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
pub struct EffectiveProgramValues<'a> {
    /// Final bank after the actual ordered static/dynamic/builtin writers.
    /// This API cannot infer prior rows from only this material's static writes.
    pub vertex_constants: &'a KnownVertexBank,
    pub viewport_scale: [u32; 4],
    pub viewport_offset: [u32; 4],
    pub current_attributes: &'a [Option<[u32; 4]>; 16],
    /// Actual ordered fragment assignments; later overlapping writes win.
    pub fragment_writes: &'a [ConstantRowWrite],
    pub alpha_window: [u32; 4],
}
#[derive(Debug)]
pub enum CompileError {
    SourceExtent,
    UnqualifiedTexcoord { unit: usize, control: u32 },
    Constant(ConstantError),
    VertexValues(ValueError),
    Decode(compiler::ProgramError),
    Native(String),
    UniformLayout,
    MissingCurrentAttribute(u32),
    Allocation,
    VertexInputs(VertexInputError),
    ArrayMaskMismatch { compiled: u16, source: u16 },
}
pub struct CompiledMaterialPrograms {
    pub vertex: Arc<NativeProgram>,
    pub fragment: Arc<NativeProgram>,
    pub vertex_uniform: Vec<u8>,
    pub fragment_uniform: Vec<u8>,
    /// Preserve these source declarations for final interface acceptance.
    /// Compilation alone cannot claim original output-mask behavior parity.
    pub declared_vertex_input_mask: u32,
    pub declared_vertex_output_mask: u32,
    pub actual_vertex_input_mask: u32,
    pub enabled_vertex_arrays: u16,
    pub source_transform_timeout: u32,
    pub sanitized_fragment_words: usize,
    /// Explicit POC output pruning; the actual position arithmetic is retained.
    pub approximated_vertex_outputs: bool,
}
/// Owned material programs/uniforms paired with the exact shader-read geometry.
/// This is not final raster/texture admission or proof of item/family coverage.
pub struct CompiledMaterialDraw {
    pub programs: CompiledMaterialPrograms,
    pub vertices: NativeVertexInputs,
}
impl CompiledMaterialPrograms {
    pub fn with_vertex_inputs(
        self,
        draw: &DiagnosticMeshDraw,
        byte_budget: usize,
    ) -> Result<CompiledMaterialDraw, CompileError> {
        self.with_vertex_inputs_policy(draw, byte_budget, vertex_inputs::VertexInputPolicy::Exact)
    }
    fn required_vertex_arrays(
        &self,
        draw: &DiagnosticMeshDraw,
        policy: vertex_inputs::VertexInputPolicy,
    ) -> Result<u16, CompileError> {
        let source = vertex_inputs::effective_array_mask_with_policy(draw, policy)
            .map_err(CompileError::VertexInputs)?;
        self.required_vertex_arrays_for(source)
    }
    /// `required_vertex_arrays` for an already computed source array mask.
    fn required_vertex_arrays_for(&self, source: u16) -> Result<u16, CompileError> {
        if source != self.enabled_vertex_arrays {
            return Err(CompileError::ArrayMaskMismatch {
                compiled: self.enabled_vertex_arrays,
                source,
            });
        }
        let mut required = 0u16;
        for &location in &self.vertex.vertex_locations {
            if location >= 16 {
                return Err(CompileError::UniformLayout);
            }
            required |= 1 << location;
        }
        Ok(required)
    }
    pub fn with_vertex_inputs_policy(
        self,
        draw: &DiagnosticMeshDraw,
        byte_budget: usize,
        policy: vertex_inputs::VertexInputPolicy,
    ) -> Result<CompiledMaterialDraw, CompileError> {
        let required = self.required_vertex_arrays(draw, policy)?;
        let vertices =
            vertex_inputs::prepare_vertex_inputs_with_policy(draw, required, byte_budget, policy)
                .map_err(CompileError::VertexInputs)?;
        Ok(CompiledMaterialDraw {
            programs: self,
            vertices,
        })
    }
    /// Identical validation and result to `with_vertex_inputs_policy`, except
    /// the interleaved CPU vertex bytes are left empty and returned as rows to
    /// be written later (None when the vertices are GPU-produced).
    pub fn with_vertex_inputs_deferred<'a>(
        self,
        draw: &'a DiagnosticMeshDraw,
        byte_budget: usize,
        policy: vertex_inputs::VertexInputPolicy,
    ) -> Result<(CompiledMaterialDraw, Option<vertex_inputs::VertexRows<'a>>), CompileError> {
        let source = vertex_inputs::effective_array_mask_with_policy(draw, policy)
            .map_err(CompileError::VertexInputs)?;
        self.with_vertex_inputs_deferred_for_mask(draw, byte_budget, policy, source)
    }
    /// `with_vertex_inputs_deferred` for a caller that already holds
    /// `effective_array_mask_with_policy(draw, policy)` as `source_mask`
    /// (it must be exactly that value); the mask is not recomputed.
    pub fn with_vertex_inputs_deferred_for_mask<'a>(
        self,
        draw: &'a DiagnosticMeshDraw,
        byte_budget: usize,
        policy: vertex_inputs::VertexInputPolicy,
        source_mask: u16,
    ) -> Result<(CompiledMaterialDraw, Option<vertex_inputs::VertexRows<'a>>), CompileError> {
        let required = self.required_vertex_arrays_for(source_mask)?;
        let plan = vertex_inputs::plan_vertex_inputs(draw, required, byte_budget, policy)
            .map_err(CompileError::VertexInputs)?;
        let (vertices, rows) = plan.split(Vec::new());
        Ok((
            CompiledMaterialDraw {
                programs: self,
                vertices,
            },
            rows,
        ))
    }
}
/// Bounded private failure evidence; it never changes source-value knowledge.
#[derive(Debug)]
struct VertexConstantFailure {
    missing_rows: Vec<u16>,
    missing_total: usize,
    indexed: bool,
    source_hash: String,
    read_lines: Vec<String>,
    read_line_total: usize,
}
fn vertex_constant_failure(
    source: &TechniquePrograms,
    vertex: &compiler::HlslProgram,
    reads: &compiler::VertexConstantReads,
    bank: &KnownVertexBank,
) -> VertexConstantFailure {
    use sha2::{Digest, Sha256};
    let mut hash = Sha256::new();
    for instruction in &source.vertex.instructions {
        for word in instruction {
            hash.update(word.to_le_bytes());
        }
    }
    let source_hash = format!("{:x}", hash.finalize());
    let missing = (0..512)
        .filter(|&row| {
            (reads.indexed || reads.rows.contains(&(row as u16))) && bank.row(row).is_none()
        })
        .collect::<Vec<_>>();
    let body = vertex
        .source
        .find("VSOutput main(")
        .map_or("", |at| &vertex.source[at..]);
    let mut read_lines = Vec::new();
    let mut read_line_total = 0;
    for line in body.lines().filter(|line| line.contains("vp_c[")) {
        read_line_total += 1;
        if read_lines.len() < 16 {
            read_lines.push(line.chars().take(256).collect());
        }
    }
    VertexConstantFailure {
        missing_total: missing.len(),
        missing_rows: missing.into_iter().take(32).map(|row| row as u16).collect(),
        indexed: reads.indexed,
        source_hash,
        read_lines,
        read_line_total,
    }
}
fn report_vertex_constant_failure(
    source: &TechniquePrograms,
    vertex: &compiler::HlslProgram,
    reads: &compiler::VertexConstantReads,
    bank: &KnownVertexBank,
    enabled_arrays: u16,
    known_current: u16,
) {
    let context = vertex_constant_failure(source, vertex, reads, bank);
    eprintln!(
        "[mag main VP constant failure] vp_words_le_sha256={} start_slot={} owned_instructions={} compiled_instructions={} emitted_instructions={} truncated_instructions={} enabled_arrays={enabled_arrays:04x} known_current={known_current:04x} used_current={:04x} indexed={} read_rows={:?} read_total={} missing_rows={:?} missing_total={} read_lines={} read_line_total={}",
        context.source_hash,
        source.vertex.load_slot,
        source.vertex.instructions.len(),
        vertex.instructions,
        source.vertex.instructions.len().min(512),
        source.vertex.instructions.len().saturating_sub(512),
        vertex.current_attribute_mask,
        context.indexed,
        &reads.rows[..reads.rows.len().min(32)],
        reads.rows.len(),
        context.missing_rows,
        context.missing_total,
        context.read_lines.len(),
        context.read_line_total
    );
    // Private live stderr only: complete owned words, at most the admitted512
    // instructions. These are static source instructions, not executed witnesses.
    for (index, words) in source.vertex.instructions.iter().take(512).enumerate() {
        eprintln!(
            "[mag main VP source instruction] index={index} slot={} words={:08x?}",
            usize::from(source.vertex.load_slot) + index,
            words
        );
    }
    for (index, line) in context.read_lines.iter().enumerate() {
        eprintln!("[mag main VP emitted read] index={index} {line}");
    }
}
fn report_vertex_decode_failure(
    source: &TechniquePrograms,
    controls: &EffectiveShaderControls,
    known: u16,
    error: &compiler::ProgramError,
) {
    use sha2::{Digest, Sha256};
    use std::sync::atomic::{AtomicUsize, Ordering};
    static REPORTED: AtomicUsize = AtomicUsize::new(0);
    if REPORTED
        .try_update(Ordering::Relaxed, Ordering::Relaxed, |n| {
            if n < 4 { Some(n + 1) } else { None }
        })
        .is_err()
    {
        return;
    }
    let mut hash = Sha256::new();
    for words in &source.vertex.instructions {
        for word in words {
            hash.update(word.to_le_bytes());
        }
    }
    // The declared header mask is not an analyzed read mask. Keep that
    // distinction explicit: unknown declared inputs need not be position-live.
    let unknown_declared =
        source.vertex.input_mask & !(u32::from(controls.enabled_arrays) | u32::from(known));
    eprintln!(
        "[mag main VP decode failure] error={error:?} enabled_arrays={:04x} known_current={known:04x} source_header_input_mask={:08x} unknown_declared_inputs={unknown_declared:08x} load_slot={} owned_instructions={} vp_words_le_sha256={:x}",
        controls.enabled_arrays,
        source.vertex.input_mask,
        source.vertex.load_slot,
        source.vertex.instructions.len(),
        hash.finalize()
    );
}
fn append_row(bytes: &mut Vec<u8>, row: [u32; 4]) {
    append_rows(bytes, &[row]);
}
/// Little-endian words of `rows`, identical to `append_row` per row, written
/// with one length change instead of a capacity check per word.
pub(crate) fn append_rows(bytes: &mut Vec<u8>, rows: &[[u32; 4]]) {
    let start = bytes.len();
    bytes.resize(start + rows.len() * 16, 0);
    for (out, row) in bytes[start..].as_chunks_mut::<16>().0.iter_mut().zip(rows) {
        for (lane, word) in out.as_chunks_mut::<4>().0.iter_mut().zip(row) {
            *lane = word.to_le_bytes();
        }
    }
}
pub fn compile_material_programs(
    source: &OwnedMaterialSources,
    controls: &EffectiveShaderControls,
    values: EffectiveProgramValues<'_>,
) -> Result<CompiledMaterialPrograms, CompileError> {
    compile_technique_programs(
        &source.programs,
        &source.fragment_template,
        controls,
        values,
    )
}
pub fn compile_technique_programs(
    source: &TechniquePrograms,
    fragment_bytes: &[u8],
    controls: &EffectiveShaderControls,
    values: EffectiveProgramValues<'_>,
) -> Result<CompiledMaterialPrograms, CompileError> {
    compile_technique_inner(
        source,
        fragment_bytes,
        controls,
        values,
        false,
        &mut |program, stage| native_compile::lower(program, stage).map(Arc::new),
    )
}
/// Reuses immutable native shader outputs across draws/frames; dynamic values
/// are evaluated and owned separately for every result. The owner bounds cache
/// retention and clears it at its own source/compiler lifetime boundaries.
pub fn compile_material_programs_cached(
    cache: &mut NativeCompilerCache,
    source: &OwnedMaterialSources,
    controls: &EffectiveShaderControls,
    values: EffectiveProgramValues<'_>,
) -> Result<CompiledMaterialPrograms, CompileError> {
    compile_technique_programs_cached(
        cache,
        &source.programs,
        &source.fragment_template,
        controls,
        values,
    )
}
pub fn compile_technique_programs_cached(
    cache: &mut NativeCompilerCache,
    source: &TechniquePrograms,
    fragment_bytes: &[u8],
    controls: &EffectiveShaderControls,
    values: EffectiveProgramValues<'_>,
) -> Result<CompiledMaterialPrograms, CompileError> {
    compile_technique_inner_cached(
        source,
        fragment_bytes,
        controls,
        values,
        FragmentMode::Source,
        Lowering::Standard,
        Some((&mut cache.vertex, &mut cache.fragment)),
        &mut |program, stage| cache.native.compile(program, stage),
    )
}
/// Required Metal stored-depth variant for a source-qualified readonly-depth
/// producer. Only the fragment lowering changes; source decoding, writer order,
/// reflected VP requirements and uniform packing are identical to ordinary
/// materials. The caller must separately prove depth writes are disabled and
/// bind the typed auxiliary arguments through the native bounds contract.
/// No ungated HLSL/SPIR-V fallback is produced for this fragment program.
pub fn compile_technique_programs_metal_stored_depth(
    cache: &mut NativeCompilerCache,
    source: &TechniquePrograms,
    fragment_bytes: &[u8],
    controls: &EffectiveShaderControls,
    values: EffectiveProgramValues<'_>,
) -> Result<CompiledMaterialPrograms, CompileError> {
    compile_technique_inner_cached(
        source,
        fragment_bytes,
        controls,
        values,
        FragmentMode::Source,
        Lowering::StoredDepthFragment,
        Some((&mut cache.vertex, &mut cache.fragment)),
        &mut |program, stage| match stage {
            Stage::Vertex => cache.native.compile(program, stage),
            Stage::Fragment => cache.native.compile_metal_stored_depth(program),
        },
    )
}
/// Explicit reduced-fidelity POC only: retain the real vertex position chain
/// and its source values, prune only proven-unused straight-line VP outputs,
/// and replace the fragment with authored flat color. Unsupported VP forms
/// retain the complete native VP and all its required source values.
/// This does not claim source material/texture/alpha/depth-export appearance.
/// Runtime opt-in and approximation accounting belong to the Main planner.
pub fn compile_material_programs_poc_unlit_cached(
    cache: &mut NativeCompilerCache,
    source: &OwnedMaterialSources,
    controls: &EffectiveShaderControls,
    values: EffectiveProgramValues<'_>,
) -> Result<CompiledMaterialPrograms, CompileError> {
    compile_technique_inner_cached(
        &source.programs,
        &source.fragment_template,
        controls,
        values,
        FragmentMode::Flat,
        Lowering::Standard,
        Some((&mut cache.vertex, &mut cache.fragment)),
        &mut |program, stage| cache.native.compile(program, stage),
    )
}
/// Explicit POC unlit base color: source sample/alpha discard/HDR output are
/// retained; only a proven base-RGB times deferred-lighting operation is omitted.
/// The full original VP and all actual values remain required.
pub fn compile_material_programs_poc_base_color_cached(
    cache: &mut NativeCompilerCache,
    source: &OwnedMaterialSources,
    controls: &EffectiveShaderControls,
    values: EffectiveProgramValues<'_>,
) -> Result<CompiledMaterialPrograms, CompileError> {
    compile_technique_inner_cached(
        &source.programs,
        &source.fragment_template,
        controls,
        values,
        FragmentMode::UnlitColor,
        Lowering::Standard,
        Some((&mut cache.vertex, &mut cache.fragment)),
        &mut |program, stage| cache.native.compile(program, stage),
    )
}
/// Which compile entry point a reused technique stands for.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ProgramVariant {
    /// `compile_material_programs_cached` / `compile_technique_programs_cached`.
    Source,
    /// `compile_material_programs_poc_unlit_cached`.
    PocUnlit,
    /// `compile_material_programs_poc_base_color_cached`.
    PocBaseColor,
    /// `compile_technique_programs_metal_stored_depth`.
    MetalStoredDepth,
}
impl ProgramVariant {
    fn parts(self) -> (FragmentMode, Lowering) {
        match self {
            Self::Source => (FragmentMode::Source, Lowering::Standard),
            Self::PocUnlit => (FragmentMode::Flat, Lowering::Standard),
            Self::PocBaseColor => (FragmentMode::UnlitColor, Lowering::Standard),
            Self::MetalStoredDepth => (FragmentMode::Source, Lowering::StoredDepthFragment),
        }
    }
}
/// The named compile entry point for `source`/`fragment_bytes`. With a
/// technique from `NativeCompilerCache::reuse_technique` (which must be for
/// these same sources) the value-independent preparation is reused across
/// plans; the result, including every error, is identical either way.
pub fn compile_variant_cached(
    cache: &mut NativeCompilerCache,
    technique: Option<&ReusedTechnique<'_>>,
    source: &TechniquePrograms,
    fragment_bytes: &[u8],
    variant: ProgramVariant,
    controls: &EffectiveShaderControls,
    values: EffectiveProgramValues<'_>,
) -> Result<CompiledMaterialPrograms, CompileError> {
    compile_variant_inner(
        cache,
        technique,
        source,
        fragment_bytes,
        variant,
        controls,
        values,
        false,
    )
    .map(|(programs, _)| programs)
}
/// `compile_variant_cached` with the vertex uniform left out of the result's
/// `vertex_uniform` (empty) and returned by parts, for a caller that writes
/// it straight into its destination. Every check and error is unchanged.
pub fn compile_variant_cached_deferred_uniform<'v>(
    cache: &mut NativeCompilerCache,
    technique: Option<&ReusedTechnique<'_>>,
    source: &TechniquePrograms,
    fragment_bytes: &[u8],
    variant: ProgramVariant,
    controls: &EffectiveShaderControls,
    values: EffectiveProgramValues<'v>,
) -> Result<(CompiledMaterialPrograms, VertexUniform<'v>), CompileError> {
    let (programs, parts) = compile_variant_inner(
        cache,
        technique,
        source,
        fragment_bytes,
        variant,
        controls,
        values,
        true,
    )?;
    Ok((programs, parts.ok_or(CompileError::UniformLayout)?))
}
#[allow(clippy::too_many_arguments)]
fn compile_variant_inner<'v>(
    cache: &mut NativeCompilerCache,
    technique: Option<&ReusedTechnique<'_>>,
    source: &TechniquePrograms,
    fragment_bytes: &[u8],
    variant: ProgramVariant,
    controls: &EffectiveShaderControls,
    values: EffectiveProgramValues<'v>,
    defer: bool,
) -> Result<(CompiledMaterialPrograms, Option<VertexUniform<'v>>), CompileError> {
    let (mode, lowering) = variant.parts();
    let Some(technique) = technique else {
        let known = known_current(values.current_attributes);
        let decoded = decode_technique(
            source,
            fragment_bytes,
            controls,
            known,
            mode,
            Some((&mut cache.vertex, &mut cache.fragment)),
        )?;
        return finish_technique(
            source,
            fragment_bytes,
            controls,
            values,
            mode,
            lowering,
            &decoded,
            None,
            defer,
            &mut |program, stage| match (lowering, stage) {
                (Lowering::StoredDepthFragment, Stage::Fragment) => {
                    cache.native.compile_metal_stored_depth(program)
                }
                _ => cache.native.compile(program, stage),
            },
        );
    };
    debug_assert!(
        std::ptr::eq(technique.source, source) && std::ptr::eq(technique.fragment, fragment_bytes)
    );
    let (source, fragment_bytes) = (technique.source, technique.fragment);
    let known = known_current(values.current_attributes);
    let logical_slots = if matches!(mode, FragmentMode::Flat) {
        0
    } else {
        fragment_logical_slots(values.fragment_writes)
    };
    let key =
        plan_reuse::ProgramKey::new(technique, mode, lowering, known, logical_slots, controls);
    let reused = cache.reuse.programs.get(&key).map(|entry| {
        (
            DecodedTechnique {
                fragment: Arc::clone(&entry.decoded.fragment),
                vertex: Arc::clone(&entry.decoded.vertex),
            },
            entry.layout.clone(),
        )
    });
    let hit = reused.is_some();
    let (decoded, mut layout) = match reused {
        Some(found) => {
            cache.reuse.stats.program_hits += 1;
            found
        }
        None => {
            cache.reuse.stats.program_misses += 1;
            let decoded = decode_technique(
                source,
                fragment_bytes,
                controls,
                known,
                mode,
                Some((&mut cache.vertex, &mut cache.fragment)),
            )?;
            (decoded, None)
        }
    };
    let result = finish_technique(
        source,
        fragment_bytes,
        controls,
        values,
        mode,
        lowering,
        &decoded,
        Some(&mut layout),
        defer,
        &mut |program, stage| match (lowering, stage) {
            (Lowering::StoredDepthFragment, Stage::Fragment) => {
                cache.native.compile_metal_stored_depth(program)
            }
            _ => cache.native.compile(program, stage),
        },
    );
    // Decoding (and any layout decoded) is value-independent, so it is kept
    // even when a value-dependent later step refused this draw.
    if !hit {
        cache
            .reuse
            .insert_program(key, plan_reuse::ProgramEntry { decoded, layout });
    }
    result
}
/// Fragment texture units read by a fragment template, retained per reused
/// technique (only successful answers).
pub fn fragment_texture_units_cached(
    cache: &mut NativeCompilerCache,
    technique: Option<&ReusedTechnique<'_>>,
    fragment_template: &[u8],
) -> Result<u16, compiler::ProgramError> {
    let Some(technique) = technique else {
        return compiler::fragment_texture_units(fragment_template);
    };
    if let Some(&units) = cache.reuse.units.get(&technique.id) {
        cache.reuse.stats.requirement_hits += 1;
        return Ok(units);
    }
    cache.reuse.stats.requirement_misses += 1;
    let units = compiler::fragment_texture_units(technique.fragment)?;
    cache.reuse.units.insert(technique.id, units);
    Ok(units)
}
/// The source vertex program's structural input mask and constant reads
/// (see `source_vertex_requirements`), borrowed from the retained decode.
#[expect(
    dead_code,
    reason = "used by the Main scene planner, which is migrated later"
)]
pub(crate) struct VertexRequirements {
    decoded: Arc<DecodedVertex>,
}
#[expect(
    dead_code,
    reason = "used by the Main scene planner, which is migrated later"
)]
impl VertexRequirements {
    pub(crate) fn input_mask(&self) -> u16 {
        self.decoded.program.input_mask as u16
    }
    pub(crate) fn indexed(&self) -> bool {
        self.decoded.reads.indexed
    }
    pub(crate) fn rows(&self) -> &[u16] {
        &self.decoded.reads.rows
    }
}
/// `source_vertex_requirements` without copying the read rows, retained per
/// reused technique and vertex texture mask when a technique is given.
#[expect(
    dead_code,
    reason = "used by the Main scene planner, which is migrated later"
)]
pub(crate) fn source_vertex_requirements_cached(
    cache: &mut NativeCompilerCache,
    technique: Option<&ReusedTechnique<'_>>,
    source: &TechniquePrograms,
    controls: &EffectiveShaderControls,
) -> Result<VertexRequirements, CompileError> {
    let key = technique.map(|t| (t.id, controls.vertex_texture_mask));
    if let Some(key) = key
        && let Some(decoded) = cache.reuse.requirements.get(&key)
    {
        cache.reuse.stats.requirement_hits += 1;
        return Ok(VertexRequirements {
            decoded: Arc::clone(decoded),
        });
    }
    let mut symbolic = controls.clone();
    symbolic.enabled_arrays = u16::MAX;
    let decoded = cache.vertex.get(source, &symbolic, 0, false, || {
        decode_vertex(source, &symbolic, 0, false)
    })?;
    if let Some(key) = key {
        cache.reuse.stats.requirement_misses += 1;
        cache.reuse.insert_requirement(key, Arc::clone(&decoded));
    }
    Ok(VertexRequirements { decoded })
}
/// Structural source interface/read preflight only. The all-array intermediate
/// is never lowered or submitted; actual binding and values remain mandatory.
#[expect(
    dead_code,
    reason = "used by the Main scene planner, which is migrated later"
)]
pub(crate) fn source_vertex_requirements(
    cache: &mut NativeCompilerCache,
    source: &TechniquePrograms,
    controls: &EffectiveShaderControls,
) -> Result<(u16, bool, Vec<u16>), CompileError> {
    let r = source_vertex_requirements_cached(cache, None, source, controls)?;
    Ok((r.input_mask(), r.indexed(), r.rows().to_vec()))
}
/// Bounded immutable shader/control signatures, never guest reads or shader
/// byte dumps. Different pipeline stages/families cannot consume one another's
/// first-eight budget and hide the dominant material family.
pub(crate) fn report_unlit_failure_context(
    stage: &'static str,
    source: &TechniquePrograms,
    bytes: &[u8],
    controls: &EffectiveShaderControls,
    error: &CompileError,
    context: (u16, Option<String>),
) {
    use sha2::{Digest, Sha256};
    use std::sync::{
        Mutex,
        atomic::{AtomicUsize, Ordering},
    };
    static EXAMINED: AtomicUsize = AtomicUsize::new(0);
    static SEEN: Mutex<Vec<[u8; 32]>> = Mutex::new(Vec::new());
    if EXAMINED
        .try_update(Ordering::Relaxed, Ordering::Relaxed, |n| {
            (n < 4096).then_some(n + 1)
        })
        .is_err()
    {
        return;
    }
    let mut vp = Sha256::new();
    for words in &source.vertex.instructions {
        for word in words {
            vp.update(word.to_be_bytes());
        }
    }
    let vp = vp.finalize();
    let fp = Sha256::digest(
        bytes
            .get(..source.fragment.semantic_bytes as usize)
            .unwrap_or(bytes),
    );
    let (known, identity) = context;
    let metadata = format!(
        "shader_control={:08x} texcoords={:?} alpha={:?} srgb={} grid={:?} cube={:04x} volume={:04x} unnorm={:04x} dimensions={:?} gamma={:?} expand={:?} shadow={:?} arrays={:04x} known_current={known:04x}",
        source.fragment.shader_control,
        source.fragment.texcoord_controls,
        controls.alpha_function,
        controls.srgb_output,
        controls.sample_grid,
        controls.cube_mask,
        controls.volume_mask,
        controls.unnormalized_mask,
        controls.texture_dimensions,
        controls.gamma,
        controls.expand,
        controls
            .shadow
            .map(|s| [s.enabled, s.function, s.linear, s.unnormalized]),
        controls.enabled_arrays
    );
    let mut key = Sha256::new();
    key.update(stage);
    key.update(vp);
    key.update(fp);
    key.update(&metadata);
    key.update(format!("{error:?}"));
    let key: [u8; 32] = key.finalize().into();
    let Ok(mut seen) = SEEN.lock() else {
        return;
    };
    if seen.len() >= 96 || seen.contains(&key) {
        return;
    }
    seen.push(key);
    drop(seen);
    eprintln!(
        "[mag main unlit stage refusal] stage={stage} identity={identity:?} vp_sha256={vp:x} fp_sha256={fp:x} error={error:?} {metadata}"
    );
}
fn decode_fragment(
    source: &TechniquePrograms,
    fragment_bytes: &[u8],
    controls: &EffectiveShaderControls,
) -> Result<compiler::HlslProgram, CompileError> {
    compiler::fragment_with_texcoords(
        compiler::FragmentProgram {
            bytes: fragment_bytes,
            shader_control: source.fragment.shader_control,
            cube_mask: controls.cube_mask,
            volume_mask: controls.volume_mask,
            shadow: controls.shadow,
            gamma: controls.gamma,
            expand: controls.expand,
            alpha_function: controls.alpha_function,
            srgb_output: controls.srgb_output,
            sample_grid: controls.sample_grid,
            unnormalized_mask: controls.unnormalized_mask,
            texture_dimensions: controls.texture_dimensions,
        },
        source.fragment.texcoord_controls,
    )
    .map_err(CompileError::Decode)
}
fn decode_vertex(
    source: &TechniquePrograms,
    controls: &EffectiveShaderControls,
    known: u16,
    poc_unlit: bool,
) -> Result<DecodedVertex, CompileError> {
    decode_vertex_policy(
        source,
        controls,
        known,
        if poc_unlit {
            VertexPolicy::Position
        } else {
            VertexPolicy::Full
        },
    )
}
fn decode_vertex_policy(
    source: &TechniquePrograms,
    controls: &EffectiveShaderControls,
    known: u16,
    policy: VertexPolicy,
) -> Result<DecodedVertex, CompileError> {
    let words: Vec<u32> = source
        .vertex
        .instructions
        .iter()
        .flatten()
        .copied()
        .collect();
    let input = || compiler::VertexProgram {
        words: &words,
        start_slot: u32::from(source.vertex.load_slot),
        vertex_textures: controls.vertex_texture_mask,
        enabled_arrays: controls.enabled_arrays,
        known_current_attributes: known,
    };
    let decoded = match policy {
        VertexPolicy::Full => compiler::vertex(input()).map(|v| (v, false)),
        VertexPolicy::Position | VertexPolicy::Outputs(_) => {
            let specialized = match policy {
                VertexPolicy::Position => compiler::vertex_position_source_poc(input()),
                VertexPolicy::Outputs(outputs) => {
                    compiler::vertex_outputs_source_poc(input(), outputs)
                }
                VertexPolicy::Full => unreachable!(),
            };
            match specialized {
                Ok(v) => Ok((v, true)),
                // Unsupported grammar may use the complete native source VP;
                // it still requires every actual input/constant/current value.
                Err(compiler::ProgramError::Unsupported) => {
                    compiler::vertex(input()).map(|v| (v, false))
                }
                Err(error) => Err(error),
            }
        }
    };
    let (vertex, approximated_vertex_outputs) = decoded.map_err(|error| {
        report_vertex_decode_failure(source, controls, known, &error);
        CompileError::Decode(error)
    })?;
    let reads = compiler::vertex_constant_reads(&vertex).map_err(CompileError::Decode)?;
    Ok(DecodedVertex {
        program: vertex,
        reads,
        approximated_outputs: approximated_vertex_outputs,
        native: Default::default(),
    })
}
fn compile_technique_inner(
    source: &TechniquePrograms,
    fragment_bytes: &[u8],
    controls: &EffectiveShaderControls,
    values: EffectiveProgramValues<'_>,
    poc_unlit: bool,
    lower: &mut impl FnMut(&compiler::HlslProgram, Stage) -> Result<Arc<NativeProgram>, String>,
) -> Result<CompiledMaterialPrograms, CompileError> {
    compile_technique_inner_cached(
        source,
        fragment_bytes,
        controls,
        values,
        if poc_unlit {
            FragmentMode::Flat
        } else {
            FragmentMode::Source
        },
        Lowering::Standard,
        None,
        lower,
    )
}
/// Main's ARGB8 colour target holds a shared-exponent HDR encoding: source
/// materials divide RGB by `m = max(r, g, b, floor)` and store `m / 8` in alpha,
/// so later readers reconstruct `rgb * alpha * 8`. An authored flat colour that
/// writes alpha 1 therefore reads back eight times brighter than intended and
/// dominates later luminance-driven stages. The POC flat colour now stores the
/// unit scale (1/8) instead. `PS3_RENDER_POC_FLAT_ALPHA_ONE=1` restores the old
/// opaque-alpha output for A/B comparison. This remains an explicit
/// approximation; it does not claim source material appearance.
fn flat_fragment_alpha_one() -> bool {
    static MODE: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    *MODE
        .get_or_init(|| std::env::var_os("PS3_RENDER_POC_FLAT_ALPHA_ONE").is_some_and(|v| v == "1"))
}
fn flat_fragment_source() -> &'static str {
    if flat_fragment_alpha_one() {
        "struct PSInput { float4 position : SV_Position; };\nfloat4 main(PSInput input) : SV_Target0 { return float4(0.65, 0.7, 0.75, 1.0); }"
    } else {
        "struct PSInput { float4 position : SV_Position; };\nfloat4 main(PSInput input) : SV_Target0 { return float4(0.65, 0.7, 0.75, 0.125); }"
    }
}
/// Memoized native lowerings of one immutable decoded program, indexed by
/// lowering kind. Only successful lowerings are kept.
pub(crate) struct NativeMemo<const N: usize>([std::sync::OnceLock<Arc<NativeProgram>>; N]);
impl<const N: usize> Default for NativeMemo<N> {
    fn default() -> Self {
        Self(core::array::from_fn(|_| std::sync::OnceLock::new()))
    }
}
impl<const N: usize> NativeMemo<N> {
    fn get_or_lower(
        &self,
        kind: usize,
        lower: impl FnOnce() -> Result<Arc<NativeProgram>, String>,
    ) -> Result<Arc<NativeProgram>, String> {
        let cell = &self.0[kind];
        if let Some(native) = cell.get() {
            return Ok(Arc::clone(native));
        }
        let native = lower()?;
        Ok(Arc::clone(cell.get_or_init(|| native)))
    }
}
/// Which native lowering the caller's `lower` closure applies to fragments.
#[derive(Clone, Copy)]
enum Lowering {
    Standard = 0,
    StoredDepthFragment = 1,
}
#[derive(Clone, Copy)]
enum FragmentMode {
    Source = 0,
    Flat = 1,
    UnlitColor = 2,
}
// `lowering` names which native lowering `lower` performs, so memoized
// lowerings of one decoded program are never mixed between kinds.
#[allow(clippy::too_many_arguments)]
fn compile_technique_inner_cached(
    source: &TechniquePrograms,
    fragment_bytes: &[u8],
    controls: &EffectiveShaderControls,
    values: EffectiveProgramValues<'_>,
    mode: FragmentMode,
    lowering: Lowering,
    decode_caches: Option<(&mut VertexDecodeCache, &mut FragmentDecodeCache)>,
    lower: &mut impl FnMut(&compiler::HlslProgram, Stage) -> Result<Arc<NativeProgram>, String>,
) -> Result<CompiledMaterialPrograms, CompileError> {
    let known = known_current(values.current_attributes);
    let decoded = decode_technique(source, fragment_bytes, controls, known, mode, decode_caches)?;
    finish_technique(
        source,
        fragment_bytes,
        controls,
        values,
        mode,
        lowering,
        &decoded,
        None,
        false,
        lower,
    )
    .map(|(programs, _)| programs)
}
/// Bit `i` set when current attribute `i` has a known value.
fn known_current(current: &[Option<[u32; 4]>; 16]) -> u16 {
    current.iter().enumerate().fold(0u16, |mask, (i, v)| {
        mask | if v.is_some() { 1 << i } else { 0 }
    })
}
/// The value-independent result of one technique's source checks and
/// decoding: a pure function of the source programs, fragment bytes, every
/// control, the known-current mask and the fragment mode.
pub(crate) struct DecodedTechnique {
    fragment: Arc<DecodedFragment>,
    vertex: Arc<DecodedVertex>,
}
/// Source extent/control checks and immutable FP/VP decoding, in the
/// unmemoized compile order. Every value-dependent step is in `finish_technique`.
fn decode_technique(
    source: &TechniquePrograms,
    fragment_bytes: &[u8],
    controls: &EffectiveShaderControls,
    known: u16,
    mode: FragmentMode,
    decode_caches: Option<(&mut VertexDecodeCache, &mut FragmentDecodeCache)>,
) -> Result<DecodedTechnique, CompileError> {
    let poc_unlit = matches!(mode, FragmentMode::Flat);
    let unlit_color = matches!(mode, FragmentMode::UnlitColor);
    if !poc_unlit
        && ![
            source.fragment.semantic_bytes as usize,
            source.fragment.transfer_bytes as usize,
        ]
        .contains(&fragment_bytes.len())
        || source.vertex.instructions.len() > 512
    {
        return Err(CompileError::SourceExtent);
    }
    let fragment_bytes = if poc_unlit {
        &[][..]
    } else {
        fragment_bytes
            .get(..source.fragment.semantic_bytes as usize)
            .ok_or(CompileError::SourceExtent)?
    };
    // Only source-qualified 2D and centroid interpolation controls are admitted.
    for (unit, &control) in source.fragment.texcoord_controls.iter().enumerate() {
        if !poc_unlit && control & !0x11 != 0 {
            return Err(CompileError::UnqualifiedTexcoord { unit, control });
        }
    }
    let (vertex_cache, fragment_cache) =
        decode_caches.map_or((None, None), |(v, f)| (Some(v), Some(f)));
    let fragment = if poc_unlit {
        Arc::new(DecodedFragment::source(compiler::HlslProgram {
            source: flat_fragment_source().into(),
            instructions: 1,
            constant_slots: 0,
            input_mask: 0,
            current_attribute_mask: 0,
            constant_buffer_bytes: 0,
        }))
    } else if let Some(cache) = fragment_cache {
        cache.get(source, fragment_bytes, controls, unlit_color, || {
            let decoded =
                decode_fragment(source, fragment_bytes, controls).inspect_err(|error| {
                    if unlit_color {
                        report_unlit_failure_context(
                            "fragment-emission",
                            source,
                            fragment_bytes,
                            controls,
                            error,
                            (known, None),
                        );
                    }
                })?;
            if unlit_color {
                let specialized = unlit_color::specialize(decoded).inspect_err(|error| {
                    report_unlit_failure_context(
                        "fragment-specialization",
                        source,
                        fragment_bytes,
                        controls,
                        error,
                        (known, None),
                    );
                })?;
                Ok(DecodedFragment {
                    program: specialized.program,
                    outputs: Some(specialized.outputs),
                    native: Default::default(),
                })
            } else {
                Ok(DecodedFragment::source(decoded))
            }
        })?
    } else {
        let decoded = decode_fragment(source, fragment_bytes, controls)?;
        Arc::new(if unlit_color {
            let specialized = unlit_color::specialize(decoded)?;
            DecodedFragment {
                program: specialized.program,
                outputs: Some(specialized.outputs),
                native: Default::default(),
            }
        } else {
            DecodedFragment::source(decoded)
        })
    };
    let fragment_info = fragment;
    let policy = match fragment_info.outputs {
        Some(outputs) => VertexPolicy::Outputs(outputs),
        None if poc_unlit => VertexPolicy::Position,
        None => VertexPolicy::Full,
    };
    let mut vertex_cache = vertex_cache;
    if matches!(policy, VertexPolicy::Outputs(_)) {
        let mut symbolic = controls.clone();
        symbolic.enabled_arrays = u16::MAX;
        let structural = if let Some(cache) = vertex_cache.as_deref_mut() {
            cache.get_policy(source, &symbolic, 0, policy, || {
                decode_vertex_policy(source, &symbolic, 0, policy).inspect_err(|error| {
                    report_unlit_failure_context(
                        "vertex-symbolic-outputs",
                        source,
                        fragment_bytes,
                        controls,
                        error,
                        (known, None),
                    );
                })
            })?
        } else {
            Arc::new(decode_vertex_policy(source, &symbolic, 0, policy)?)
        };
        let missing = structural.program.input_mask & !u32::from(controls.enabled_arrays | known);
        if missing != 0 {
            return Err(CompileError::MissingCurrentAttribute(
                missing.trailing_zeros(),
            ));
        }
    }
    let decoded = if let Some(cache) = vertex_cache {
        cache.get_policy(source, controls, known, policy, || {
            decode_vertex_policy(source, controls, known, policy).inspect_err(|error| {
                if unlit_color {
                    report_unlit_failure_context(
                        "vertex-live-interface",
                        source,
                        fragment_bytes,
                        controls,
                        error,
                        (known, None),
                    );
                }
            })
        })?
    } else {
        Arc::new(decode_vertex_policy(source, controls, known, policy)?)
    };
    Ok(DecodedTechnique {
        fragment: fragment_info,
        vertex: decoded,
    })
}
/// Logical fragment row domain of the actual ordered writes.
fn fragment_logical_slots(writes: &[ConstantRowWrite]) -> usize {
    writes
        .iter()
        .map(|w| w.logical_slot as usize + 1)
        .max()
        .unwrap_or(0)
}
/// Every value-dependent step after `decode_technique`, in the unmemoized
/// order: fragment layout/evaluation, vertex constant knowledge, uniform
/// packing, then native lowering (memoized on the decoded programs).
/// `layout`, when given, holds this technique's fragment constant layout for
/// the values' logical slot count (decoded and stored here when empty).
/// With `defer_vertex_uniform`, `vertex_uniform` stays empty and its exact
/// image is returned by parts instead (no other step changes).
#[allow(clippy::too_many_arguments)]
fn finish_technique<'v>(
    source: &TechniquePrograms,
    fragment_bytes: &[u8],
    controls: &EffectiveShaderControls,
    values: EffectiveProgramValues<'v>,
    mode: FragmentMode,
    lowering: Lowering,
    decoded_technique: &DecodedTechnique,
    layout: Option<&mut Option<Arc<FragmentConstantLayout>>>,
    defer_vertex_uniform: bool,
    lower: &mut impl FnMut(&compiler::HlslProgram, Stage) -> Result<Arc<NativeProgram>, String>,
) -> Result<(CompiledMaterialPrograms, Option<VertexUniform<'v>>), CompileError> {
    let poc_unlit = matches!(mode, FragmentMode::Flat);
    let unlit_color = matches!(mode, FragmentMode::UnlitColor);
    let known = known_current(values.current_attributes);
    let fragment_bytes = if poc_unlit {
        &[][..]
    } else {
        fragment_bytes
            .get(..source.fragment.semantic_bytes as usize)
            .ok_or(CompileError::SourceExtent)?
    };
    let fragment_info = &decoded_technique.fragment;
    let decoded = &decoded_technique.vertex;
    let vertex = &decoded.program;
    let approximated_vertex_outputs = decoded.approximated_outputs;
    let fragment = &fragment_info.program;
    let fragment_values = if poc_unlit {
        None
    } else {
        let logical_slots = fragment_logical_slots(values.fragment_writes);
        let decode_layout = || {
            FragmentConstantLayout::decode(
                fragment_bytes,
                &source.fragment.relocation_directory,
                logical_slots,
                ConstantBudget {
                    program_bytes: 1024 * 1024,
                    inline_rows: 2048,
                    logical_slots: 1024,
                    relocations: 65536,
                },
            )
            .map_err(CompileError::Constant)
        };
        let layout = match layout {
            Some(Some(layout)) => Arc::clone(layout),
            Some(slot) => Arc::clone(slot.insert(Arc::new(decode_layout()?))),
            None => Arc::new(decode_layout()?),
        };
        let fragment_values = layout
            .evaluate(values.fragment_writes, values.alpha_window)
            .map_err(CompileError::Constant)?;
        if fragment_values.inline_rows.len() != fragment.constant_slots as usize {
            return Err(CompileError::UniformLayout);
        }
        Some(fragment_values)
    };
    let reads = &decoded.reads;
    let vertex_rows = values
        .vertex_constants
        .required_row_bytes(&reads.rows, reads.indexed)
        .map_err(|error| {
            if matches!(error, ValueError::UnknownVertexConstant(_)) {
                report_vertex_constant_failure(
                    source,
                    vertex,
                    reads,
                    values.vertex_constants,
                    controls.enabled_arrays,
                    known,
                );
            }
            CompileError::VertexValues(error)
        })?;
    let mut vertex_uniform = Vec::new();
    if !defer_vertex_uniform {
        vertex_uniform
            .try_reserve_exact(vertex.constant_buffer_bytes as usize)
            .map_err(|_| CompileError::Allocation)?;
    }
    let mut parts = VertexUniform {
        bank: vertex_rows,
        viewport: [values.viewport_scale, values.viewport_offset],
        current: None,
    };
    if vertex.current_attribute_mask != 0 {
        let mut current_rows = [[0; 4]; 16];
        for (index, current) in values.current_attributes.iter().enumerate() {
            if vertex.current_attribute_mask & (1 << index) != 0 && current.is_none() {
                return Err(CompileError::MissingCurrentAttribute(index as u32));
            }
            // Padding only for statically unused current attributes; the compiler
            // has already rejected every used attribute whose value is unknown.
            current_rows[index] = current.unwrap_or([0; 4]);
        }
        parts.current = Some(current_rows);
    }
    if !defer_vertex_uniform {
        parts.append_to(&mut vertex_uniform);
    }
    let vertex_uniform_len = parts.len();
    let mut fragment_uniform = Vec::new();
    fragment_uniform
        .try_reserve_exact(fragment.constant_buffer_bytes as usize)
        .map_err(|_| CompileError::Allocation)?;
    if let Some(fragment_values) = &fragment_values {
        if fragment_values.inline_rows.is_empty() {
            append_row(&mut fragment_uniform, [0; 4]);
        }
        append_rows(&mut fragment_uniform, &fragment_values.inline_rows);
        append_row(&mut fragment_uniform, fragment_values.alpha_window);
    }
    if vertex_uniform_len != vertex.constant_buffer_bytes as usize
        || fragment_uniform.len() != fragment.constant_buffer_bytes as usize
    {
        return Err(CompileError::UniformLayout);
    }
    let fragment = fragment_info
        .native
        .get_or_lower(lowering as usize, || lower(fragment, Stage::Fragment))
        .map_err(CompileError::Native)?;
    // The source-qualified lighting multiplication is the only omission. Any
    // surviving input besides base color means this semantic form is not ours.
    if unlit_color
        && (fragment.textures != [0]
            || fragment.samplers != [0]
            || fragment.stored_depth_aux.is_some())
    {
        let error = CompileError::Decode(compiler::ProgramError::Unsupported);
        report_unlit_failure_context(
            "native-resource-closure",
            source,
            fragment_bytes,
            controls,
            &error,
            (
                known,
                Some(format!(
                    "textures={:?} samplers={:?}",
                    fragment.textures, fragment.samplers
                )),
            ),
        );
        return Err(error);
    }
    let programs = CompiledMaterialPrograms {
        vertex: decoded
            .native
            .get_or_lower(0, || lower(vertex, Stage::Vertex))
            .map_err(CompileError::Native)?,
        fragment,
        vertex_uniform,
        fragment_uniform,
        declared_vertex_input_mask: source.vertex.input_mask,
        declared_vertex_output_mask: source.vertex.output_mask,
        actual_vertex_input_mask: vertex.input_mask,
        enabled_vertex_arrays: controls.enabled_arrays,
        source_transform_timeout: source.vertex.unresolved_transform_word,
        sanitized_fragment_words: fragment_values.as_ref().map_or(0, |v| v.sanitized_words),
        approximated_vertex_outputs,
    };
    Ok((programs, defer_vertex_uniform.then_some(parts)))
}
/// One compile's vertex uniform image by parts: the validated constant bank
/// (unknown rows already zero padding), viewport scale/offset, then the
/// current attribute rows when the program reads any. `append_to` produces
/// exactly the bytes a non-deferred compile stores in `vertex_uniform`.
pub struct VertexUniform<'a> {
    bank: &'a [[u8; 16]; 512],
    viewport: [[u32; 4]; 2],
    current: Option<[[u32; 4]; 16]>,
}
impl VertexUniform<'_> {
    pub fn len(&self) -> usize {
        (512 + 2 + if self.current.is_some() { 16 } else { 0 }) * 16
    }
    pub fn is_empty(&self) -> bool {
        false
    }
    pub fn append_to(&self, out: &mut Vec<u8>) {
        out.extend_from_slice(self.bank.as_flattened());
        append_rows(out, &self.viewport);
        if let Some(current) = &self.current {
            append_rows(out, current);
        }
    }
}

#[cfg(test)]
mod poc_unlit_tests {
    use super::*;
    use crate::material_programs::{
        FragmentProgramSource, FragmentSourceReference, VertexProgramSource,
    };
    pub(super) fn source() -> (TechniquePrograms, Vec<u8>) {
        let operand = 0x1b02;
        let vertex = VertexProgramSource {
            header_words: [0; 6],
            load_slot: 0,
            input_mask: 1,
            output_mask: 0x80,
            unresolved_transform_word: 7,
            instructions: vec![[
                (1 << 30) | (63 << 15),
                (1 << 22) | (operand >> 9),
                (operand & 511) << 23,
                (15 << 13) | 1,
            ]],
            static_constants: vec![],
        };
        let bytes = [
            (1u32 << 24) | (15 << 9) | 1,
            2 | 0x1c800 | (7 << 18),
            0x1c800,
            0x1c800,
            1f32.to_bits(),
            0,
            0,
            1f32.to_bits(),
        ]
        .iter()
        .flat_map(|w| w.rotate_left(16).to_be_bytes())
        .collect();
        let fragment = FragmentProgramSource {
            header_words: [0; 6],
            semantic_bytes: 32,
            transfer_bytes: 32,
            reference: FragmentSourceReference::Unresolved { tag: 2, encoded: 2 },
            shader_control: 0x40,
            texcoord_source_masks: [0; 2],
            texcoord_controls: [0; 10],
            relocation_directory: vec![0; 24],
        };
        (TechniquePrograms { vertex, fragment }, bytes)
    }
    pub(super) fn controls() -> EffectiveShaderControls {
        EffectiveShaderControls {
            vertex_texture_mask: 0,
            enabled_arrays: 1,
            cube_mask: 0,
            volume_mask: 0,
            shadow: [ShadowTexture::default(); 16],
            gamma: [0; 16],
            expand: [0; 16],
            alpha_function: None,
            srgb_output: false,
            sample_grid: [1, 1],
            unnormalized_mask: 0,
            texture_dimensions: [[1, 1]; 16],
        }
    }
    fn values<'a>(
        bank: &'a KnownVertexBank,
        current: &'a [Option<[u32; 4]>; 16],
    ) -> EffectiveProgramValues<'a> {
        EffectiveProgramValues {
            vertex_constants: bank,
            viewport_scale: [1f32.to_bits(); 4],
            viewport_offset: [0; 4],
            current_attributes: current,
            fragment_writes: &[],
            alpha_window: [0; 4],
        }
    }
    #[test]
    fn flat_fragment_preserves_actual_vertex_program_and_has_no_fragment_resources() {
        let (source, bytes) = source();
        let controls = controls();
        let bank = KnownVertexBank::unknown();
        let current = [None; 16];
        let mut cache = NativeCompilerCache::new(CacheLimits {
            entries: 8,
            retained_payload_bytes: 1024 * 1024,
        });
        let ordinary = compile_technique_programs_cached(
            &mut cache,
            &source,
            &bytes,
            &controls,
            values(&bank, &current),
        )
        .unwrap();
        let flat = compile_technique_inner(
            &source,
            &bytes,
            &controls,
            values(&bank, &current),
            true,
            &mut |p, s| cache.compile(p, s),
        )
        .unwrap();
        assert!(!ordinary.approximated_vertex_outputs);
        assert!(flat.approximated_vertex_outputs);
        assert!(ordinary.vertex.hlsl.contains("o[0].xyzw = _v.xyzw"));
        assert!(flat.vertex.hlsl.contains("o[0].xyzw = _v.xyzw"));
        assert_eq!(ordinary.vertex_uniform, flat.vertex_uniform);
        assert_eq!(
            ordinary.actual_vertex_input_mask,
            flat.actual_vertex_input_mask
        );
        assert_eq!(flat.source_transform_timeout, 7);
        assert!(flat.fragment_uniform.is_empty());
        assert!(
            flat.fragment.buffers.is_empty()
                && flat.fragment.texture_bindings.is_empty()
                && flat.fragment.sampler_bindings.is_empty()
        );
        assert!(flat.fragment.hlsl.contains("0.65"));
        // Unit HDR scale, not an eightfold-bright alpha of one.
        assert!(flat.fragment.hlsl.contains("0.75, 0.125)"));
        assert!(!flat.fragment.hlsl.contains("0.75, 1.0)"));
        assert!(!flat.fragment.hlsl.contains("discard"));
        assert!(flat.fragment.stored_depth_aux.is_none());
    }
    #[test]
    fn poc_drops_only_unused_vertex_outputs_without_inventing_constants() {
        let (mut source, bytes) = source();
        source.vertex.instructions[0][3] &= !1;
        let operand = 0x1b03;
        source.vertex.instructions.push([
            (1 << 30) | (63 << 15),
            (1 << 22) | (21 << 12) | (operand >> 9),
            (operand & 511) << 23,
            (15 << 13) | (11 << 2) | 1,
        ]);
        let controls = controls();
        let bank = KnownVertexBank::unknown();
        let current = [None; 16];
        let mut cache = NativeCompilerCache::new(CacheLimits {
            entries: 8,
            retained_payload_bytes: 1024 * 1024,
        });
        assert!(matches!(
            compile_technique_programs_cached(
                &mut cache,
                &source,
                &bytes,
                &controls,
                values(&bank, &current)
            ),
            Err(CompileError::VertexValues(
                ValueError::UnknownVertexConstant(21)
            ))
        ));
        let flat = compile_technique_inner(
            &source,
            &bytes,
            &controls,
            values(&bank, &current),
            true,
            &mut |p, s| cache.compile(p, s),
        )
        .unwrap();
        assert!(flat.vertex.hlsl.contains("o[0].xyzw = _v.xyzw"));
        assert!(!flat.vertex.hlsl.contains("vp_c[21]"));
        assert!(!flat.vertex.hlsl.contains("o[11].xyzw = _v.xyzw"));
        assert!(!flat.vertex.hlsl.contains("Out.t4"));
        assert!(
            flat.vertex
                .hlsl
                .contains("_p.xyz * vp_posscale.xyz + _p.w * vp_posoffset.xyz")
        );
        assert_eq!(
            flat.actual_vertex_input_mask,
            ordinary_input_mask(&source, &controls, &current)
        );
        assert_eq!(bank.row(21), None);
        assert!(flat.approximated_vertex_outputs);
        // A missing constant on the actual position path is still required.
        source.vertex.instructions[0] = source.vertex.instructions[1];
        source.vertex.instructions[0][3] = 15 << 13;
        assert!(matches!(
            compile_technique_inner(
                &source,
                &bytes,
                &controls,
                values(&bank, &current),
                true,
                &mut |p, s| cache.compile(p, s),
            ),
            Err(CompileError::VertexValues(
                ValueError::UnknownVertexConstant(21)
            ))
        ));
    }
    #[test]
    fn poc_checks_only_position_live_current_inputs_while_exact_path_stays_strict() {
        let (mut source, bytes) = source();
        source.vertex.input_mask = 0x8001;
        source.vertex.instructions[0][3] &= !1;
        let mut unused = source.vertex.instructions[0];
        unused[1] = (unused[1] & !(15 << 8)) | (15 << 8);
        unused[3] |= (11 << 2) | 1;
        source.vertex.instructions.push(unused);
        let controls = controls();
        let bank = KnownVertexBank::unknown();
        let mut current = [None; 16];
        let mut cache = NativeCompilerCache::new(CacheLimits {
            entries: 8,
            retained_payload_bytes: 1024 * 1024,
        });
        assert!(matches!(
            compile_technique_programs_cached(
                &mut cache,
                &source,
                &bytes,
                &controls,
                values(&bank, &current)
            ),
            Err(CompileError::Decode(compiler::ProgramError::Invalid))
        ));
        let flat = compile_technique_inner(
            &source,
            &bytes,
            &controls,
            values(&bank, &current),
            true,
            &mut |p, s| cache.compile(p, s),
        )
        .unwrap();
        assert_eq!(flat.actual_vertex_input_mask, 1);
        assert_eq!(flat.vertex.vertex_locations, [0]);
        assert_eq!(flat.vertex_uniform.len(), 514 * 16);
        assert!(flat.approximated_vertex_outputs);
        assert!(current[15].is_none());
        // Now v15 really feeds position. Unknown current15 must still refuse.
        source.vertex.instructions[0][1] = unused[1];
        source.vertex.instructions[1][1] &= !(15 << 8);
        assert!(matches!(
            compile_technique_inner(
                &source,
                &bytes,
                &controls,
                values(&bank, &current),
                true,
                &mut |p, s| cache.compile(p, s)
            ),
            Err(CompileError::Decode(compiler::ProgramError::Invalid))
        ));
        current[15] = Some([1f32.to_bits(); 4]);
        let flat = compile_technique_inner(
            &source,
            &bytes,
            &controls,
            values(&bank, &current),
            true,
            &mut |p, s| cache.compile(p, s),
        )
        .unwrap();
        assert_eq!(flat.actual_vertex_input_mask, 0x8000);
        assert_eq!(flat.vertex_uniform.len(), 530 * 16);
        assert_eq!(
            &flat.vertex_uniform[(514 + 15) * 16..530 * 16],
            &[1f32.to_bits(); 4]
                .into_iter()
                .flat_map(u32::to_le_bytes)
                .collect::<Vec<_>>()
        );
    }
    fn ordinary_input_mask(
        source: &TechniquePrograms,
        controls: &EffectiveShaderControls,
        current: &[Option<[u32; 4]>; 16],
    ) -> u32 {
        let words: Vec<u32> = source
            .vertex
            .instructions
            .iter()
            .flatten()
            .copied()
            .collect();
        let known = current.iter().enumerate().fold(0u16, |mask, (i, v)| {
            mask | if v.is_some() { 1 << i } else { 0 }
        });
        compiler::vertex(compiler::VertexProgram {
            words: &words,
            start_slot: source.vertex.load_slot.into(),
            vertex_textures: controls.vertex_texture_mask,
            enabled_arrays: controls.enabled_arrays,
            known_current_attributes: known,
        })
        .unwrap()
        .input_mask
    }
    #[test]
    fn unsupported_position_slice_keeps_complete_native_vertex_and_value_checks() {
        let (mut source, bytes) = source();
        // An address-register instruction is outside the POC slice grammar.
        // Keep real position output first, then a separate dead ARL operation.
        source.vertex.instructions[0][3] &= !1;
        let input = 0x1b02;
        source.vertex.instructions.push([
            63 << 15,
            (0x0d << 22) | (input >> 9),
            (input & 511) << 23,
            (1 << 16) | 1,
        ]);
        let controls = controls();
        let bank = KnownVertexBank::unknown();
        let current = [None; 16];
        let mut cache = NativeCompilerCache::new(CacheLimits {
            entries: 8,
            retained_payload_bytes: 1024 * 1024,
        });
        let strict = compile_technique_programs_cached(
            &mut cache,
            &source,
            &bytes,
            &controls,
            values(&bank, &current),
        )
        .unwrap();
        let flat = compile_technique_inner(
            &source,
            &bytes,
            &controls,
            values(&bank, &current),
            true,
            &mut |p, s| cache.compile(p, s),
        )
        .unwrap();
        assert!(Arc::ptr_eq(&strict.vertex, &flat.vertex));
        assert!(!flat.approximated_vertex_outputs);
        assert_eq!(strict.vertex_uniform, flat.vertex_uniform);
        // Even when slicing refuses, a missing live constant cannot be padded.
        let operand = 0x1b03;
        source.vertex.instructions[0][1] = (1 << 22) | (21 << 12) | (operand >> 9);
        source.vertex.instructions[0][2] = (operand & 511) << 23;
        assert!(matches!(
            compile_technique_inner(
                &source,
                &bytes,
                &controls,
                values(&bank, &current),
                true,
                &mut |p, s| cache.compile(p, s),
            ),
            Err(CompileError::VertexValues(
                ValueError::UnknownVertexConstant(21)
            ))
        ));
    }
    #[test]
    fn malformed_original_fragment_is_bypassed_only_by_explicit_poc_path() {
        let (mut source, _) = source();
        source.fragment.texcoord_controls[0] = u32::MAX;
        source.fragment.relocation_directory.clear();
        let controls = controls();
        let bank = KnownVertexBank::unknown();
        let current = [None; 16];
        assert!(
            compile_technique_programs(&source, &[], &controls, values(&bank, &current)).is_err()
        );
        let mut cache = NativeCompilerCache::new(CacheLimits {
            entries: 8,
            retained_payload_bytes: 1024 * 1024,
        });
        assert!(
            compile_technique_inner(
                &source,
                &[],
                &controls,
                values(&bank, &current),
                true,
                &mut |p, s| cache.compile(p, s)
            )
            .is_ok()
        );
        let mut missing_current = controls;
        missing_current.enabled_arrays = 0;
        assert!(
            compile_technique_inner(
                &source,
                &[],
                &missing_current,
                values(&bank, &current),
                true,
                &mut |p, s| cache.compile(p, s)
            )
            .is_err()
        );
    }
    #[test]
    fn cached_poc_specialization_skips_decode_and_rebuilds_draw_uniforms() {
        let (source, bytes) = source();
        let controls = controls();
        let bank = KnownVertexBank::unknown();
        let current = [None; 16];
        let mut cache = NativeCompilerCache::new(CacheLimits {
            entries: 8,
            retained_payload_bytes: 1024 * 1024,
        });
        let a = compile_technique_inner_cached(
            &source,
            &bytes,
            &controls,
            values(&bank, &current),
            FragmentMode::Flat,
            Lowering::Standard,
            Some((&mut cache.vertex, &mut cache.fragment)),
            &mut |p, s| cache.native.compile(p, s),
        )
        .unwrap();
        let mut changed = values(&bank, &current);
        changed.viewport_offset = [2f32.to_bits(); 4];
        let b = compile_technique_inner_cached(
            &source,
            &bytes,
            &controls,
            changed,
            FragmentMode::Flat,
            Lowering::Standard,
            Some((&mut cache.vertex, &mut cache.fragment)),
            &mut |p, s| cache.native.compile(p, s),
        )
        .unwrap();
        assert!(a.approximated_vertex_outputs && b.approximated_vertex_outputs);
        assert!(Arc::ptr_eq(&a.vertex, &b.vertex));
        assert_ne!(a.vertex_uniform, b.vertex_uniform);
        assert_eq!(cache.vertex_decode_stats().decode_invocations, 1);
        assert_eq!(cache.vertex_decode_stats().hits, 1);
        assert!(cache.retained_payload_bytes() <= 1024 * 1024);
        // A warm decode reuses its memoized native lowering: the vertex is not
        // handed to the native cache again (the flat FP is built per call).
        let mut lowered = vec![];
        let c = compile_technique_inner_cached(
            &source,
            &bytes,
            &controls,
            values(&bank, &current),
            FragmentMode::Flat,
            Lowering::Standard,
            Some((&mut cache.vertex, &mut cache.fragment)),
            &mut |p, s| {
                lowered.push(s);
                cache.native.compile(p, s)
            },
        )
        .unwrap();
        assert_eq!(lowered, [Stage::Fragment]);
        assert!(Arc::ptr_eq(&a.vertex, &c.vertex));
        assert_eq!(a.vertex_uniform, c.vertex_uniform);
        assert_eq!(a.fragment_uniform, c.fragment_uniform);
    }
}

#[cfg(test)]
mod constant_failure_tests {
    use super::*;
    #[test]
    fn missing_rows_and_read_context_are_bounded_without_mutating_known_bank() {
        let (mut source, _) = super::poc_unlit_tests::source();
        source.vertex.instructions.resize(512, [0; 4]);
        let vertex = compiler::HlslProgram {
            source: format!(
                "VSOutput main(VSInput input) {{\n{}",
                (0..40)
                    .map(|n| format!("x = vp_c[{n}];\n"))
                    .collect::<String>()
            ),
            instructions: 512,
            constant_slots: 512,
            input_mask: 0,
            current_attribute_mask: 0,
            constant_buffer_bytes: 8224,
        };
        let mut bank = KnownVertexBank::unknown();
        bank.write(7, [1; 4]).unwrap();
        let reads = compiler::VertexConstantReads {
            rows: (0..40).collect(),
            indexed: false,
        };
        let context = vertex_constant_failure(&source, &vertex, &reads, &bank);
        assert_eq!(context.missing_total, 39);
        assert_eq!(context.missing_rows.len(), 32);
        assert!(!context.missing_rows.contains(&7));
        assert_eq!(context.read_lines.len(), 16);
        assert_eq!(context.read_line_total, 40);
        assert_eq!(context.source_hash.len(), 64);
        assert_eq!(bank.row(7), Some([1; 4]));
        assert_eq!(bank.row(21), None);
        source.vertex.instructions[511][3] = 1;
        assert_ne!(
            vertex_constant_failure(&source, &vertex, &reads, &bank).source_hash,
            context.source_hash
        );
        let indexed = compiler::VertexConstantReads {
            rows: vec![7],
            indexed: true,
        };
        let context = vertex_constant_failure(&source, &vertex, &indexed, &bank);
        assert!(context.indexed);
        assert_eq!(context.missing_total, 511);
        assert_eq!(context.missing_rows.len(), 32);
    }
}
