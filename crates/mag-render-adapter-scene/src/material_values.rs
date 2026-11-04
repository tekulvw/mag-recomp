//! Ordered native material values. No state of the original job or GPU is read or seeded.
use crate::{
    material_constants::{
        BuiltinError, BuiltinPassValues, ConstantRowWrite, CurrentVertexAttributes,
        FragmentBuiltinRequest, ParameterAssignment, ParameterError, apply_current_attributes,
        ordinary_vertex_rows,
    },
    material_programs::StaticVertexConstant,
};
pub use mag_render_adapter_core::material_shader_math as shader_math;
pub type ConstantRow = [u32; 4];
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum ValueError {
    VertexConstantRange(u32),
    UnknownVertexConstant(u32),
    Parameter(ParameterError),
    Builtin(BuiltinError),
    Capture(crate::material_capture::MaterialCaptureError),
    SourceBounds,
    MissingTransform,
    MissingView,
    MissingAtlas,
    MissingParameters,
    MissingDepthAdjustment,
    UnqualifiedGroup(u16),
    Allocation,
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum ParameterColumn {
    Unused,
    NullSource,
    Values([u32; 8]),
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct MaterialDrawSources {
    pub technique: [u8; 32],
    pub mesh_group: u16,
    pub transform: [u8; 112],
    pub mesh_constant: ConstantRow,
    pub view: Option<[[u32; 4]; 4]>,
    pub atlas_dimensions: Option<[u32; 2]>,
    pub parameter_column: ParameterColumn,
    pub depth_adjustment: Option<[u32; 2]>,
    pub pass_control: u32,
    pub binding_scalar: ConstantRow,
}
fn word(bytes: &[u8], at: usize) -> u32 {
    u32::from_be_bytes(bytes[at..at + 4].try_into().unwrap())
}
/// The Main-local48-vector staging object is tracked separately from the full
/// GPU bank. No original BSS/cache bytes are inputs. Rows become known through
/// source-qualified writes, then carry across the ordered draws in this scope.
#[derive(Clone)]
pub struct MaterialValueState {
    pub vertex: KnownVertexBank,
    pub current: CurrentVertexAttributes,
    globals: [Option<ConstantRow>; 48],
    prior_processed: Option<BindingIdentity>,
    bound_selector: Option<u32>,
    fragment: Vec<ConstantRowWrite>,
    omitted_computed_fp: bool,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct BindingIdentity {
    material: u32,
    technique: u32,
    selector: u32,
    group: u16,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum MaterialBindingAction {
    Full,
    MaterialOnly,
    Unchanged,
}
impl MaterialValueState {
    pub fn unknown() -> Self {
        Self {
            vertex: KnownVertexBank::unknown(),
            current: [None; 16],
            globals: [None; 48],
            prior_processed: None,
            bound_selector: None,
            fragment: Vec::new(),
            omitted_computed_fp: false,
        }
    }
    /// Every Main entry starts from zeroed global material rows and no
    /// processed or bound material. This uploads no GPU constants: only the
    /// selected technique's range is published later. GPU constant and
    /// current-attribute knowledge carries over independently.
    pub fn begin_main(&mut self) {
        self.globals.fill(Some([0; 4]));
        self.prior_processed = None;
        self.bound_selector = None;
    }
    /// A group-0 item under an update-only pass control (unsigned pass +0x144
    /// at most 2) draws nothing but still commits the processed material,
    /// chunk and group. The selector of the last bound material stays
    /// separate, like the game's technique selection cache.
    pub fn observe_update_only(
        &mut self,
        source: &crate::materials::OwnedMaterialSources,
    ) -> Result<(), ValueError> {
        let draw = source.draw_sources.as_ref().map_err(Clone::clone)?;
        if draw.mesh_group != 0 {
            return Err(ValueError::UnqualifiedGroup(draw.mesh_group));
        }
        self.prior_processed = Some(BindingIdentity {
            material: source.material_address,
            technique: source.technique.source_address,
            selector: source.technique.selector,
            group: draw.mesh_group,
        });
        Ok(())
    }
    pub fn fragment_rows(&self) -> &[ConstantRowWrite] {
        &self.fragment
    }
    /// An exact fragment may not consume a POC bank with omitted computations.
    pub fn fragment_values_complete(&self) -> bool {
        !self.omitted_computed_fp
    }
    /// Source binder decisions are separate from GPU pipeline cache identity.
    /// Within this actual ordered source window, material-only changes still
    /// write static VP rows; an unchanged binder must not reapply old statics
    /// over values carried from the previous per-item writer. Group13 forces a
    /// fresh binder. No address here is used as a native resource generation.
    pub fn compose_item(
        &mut self,
        source: &crate::materials::OwnedMaterialSources,
        mbi: &[u8; 32],
    ) -> Result<MaterialBindingAction, ValueError> {
        self.compose_item_inner(source, mbi, None, true)
            .map(|(action, _)| action)
    }
    /// The same composition for a caller that discards this state on any
    /// error (the Main planner aborts the whole plan): it updates in place
    /// instead of cloning the whole value state per item. Successful results
    /// and the resulting state are identical to `compose_item`/POC variants.
    #[expect(
        dead_code,
        reason = "used by the Main scene planner, which is migrated later"
    )]
    pub(crate) fn compose_item_discarding_on_error(
        &mut self,
        source: &crate::materials::OwnedMaterialSources,
        mbi: &[u8; 32],
        poc_values: Option<BuiltinPassValues>,
    ) -> Result<(MaterialBindingAction, usize), ValueError> {
        self.compose_item_inner(source, mbi, poc_values, false)
    }
    /// Explicit flat-fragment POC only. Remove31/32 FP computations that have
    /// no consumer in that shader; rebuild every other builtin in source order
    /// from already owned values. Ordinary VS/current and cache effects remain.
    pub fn compose_item_poc_unlit(
        &mut self,
        source: &crate::materials::OwnedMaterialSources,
        mbi: &[u8; 32],
        values: BuiltinPassValues,
    ) -> Result<(MaterialBindingAction, usize), ValueError> {
        self.compose_item_inner(source, mbi, Some(values), true)
    }
    fn compose_item_inner(
        &mut self,
        source: &crate::materials::OwnedMaterialSources,
        mbi: &[u8; 32],
        unused_computed_fp: Option<BuiltinPassValues>,
        transactional: bool,
    ) -> Result<(MaterialBindingAction, usize), ValueError> {
        let draw = source.draw_sources.as_ref().map_err(Clone::clone)?;
        let identity = BindingIdentity {
            material: source.material_address,
            technique: source.technique.source_address,
            selector: source.technique.selector,
            group: draw.mesh_group,
        };
        let material_changed = identity.group == 13
            || self
                .prior_processed
                .is_none_or(|p| p.material != identity.material);
        let technique_changed = identity.group == 13
            || self
                .prior_processed
                .is_none_or(|p| p.technique != identity.technique || p.group != identity.group);
        // An exact/ordinary fragment consumer must never inherit a POC bank
        // whose computed rows were intentionally omitted, even for an otherwise
        // unchanged source binder. Rebinding performs normal exact validation.
        let technique_changed = technique_changed
            || self.bound_selector != Some(identity.selector)
            || (unused_computed_fp.is_none() && self.omitted_computed_fp);
        let action = if technique_changed {
            MaterialBindingAction::Full
        } else if material_changed {
            MaterialBindingAction::MaterialOnly
        } else {
            MaterialBindingAction::Unchanged
        };
        let mut copy = None;
        let next = if transactional {
            copy.insert(self.clone())
        } else {
            &mut *self
        };
        let mut omitted = 0;
        // `next` is either a private copy or a state the caller discards on
        // error, so neither step needs its own copy for atomicity.
        if action != MaterialBindingAction::Unchanged {
            (next.fragment, omitted) = next.bind_material_in_place(source, unused_computed_fp)?;
        }
        next.apply_draw_in_place(draw, mbi)?;
        next.prior_processed = Some(identity);
        next.bound_selector = Some(identity.selector);
        if let Some(next) = copy {
            *self = next;
        }
        Ok((action, omitted))
    }
    pub fn bind_material(
        &mut self,
        source: &crate::materials::OwnedMaterialSources,
    ) -> Result<Vec<ConstantRowWrite>, ValueError> {
        let mut next = self.clone();
        let (rows, _) = next.bind_material_in_place(source, None)?;
        *self = next;
        Ok(rows)
    }
    /// The binder's writes applied directly to `self`. On error `self` may be
    /// partially updated; callers either work on a copy or discard the state.
    fn bind_material_in_place(
        &mut self,
        source: &crate::materials::OwnedMaterialSources,
        unused_computed_fp: Option<BuiltinPassValues>,
    ) -> Result<(Vec<ConstantRowWrite>, usize), ValueError> {
        let ordinary = source
            .parameters
            .as_ref()
            .map_err(|e| ValueError::Parameter(e.clone()))?;
        let current = source
            .current_attribute_updates
            .as_ref()
            .map_err(|e| ValueError::Parameter(e.clone()))?;
        let fp = source
            .ordinary_fragment_rows
            .as_ref()
            .map_err(|e| ValueError::Parameter(e.clone()))?;
        let mut omitted = 0;
        let rebuilt;
        let builtin = match (&source.fragment_builtin_rows, unused_computed_fp) {
            (Err(BuiltinError::UnqualifiedSemantic(31 | 32)), Some(values)) => {
                let requests = source
                    .builtin_requests
                    .as_ref()
                    .map_err(|e| ValueError::Builtin(e.clone()))?;
                let mut kept = Vec::new();
                kept.try_reserve_exact(requests.len())
                    .map_err(|_| ValueError::Allocation)?;
                for request in requests {
                    if matches!(
                        request,
                        FragmentBuiltinRequest::Unqualified {
                            semantic: 31 | 32,
                            ..
                        } | FragmentBuiltinRequest::Animated { .. }
                    ) {
                        omitted += 1;
                    } else {
                        kept.push(*request);
                    }
                }
                if omitted == 0 {
                    return Err(ValueError::Builtin(
                        source.fragment_builtin_rows.as_ref().unwrap_err().clone(),
                    ));
                }
                rebuilt =
                    crate::fragment_builtin_rows::prepare_fragment_builtin_rows(&kept, values)
                        .map_err(ValueError::Builtin)?;
                &rebuilt
            }
            _ => source
                .fragment_builtin_rows
                .as_ref()
                .map_err(|e| ValueError::Builtin(e.clone()))?,
        };
        let draw = source.draw_sources.as_ref().map_err(Clone::clone)?;
        let mut writes = Vec::new();
        writes
            .try_reserve_exact(fp.len() + builtin.len())
            .map_err(|_| ValueError::Allocation)?;
        writes.extend_from_slice(fp);
        writes.extend_from_slice(builtin);
        let current =
            apply_current_attributes(self.current, current).map_err(ValueError::Parameter)?;
        self.vertex
            .apply_material(ordinary, &source.programs.vertex.static_constants)?;
        self.vertex.write(24, draw.binding_scalar)?;
        self.current = current;
        self.omitted_computed_fp = omitted != 0;
        Ok((writes, omitted))
    }
    pub fn apply_draw(
        &mut self,
        source: &MaterialDrawSources,
        mbi: &[u8; 32],
    ) -> Result<(), ValueError> {
        let mut next = self.clone();
        next.apply_draw_in_place(source, mbi)?;
        *self = next;
        Ok(())
    }
    /// `apply_draw` writing directly into `self` (partial on error; see
    /// `bind_material_in_place`).
    fn apply_draw_in_place(
        &mut self,
        source: &MaterialDrawSources,
        mbi: &[u8; 32],
    ) -> Result<(), ValueError> {
        if matches!(source.mesh_group, 14 | 15) {
            return Err(ValueError::UnqualifiedGroup(source.mesh_group));
        }
        let flags = u64::from_be_bytes(source.technique[8..16].try_into().unwrap());
        let first = usize::from(source.technique[5]);
        let count = usize::from(source.technique[6]);
        if count != 0 && first + count > 48 {
            return Err(ValueError::SourceBounds);
        }
        let next = self;
        if flags & (1 << 27) != 0 {
            let values = match source.parameter_column {
                ParameterColumn::Values(words) => words,
                ParameterColumn::NullSource => [0, 0, 0, 0x3a83126e, 0x3f800000, 0, 0, 0],
                ParameterColumn::Unused => return Err(ValueError::MissingParameters),
            };
            for (i, words) in values.as_chunks::<4>().0.iter().enumerate() {
                next.vertex.write(44 + i as u32, *words)?;
            }
        }
        if flags & (1 << 26) != 0 {
            next.globals[13] = Some(source.mesh_constant);
        }
        if flags & (1 << 29) != 0 {
            let dimensions = source.atlas_dimensions.ok_or(ValueError::MissingAtlas)?;
            let numerators = [
                ((u32::from(mbi[24]) >> 2) + 1) * 16,
                ((u32::from(u16::from_be_bytes(mbi[24..26].try_into().unwrap())) & 1008) | 15) + 1,
                ((word(mbi, 24) >> 14) & 63) * 16,
                u32::from(mbi[26] & 63) * 16,
            ];
            next.globals[18] = Some(shader_math::atlas(
                numerators,
                [dimensions[0], dimensions[1], dimensions[0], dimensions[1]],
            ));
        }
        if flags & (1 << 23) != 0 && source.transform[0x68] != 0 {
            next.globals[12] = Some([
                word(&source.transform, 0x60),
                word(&source.transform, 0x64),
                0,
                0,
            ]);
        }
        for i in first..first + count {
            next.vertex.set_row(i, next.globals[i]);
        }
        let projection = std::array::from_fn(|r| {
            std::array::from_fn(|c| word(&source.transform, 0x20 + r * 16 + c * 4))
        });
        let projection = if source.mesh_group == 11 {
            let [anchor, scale] = source
                .depth_adjustment
                .ok_or(ValueError::MissingDepthAdjustment)?;
            shader_math::depth_axis_projection(projection, anchor, scale)
        } else {
            projection
        };
        let affine = if flags & 0xc0 != 0 {
            Some(shader_math::affine(std::array::from_fn(|i| {
                word(&source.transform, i * 4)
            })))
        } else {
            None
        };
        let mut cursor = if flags & 128 != 0 { 0 } else { 3 };
        if flags & 128 != 0 {
            for row in affine.unwrap() {
                next.vertex.write(cursor, row)?;
                cursor += 1;
            }
        }
        for row in projection {
            next.vertex.write(cursor, row)?;
            cursor += 1;
        }
        if flags & 64 != 0 {
            let view = source.view.ok_or(ValueError::MissingView)?;
            for row in shader_math::view(affine.unwrap(), view) {
                next.vertex.write(cursor, row)?;
                cursor += 1;
            }
        }
        Ok(())
    }
}
/// Persistent rows known through actual observed semantic writes. The renderer
/// associates this state with its VM/native owner lifetime. New/unknown or
/// unobserved outside writers cannot acquire an implicit all-zero baseline.
///
/// Storage: each row as its four little-endian words, plus a separate
/// known-row bit set. An unknown row's bytes are always zero, which is exactly
/// the padding `required_words` produces, so a validated bank is its own
/// packed uniform image (`required_row_bytes`).
#[derive(Clone)]
pub struct KnownVertexBank {
    bytes: [[u8; 16]; 512],
    known: [u64; 8],
}
fn row_bytes(words: ConstantRow) -> [u8; 16] {
    let mut out = [0; 16];
    for (lane, word) in out.as_chunks_mut::<4>().0.iter_mut().zip(words) {
        *lane = word.to_le_bytes();
    }
    out
}
fn row_words(bytes: &[u8; 16]) -> ConstantRow {
    std::array::from_fn(|i| u32::from_le_bytes(bytes.as_chunks::<4>().0[i]))
}
impl KnownVertexBank {
    pub fn unknown() -> Self {
        Self {
            bytes: [[0; 16]; 512],
            known: [0; 8],
        }
    }
    #[inline]
    fn is_known(&self, index: usize) -> bool {
        self.known[index / 64] & (1 << (index % 64)) != 0
    }
    #[inline]
    fn set(&mut self, index: usize, words: ConstantRow) {
        self.bytes[index] = row_bytes(words);
        self.known[index / 64] |= 1 << (index % 64);
    }
    #[inline]
    fn clear(&mut self, index: usize) {
        self.bytes[index] = [0; 16];
        self.known[index / 64] &= !(1 << (index % 64));
    }
    pub fn row(&self, index: usize) -> Option<ConstantRow> {
        (index < 512 && self.is_known(index)).then(|| row_words(&self.bytes[index]))
    }
    /// Whether all 512 rows are known.
    pub fn all_known(&self) -> bool {
        self.known.iter().all(|&w| w == u64::MAX)
    }
    /// Every row with its knowledge, in slot order.
    pub fn rows(&self) -> [Option<ConstantRow>; 512] {
        std::array::from_fn(|i| self.row(i))
    }
    /// Replace one in-range row's knowledge (None makes it unknown again).
    pub(crate) fn set_row(&mut self, index: usize, row: Option<ConstantRow>) {
        match row {
            Some(words) => self.set(index, words),
            None => self.clear(index),
        }
    }
    pub fn write(&mut self, index: u32, words: ConstantRow) -> Result<(), ValueError> {
        if index as usize >= 512 {
            return Err(ValueError::VertexConstantRange(index));
        }
        self.set(index as usize, words);
        Ok(())
    }
    /// Retained/unowned writers must be accounted before carrying this bank.
    /// Unknown destinations invalidate the whole bank; exact known destinations
    /// invalidate only those rows. VM/backend reset starts a new unknown bank
    /// unless its true source reset values are separately supplied as writes.
    pub fn invalidate(&mut self, indices: Option<&[u32]>) {
        if let Some(indices) = indices {
            for &index in indices {
                if (index as usize) < 512 {
                    self.clear(index as usize);
                } else {
                    *self = Self::unknown();
                    return;
                }
            }
        } else {
            *self = Self::unknown();
        }
    }
    pub fn apply_writes(&mut self, writes: &[ConstantRowWrite]) -> Result<(), ValueError> {
        for write in writes {
            if write.logical_slot >= 512 {
                return Err(ValueError::VertexConstantRange(write.logical_slot));
            }
        }
        for write in writes {
            self.set(write.logical_slot as usize, write.words);
        }
        Ok(())
    }
    /// Actual binder order is ordinary VP parameter writes, then shader static
    /// VP rows, including static uploads in the material-only shader sequence.
    /// Later Main per-item uploads are a separate phase and must follow this.
    pub fn apply_material(
        &mut self,
        ordinary: &[ParameterAssignment],
        static_rows: &[StaticVertexConstant],
    ) -> Result<(), ValueError> {
        let ordinary = ordinary_vertex_rows(ordinary).map_err(ValueError::Parameter)?;
        for row in static_rows {
            if row.slot >= 512 {
                return Err(ValueError::VertexConstantRange(row.slot));
            }
        }
        self.apply_writes(&ordinary)?;
        for row in static_rows {
            self.set(row.slot as usize, row.words);
        }
        Ok(())
    }
    /// Only reflection-proved unused rows become zero padding. Dynamic constant
    /// addressing conservatively requires every row. This returns owned uniform
    /// words and never upgrades padding into known persistent state.
    pub fn required_words(
        &self,
        required: &[u16],
        dynamic: bool,
    ) -> Result<[ConstantRow; 512], ValueError> {
        self.required_row_bytes(required, dynamic)
            .map(|bytes| std::array::from_fn(|i| row_words(&bytes[i])))
    }
    /// `required_words` as its little-endian uniform bytes, without a copy:
    /// the same checks in the same order, then the bank's own rows, whose
    /// unknown rows already hold zero padding.
    pub fn required_row_bytes(
        &self,
        required: &[u16],
        dynamic: bool,
    ) -> Result<&[[u8; 16]; 512], ValueError> {
        if dynamic && let Some(word) = self.known.iter().position(|&w| w != u64::MAX) {
            let bit = (!self.known[word]).trailing_zeros() as usize;
            return Err(ValueError::UnknownVertexConstant((word * 64 + bit) as u32));
        }
        for &index in required {
            if index >= 512 {
                return Err(ValueError::VertexConstantRange(u32::from(index)));
            }
            if !self.is_known(index as usize) {
                return Err(ValueError::UnknownVertexConstant(u32::from(index)));
            }
        }
        Ok(&self.bytes)
    }
}
