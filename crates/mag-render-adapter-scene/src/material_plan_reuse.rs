//! Opt-in cross-plan reuse of value-independent program preparation
//! (`MAG_RENDER_PLAN_REUSE=1`). Pass-generic: any planner that compiles
//! source techniques through `NativeCompilerCache` can use it.
//!
//! What is reused, and why that is exact:
//! - A technique identity names one exact source program/fragment byte
//!   string. A sampled key only picks candidates; every reuse is authorised by
//!   full equality with the retained copy, re-checked on every plan.
//! - A prepared program entry is the result of the source checks and immutable
//!   decoding (`decode_technique`) plus the fragment constant layout. Those are
//!   pure functions of the technique, every shader control, the known-current
//!   mask, the fragment mode/lowering and the logical fragment row count, which
//!   together form the key. Only successful decodes are retained; a later
//!   value-dependent refusal is recomputed on every use.
//! - Nothing that depends on values is retained: every hit still evaluates the
//!   fragment constants, checks and packs the actual vertex constant bank,
//!   current attributes, viewport and alpha rows, and checks the uniform
//!   layout, in the same order and with the same errors as a full compile.
use super::{DecodedTechnique, EffectiveShaderControls, FragmentMode, Lowering, TechniquePrograms};
use crate::fast_hash::FastHashMap;
use crate::material_constants::FragmentConstantLayout;
use std::{hash::Hasher, sync::Arc};

/// Retention bounds. Exceeding one clears that table (counted as a reset);
/// technique identities are never reused, so a cleared entry only misses.
const MAX_TECHNIQUES: usize = 8192;
const MAX_TECHNIQUE_BYTES: usize = 64 * 1024 * 1024;
const MAX_PROGRAMS: usize = 8192;
const MAX_REQUIREMENTS: usize = 8192;

fn enabled_from_environment() -> bool {
    static MODE: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    *MODE.get_or_init(|| {
        let on = std::env::var_os("MAG_RENDER_PLAN_REUSE").is_some_and(|v| v == "1");
        if on {
            eprintln!("[mag main plan reuse] enabled=1");
        }
        on
    })
}

/// Cumulative counters; callers report per-plan differences.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct PlanReuseStats {
    /// Techniques found by exact content equality / newly identified.
    pub technique_hits: u64,
    pub technique_misses: u64,
    /// Prepared programs reused / decoded afresh.
    pub program_hits: u64,
    pub program_misses: u64,
    /// Structural vertex requirement and fragment texture-unit lookups.
    pub requirement_hits: u64,
    pub requirement_misses: u64,
    /// Table clears forced by the retention bounds.
    pub resets: u64,
}
impl PlanReuseStats {
    pub fn since(&self, earlier: &Self) -> Self {
        Self {
            technique_hits: self.technique_hits - earlier.technique_hits,
            technique_misses: self.technique_misses - earlier.technique_misses,
            program_hits: self.program_hits - earlier.program_hits,
            program_misses: self.program_misses - earlier.program_misses,
            requirement_hits: self.requirement_hits - earlier.requirement_hits,
            requirement_misses: self.requirement_misses - earlier.requirement_misses,
            resets: self.resets - earlier.resets,
        }
    }
}

/// One exactly identified technique, borrowed from the plan's own sources.
/// Only `NativeCompilerCache::reuse_technique` creates it, so the id always
/// names exactly these bytes.
#[derive(Clone, Copy)]
pub struct ReusedTechnique<'a> {
    pub(super) id: u64,
    pub(super) source: &'a TechniquePrograms,
    pub(super) fragment: &'a [u8],
}
impl ReusedTechnique<'_> {
    pub fn source(&self) -> &TechniquePrograms {
        self.source
    }
    pub fn fragment(&self) -> &[u8] {
        self.fragment
    }
}

struct TechniqueEntry {
    source: TechniquePrograms,
    fragment: Vec<u8>,
    id: u64,
}
fn technique_bytes(source: &TechniquePrograms, fragment: &[u8]) -> usize {
    source.vertex.instructions.len() * 16
        + source.vertex.static_constants.len() * 20
        + source.fragment.relocation_directory.len()
        + fragment.len()
        + 256
}
/// Candidate selector only (exact equality authorises reuse): lengths, the
/// small headers and up to 64 words/bytes from each end of every array.
fn technique_probe(source: &TechniquePrograms, fragment: &[u8]) -> u64 {
    let mut h = crate::fast_hash::FastHasher::default();
    let v = &source.vertex;
    let f = &source.fragment;
    for w in v.header_words.iter().chain(&f.header_words) {
        h.write_u32(*w);
    }
    h.write_u32(u32::from(v.load_slot));
    h.write_usize(v.instructions.len());
    h.write_usize(v.static_constants.len());
    h.write_usize(f.relocation_directory.len());
    h.write_usize(fragment.len());
    let ends = |words: &[[u32; 4]], h: &mut crate::fast_hash::FastHasher| {
        let n = words.len();
        for row in words[..n.min(16)].iter().chain(&words[n - n.min(16)..]) {
            for w in row {
                h.write_u32(*w);
            }
        }
    };
    ends(&v.instructions, &mut h);
    let n = fragment.len();
    h.write(&fragment[..n.min(256)]);
    h.write(&fragment[n - n.min(256)..]);
    h.finish()
}

/// Every `EffectiveShaderControls` field, as words. Destructured so a new
/// control cannot be added without becoming part of this key.
fn control_words(c: &EffectiveShaderControls) -> [u32; 66] {
    let EffectiveShaderControls {
        vertex_texture_mask,
        enabled_arrays,
        cube_mask,
        volume_mask,
        shadow,
        gamma,
        expand,
        alpha_function,
        srgb_output,
        sample_grid,
        unnormalized_mask,
        texture_dimensions,
    } = c;
    let mut out = [0u32; 66];
    let mut at = 0;
    let mut push = |w: u32| {
        out[at] = w;
        at += 1;
    };
    push(*vertex_texture_mask);
    push(u32::from(*enabled_arrays));
    push(*cube_mask);
    push(*volume_mask);
    for s in shadow {
        push(u32::from_le_bytes([
            s.enabled,
            s.function,
            s.linear,
            s.unnormalized,
        ]));
    }
    for bytes in gamma
        .as_chunks::<4>()
        .0
        .iter()
        .chain(expand.as_chunks::<4>().0)
    {
        push(u32::from_le_bytes(*bytes));
    }
    push(u32::from(alpha_function.is_some()));
    push(alpha_function.unwrap_or(0));
    push(u32::from(*srgb_output));
    push(sample_grid[0]);
    push(sample_grid[1]);
    push(*unnormalized_mask);
    for [w, h] in texture_dimensions {
        push(*w);
        push(*h);
    }
    debug_assert_eq!(at, 66);
    out
}
#[derive(Clone, PartialEq, Eq, Hash)]
pub(super) struct ProgramKey {
    technique: u64,
    mode: u8,
    lowering: u8,
    known: u16,
    logical_slots: usize,
    controls: [u32; 66],
}
impl ProgramKey {
    pub(super) fn new(
        technique: &ReusedTechnique<'_>,
        mode: FragmentMode,
        lowering: Lowering,
        known: u16,
        logical_slots: usize,
        controls: &EffectiveShaderControls,
    ) -> Self {
        Self {
            technique: technique.id,
            mode: mode as u8,
            lowering: lowering as u8,
            known,
            logical_slots,
            controls: control_words(controls),
        }
    }
}
pub(super) struct ProgramEntry {
    pub(super) decoded: DecodedTechnique,
    pub(super) layout: Option<Arc<FragmentConstantLayout>>,
}

pub(crate) struct PlanReuse {
    enabled: bool,
    next_id: u64,
    techniques: FastHashMap<u64, Vec<TechniqueEntry>>,
    technique_count: usize,
    technique_bytes: usize,
    pub(super) programs: FastHashMap<ProgramKey, ProgramEntry>,
    /// Symbolic all-array vertex decode per (technique, vertex texture mask).
    pub(super) requirements: FastHashMap<(u64, u32), Arc<super::DecodedVertex>>,
    /// Fragment texture units read by the technique's fragment template.
    pub(super) units: FastHashMap<u64, u16>,
    pub(super) stats: PlanReuseStats,
}
impl PlanReuse {
    pub(crate) fn new() -> Self {
        Self::with_enabled(enabled_from_environment())
    }
    pub(crate) fn with_enabled(enabled: bool) -> Self {
        Self {
            enabled,
            next_id: 1,
            techniques: Default::default(),
            technique_count: 0,
            technique_bytes: 0,
            programs: Default::default(),
            requirements: Default::default(),
            units: Default::default(),
            stats: PlanReuseStats::default(),
        }
    }
    pub(crate) fn enabled(&self) -> bool {
        self.enabled
    }
    pub(crate) fn set_enabled(&mut self, enabled: bool) {
        if !enabled {
            self.clear();
        }
        self.enabled = enabled;
    }
    pub(crate) fn clear(&mut self) {
        self.techniques.clear();
        self.technique_count = 0;
        self.technique_bytes = 0;
        self.programs.clear();
        self.requirements.clear();
        self.units.clear();
    }
    pub(crate) fn technique<'a>(
        &mut self,
        source: &'a TechniquePrograms,
        fragment: &'a [u8],
    ) -> Option<ReusedTechnique<'a>> {
        if !self.enabled {
            return None;
        }
        let probe = technique_probe(source, fragment);
        if let Some(entry) = self.techniques.get(&probe).and_then(|bucket| {
            bucket
                .iter()
                .find(|e| e.fragment == fragment && e.source == *source)
        }) {
            self.stats.technique_hits += 1;
            return Some(ReusedTechnique {
                id: entry.id,
                source,
                fragment,
            });
        }
        self.stats.technique_misses += 1;
        let bytes = technique_bytes(source, fragment);
        if self.technique_count >= MAX_TECHNIQUES
            || self.technique_bytes + bytes > MAX_TECHNIQUE_BYTES
        {
            // Retained programs stay valid: their ids still name exact bytes.
            self.techniques.clear();
            self.technique_count = 0;
            self.technique_bytes = 0;
            self.stats.resets += 1;
        }
        let id = self.next_id;
        self.next_id += 1;
        self.techniques
            .entry(probe)
            .or_default()
            .push(TechniqueEntry {
                source: source.clone(),
                fragment: fragment.to_vec(),
                id,
            });
        self.technique_count += 1;
        self.technique_bytes += bytes;
        Some(ReusedTechnique {
            id,
            source,
            fragment,
        })
    }
    pub(super) fn insert_program(&mut self, key: ProgramKey, entry: ProgramEntry) {
        if self.programs.len() >= MAX_PROGRAMS {
            self.programs.clear();
            self.stats.resets += 1;
        }
        self.programs.insert(key, entry);
    }
    pub(super) fn insert_requirement(&mut self, key: (u64, u32), value: Arc<super::DecodedVertex>) {
        if self.requirements.len() >= MAX_REQUIREMENTS {
            self.requirements.clear();
            self.units.clear();
            self.stats.resets += 1;
        }
        self.requirements.insert(key, value);
    }
}
