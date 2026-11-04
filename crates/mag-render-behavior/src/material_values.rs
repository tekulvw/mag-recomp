//! Main material value composition (`material-values.md`).

use crate::guest::{GuestMemory, be_u16, be_u32, be_words};

pub const VERTEX_ROWS: usize = 512;
pub const STAGING_ROWS: usize = 48;
pub const FRAGMENT_ROWS: u32 = 1024;

/// Rows 44 and 45 when there is no parameter column: `(0, 0, 0, 0.001)` and `(1, 0, 0, 0)`.
pub const NULL_PARAMETER_COLUMN: [[u32; 4]; 2] = [[0, 0, 0, 0x3a83_126f], [0x3f80_0000, 0, 0, 0]];
/// Forced distances for categories 0, 1 and 2.
pub const FORCED_DISTANCES: [f32; 3] = [30.0, 100.0, 200.0];
/// Forced denominator.
pub const FORCED_DENOMINATOR: f32 = 15.0;
/// Inverse used when the denominator is negative, zero or subnormal.
pub const FALLBACK_INVERSE: f32 = 100.0;

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum MaterialError {
    VertexConstantRange(u32),
    UnknownVertexConstant(u32),
    ValueType(u8),
    Mapping {
        slot: u32,
        rows: u32,
    },
    CurrentAttributeSlot(u32),
    /// A matrix-typed parameter aimed at a current attribute.
    Unqualified(&'static str),
    UnqualifiedGroup(u32),
    MissingParameters,
    MissingAtlas,
    MissingTransform,
    MissingView,
    MissingDepthAdjustment,
    SourceBounds {
        first: u32,
        count: u32,
    },
    /// An arithmetic input with an all-ones exponent.
    Domain(&'static str),
    /// A result whose magnitude exceeds the binary32 range.
    Overflow(&'static str),
    Read(String),
}

/// The 512-row vertex constant bank with per-row knowledge.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct VertexBank {
    rows: Vec<Option<[u32; 4]>>,
}

impl Default for VertexBank {
    fn default() -> Self {
        Self {
            rows: vec![None; VERTEX_ROWS],
        }
    }
}

impl VertexBank {
    pub fn row(&self, row: u32) -> Option<[u32; 4]> {
        self.rows.get(row as usize).copied().flatten()
    }

    pub fn write(&mut self, row: u32, value: [u32; 4]) -> Result<(), MaterialError> {
        let slot = self
            .rows
            .get_mut(row as usize)
            .ok_or(MaterialError::VertexConstantRange(row))?;
        *slot = Some(value);
        Ok(())
    }

    /// Range-checks every row, then writes them all.
    pub fn write_batch(&mut self, writes: &[(u32, [u32; 4])]) -> Result<(), MaterialError> {
        if let Some((row, _)) = writes.iter().find(|(row, _)| *row as usize >= VERTEX_ROWS) {
            return Err(MaterialError::VertexConstantRange(*row));
        }
        for (row, value) in writes {
            self.rows[*row as usize] = Some(*value);
        }
        Ok(())
    }

    /// With no list every row becomes unknown; a listed index of 512 or more also does that.
    pub fn invalidate(&mut self, rows: Option<&[u32]>) {
        match rows {
            Some(list) if list.iter().all(|row| (*row as usize) < VERTEX_ROWS) => {
                for row in list {
                    self.rows[*row as usize] = None;
                }
            }
            _ => self.rows.iter_mut().for_each(|row| *row = None),
        }
    }

    /// Checks the rows a program reads and returns the uniform image (unknown rows as zeros).
    pub fn read_for_draw(
        &self,
        required: &[u32],
        dynamic: bool,
    ) -> Result<Vec<[u32; 4]>, MaterialError> {
        if dynamic {
            if let Some(row) = self.rows.iter().position(Option::is_none) {
                return Err(MaterialError::UnknownVertexConstant(row as u32));
            }
        } else {
            let mut sorted = required.to_vec();
            sorted.sort_unstable();
            for row in sorted {
                if row as usize >= VERTEX_ROWS {
                    return Err(MaterialError::VertexConstantRange(row));
                }
                if self.rows[row as usize].is_none() {
                    return Err(MaterialError::UnknownVertexConstant(row));
                }
            }
        }
        Ok(self.rows.iter().map(|row| row.unwrap_or([0; 4])).collect())
    }
}

/// An item's binding identity.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Identity {
    pub material: u32,
    pub technique: u32,
    pub selector: u32,
    pub group: u32,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum BindingAction {
    Full,
    MaterialOnly,
    Unchanged,
}

/// Material value state carried from item to item.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct MaterialValueState {
    pub bank: VertexBank,
    pub current: [Option<[u32; 4]>; 16],
    pub fragment_writes: Vec<(u32, [u32; 4])>,
    pub staging: [Option<[u32; 4]>; STAGING_ROWS],
    remembered: Option<Identity>,
    bound_selector: Option<u32>,
}

/// One parameter record of the selected technique, with its words already read.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Parameter {
    pub value_type: u8,
    pub words: Vec<u32>,
    /// The destination word from the technique's program chunk.
    pub destination: u32,
}

/// The selected technique record.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct TechniqueRecord {
    pub membership_bit: u8,
    pub first_parameter: u8,
    pub staging_first: u8,
    pub staging_count: u8,
    pub flags: u64,
}

/// Number of value words of a parameter type.
pub fn value_words(value_type: u8) -> Result<usize, MaterialError> {
    Ok(match value_type {
        1 | 2 => 1,
        3 => 0,
        4..=7 => (value_type - 3) as usize,
        8 => 8,
        9 => 12,
        10 => 16,
        other => return Err(MaterialError::ValueType(other)),
    })
}

/// Converts a signed 32-bit integer to binary32, rounding toward zero.
pub fn int_toward_zero(value: i32) -> u32 {
    let nearest = value as f32;
    if (nearest as f64).abs() > (value as f64).abs() {
        nearest.to_bits() - 1
    } else {
        nearest.to_bits()
    }
}

/// Reads the selected technique and walks its parameter records.
pub fn read_technique<M: GuestMemory + ?Sized>(
    memory: &M,
    material: u32,
    chunk: u32,
    selector: u32,
) -> Result<(TechniqueRecord, Vec<Parameter>), MaterialError> {
    let read = |address: u32, length: u32| -> Result<Vec<u8>, MaterialError> {
        let bytes = memory
            .read(address, length)
            .map_err(|e| MaterialError::Read(e.to_string()))?;
        if bytes.len() != length as usize {
            return Err(MaterialError::Read(format!("short read at {address:#x}")));
        }
        Ok(bytes)
    };
    let header = read(material, 0x50)?;
    let records = be_u16(&header, 0x44) as u32;
    let values = be_u16(&header, 0x46) as u32;
    let techniques = be_u16(&header, 0x48) as u32;
    let count = be_u16(&header, 0x4c) as u32;
    let t = read(material + techniques + 32 * selector, 32)?;
    let technique = TechniqueRecord {
        membership_bit: t[1],
        first_parameter: t[3],
        staging_first: t[5],
        staging_count: t[6],
        flags: u64::from_be_bytes(t[8..16].try_into().expect("eight bytes")),
    };
    if technique.membership_bit >= 32 {
        return Err(MaterialError::Read(format!(
            "membership bit {}",
            technique.membership_bit
        )));
    }
    let mut parameters = Vec::new();
    let mut index = technique.first_parameter as u32;
    while index < count {
        let record = read(material + records + 16 * index, 16)?;
        if be_u32(&record, 4) & (1 << technique.membership_bit) == 0 {
            break;
        }
        let value_type = record[10];
        let words = value_words(value_type)?;
        if words > 0 {
            let value_index = be_u16(&record, 8) as u32;
            let bytes = read(material + values + 4 * value_index, 4 * words as u32)?;
            let destination = be_u32(&read(chunk + be_u16(&record, 12) as u32, 4)?, 0);
            parameters.push(Parameter {
                value_type,
                words: (0..words).map(|i| be_u32(&bytes, 4 * i)).collect(),
                destination,
            });
        } else {
            parameters.push(Parameter {
                value_type,
                words: Vec::new(),
                destination: 0,
            });
        }
        index += 1;
    }
    Ok((technique, parameters))
}

/// The ordinary writes of a technique's parameters, each destination in record order.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct OrdinaryWrites {
    pub vertex: Vec<(u32, [u32; 4])>,
    pub fragment: Vec<(u32, [u32; 4])>,
    pub current: Vec<(u32, [u32; 4])>,
}

const PAD: [u32; 4] = [0, 0, 0, 0x3f80_0000];

fn padded(words: &[u32]) -> [u32; 4] {
    std::array::from_fn(|lane| words.get(lane).copied().unwrap_or(PAD[lane]))
}

/// Splits the parameters into vertex, fragment and current-attribute writes.
pub fn ordinary_writes(parameters: &[Parameter]) -> Result<OrdinaryWrites, MaterialError> {
    let mut out = OrdinaryWrites::default();
    for parameter in parameters {
        let count = value_words(parameter.value_type)?;
        if count == 0 {
            continue;
        }
        let words: Vec<u32> = if matches!(parameter.value_type, 1 | 2) {
            vec![int_toward_zero(parameter.words[0] as i32)]
        } else {
            parameter.words[..count].to_vec()
        };
        let kind = (parameter.destination >> 24) & 0xf;
        let slot = (parameter.destination >> 14) & 0x3ff;
        match kind {
            2 | 3 => {
                let rows = words.len().div_ceil(4) as u32;
                let limit = if kind == 2 {
                    VERTEX_ROWS as u32
                } else {
                    FRAGMENT_ROWS
                };
                if slot + rows > limit {
                    return Err(MaterialError::Mapping { slot, rows });
                }
                let target = if kind == 2 {
                    &mut out.vertex
                } else {
                    &mut out.fragment
                };
                for (index, chunk) in words.chunks(4).enumerate() {
                    target.push((slot + index as u32, padded(chunk)));
                }
            }
            1 => {
                if slot >= 16 {
                    return Err(MaterialError::CurrentAttributeSlot(slot));
                }
                if parameter.value_type >= 8 {
                    return Err(MaterialError::Unqualified("matrix current attribute"));
                }
                out.current.push((slot, padded(&words)));
            }
            _ => {}
        }
    }
    Ok(out)
}

/// The 112-byte transform record of a cohort item.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct TransformRecord {
    pub quaternion: [u32; 4],
    pub translation: [u32; 3],
    pub scale: u32,
    /// Row-major `P`.
    pub matrix: [u32; 16],
    pub extra: [u32; 2],
    pub extra_enable: u8,
}

impl TransformRecord {
    pub fn from_bytes(bytes: &[u8; 112]) -> Self {
        Self {
            quaternion: be_words(bytes, 0),
            translation: be_words(bytes, 0x10),
            scale: be_u32(bytes, 0x1c),
            matrix: be_words(bytes, 0x20),
            extra: be_words(bytes, 0x60),
            extra_enable: bytes[0x68],
        }
    }
}

/// Main pass object values used by composition.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PassValues {
    /// `+0x68`.
    pub depth_anchor: u32,
    /// `+0x1DC`.
    pub depth_scale: u32,
    /// `+0x80`, row-major.
    pub view: [u32; 16],
    /// `+0xD0`.
    pub lane3: u32,
    /// Words `+0x118` through `+0x12C`.
    pub distance_words: [u32; 6],
    /// `+0x140`.
    pub parameter_column_base: u32,
    /// The pass control word.
    pub control: u32,
}

impl PassValues {
    fn word(&self, offset: u32) -> u32 {
        self.distance_words[((offset - 0x118) / 4) as usize]
    }
}

/// The item's parameter column, read with its sources.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ParameterColumn {
    /// The sources were read under technique flags with bit 27 clear: no column was recorded.
    Unused,
    /// Rows 44 and 45: the column read from the table, or the null rows.
    Rows([[u32; 4]; 2]),
}

/// Reads the parameter column when the technique flags have bit 27 set. An unreadable column
/// block fails with the read error.
pub fn read_parameter_column<M: GuestMemory + ?Sized>(
    memory: &M,
    technique_flags: u64,
    mbi: &[u8; 32],
    table_base: u32,
) -> Result<ParameterColumn, MaterialError> {
    if technique_flags & (1 << 27) == 0 {
        return Ok(ParameterColumn::Unused);
    }
    let index = (u32::from_be_bytes([mbi[24], mbi[25], mbi[26], mbi[27]]) >> 15) & 0x7ff;
    if index == 0 || table_base == 0 {
        return Ok(ParameterColumn::Rows(NULL_PARAMETER_COLUMN));
    }
    let address = table_base
        .checked_add(128 * ((index - 1) / 4))
        .ok_or_else(|| MaterialError::Read(format!("parameter column at {table_base:#x}")))?;
    let bytes = memory
        .read(address, 128)
        .map_err(|e| MaterialError::Read(e.to_string()))?;
    if bytes.len() != 128 {
        return Err(MaterialError::Read(format!(
            "short parameter column at {address:#x}"
        )));
    }
    let column = ((index - 1) % 4) as usize;
    let words: [u32; 8] = std::array::from_fn(|row| be_u32(&bytes, 16 * row + 4 * column));
    Ok(ParameterColumn::Rows([
        [words[0], words[1], words[2], words[3]],
        [words[4], words[5], words[6], words[7]],
    ]))
}

/// Everything one prepared item's composition needs besides guest reads.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ItemInputs<'a> {
    pub identity: Identity,
    pub technique: TechniqueRecord,
    pub parameters: &'a [Parameter],
    pub material_class: u8,
    pub material_flags: u32,
    pub mbi: [u8; 32],
    pub transform: Option<TransformRecord>,
    /// The parameter column read with the sources (`read_parameter_column`).
    pub parameter_column: ParameterColumn,
    pub mesh_constant: [u32; 4],
    pub pass: PassValues,
    /// The vertex program's static constant rows, in program order.
    pub static_rows: &'a [(u32, [u32; 4])],
    /// Fragment built-in rows, in order.
    pub builtin_rows: &'a [(u32, [u32; 4])],
}

/// The result of composing one item.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Composition {
    pub action: BindingAction,
    /// Fragment writes in force after this item.
    pub fragment_writes: Vec<(u32, [u32; 4])>,
    /// Always `false` here: only the opt-in reduced-fidelity mode omits rows.
    pub omits_computed_rows: bool,
}

fn domain(bits: u32, what: &'static str) -> Result<f64, MaterialError> {
    let value = f32::from_bits(bits);
    if (bits >> 23) & 0xff == 0xff {
        return Err(MaterialError::Domain(what));
    }
    Ok(if value.is_subnormal() {
        0.0
    } else {
        value as f64
    })
}

fn narrow(value: f64, what: &'static str) -> Result<u32, MaterialError> {
    if value.abs() > f32::MAX as f64 {
        return Err(MaterialError::Overflow(what));
    }
    Ok((value as f32).to_bits())
}

/// `s·R(q)` with the quaternion used as stored.
pub fn scaled_rotation(q: [f64; 4], s: f64) -> [[f64; 3]; 3] {
    let [x, y, z, w] = q;
    [
        [
            1.0 - 2.0 * y * y - 2.0 * z * z,
            2.0 * x * y - 2.0 * z * w,
            2.0 * x * z + 2.0 * y * w,
        ],
        [
            2.0 * x * y + 2.0 * z * w,
            1.0 - 2.0 * z * z - 2.0 * x * x,
            2.0 * y * z - 2.0 * x * w,
        ],
        [
            2.0 * x * z - 2.0 * y * w,
            2.0 * y * z + 2.0 * x * w,
            1.0 - 2.0 * x * x - 2.0 * y * y,
        ],
    ]
    .map(|row| row.map(|v| v * s))
}

fn read_bytes<M: GuestMemory + ?Sized>(memory: &M, address: u32, length: u32) -> Option<Vec<u8>> {
    let bytes = memory.read(address, length).ok()?;
    (bytes.len() == length as usize).then_some(bytes)
}

impl Default for MaterialValueState {
    fn default() -> Self {
        Self {
            bank: VertexBank::default(),
            current: [None; 16],
            fragment_writes: Vec::new(),
            staging: [None; STAGING_ROWS],
            remembered: None,
            bound_selector: None,
        }
    }
}

impl MaterialValueState {
    pub fn new() -> Self {
        Self::default()
    }

    /// Begins a Main window: staging rows become known zeros and the binding memory is cleared.
    pub fn begin_window(&mut self) {
        self.staging = [Some([0; 4]); STAGING_ROWS];
        self.remembered = None;
        self.bound_selector = None;
    }

    /// The binding action an item would get.
    pub fn binding_action(&self, identity: &Identity) -> BindingAction {
        let group13 = identity.group == 13;
        let technique_changed = group13
            || self
                .remembered
                .is_none_or(|r| r.technique != identity.technique || r.group != identity.group)
            || self.bound_selector != Some(identity.selector);
        let material_changed = group13
            || self
                .remembered
                .is_none_or(|r| r.material != identity.material);
        if technique_changed {
            BindingAction::Full
        } else if material_changed {
            BindingAction::MaterialOnly
        } else {
            BindingAction::Unchanged
        }
    }

    /// An update-only item records its identity and nothing else.
    pub fn observe_update_only(&mut self, identity: Identity) -> Result<(), MaterialError> {
        if identity.group != 0 {
            return Err(MaterialError::UnqualifiedGroup(identity.group));
        }
        self.remembered = Some(identity);
        Ok(())
    }

    /// Composes one prepared item. On failure the state is unchanged.
    pub fn compose<M: GuestMemory + ?Sized>(
        &mut self,
        item: &ItemInputs<'_>,
        memory: &M,
    ) -> Result<Composition, MaterialError> {
        let mut next = self.clone();
        let composition = next.compose_in_place(item, memory)?;
        *self = next;
        Ok(composition)
    }

    fn compose_in_place<M: GuestMemory + ?Sized>(
        &mut self,
        item: &ItemInputs<'_>,
        memory: &M,
    ) -> Result<Composition, MaterialError> {
        let group = item.identity.group;
        if group >= 14 {
            return Err(MaterialError::UnqualifiedGroup(group));
        }
        let action = self.binding_action(&item.identity);
        if action != BindingAction::Unchanged {
            let writes = ordinary_writes(item.parameters)?;
            let binding_row = binding_row(item)?;
            self.fragment_writes = writes
                .fragment
                .iter()
                .chain(item.builtin_rows)
                .copied()
                .collect();
            for (slot, value) in &writes.current {
                self.current[*slot as usize] = Some(*value);
            }
            self.bank.write_batch(&writes.vertex)?;
            self.bank.write_batch(item.static_rows)?;
            self.bank.write(24, binding_row)?;
        }
        self.draw_values(item, memory)?;
        self.remembered = Some(item.identity);
        self.bound_selector = Some(item.identity.selector);
        Ok(Composition {
            action,
            fragment_writes: self.fragment_writes.clone(),
            omits_computed_rows: false,
        })
    }

    fn draw_values<M: GuestMemory + ?Sized>(
        &mut self,
        item: &ItemInputs<'_>,
        memory: &M,
    ) -> Result<(), MaterialError> {
        let flags = item.technique.flags;
        let bit = |n: u32| flags & (1 << n) != 0;
        let mbi24 = u32::from_be_bytes(item.mbi[24..28].try_into().expect("four bytes"));

        // 1. Parameter column, read with the item's sources.
        if bit(27) {
            let ParameterColumn::Rows(rows) = item.parameter_column else {
                return Err(MaterialError::MissingParameters);
            };
            self.bank.write(44, rows[0])?;
            self.bank.write(45, rows[1])?;
        }
        // 2. Mesh constant.
        if bit(26) {
            self.staging[13] = Some(item.mesh_constant);
        }
        // 3. Atlas.
        if bit(29) {
            if item.material_flags & 0x10 == 0 || mbi24 & 1 == 0 {
                return Err(MaterialError::MissingAtlas);
            }
            let record = u32::from_be_bytes(item.mbi[9..13].try_into().expect("four bytes")) & !0xf;
            let size = record
                .checked_add(72)
                .and_then(|address| read_bytes(memory, address, 4))
                .ok_or(MaterialError::MissingAtlas)?;
            let wh = be_u32(&size, 0);
            let (width, height) = ((wh >> 16) as f32, (wh & 0xffff) as f32);
            let b = &item.mbi[24..28];
            let n0 = ((b[0] as u32 / 4) + 1) * 16;
            let n1 = ((u16::from_be_bytes([b[0], b[1]]) as u32 & 0x3f0) | 15) + 1;
            let n2 = ((mbi24 >> 14) & 63) * 16;
            let n3 = (b[2] as u32 & 63) * 16;
            let lanes = [
                n0 as f32 / width,
                n1 as f32 / height,
                n2 as f32 / width,
                n3 as f32 / height,
            ];
            if lanes.iter().any(|v| !v.is_finite()) {
                return Err(MaterialError::Overflow("atlas"));
            }
            self.staging[18] = Some(lanes.map(f32::to_bits));
        }
        let transform = item.transform.ok_or(MaterialError::MissingTransform)?;
        // 4. Extra row.
        if bit(23) && transform.extra_enable != 0 {
            self.staging[12] = Some([transform.extra[0], transform.extra[1], 0, 0]);
        }
        // 5. Staging copy.
        let (first, count) = (
            item.technique.staging_first as u32,
            item.technique.staging_count as u32,
        );
        if count > 0 {
            if first + count > STAGING_ROWS as u32 {
                return Err(MaterialError::SourceBounds { first, count });
            }
            for row in first..first + count {
                self.bank.rows[row as usize] = self.staging[row as usize];
            }
        }
        // 6. Transform rows. The domain rule applies only to values this item computes with;
        // everything else is copied bit for bit.
        let rotation = if bit(6) || bit(7) {
            let mut q = [0.0f64; 4];
            for (lane, word) in transform.quaternion.iter().enumerate() {
                q[lane] = domain(*word, "quaternion")?;
            }
            let s = domain(transform.scale, "scale")?;
            Some(scaled_rotation(q, s))
        } else {
            None
        };
        if bit(7) {
            let rotation = rotation.expect("computed for bit 7");
            for r in 0..3 {
                let mut row = [0u32; 4];
                for c in 0..3 {
                    row[c] = narrow(rotation[r][c], "transform")?;
                }
                row[3] = transform.translation[r];
                self.bank.write(r as u32, row)?;
            }
        }
        let adjust = if group_is_eleven(item.identity.group) {
            let a = domain(item.pass.depth_anchor, "depth anchor")?;
            let k = domain(item.pass.depth_scale, "depth scale")?;
            Some((a, k))
        } else {
            None
        };
        for r in 0..4 {
            let mut row: [u32; 4] = std::array::from_fn(|l| transform.matrix[4 * r + l]);
            if let Some((a, k)) = adjust {
                let p2 = domain(row[2], "P")?;
                let p3 = domain(row[3], "P")?;
                row[2] = narrow(p2 * k + p3 * (a * k - a), "P")?;
            }
            self.bank.write(3 + r as u32, row)?;
        }
        if bit(6) {
            let rotation = rotation.expect("computed for bit 6");
            let mut w = [[0.0f64; 4]; 4];
            for (index, word) in item.pass.view.iter().enumerate() {
                w[index / 4][index % 4] = domain(*word, "view")?;
            }
            let mut t4 = [[0.0f64; 4]; 4];
            for r in 0..3 {
                t4[r] = [
                    rotation[r][0],
                    rotation[r][1],
                    rotation[r][2],
                    domain(transform.translation[r], "translation")?,
                ];
            }
            t4[3] = [0.0, 0.0, 0.0, 1.0];
            for c in 0..3 {
                let mut row = [0u32; 4];
                for l in 0..4 {
                    row[l] = narrow((0..4).map(|k| w[k][c] * t4[k][l]).sum(), "view")?;
                }
                self.bank.write(7 + c as u32, row)?;
            }
        }
        Ok(())
    }
}

fn group_is_eleven(group: u32) -> bool {
    group == 11
}

/// Vertex row 24.
pub fn binding_row(item: &ItemInputs<'_>) -> Result<[u32; 4], MaterialError> {
    let category = (item.mbi[4] & 7).min(2) as u32;
    let alternate = matches!(item.material_class, 1 | 2);
    let forced = alternate && item.material_flags & 0x8000 != 0;
    let (distance, inverse) = if forced {
        (
            FORCED_DISTANCES[category as usize].to_bits(),
            narrow(1.0 / FORCED_DENOMINATOR as f64, "inverse")?,
        )
    } else {
        let distance = item
            .pass
            .word(if alternate { 0x11c } else { 0x118 } + 4 * category);
        let q_bits = item.pass.word(0x124 + 4 * category);
        let q = domain(q_bits, "denominator")?;
        let inverse = if q <= 0.0 {
            FALLBACK_INVERSE.to_bits()
        } else {
            narrow(1.0 / q, "inverse")?
        };
        (distance, inverse)
    };
    let one = 0x3f80_0000;
    let lane2 = match item.identity.group {
        8 | 9 | 12 | 13 => one,
        0 | 10 => 0,
        _ if item.pass.control == 5 => one,
        _ => 0,
    };
    Ok([distance, inverse, lane2, item.pass.lane3])
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::guest::SparseMemory;

    const IDENTITY_Q: [f32; 4] = [0.0, 0.0, 0.0, 1.0];

    fn f(x: f32) -> u32 {
        x.to_bits()
    }

    fn transform(q: [f32; 4], t: [f32; 3], s: f32) -> TransformRecord {
        let mut matrix = [0u32; 16];
        for i in 0..4 {
            matrix[5 * i] = f(1.0);
        }
        TransformRecord {
            quaternion: q.map(f),
            translation: t.map(f),
            scale: f(s),
            matrix,
            extra: [f(7.0), f(8.0)],
            extra_enable: 0,
        }
    }

    fn pass() -> PassValues {
        PassValues {
            depth_anchor: f(2.0),
            depth_scale: f(0.5),
            view: transform(IDENTITY_Q, [0.0; 3], 1.0).matrix,
            lane3: f(7.0),
            distance_words: [f(10.0), f(50.0), f(60.0), f(1.0), f(4.0), f(8.0)],
            parameter_column_base: 0,
            control: 3,
        }
    }

    fn item<'a>(group: u32, flags: u64, material: u32) -> ItemInputs<'a> {
        ItemInputs {
            identity: Identity {
                material,
                technique: 0x100,
                selector: 0,
                group,
            },
            technique: TechniqueRecord {
                membership_bit: 0,
                first_parameter: 0,
                staging_first: 0,
                staging_count: 0,
                flags,
            },
            parameters: &[],
            material_class: 0,
            material_flags: 0,
            mbi: [0; 32],
            transform: Some(transform(IDENTITY_Q, [0.0; 3], 1.0)),
            parameter_column: ParameterColumn::Unused,
            mesh_constant: [1, 2, 3, 4],
            pass: pass(),
            static_rows: &[],
            builtin_rows: &[],
        }
    }

    fn compose(
        state: &mut MaterialValueState,
        item: &ItemInputs<'_>,
    ) -> Result<Composition, MaterialError> {
        state.compose(item, &SparseMemory::new())
    }

    fn row(state: &MaterialValueState, r: u32) -> [f32; 4] {
        state.bank.row(r).expect("known row").map(f32::from_bits)
    }

    fn started() -> MaterialValueState {
        let mut state = MaterialValueState::new();
        state.begin_window();
        state
    }

    #[test]
    fn v1_half_turn_about_z() {
        let mut state = started();
        let mut i = item(2, 1 << 7, 1);
        i.transform = Some(transform([0.0, 0.0, 1.0, 0.0], [4.0, 5.0, 6.0], 3.0));
        compose(&mut state, &i).unwrap();
        assert_eq!(row(&state, 0), [-3.0, 0.0, 0.0, 4.0]);
        assert_eq!(row(&state, 1), [0.0, -3.0, 0.0, 5.0]);
        assert_eq!(row(&state, 2), [0.0, 0.0, 3.0, 6.0]);
    }

    #[test]
    fn v2_v3_diagonal_and_no_normalisation() {
        let mut state = started();
        let mut i = item(2, 1 << 7, 1);
        i.transform = Some(transform([0.5; 4], [0.0; 3], 1.0));
        compose(&mut state, &i).unwrap();
        assert_eq!(row(&state, 0), [0.0, 0.0, 1.0, 0.0]);
        assert_eq!(row(&state, 1), [1.0, 0.0, 0.0, 0.0]);
        assert_eq!(row(&state, 2), [0.0, 1.0, 0.0, 0.0]);
        i.transform = Some(transform([0.0, 0.0, 2.0, 0.0], [0.0; 3], 1.0));
        compose(&mut state, &i).unwrap();
        assert_eq!(row(&state, 0)[0], -7.0);
    }

    #[test]
    fn v4_view() {
        let mut state = started();
        let mut i = item(2, (1 << 6) | (1 << 7), 1);
        i.pass.view[12..16].copy_from_slice(&[f(10.0), f(20.0), f(30.0), f(1.0)]);
        compose(&mut state, &i).unwrap();
        assert_eq!(row(&state, 7), [1.0, 0.0, 0.0, 10.0]);
        assert_eq!(row(&state, 8), [0.0, 1.0, 0.0, 20.0]);
        assert_eq!(row(&state, 9), [0.0, 0.0, 1.0, 30.0]);
        let mut state = started();
        i.technique.flags = 1 << 6;
        compose(&mut state, &i).unwrap();
        assert_eq!(state.bank.row(0), None);
        assert_eq!(row(&state, 9), [0.0, 0.0, 1.0, 30.0]);
    }

    #[test]
    fn v5_group_eleven() {
        let mut state = started();
        compose(&mut state, &item(11, 0, 1)).unwrap();
        assert_eq!(row(&state, 3), [1.0, 0.0, 0.0, 0.0]);
        assert_eq!(row(&state, 4), [0.0, 1.0, 0.0, 0.0]);
        assert_eq!(row(&state, 5), [0.0, 0.0, 0.5, 0.0]);
        assert_eq!(row(&state, 6), [0.0, 0.0, -1.0, 1.0]);
    }

    fn atlas_memory() -> SparseMemory {
        let mut m = SparseMemory::new();
        m.put_u32(0x5000 + 72, (256 << 16) | 128);
        m
    }

    fn atlas_item<'a>() -> ItemInputs<'a> {
        let mut i = item(2, 1 << 29, 1);
        i.material_flags = 0x10;
        i.mbi[24..28].copy_from_slice(&[0x0c, 0x5f, 0x83, 0x01]);
        i.mbi[9..13].copy_from_slice(&0x5007u32.to_be_bytes());
        i
    }

    #[test]
    fn v6_atlas() {
        let mut state = started();
        let mut i = atlas_item();
        // Bit 0 of mbi+24 must be set; the vector's byte 3 is 0 there, which would refuse.
        state.compose(&i, &atlas_memory()).unwrap();
        assert_eq!(
            state.staging[18].unwrap().map(f32::from_bits),
            [0.25, 0.75, 3.875, 0.375]
        );
        i.mbi[27] = 0;
        assert_eq!(
            state.compose(&i, &atlas_memory()),
            Err(MaterialError::MissingAtlas)
        );
        i.mbi[27] = 1;
        i.material_flags = 0;
        assert_eq!(
            state.compose(&i, &atlas_memory()),
            Err(MaterialError::MissingAtlas)
        );
    }

    #[test]
    fn v7_parameter_column() {
        let mut memory = SparseMemory::new();
        let base = 0x8000;
        let words: Vec<u32> = (0..64).collect();
        memory.put_words(base, &words);
        let flags = 1u64 << 27;
        let mut mbi = [0u8; 32];
        mbi[24..28].copy_from_slice(&0x0003_0000u32.to_be_bytes());
        // idx 6: block 1 (base + 128), column 1.
        let column = read_parameter_column(&memory, flags, &mbi, base).unwrap();
        assert_eq!(
            column,
            ParameterColumn::Rows([[33, 37, 41, 45], [49, 53, 57, 61]])
        );
        let mut i = item(2, flags, 1);
        i.parameter_column = column;
        let mut state = started();
        state.compose(&i, &memory).unwrap();
        assert_eq!(state.bank.row(44), Some([33, 37, 41, 45]));
        assert_eq!(state.bank.row(45), Some([49, 53, 57, 61]));
        // No column index, or no table: the null rows.
        let null = ParameterColumn::Rows(NULL_PARAMETER_COLUMN);
        assert_eq!(
            read_parameter_column(&memory, flags, &[0; 32], base),
            Ok(null)
        );
        assert_eq!(read_parameter_column(&memory, flags, &mbi, 0), Ok(null));
        i.parameter_column = null;
        state.compose(&i, &memory).unwrap();
        assert_eq!(
            state.bank.row(44).map(|r| r.map(f32::from_bits)),
            Some([0.0, 0.0, 0.0, 0.001])
        );
        assert_eq!(
            state.bank.row(45).map(|r| r.map(f32::from_bits)),
            Some([1.0, 0.0, 0.0, 0.0])
        );
        // Sources read with bit 27 clear record no column; composing with bit 27 set then fails.
        assert_eq!(
            read_parameter_column(&memory, 0, &mbi, base),
            Ok(ParameterColumn::Unused)
        );
        i.parameter_column = ParameterColumn::Unused;
        assert_eq!(
            state.compose(&i, &memory),
            Err(MaterialError::MissingParameters)
        );
        // An unreadable block fails with the read error, not as unused.
        mbi[24..28].copy_from_slice(&0x0300_0000u32.to_be_bytes());
        assert!(matches!(
            read_parameter_column(&memory, flags, &mbi, base),
            Err(MaterialError::Read(_))
        ));
    }

    #[test]
    fn domain_rule_applies_only_to_computed_values() {
        let nan = f32::NAN.to_bits();
        let mut state = started();
        // Not computed: a NaN quaternion, translation or P lane is copied.
        let mut i = item(2, 0, 1);
        let mut t = transform([f32::NAN, 0.0, 0.0, 1.0], [f32::NAN, 0.0, 0.0], 1.0);
        t.matrix[2] = nan;
        i.transform = Some(t);
        compose(&mut state, &i).unwrap();
        assert_eq!(state.bank.row(3).unwrap()[2], nan);
        // With bit 7 the quaternion is computed with; the translation is still copied.
        i.technique.flags = 1 << 7;
        assert_eq!(
            compose(&mut state, &i),
            Err(MaterialError::Domain("quaternion"))
        );
        let mut t = transform(IDENTITY_Q, [f32::NAN, 0.0, 0.0], 1.0);
        i.transform = Some(t);
        compose(&mut state, &i).unwrap();
        assert_eq!(state.bank.row(0).unwrap()[3], nan);
        // With bit 6 the translation is computed with.
        i.technique.flags = 1 << 6;
        assert_eq!(
            compose(&mut state, &i),
            Err(MaterialError::Domain("translation"))
        );
        // Group 11: lanes 2 and 3 are computed with, lanes 0 and 1 are copied.
        t.translation = [0; 3];
        t.matrix[0] = nan;
        let mut g11 = item(11, 0, 1);
        g11.transform = Some(t);
        compose(&mut state, &g11).unwrap();
        assert_eq!(state.bank.row(3).unwrap()[0], nan);
        t.matrix[3] = nan;
        g11.transform = Some(t);
        assert_eq!(compose(&mut state, &g11), Err(MaterialError::Domain("P")));
    }

    #[test]
    fn v8_binding_row() {
        let mut i = item(5, 0, 1);
        i.mbi[4] = 1;
        assert_eq!(
            binding_row(&i).unwrap().map(f32::from_bits),
            [50.0, 0.25, 0.0, 7.0]
        );
        i.material_class = 1;
        i.material_flags = 0x8000;
        i.mbi[4] = 6;
        let r = binding_row(&i).unwrap();
        assert_eq!(f32::from_bits(r[0]), 200.0);
        assert!(crate::testutil::ulps(f32::from_bits(r[1]), f32::from_bits(0x3d88_8889)) <= 1);
        assert_eq!((r[2], f32::from_bits(r[3])), (0, 7.0));
        // Not forced with a negative or subnormal denominator uses the fallback inverse.
        let mut i = item(5, 0, 1);
        i.mbi[4] = 1;
        i.pass.distance_words[4] = f(-2.0);
        assert_eq!(f32::from_bits(binding_row(&i).unwrap()[1]), 100.0);
        i.pass.distance_words[4] = 1;
        assert_eq!(f32::from_bits(binding_row(&i).unwrap()[1]), 100.0);
        // Lane 2.
        for (group, control, lane2) in [
            (12, 3, 1.0),
            (13, 3, 1.0),
            (0, 5, 0.0),
            (10, 5, 0.0),
            (5, 5, 1.0),
            (5, 3, 0.0),
        ] {
            let mut i = item(group, 0, 1);
            i.pass.control = control;
            assert_eq!(f32::from_bits(binding_row(&i).unwrap()[2]), lane2);
        }
        // An alternate class reads the next distance word.
        let mut i = item(5, 0, 1);
        i.material_class = 2;
        assert_eq!(f32::from_bits(binding_row(&i).unwrap()[0]), 50.0);
    }

    #[test]
    fn v9_binding_action() {
        let mut state = started();
        let make = |material, group| ItemInputs {
            identity: Identity {
                material,
                technique: 0x100,
                selector: 0,
                group,
            },
            ..item(group, 0, material)
        };
        let actions: Vec<_> = [make(1, 2), make(2, 2), make(2, 2), make(2, 13)]
            .iter()
            .map(|i| compose(&mut state, i).unwrap().action)
            .collect();
        assert_eq!(
            actions,
            vec![
                BindingAction::Full,
                BindingAction::MaterialOnly,
                BindingAction::Unchanged,
                BindingAction::Full
            ]
        );
    }

    #[test]
    fn v10_update_only_then_prepared() {
        let mut state = started();
        let i = item(0, 0, 1);
        state.observe_update_only(i.identity).unwrap();
        assert_eq!(compose(&mut state, &i).unwrap().action, BindingAction::Full);
        assert_eq!(
            state.observe_update_only(Identity {
                group: 3,
                ..i.identity
            }),
            Err(MaterialError::UnqualifiedGroup(3))
        );
    }

    fn parameter(value_type: u8, words: &[u32], kind: u32, slot: u32) -> Parameter {
        Parameter {
            value_type,
            words: words.to_vec(),
            destination: (kind << 24) | (slot << 14),
        }
    }

    #[test]
    fn v11_scalar_parameters() {
        let writes = ordinary_writes(&[parameter(1, &[(-7i32) as u32], 2, 30)]).unwrap();
        assert_eq!(writes.vertex, vec![(30, [f(-7.0), 0, 0, f(1.0)])]);
        let writes = ordinary_writes(&[parameter(1, &[16_777_217], 2, 30)]).unwrap();
        assert_eq!(writes.vertex[0].1[0], f(16_777_216.0));
        assert_eq!(f32::from_bits(int_toward_zero(-16_777_219)), -16_777_218.0);
        assert_eq!(
            ordinary_writes(&[parameter(9, &[0; 12], 3, 1023)]),
            Err(MaterialError::Mapping {
                slot: 1023,
                rows: 3
            })
        );
        assert_eq!(
            ordinary_writes(&[parameter(11, &[], 2, 0)]),
            Err(MaterialError::ValueType(11))
        );
        assert_eq!(
            ordinary_writes(&[parameter(4, &[1], 1, 16)]),
            Err(MaterialError::CurrentAttributeSlot(16))
        );
        assert_eq!(
            ordinary_writes(&[parameter(8, &[0; 8], 1, 2)]),
            Err(MaterialError::Unqualified("matrix current attribute"))
        );
        let writes = ordinary_writes(&[
            parameter(5, &[9, 8], 1, 6),
            parameter(3, &[], 2, 0),
            parameter(6, &[1, 2, 3], 7, 0),
        ])
        .unwrap();
        assert_eq!(writes.current, vec![(6, [9, 8, 0, f(1.0)])]);
        assert!(writes.vertex.is_empty());
        let writes = ordinary_writes(&[parameter(8, &(0..8).collect::<Vec<_>>(), 3, 4)]).unwrap();
        assert_eq!(writes.fragment, vec![(4, [0, 1, 2, 3]), (5, [4, 5, 6, 7])]);
    }

    #[test]
    fn v12_staging() {
        let mut state = started();
        let mut i = atlas_item();
        i.technique.staging_first = 12;
        i.technique.staging_count = 7;
        state.compose(&i, &atlas_memory()).unwrap();
        for r in 12..18 {
            assert_eq!(state.bank.row(r), Some([0; 4]));
        }
        assert_eq!(row(&state, 18), [0.25, 0.75, 3.875, 0.375]);
        i.technique.staging_first = 40;
        i.technique.staging_count = 9;
        assert_eq!(
            state.compose(&i, &atlas_memory()),
            Err(MaterialError::SourceBounds {
                first: 40,
                count: 9
            })
        );
        // Without a window start, staging rows are unknown and copy their unknown state.
        let mut cold = MaterialValueState::new();
        let mut i = item(2, 0, 1);
        i.technique.staging_first = 0;
        i.technique.staging_count = 2;
        cold.bank.write(0, [5; 4]).unwrap();
        compose(&mut cold, &i).unwrap();
        assert_eq!(cold.bank.row(0), None);
    }

    #[test]
    fn v13_bank_read() {
        let mut bank = VertexBank::default();
        for r in 0..10 {
            bank.write(r, [r + 1; 4]).unwrap();
        }
        let image = bank.read_for_draw(&[3, 9], false).unwrap();
        assert_eq!(image.len(), 512);
        assert_eq!(image[10], [0; 4]);
        assert_eq!(
            bank.read_for_draw(&[10], false),
            Err(MaterialError::UnknownVertexConstant(10))
        );
        assert_eq!(
            bank.read_for_draw(&[], true),
            Err(MaterialError::UnknownVertexConstant(10))
        );
        assert_eq!(
            bank.read_for_draw(&[512], false),
            Err(MaterialError::VertexConstantRange(512))
        );
        assert_eq!(bank.row(10), None);
    }

    #[test]
    fn v14_invalidate_and_batch() {
        let mut bank = VertexBank::default();
        bank.write_batch(&[(1, [1; 4]), (5, [5; 4])]).unwrap();
        bank.invalidate(Some(&[5]));
        assert_eq!((bank.row(1), bank.row(5)), (Some([1; 4]), None));
        bank.invalidate(Some(&[5, 600]));
        assert_eq!(bank.row(1), None);
        assert_eq!(
            bank.write_batch(&[(2, [2; 4]), (512, [0; 4])]),
            Err(MaterialError::VertexConstantRange(512))
        );
        assert_eq!(bank.row(2), None);
        assert_eq!(
            bank.write(512, [0; 4]),
            Err(MaterialError::VertexConstantRange(512))
        );
    }

    #[test]
    fn binding_order_and_atomicity() {
        let mut state = started();
        let params = [
            parameter(7, &[1, 2, 3, 4], 2, 24),
            parameter(7, &[5, 6, 7, 8], 3, 0),
            parameter(7, &[9; 4], 2, 30),
        ];
        let statics = [(30, [7; 4])];
        let builtins = [(9, [3; 4])];
        let mut i = item(2, 0, 1);
        i.parameters = &params;
        i.static_rows = &statics;
        i.builtin_rows = &builtins;
        let c = compose(&mut state, &i).unwrap();
        assert_eq!(c.fragment_writes, vec![(0, [5, 6, 7, 8]), (9, [3; 4])]);
        // Static rows win over ordinary rows; the binding row wins over both at row 24.
        assert_eq!(state.bank.row(30), Some([7; 4]));
        assert_eq!(row(&state, 24)[0], 10.0);
        // Unchanged keeps the previous fragment writes.
        let unchanged = ItemInputs {
            builtin_rows: &[],
            ..i.clone()
        };
        assert_eq!(
            compose(&mut state, &unchanged).unwrap().fragment_writes,
            c.fragment_writes
        );
        // A failure leaves the state unchanged.
        let before = state.clone();
        let mut bad = item(2, 0, 9);
        bad.transform = None;
        assert_eq!(
            compose(&mut state, &bad),
            Err(MaterialError::MissingTransform)
        );
        assert_eq!(state, before);
        let mut bad = item(2, 1 << 7, 9);
        bad.transform = Some(transform([f32::NAN, 0.0, 0.0, 1.0], [0.0; 3], 1.0));
        assert_eq!(
            compose(&mut state, &bad),
            Err(MaterialError::Domain("quaternion"))
        );
        assert_eq!(
            compose(&mut state, &item(14, 0, 1)),
            Err(MaterialError::UnqualifiedGroup(14))
        );
        assert_eq!(state, before);
        // Overflowing results fail.
        let mut big = item(2, 1 << 7, 9);
        big.transform = Some(transform([1e20, 0.0, 0.0, 1.0], [0.0; 3], 1.0));
        assert_eq!(
            compose(&mut state, &big),
            Err(MaterialError::Overflow("transform"))
        );
    }

    #[test]
    fn extra_and_mesh_rows() {
        let mut state = started();
        let mut i = item(2, (1 << 23) | (1 << 26), 1);
        i.transform.as_mut().unwrap().extra_enable = 1;
        i.technique.staging_first = 12;
        i.technique.staging_count = 2;
        compose(&mut state, &i).unwrap();
        assert_eq!(state.bank.row(12), Some([f(7.0), f(8.0), 0, 0]));
        assert_eq!(state.bank.row(13), Some([1, 2, 3, 4]));
    }

    #[test]
    fn technique_walk_reads_guest_records() {
        let material = 0x1_0000;
        let chunk = 0x2_0000;
        let mut m = SparseMemory::new();
        let mut header = vec![0u8; 0x50];
        header[0x44..0x46].copy_from_slice(&0x100u16.to_be_bytes());
        header[0x46..0x48].copy_from_slice(&0x200u16.to_be_bytes());
        header[0x48..0x4a].copy_from_slice(&0x80u16.to_be_bytes());
        header[0x4c..0x4e].copy_from_slice(&3u16.to_be_bytes());
        m.put(material, &header);
        let mut technique = [0u8; 32];
        technique[1] = 2;
        technique[3] = 0;
        technique[8..16].copy_from_slice(&(1u64 << 7).to_be_bytes());
        m.put(material + 0x80 + 32, &technique);
        for (index, (mask, value_index, value_type, offset)) in
            [(4u32, 0u16, 4u8, 0u16), (4, 1, 5, 4), (1, 0, 4, 8)]
                .into_iter()
                .enumerate()
        {
            let mut record = [0u8; 16];
            record[4..8].copy_from_slice(&mask.to_be_bytes());
            record[8..10].copy_from_slice(&value_index.to_be_bytes());
            record[10] = value_type;
            record[12..14].copy_from_slice(&offset.to_be_bytes());
            m.put(material + 0x100 + 16 * index as u32, &record);
        }
        m.put_words(material + 0x200, &[11, 12, 13]);
        m.put_words(chunk, &[(2 << 24) | (40 << 14), (3 << 24) | (2 << 14), 0]);
        let (t, parameters) = read_technique(&m, material, chunk, 1).unwrap();
        assert_eq!(t.flags, 1 << 7);
        // The third record does not belong to membership bit 2, so the walk stops.
        assert_eq!(parameters.len(), 2);
        let writes = ordinary_writes(&parameters).unwrap();
        assert_eq!(writes.vertex, vec![(40, [11, 0, 0, f(1.0)])]);
        assert_eq!(writes.fragment, vec![(2, [12, 13, 0, f(1.0)])]);
    }

    /// Property: transform rows stay within the stated rotation tolerance.
    #[test]
    fn property_rotation_tolerance() {
        let mut rng = crate::testutil::Rng::new(81);
        for _ in 0..2_000 {
            let q: [f32; 4] = std::array::from_fn(|_| rng.range_f32(-2.0, 2.0));
            let s = rng.range_f32(-10.0, 10.0);
            let mut state = started();
            let mut i = item(2, 1 << 7, 1);
            i.transform = Some(transform(q, [0.0; 3], s));
            compose(&mut state, &i).unwrap();
            let exact = scaled_rotation(q.map(f64::from), s as f64);
            let size: f64 = 1.0 + q.iter().map(|v| (*v as f64).powi(2)).sum::<f64>();
            for r in 0..3 {
                let got = state.bank.row(r as u32).unwrap();
                for c in 0..3 {
                    let error = (f32::from_bits(got[c]) as f64 - exact[r][c]).abs();
                    assert!(error <= 2f64.powi(-21) * (s as f64).abs() * size + 1e-38);
                }
            }
        }
    }
}
