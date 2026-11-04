//! Main program compilation (`program-compilation.md`).
//!
//! The RSX-to-native translation is the `mag-render-programs` crate. This module states the
//! frontend side of that contract: source checks, the fragment inline-constant layout and
//! relocation directory, value evaluation and sanitisation, uniform packing, the vertex-array
//! join and the cache identities. The translator is reached through [`ProgramTranslator`], so
//! this crate needs no native toolchain.

use crate::guest::{be_u16, be_u32};
use std::collections::HashMap;

/// Sanitisation threshold for fragment constant lanes.
/// The binary32 value nearest `3.0e38`; a lane whose magnitude equals it is kept.
pub const SANITISE_LIMIT: u32 = 0x7f61_b1e6;
/// The flat variant's colour.
pub const FLAT_COLOUR: [f32; 4] = [0.65, 0.7, 0.75, 0.125];

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum CompileError {
    SourceExtent,
    UnqualifiedTexcoord { unit: usize, control: u32 },
    ProgramExtent,
    Unterminated,
    TrailingBytes,
    UnqualifiedFlow,
    Mapping,
    Bounds,
    UnknownVertexConstant(u32),
    VertexConstantRange(u32),
    Decode(String),
    Unsupported(&'static str),
    Native(String),
    UniformLayout,
    MissingCurrentAttribute(u32),
    VertexInputLocation(u32),
    ArrayMaskMismatch { compiled: u16, source: u16 },
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum Variant {
    Source,
    StoredDepth,
    BaseColour,
    Flat,
}

/// Source checks: extent and texture-coordinate controls. Skipped for flat.
pub fn source_checks(
    variant: Variant,
    vertex_instructions: usize,
    fragment_bytes: usize,
    semantic_bytes: usize,
    transfer_bytes: usize,
    texcoord_controls: &[u32; 10],
) -> Result<(), CompileError> {
    if variant == Variant::Flat {
        return Ok(());
    }
    if vertex_instructions > 512
        || (fragment_bytes != semantic_bytes && fragment_bytes != transfer_bytes)
    {
        return Err(CompileError::SourceExtent);
    }
    if let Some((unit, control)) = texcoord_controls
        .iter()
        .enumerate()
        .find(|(_, c)| **c & !0x11 != 0)
    {
        return Err(CompileError::UnqualifiedTexcoord {
            unit,
            control: *control,
        });
    }
    Ok(())
}

fn exchanged(bytes: &[u8], at: usize) -> u32 {
    be_u32(bytes, at).rotate_left(16)
}

/// The inline constant rows of a fragment program, in program order.
#[derive(Clone, Debug, PartialEq, Eq, Hash)]
pub struct InlineLayout {
    /// (byte offset in the program, seed words).
    pub rows: Vec<(usize, [u32; 4])>,
}

/// Scans fragment microcode for inline constant rows.
pub fn inline_layout(microcode: &[u8]) -> Result<InlineLayout, CompileError> {
    if microcode.is_empty() || !microcode.len().is_multiple_of(16) {
        return Err(CompileError::ProgramExtent);
    }
    let mut rows = Vec::new();
    let mut at = 0;
    loop {
        if at + 16 > microcode.len() {
            return Err(CompileError::Unterminated);
        }
        let words: [u32; 4] = std::array::from_fn(|i| exchanged(microcode, at + 4 * i));
        if words[2] >> 31 != 0 {
            return Err(CompileError::UnqualifiedFlow);
        }
        let last = words[0] & 1 != 0;
        at += 16;
        if words[1..].iter().any(|w| w & 3 == 2) {
            if at + 16 > microcode.len() {
                return Err(CompileError::Unterminated);
            }
            rows.push((
                at,
                std::array::from_fn(|i| exchanged(microcode, at + 4 * i)),
            ));
            at += 16;
        }
        if last {
            break;
        }
    }
    if at != microcode.len() {
        return Err(CompileError::TrailingBytes);
    }
    Ok(InlineLayout { rows })
}

/// For each logical slot of the domain, the inline rows it maps to.
pub fn relocation_map(
    directory: &[u8],
    layout: &InlineLayout,
    domain: usize,
) -> Result<Vec<Vec<usize>>, CompileError> {
    let mut map = Vec::with_capacity(domain);
    for slot in 0..domain {
        let entry = 24 + 4 * slot;
        if entry + 4 > directory.len() {
            return Err(CompileError::Bounds);
        }
        let list = be_u32(directory, entry) as usize;
        if list + 2 > directory.len() {
            return Err(CompileError::Bounds);
        }
        let count = be_u16(directory, list) as usize;
        if list + 2 + 2 * count > directory.len() {
            return Err(CompileError::Bounds);
        }
        let mut rows = Vec::with_capacity(count);
        for k in 0..count {
            let value = be_u16(directory, list + 2 + 2 * k) as usize;
            let offset = (16 * value) % 65536;
            let row = layout
                .rows
                .iter()
                .position(|(at, _)| *at == offset)
                .ok_or(CompileError::Mapping)?;
            rows.push(row);
        }
        map.push(rows);
    }
    Ok(map)
}

/// The logical-slot domain of a list of writes.
pub fn slot_domain(writes: &[(u32, [u32; 4])]) -> usize {
    writes
        .iter()
        .map(|(slot, _)| *slot as usize + 1)
        .max()
        .unwrap_or(0)
}

/// Replaces NaN lanes and lanes whose magnitude exceeds `3.0e38` with `+0`.
pub fn sanitise(row: [u32; 4], replaced: &mut u32) -> [u32; 4] {
    row.map(|word| {
        let value = f32::from_bits(word);
        if value.is_nan() || value.abs() > f32::from_bits(SANITISE_LIMIT) {
            *replaced += 1;
            0
        } else {
            word
        }
    })
}

/// Evaluates inline rows: sanitised seeds, then sanitised writes in order (later wins).
/// The count is of replacements in the inputs: every seed lane, even when later overwritten, plus
/// each write's lanes once per write, however many rows the write maps to.
pub fn evaluate_inline_rows(
    layout: &InlineLayout,
    map: &[Vec<usize>],
    writes: &[(u32, [u32; 4])],
) -> Result<(Vec<[u32; 4]>, u32), CompileError> {
    let mut replaced = 0;
    let mut rows: Vec<[u32; 4]> = layout
        .rows
        .iter()
        .map(|(_, seed)| sanitise(*seed, &mut replaced))
        .collect();
    for (slot, value) in writes {
        let targets = map.get(*slot as usize).ok_or(CompileError::Mapping)?;
        let value = sanitise(*value, &mut replaced);
        for row in targets {
            rows[*row] = value;
        }
    }
    Ok((rows, replaced))
}

/// The constant rows a vertex program reads.
#[derive(Clone, Debug, PartialEq, Eq, Hash)]
pub enum ConstantReads {
    Indexed,
    Rows(Vec<u32>),
}

/// Checks the read rows and packs the vertex uniform.
pub fn vertex_uniform(
    bank: &[Option<[u32; 4]>],
    reads: &ConstantReads,
    viewport_scale: [u32; 4],
    viewport_offset: [u32; 4],
    current_attribute_mask: u16,
    current: &[Option<[u32; 4]>; 16],
) -> Result<Vec<u8>, CompileError> {
    match reads {
        ConstantReads::Indexed => {
            if let Some(row) = (0..512).find(|row| bank.get(*row).copied().flatten().is_none()) {
                return Err(CompileError::UnknownVertexConstant(row as u32));
            }
        }
        ConstantReads::Rows(rows) => {
            let mut sorted = rows.clone();
            sorted.sort_unstable();
            for row in sorted {
                if row >= 512 {
                    return Err(CompileError::VertexConstantRange(row));
                }
                if bank.get(row as usize).copied().flatten().is_none() {
                    return Err(CompileError::UnknownVertexConstant(row));
                }
            }
        }
    }
    let mut bytes = Vec::with_capacity(8480);
    let mut push = |row: [u32; 4]| {
        row.iter()
            .for_each(|w| bytes.extend_from_slice(&w.to_le_bytes()))
    };
    for row in 0..512 {
        push(bank.get(row).copied().flatten().unwrap_or([0; 4]));
    }
    push(viewport_scale);
    push(viewport_offset);
    if current_attribute_mask != 0 {
        for (input, value) in current.iter().enumerate() {
            if current_attribute_mask & (1 << input) == 0 {
                push([0; 4]);
            } else {
                push(value.ok_or(CompileError::MissingCurrentAttribute(input as u32))?);
            }
        }
    }
    Ok(bytes)
}

/// Packs the fragment uniform: a zero row when there are no inline rows, the rows, the alpha
/// window.
pub fn fragment_uniform(rows: &[[u32; 4]], alpha_window: [u32; 4]) -> Vec<u8> {
    let mut bytes = Vec::with_capacity(16 * (rows.len() + 2));
    let mut push = |row: &[u32; 4]| {
        row.iter()
            .for_each(|w| bytes.extend_from_slice(&w.to_le_bytes()))
    };
    if rows.is_empty() {
        push(&[0; 4]);
    }
    rows.iter().for_each(&mut push);
    push(&alpha_window);
    bytes
}

/// Effective shader controls (every field from effective state, no defaults).
#[derive(Clone, Debug, PartialEq, Eq, Hash)]
pub struct ShaderControls {
    pub vertex_texture_mask: u16,
    pub enabled_arrays: u16,
    pub cube_mask: u16,
    pub volume_mask: u16,
    pub unnormalized_mask: u16,
    /// Per unit: (enabled, function, linear, unnormalised).
    pub shadow: [(u32, u32, u32, u32); 16],
    pub gamma: [u32; 16],
    pub expand: [u32; 16],
    /// `None`, or `0x200 + code`.
    pub alpha_function: Option<u32>,
    pub srgb_output: bool,
    pub sample_grid: (u32, u32),
    pub texture_dimensions: [(u32, u32); 16],
}

/// A technique's sources.
#[derive(Clone, Debug, PartialEq, Eq, Hash)]
pub struct TechniqueSource {
    pub vertex_instructions: Vec<[u32; 4]>,
    pub load_slot: u32,
    pub declared_input_mask: u16,
    pub declared_output_mask: u32,
    pub transform_word: u32,
    pub fragment_bytes: Vec<u8>,
    pub semantic_bytes: usize,
    pub transfer_bytes: usize,
    pub shader_control: u32,
    pub texcoord_controls: [u32; 10],
    pub directory: Vec<u8>,
}

/// Effective values, checked and packed on every compile.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct EffectiveValues {
    pub vertex_bank: Vec<Option<[u32; 4]>>,
    pub viewport_scale: [u32; 4],
    pub viewport_offset: [u32; 4],
    pub current: [Option<[u32; 4]>; 16],
    pub fragment_writes: Vec<(u32, [u32; 4])>,
    pub alpha_window: [u32; 4],
}

impl EffectiveValues {
    pub fn known_current_mask(&self) -> u16 {
        (0..16)
            .filter(|i| self.current[*i].is_some())
            .fold(0, |m, i| m | (1 << i))
    }
}

/// What the fragment translator reports.
#[derive(Clone, Debug, PartialEq, Eq, Hash)]
pub struct FragmentTranslation {
    pub program: Vec<u8>,
    pub constant_slots: usize,
    pub constant_buffer_bytes: usize,
    /// Units sampled and the sampler each uses (base-colour check).
    pub textures: Vec<(u32, u32)>,
    pub needs_stored_depth_arguments: bool,
}

/// What the vertex translator reports.
#[derive(Clone, Debug, PartialEq, Eq, Hash)]
pub struct VertexTranslation {
    pub program: Vec<u8>,
    pub input_mask: u16,
    pub current_attribute_mask: u16,
    pub constant_buffer_bytes: usize,
    pub reads: ConstantReads,
    pub samples_textures: bool,
}

/// A lowered native program.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct NativeProgram {
    pub bytes: Vec<u8>,
    /// Vertex input locations (vertex stage only).
    pub vertex_input_locations: Vec<u32>,
}

/// The `mag-render-programs` contract.
pub trait ProgramTranslator {
    fn translate_fragment(
        &self,
        microcode: &[u8],
        shader_control: u32,
        controls: &ShaderControls,
        texcoord_controls: &[u32; 10],
        variant: Variant,
    ) -> Result<FragmentTranslation, CompileError>;
    /// Translates with the given enabled arrays and known current mask. Missing inputs are the
    /// translator's `MissingCurrentAttribute` error.
    fn translate_vertex(
        &self,
        instructions: &[[u32; 4]],
        load_slot: u32,
        vertex_texture_mask: u16,
        enabled_arrays: u16,
        known_current: u16,
    ) -> Result<VertexTranslation, CompileError>;
    fn lower_vertex(&self, translation: &VertexTranslation) -> Result<NativeProgram, CompileError>;
    fn lower_fragment(
        &self,
        translation: &FragmentTranslation,
        stored_depth: bool,
    ) -> Result<NativeProgram, CompileError>;
}

/// A compiled program pair.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct CompiledPair {
    pub vertex: NativeProgram,
    pub fragment: NativeProgram,
    pub vertex_uniform: Vec<u8>,
    pub fragment_uniform: Vec<u8>,
    pub declared_vertex_input_mask: u16,
    pub declared_vertex_output_mask: u32,
    pub actual_vertex_input_mask: u16,
    pub enabled_vertex_arrays: u16,
    pub transform_word: u32,
    pub sanitized_fragment_words: u32,
}

impl CompiledPair {
    /// The vertex arrays the program requires, checked against the draw's effective mask.
    pub fn required_arrays(&self, effective_mask: u16) -> Result<u16, CompileError> {
        let mut mask = 0u16;
        for location in &self.vertex.vertex_input_locations {
            if *location >= 16 {
                return Err(CompileError::VertexInputLocation(*location));
            }
            mask |= 1 << location;
        }
        if effective_mask != self.enabled_vertex_arrays {
            return Err(CompileError::ArrayMaskMismatch {
                compiled: self.enabled_vertex_arrays,
                source: effective_mask,
            });
        }
        Ok(mask)
    }
}

/// The prepared-technique cache key.
#[derive(Clone, Debug, PartialEq, Eq, Hash)]
pub struct PreparedKey {
    pub technique: TechniqueSource,
    pub variant: Variant,
    pub known_current: u16,
    pub domain: usize,
    pub controls: ShaderControls,
}

#[derive(Clone, Debug)]
struct Prepared {
    fragment: Option<(FragmentTranslation, InlineLayout, Vec<Vec<usize>>)>,
    vertex: VertexTranslation,
}

/// Compiles program pairs, optionally reusing prepared techniques. Values are never cached.
pub struct ProgramCompiler<T: ProgramTranslator> {
    pub translator: T,
    prepared: Option<HashMap<PreparedKey, Prepared>>,
    /// Retention bound; exceeding it clears the table.
    pub capacity: usize,
}

impl<T: ProgramTranslator> ProgramCompiler<T> {
    pub fn new(translator: T, reuse: bool) -> Self {
        Self {
            translator,
            prepared: reuse.then(HashMap::new),
            capacity: 256,
        }
    }

    fn prepare(
        &self,
        technique: &TechniqueSource,
        variant: Variant,
        controls: &ShaderControls,
        values: &EffectiveValues,
        domain: usize,
    ) -> Result<Prepared, CompileError> {
        source_checks(
            variant,
            technique.vertex_instructions.len(),
            technique.fragment_bytes.len(),
            technique.semantic_bytes,
            technique.transfer_bytes,
            &technique.texcoord_controls,
        )?;
        let fragment = if variant == Variant::Flat {
            None
        } else {
            let microcode = &technique.fragment_bytes[..technique.semantic_bytes];
            let translation = self.translator.translate_fragment(
                microcode,
                technique.shader_control,
                controls,
                &technique.texcoord_controls,
                variant,
            )?;
            if variant == Variant::BaseColour
                && (translation.textures != [(0, 0)] || translation.needs_stored_depth_arguments)
            {
                return Err(CompileError::Unsupported("base-colour resources"));
            }
            let layout = inline_layout(microcode)?;
            if layout.rows.len() != translation.constant_slots {
                return Err(CompileError::UniformLayout);
            }
            let map = relocation_map(&technique.directory, &layout, domain)?;
            Some((translation, layout, map))
        };
        let vertex = self.translator.translate_vertex(
            &technique.vertex_instructions,
            technique.load_slot,
            controls.vertex_texture_mask,
            controls.enabled_arrays,
            values.known_current_mask(),
        )?;
        Ok(Prepared { fragment, vertex })
    }

    /// Compiles one technique with its controls and values.
    pub fn compile(
        &mut self,
        technique: &TechniqueSource,
        variant: Variant,
        controls: &ShaderControls,
        values: &EffectiveValues,
    ) -> Result<CompiledPair, CompileError> {
        let domain = if variant == Variant::Flat {
            0
        } else {
            slot_domain(&values.fragment_writes)
        };
        let key = PreparedKey {
            technique: technique.clone(),
            variant,
            known_current: values.known_current_mask(),
            domain,
            controls: controls.clone(),
        };
        let cached = self
            .prepared
            .as_ref()
            .and_then(|table| table.get(&key))
            .cloned();
        let prepared = match cached {
            Some(prepared) => prepared,
            None => {
                let prepared = self.prepare(technique, variant, controls, values, domain)?;
                if let Some(table) = &mut self.prepared {
                    if table.len() >= self.capacity {
                        table.clear();
                    }
                    table.insert(key, prepared.clone());
                }
                prepared
            }
        };

        // Values: checked and packed on every compile.
        let (fragment_rows, sanitized, fragment_translation) = match &prepared.fragment {
            Some((translation, layout, map)) => {
                let (rows, sanitized) = evaluate_inline_rows(layout, map, &values.fragment_writes)?;
                (rows, sanitized, Some(translation))
            }
            None => (Vec::new(), 0, None),
        };
        let vertex = &prepared.vertex;
        let vertex_uniform = vertex_uniform(
            &values.vertex_bank,
            &vertex.reads,
            values.viewport_scale,
            values.viewport_offset,
            vertex.current_attribute_mask,
            &values.current,
        )?;
        if vertex_uniform.len() != vertex.constant_buffer_bytes {
            return Err(CompileError::UniformLayout);
        }
        let fragment_uniform = match fragment_translation {
            Some(translation) => {
                let bytes = fragment_uniform(&fragment_rows, values.alpha_window);
                if bytes.len() != translation.constant_buffer_bytes {
                    return Err(CompileError::UniformLayout);
                }
                bytes
            }
            None => Vec::new(),
        };
        let native_vertex = self.translator.lower_vertex(vertex)?;
        let native_fragment = match fragment_translation {
            Some(translation) => self
                .translator
                .lower_fragment(translation, variant == Variant::StoredDepth)?,
            None => self.translator.lower_fragment(&flat_fragment(), false)?,
        };
        Ok(CompiledPair {
            vertex: native_vertex,
            fragment: native_fragment,
            vertex_uniform,
            fragment_uniform,
            declared_vertex_input_mask: technique.declared_input_mask,
            declared_vertex_output_mask: technique.declared_output_mask,
            actual_vertex_input_mask: vertex.input_mask,
            enabled_vertex_arrays: controls.enabled_arrays,
            transform_word: technique.transform_word,
            sanitized_fragment_words: sanitized,
        })
    }
}

/// The flat variant's fixed fragment: no inputs, no constants, a constant colour.
pub fn flat_fragment() -> FragmentTranslation {
    FragmentTranslation {
        program: FLAT_COLOUR.iter().flat_map(|v| v.to_le_bytes()).collect(),
        constant_slots: 0,
        constant_buffer_bytes: 0,
        textures: Vec::new(),
        needs_stored_depth_arguments: false,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::cell::Cell;

    /// Stores a host word as the half-exchanged big-endian form.
    fn store(word: u32) -> [u8; 4] {
        word.rotate_left(16).to_be_bytes()
    }

    fn instruction(last: bool, constant: bool, flow: bool) -> Vec<u8> {
        let words = [
            u32::from(last),
            if constant { 2 } else { 1 },
            if flow { 1 << 31 } else { 0 },
            0,
        ];
        words.iter().flat_map(|w| store(*w)).collect()
    }

    fn constant(row: [u32; 4]) -> Vec<u8> {
        row.iter().flat_map(|w| store(*w)).collect()
    }

    /// instruction, constant (row 0 at 16), instruction, constant (row 1 at 48), instruction
    /// (last), constant (row 2 at 80).
    fn program() -> Vec<u8> {
        let mut p = Vec::new();
        p.extend(instruction(false, true, false));
        p.extend(constant([1, 2, 3, 4]));
        p.extend(instruction(false, true, false));
        p.extend(constant([5, 6, 7, 8]));
        p.extend(instruction(true, true, false));
        p.extend(constant([9, 10, 11, 12]));
        p
    }

    fn directory(slots: &[&[u16]]) -> Vec<u8> {
        let mut d = vec![0u8; 24 + 4 * slots.len()];
        for (slot, values) in slots.iter().enumerate() {
            let offset = d.len() as u32;
            d[24 + 4 * slot..28 + 4 * slot].copy_from_slice(&offset.to_be_bytes());
            d.extend((values.len() as u16).to_be_bytes());
            for v in *values {
                d.extend(v.to_be_bytes());
            }
        }
        d
    }

    #[test]
    fn inline_layout_and_errors() {
        let layout = inline_layout(&program()).unwrap();
        assert_eq!(
            layout.rows.iter().map(|r| r.0).collect::<Vec<_>>(),
            vec![16, 48, 80]
        );
        assert_eq!(layout.rows[1].1, [5, 6, 7, 8]);
        let mut trailing = program();
        trailing.extend(instruction(false, false, false));
        assert_eq!(inline_layout(&trailing), Err(CompileError::TrailingBytes));
        assert_eq!(
            inline_layout(&instruction(false, false, false)),
            Err(CompileError::Unterminated)
        );
        assert_eq!(
            inline_layout(&instruction(true, false, true)),
            Err(CompileError::UnqualifiedFlow)
        );
        assert_eq!(inline_layout(&[0; 8]), Err(CompileError::ProgramExtent));
        assert_eq!(
            inline_layout(&instruction(true, false, false))
                .unwrap()
                .rows
                .len(),
            0
        );
    }

    #[test]
    fn v4_sanitise() {
        let layout = inline_layout(&program()).unwrap();
        let map = relocation_map(&directory(&[&[3]]), &layout, 1).unwrap();
        let write = [
            f32::NAN.to_bits(),
            3.1e38f32.to_bits(),
            0x8000_0000,
            0x3f80_0000,
        ];
        let (rows, sanitized) = evaluate_inline_rows(&layout, &map, &[(0, write)]).unwrap();
        assert_eq!(rows[1], [0, 0, 0x8000_0000, 0x3f80_0000]);
        assert_eq!(sanitized, 2);
        let mut count = 0;
        assert_eq!(
            sanitise(
                [f32::INFINITY.to_bits(), 1, 2.9e38f32.to_bits(), 0],
                &mut count
            )[0],
            0
        );
        assert_eq!(count, 1);
    }

    #[test]
    fn sanitise_threshold_and_counting() {
        let limit = SANITISE_LIMIT;
        let mut count = 0;
        assert_eq!(
            sanitise([limit, limit | 0x8000_0000, limit + 1, 0], &mut count),
            [limit, limit | 0x8000_0000, 0, 0]
        );
        assert_eq!(count, 1);
        // Seeds count even when overwritten; a write counts once although it maps to two rows,
        // and an overwritten write still counts.
        let mut p = Vec::new();
        p.extend(instruction(false, true, false));
        p.extend(constant([f32::NAN.to_bits(), 1, 2, 3]));
        p.extend(instruction(true, true, false));
        p.extend(constant([4, 5, 6, 7]));
        let layout = inline_layout(&p).unwrap();
        let map = relocation_map(&directory(&[&[1, 3]]), &layout, 1).unwrap();
        let nan_write = [f32::NAN.to_bits(), 0, 0, 0];
        let (rows, replaced) =
            evaluate_inline_rows(&layout, &map, &[(0, nan_write), (0, [9; 4])]).unwrap();
        assert_eq!(rows, vec![[9; 4], [9; 4]]);
        assert_eq!(replaced, 2);
    }

    #[test]
    fn v5_later_write_wins() {
        let layout = inline_layout(&program()).unwrap();
        let map = relocation_map(&directory(&[&[1, 5]]), &layout, 1).unwrap();
        let (rows, _) = evaluate_inline_rows(&layout, &map, &[(0, [7; 4]), (0, [9; 4])]).unwrap();
        assert_eq!(rows, vec![[9; 4], [5, 6, 7, 8], [9; 4]]);
    }

    #[test]
    fn v6_directory_offset() {
        let layout = inline_layout(&program()).unwrap();
        assert_eq!(
            relocation_map(&directory(&[&[3]]), &layout, 1).unwrap(),
            vec![vec![1]]
        );
        assert_eq!(
            relocation_map(&directory(&[&[2]]), &layout, 1),
            Err(CompileError::Mapping)
        );
        // (16 · x) mod 65536.
        assert_eq!(
            relocation_map(&directory(&[&[4096 + 3]]), &layout, 1).unwrap(),
            vec![vec![1]]
        );
        assert_eq!(
            relocation_map(&directory(&[]), &layout, 1),
            Err(CompileError::Bounds)
        );
        let map = relocation_map(&directory(&[&[1]]), &layout, 1).unwrap();
        assert_eq!(
            evaluate_inline_rows(&layout, &map, &[(1, [0; 4])]),
            Err(CompileError::Mapping)
        );
        assert_eq!(slot_domain(&[(0, [0; 4]), (4, [0; 4])]), 5);
        assert_eq!(slot_domain(&[]), 0);
    }

    #[test]
    fn v8_v9_source_checks() {
        let mut controls = [0u32; 10];
        controls[3] = 0x02;
        assert_eq!(
            source_checks(Variant::Source, 1, 64, 64, 80, &controls),
            Err(CompileError::UnqualifiedTexcoord {
                unit: 3,
                control: 2
            })
        );
        controls[3] = 0x11;
        assert_eq!(
            source_checks(Variant::Source, 1, 64, 64, 80, &controls),
            Ok(())
        );
        assert_eq!(
            source_checks(Variant::Source, 1, 72, 64, 80, &controls),
            Err(CompileError::SourceExtent)
        );
        assert_eq!(
            source_checks(Variant::Source, 1, 80, 64, 80, &controls),
            Ok(())
        );
        assert_eq!(
            source_checks(Variant::Source, 513, 64, 64, 80, &controls),
            Err(CompileError::SourceExtent)
        );
        assert_eq!(
            source_checks(Variant::Flat, 513, 72, 64, 80, &[0xff; 10]),
            Ok(())
        );
    }

    fn known_bank() -> Vec<Option<[u32; 4]>> {
        vec![Some([1; 4]); 512]
    }

    #[test]
    fn v1_v2_vertex_uniform() {
        let current: [Option<[u32; 4]>; 16] = [None; 16];
        let reads = ConstantReads::Rows(vec![0]);
        let bytes = vertex_uniform(&known_bank(), &reads, [2; 4], [3; 4], 0, &current).unwrap();
        assert_eq!(bytes.len(), 8224);
        assert_eq!(&bytes[512 * 16..512 * 16 + 4], &2u32.to_le_bytes());
        assert_eq!(&bytes[513 * 16..513 * 16 + 4], &3u32.to_le_bytes());
        let mut current = current;
        current[6] = Some([0, 0x3f80_0000, 0, 0x3f00_0000]);
        let bytes =
            vertex_uniform(&known_bank(), &reads, [2; 4], [3; 4], 0x0040, &current).unwrap();
        assert_eq!(bytes.len(), 8480);
        let words: Vec<u32> = bytes[520 * 16..521 * 16]
            .chunks(4)
            .map(|c| u32::from_le_bytes(c.try_into().unwrap()))
            .collect();
        assert_eq!(words, vec![0, 0x3f80_0000, 0, 0x3f00_0000]);
        assert!(bytes[514 * 16..520 * 16].iter().all(|b| *b == 0));
        current[6] = None;
        assert_eq!(
            vertex_uniform(&known_bank(), &reads, [2; 4], [3; 4], 0x0040, &current),
            Err(CompileError::MissingCurrentAttribute(6))
        );
    }

    #[test]
    fn v3_empty_inline_rows() {
        let bytes = fragment_uniform(&[], [0x3f00_0000, 0, 0, 0]);
        assert_eq!(bytes.len(), 32);
        assert!(bytes[..16].iter().all(|b| *b == 0));
        assert_eq!(&bytes[16..20], &[0, 0, 0, 0x3f]);
    }

    #[test]
    fn v10_indexed_constants() {
        let mut bank = known_bank();
        bank[300] = None;
        let current = [None; 16];
        assert_eq!(
            vertex_uniform(&bank, &ConstantReads::Indexed, [0; 4], [0; 4], 0, &current),
            Err(CompileError::UnknownVertexConstant(300))
        );
        assert!(
            vertex_uniform(
                &bank,
                &ConstantReads::Rows(vec![299, 5]),
                [0; 4],
                [0; 4],
                0,
                &current
            )
            .is_ok()
        );
        assert_eq!(
            vertex_uniform(
                &bank,
                &ConstantReads::Rows(vec![512]),
                [0; 4],
                [0; 4],
                0,
                &current
            ),
            Err(CompileError::VertexConstantRange(512))
        );
    }

    /// A translator stub that reports a fixed shape and counts translations.
    struct Stub {
        translations: Cell<u32>,
        reads: ConstantReads,
        inputs: Vec<u32>,
    }

    impl ProgramTranslator for Stub {
        fn translate_fragment(
            &self,
            microcode: &[u8],
            _: u32,
            _: &ShaderControls,
            _: &[u32; 10],
            _: Variant,
        ) -> Result<FragmentTranslation, CompileError> {
            self.translations.set(self.translations.get() + 1);
            let slots = inline_layout(microcode)?.rows.len();
            Ok(FragmentTranslation {
                program: microcode.to_vec(),
                constant_slots: slots,
                constant_buffer_bytes: 16 * (slots.max(1) + 1),
                textures: vec![(0, 0)],
                needs_stored_depth_arguments: false,
            })
        }
        fn translate_vertex(
            &self,
            _: &[[u32; 4]],
            _: u32,
            _: u16,
            enabled: u16,
            known: u16,
        ) -> Result<VertexTranslation, CompileError> {
            self.translations.set(self.translations.get() + 1);
            let input_mask = self.inputs.iter().fold(0u16, |m, i| m | (1 << i));
            if let Some(missing) = (0..16).find(|i| input_mask & !enabled & !known & (1 << i) != 0)
            {
                return Err(CompileError::MissingCurrentAttribute(missing));
            }
            let current = input_mask & !enabled;
            Ok(VertexTranslation {
                program: Vec::new(),
                input_mask,
                current_attribute_mask: current,
                constant_buffer_bytes: if current != 0 { 8480 } else { 8224 },
                reads: self.reads.clone(),
                samples_textures: false,
            })
        }
        fn lower_vertex(&self, _: &VertexTranslation) -> Result<NativeProgram, CompileError> {
            Ok(NativeProgram {
                bytes: Vec::new(),
                vertex_input_locations: self.inputs.clone(),
            })
        }
        fn lower_fragment(
            &self,
            t: &FragmentTranslation,
            _: bool,
        ) -> Result<NativeProgram, CompileError> {
            Ok(NativeProgram {
                bytes: t.program.clone(),
                vertex_input_locations: Vec::new(),
            })
        }
    }

    fn controls(enabled_arrays: u16) -> ShaderControls {
        ShaderControls {
            vertex_texture_mask: 0,
            enabled_arrays,
            cube_mask: 0,
            volume_mask: 0,
            unnormalized_mask: 0,
            shadow: [(0, 0, 0, 0); 16],
            gamma: [0; 16],
            expand: [0; 16],
            alpha_function: None,
            srgb_output: false,
            sample_grid: (1, 1),
            texture_dimensions: [(1, 1); 16],
        }
    }

    fn technique() -> TechniqueSource {
        TechniqueSource {
            vertex_instructions: vec![[0; 4]],
            load_slot: 0,
            declared_input_mask: 0x0101,
            declared_output_mask: 1,
            transform_word: 7,
            fragment_bytes: program(),
            semantic_bytes: 96,
            transfer_bytes: 96,
            shader_control: 0,
            texcoord_controls: [0; 10],
            directory: directory(&[&[1]]),
        }
    }

    fn values() -> EffectiveValues {
        EffectiveValues {
            vertex_bank: known_bank(),
            viewport_scale: [0; 4],
            viewport_offset: [0; 4],
            current: [None; 16],
            fragment_writes: vec![(0, [4; 4])],
            alpha_window: [0; 4],
        }
    }

    #[test]
    fn v11_array_mask() {
        let stub = Stub {
            translations: Cell::new(0),
            reads: ConstantReads::Rows(vec![0]),
            inputs: vec![0, 8],
        };
        let mut compiler = ProgramCompiler::new(stub, false);
        let pair = compiler
            .compile(&technique(), Variant::Source, &controls(0x0101), &values())
            .unwrap();
        assert_eq!(
            pair.required_arrays(0x0103),
            Err(CompileError::ArrayMaskMismatch {
                compiled: 0x0101,
                source: 0x0103
            })
        );
        assert_eq!(pair.required_arrays(0x0101), Ok(0x0101));
        assert_eq!(pair.transform_word, 7);
        assert_eq!(pair.fragment_uniform.len(), 16 * 4);
        assert_eq!(&pair.fragment_uniform[..4], &4u32.to_le_bytes());
    }

    #[test]
    fn v12_reuse_equivalence() {
        let stub = Stub {
            translations: Cell::new(0),
            reads: ConstantReads::Rows(vec![7]),
            inputs: vec![0],
        };
        let mut compiler = ProgramCompiler::new(stub, true);
        let first = compiler
            .compile(&technique(), Variant::Source, &controls(0x0001), &values())
            .unwrap();
        let mut changed = values();
        changed.vertex_bank[7] = Some([9; 4]);
        let second = compiler
            .compile(&technique(), Variant::Source, &controls(0x0001), &changed)
            .unwrap();
        assert_eq!(compiler.translator.translations.get(), 2);
        assert_ne!(first.vertex_uniform, second.vertex_uniform);
        assert_eq!(
            &second.vertex_uniform[7 * 16..7 * 16 + 4],
            &9u32.to_le_bytes()
        );
        changed.vertex_bank[7] = None;
        assert_eq!(
            compiler.compile(&technique(), Variant::Source, &controls(0x0001), &changed),
            Err(CompileError::UnknownVertexConstant(7))
        );
        assert_eq!(compiler.translator.translations.get(), 2);
        // A fresh compile gives the same result and the same error.
        let mut fresh = ProgramCompiler::new(
            Stub {
                translations: Cell::new(0),
                reads: ConstantReads::Rows(vec![7]),
                inputs: vec![0],
            },
            false,
        );
        assert_eq!(
            fresh.compile(&technique(), Variant::Source, &controls(0x0001), &changed),
            Err(CompileError::UnknownVertexConstant(7))
        );
    }

    #[test]
    fn missing_current_and_flat() {
        let stub = Stub {
            translations: Cell::new(0),
            reads: ConstantReads::Rows(vec![]),
            inputs: vec![0, 6],
        };
        let mut compiler = ProgramCompiler::new(stub, false);
        assert_eq!(
            compiler.compile(&technique(), Variant::Source, &controls(0x0001), &values()),
            Err(CompileError::MissingCurrentAttribute(6))
        );
        let mut with_current = values();
        with_current.current[6] = Some([0, 0x3f80_0000, 0, 0x3f00_0000]);
        let pair = compiler
            .compile(
                &technique(),
                Variant::Source,
                &controls(0x0001),
                &with_current,
            )
            .unwrap();
        assert_eq!(pair.vertex_uniform.len(), 8480);
        // Flat ignores fragment bytes and writes entirely.
        let mut broken = technique();
        broken.fragment_bytes = vec![1, 2, 3];
        let pair = compiler
            .compile(&broken, Variant::Flat, &controls(0x0001), &with_current)
            .unwrap();
        assert!(pair.fragment_uniform.is_empty());
        assert_eq!(pair.sanitized_fragment_words, 0);
    }
}
