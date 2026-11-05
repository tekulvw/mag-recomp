//! Group-zero instance and wind deformation (`group-zero-deformation.md`).
//!
//! This is the single deformation for group 0. Every pass whose depth must coincide with Main
//! (the depth pre-pass in particular) must use these positions, or call this function with the
//! same inputs, so that depth-equal tests hold. The wind tuning coefficients are this
//! implementation's own ([`crate::waveform`]), so the original program's depth for these items
//! is not comparable bit for bit.

use crate::guest::{GuestMemory, SourceError};
use crate::rsx_location::{RsxAddressMap, read_rsx_location};
use crate::waveform::{KA1, KB1, KC, wave};

/// The guest fields that decide whether an item is admitted.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct GroupZeroAdmission {
    /// Draw-batch byte `+0x00`.
    pub draw_batch_byte0: u8,
    /// Pass `+0x144`.
    pub pass_control: u32,
    /// Pass `+0x230`.
    pub pass_mode: u32,
    /// Pass `+0x22C`.
    pub pass_workers: u32,
    /// Primary material `+0x58`.
    pub material_flags: u32,
    /// Configuration stream (stream 19) byte 4.
    pub output_selector: u8,
    /// Object transform byte `+0x6C`.
    pub instance_count: u8,
    /// Object transform `+0x60`.
    pub instance_location: u32,
    /// Any auxiliary binding (successful or failed) targets slot 12.
    pub auxiliary_slot12: bool,
    /// Mesh record `+0x34`, low 16 bits.
    pub colour_frequency: u32,
    /// Partition record `+0xFC`.
    pub batch_max: u32,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum GroupZeroError {
    Group,
    UpdateOnlyPass,
    GeometryMode(u32),
    WorkerControl(u32),
    OutputSelector(u8),
    InstancePopulation,
    InstanceLocation,
    SlotTwelveWriter,
    ColourFrequency(u32),
    BatchMaximum,
    NormalPopulation,
    WeightPopulation,
    Index(u32),
    Budget,
    /// A non-finite input or result.
    UnqualifiedFloat(&'static str),
    /// The colour pool is shorter than the indices need.
    Source,
}

/// Checks admission in table order.
pub fn admit_group_zero(a: &GroupZeroAdmission) -> Result<(), GroupZeroError> {
    if a.draw_batch_byte0 >> 4 != 0 {
        return Err(GroupZeroError::Group);
    }
    if a.pass_control <= 2 {
        return Err(GroupZeroError::UpdateOnlyPass);
    }
    let material_bit0 = a.material_flags & 1 != 0;
    let mode = (a.pass_mode | if material_bit0 { 0x20 } else { 0 }) & !0x2;
    if mode != 0x28 {
        return Err(GroupZeroError::GeometryMode(mode));
    }
    let workers = a.pass_workers.min(if material_bit0 { 1 } else { 2 });
    if workers != 1 {
        return Err(GroupZeroError::WorkerControl(workers));
    }
    if a.output_selector != 0 {
        return Err(GroupZeroError::OutputSelector(a.output_selector));
    }
    if a.instance_count == 0 {
        return Err(GroupZeroError::InstancePopulation);
    }
    if a.instance_location >> 31 == 0 {
        return Err(GroupZeroError::InstanceLocation);
    }
    if a.auxiliary_slot12 {
        return Err(GroupZeroError::SlotTwelveWriter);
    }
    if a.colour_frequency < 2 {
        return Err(GroupZeroError::ColourFrequency(a.colour_frequency));
    }
    if a.batch_max < 1 {
        return Err(GroupZeroError::BatchMaximum);
    }
    Ok(())
}

/// A "rebased half" of an instance row: every exponent is a normal exponent.
pub fn rebased_half(h: u16) -> f64 {
    let sign = if h & 0x8000 != 0 { -1.0 } else { 1.0 };
    if h & 0x7fff == 0 {
        return if sign < 0.0 { -0.0 } else { 0.0 };
    }
    let exponent = ((h >> 10) & 0x1f) as i32;
    let mantissa = (h & 0x3ff) as f64;
    sign * 2f64.powi(exponent - 15) * (1.0 + mantissa / 1024.0)
}

/// A fade half: binary16 layout, `E = 31` finite, subnormals at half the IEEE value.
pub fn fade_half(h: u16) -> f64 {
    let sign = if h & 0x8000 != 0 { -1.0 } else { 1.0 };
    let exponent = ((h >> 10) & 0x1f) as i32;
    let mantissa = (h & 0x3ff) as f64;
    match (exponent, mantissa == 0.0) {
        (0, true) => sign * 0.0,
        (0, false) => sign * mantissa * 2f64.powi(-25),
        _ => sign * 2f64.powi(exponent - 15) * (1.0 + mantissa / 1024.0),
    }
}

/// Plain inputs, as guest words.
#[derive(Clone, Copy, Debug)]
pub struct GroupZeroInputs<'a> {
    /// Pass `+0x260..+0x270`: `ω0`, `ω1` (unused), `ω2`, `ω3`, `θ`.
    pub wind: [u32; 5],
    /// Primary material rows `M0` and `M1`.
    pub material: [[u32; 4]; 2],
    /// Object transform `+0x00..+0x1C`: quaternion, translation `T`, scale.
    pub object: [u32; 8],
    /// Object transform `+0x68` and `+0x6A`: fade multiplier and fade start halves.
    pub fade: [u16; 2],
    /// View environment rows `E0..E3` (`+0x10` to `+0x40`).
    pub environment: [[u32; 4]; 4],
    /// One row of eight halves per instance.
    pub instances: &'a [[u16; 8]],
    pub colour_frequency: u32,
    pub batch_max: u32,
    /// Colour pool entries, R, G, B, A.
    pub colour_pool: &'a [[u8; 4]],
    pub positions: &'a [[u32; 4]],
    pub normals: &'a [[u32; 4]],
    pub weights: &'a [[u32; 4]],
    pub indices: &'a [u16],
}

/// Expanded instances, instance-major.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct GroupZeroOutput {
    /// `(x, y, z, 1)` per (instance, vertex).
    pub positions: Vec<[u32; 4]>,
    /// Slot-12 colour rows per (instance, vertex), as binary32 bits of `byte / 255`.
    pub colours: Vec<[u32; 4]>,
    pub indices: Vec<u32>,
}

/// Default output budget in vertices. Its size is not behaviour.
pub const DEFAULT_MAX_VERTICES: u64 = 1 << 22;

/// Colour pool index of instance `i`, vertex `v`.
pub fn colour_index(
    instance: u64,
    vertex: u64,
    vertices: u64,
    frequency: u64,
    batch_max: u64,
) -> u64 {
    let first = instance / batch_max * batch_max;
    first + ((instance - first) * vertices + vertex) / frequency
}

/// The number of colour pool entries the expansion reads.
pub fn required_pool_length(instances: u64, vertices: u64, frequency: u64, batch_max: u64) -> u64 {
    let mut length = 0;
    let mut first = 0;
    while first < instances {
        let count = batch_max.min(instances - first);
        if count * vertices > 0 {
            length = length.max(first + (count * vertices - 1) / frequency + 1);
        }
        first += batch_max;
    }
    length
}

/// The guest address of the instance rows: `G + (transform+0x60 AND 0x7FFFFFFF)`, modulo `2^32`.
pub fn instance_rows_address(geometry_base: u32, location: u32) -> u32 {
    geometry_base.wrapping_add(location & 0x7fff_ffff)
}

/// Reads `count` 16-byte instance rows. A location with bit 31 clear is refused by admission
/// before any read, so callers check [`admit_group_zero`] first.
pub fn read_instance_rows<M: GuestMemory + ?Sized>(
    memory: &M,
    geometry_base: u32,
    location: u32,
    count: usize,
) -> Result<Vec<[u16; 8]>, SourceError> {
    let address = instance_rows_address(geometry_base, location);
    let length = 16 * count as u32;
    let short = |reason: &str| SourceError {
        address,
        length,
        reason: reason.into(),
    };
    if address as u64 + length as u64 > 1 << 32 {
        return Err(short("range passes 2^32"));
    }
    let bytes = memory.read(address, length)?;
    if bytes.len() != length as usize {
        return Err(short("short read"));
    }
    Ok(bytes
        .as_chunks::<16>()
        .0
        .iter()
        .map(|r| std::array::from_fn(|i| u16::from_be_bytes([r[2 * i], r[2 * i + 1]])))
        .collect())
}

/// Reads `entries` colour pool entries at the RSX location `transform+0x64`.
pub fn read_colour_pool<M: GuestMemory + ?Sized>(
    memory: &M,
    map: &dyn RsxAddressMap,
    location: u32,
    entries: usize,
) -> Result<Vec<[u8; 4]>, SourceError> {
    let bytes = read_rsx_location(memory, map, location, 4 * entries as u32)?;
    Ok(bytes.as_chunks::<4>().0.to_vec())
}

fn finite(bits: u32, what: &'static str) -> Result<f64, GroupZeroError> {
    let value = f32::from_bits(bits);
    if value.is_finite() {
        Ok(value as f64)
    } else {
        Err(GroupZeroError::UnqualifiedFloat(what))
    }
}

/// One instance's placement: `placed = scale · (rotation · P) + offset`.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct InstancePlacement {
    pub rotation: [[f64; 3]; 3],
    pub scale: f64,
    pub offset: [f64; 3],
}

/// Everything the per-vertex evaluation needs once the inputs are admitted: the per-instance
/// placements and the per-draw wind terms.
#[derive(Clone, Debug, PartialEq)]
pub struct ValidatedGroupZero {
    pub placements: Vec<InstancePlacement>,
    /// `f = ω0 · M1.x`.
    pub frequency: f64,
    /// `A1 = M0.x · ω2` and `A2 = M0.y · ω3`.
    pub amplitudes: [f64; 2],
    /// `θ`.
    pub phase: f64,
    /// The colour pool entries the expansion reads.
    pub pool_length: u64,
}

/// Checks every refusal that does not depend on a per-vertex result and computes the
/// per-instance placements. [`deform_group_zero`] is this followed by [`deform_vertex`] for every
/// (instance, vertex); a per-vertex non-finite position is its only remaining refusal.
pub fn validate_group_zero(
    i: &GroupZeroInputs<'_>,
    max_vertices: u64,
) -> Result<ValidatedGroupZero, GroupZeroError> {
    let instances = i.instances.len() as u64;
    let vertices = i.positions.len() as u64;
    if instances == 0 {
        return Err(GroupZeroError::InstancePopulation);
    }
    if i.normals.len() as u64 != vertices {
        return Err(GroupZeroError::NormalPopulation);
    }
    if i.weights.len() as u64 != vertices {
        return Err(GroupZeroError::WeightPopulation);
    }
    if i.colour_frequency < 2 {
        return Err(GroupZeroError::ColourFrequency(i.colour_frequency));
    }
    if i.batch_max == 0 {
        return Err(GroupZeroError::BatchMaximum);
    }
    if let Some(index) = i
        .indices
        .iter()
        .find(|index| u64::from(**index) >= vertices)
    {
        return Err(GroupZeroError::Index(*index as u32));
    }
    let total = instances
        .checked_mul(vertices)
        .filter(|n| *n <= u32::MAX as u64);
    if total.is_none_or(|n| n > max_vertices) {
        return Err(GroupZeroError::Budget);
    }
    for (lane, word) in i.object.iter().enumerate() {
        finite(
            *word,
            if lane < 4 {
                "object quaternion"
            } else if lane < 7 {
                "object translation"
            } else {
                "object scale"
            },
        )?;
    }
    for row in &i.environment {
        for word in row {
            finite(*word, "environment")?;
        }
    }
    let (fade_multiplier, fade_start) = (fade_half(i.fade[0]), fade_half(i.fade[1]));
    if !fade_multiplier.is_finite() || !fade_start.is_finite() {
        return Err(GroupZeroError::UnqualifiedFloat("fade"));
    }
    let pool_length = required_pool_length(
        instances,
        vertices,
        i.colour_frequency as u64,
        i.batch_max as u64,
    );
    if (i.colour_pool.len() as u64) < pool_length {
        return Err(GroupZeroError::Source);
    }

    let f = |word: u32| f32::from_bits(word) as f64;
    let translation = [f(i.object[4]), f(i.object[5]), f(i.object[6])];
    let e3 = i.environment[3].map(f);
    let mut placements = Vec::with_capacity(i.instances.len());
    for row in i.instances {
        let h = row.map(rebased_half);
        let offset = [h[0], h[1], h[2]];
        let sigma = h[3];
        let q = [h[4], h[5], h[6], h[7]];
        let length = q.iter().map(|v| v * v).sum::<f64>().sqrt();
        let [x, y, z, w] = if length == 0.0 {
            [0.0; 4]
        } else {
            q.map(|v| v / length)
        };
        let rotation = if length == 0.0 {
            [[1.0, 0.0, 0.0], [0.0, 1.0, 0.0], [0.0, 0.0, 1.0]]
        } else {
            [
                [
                    1.0 - 2.0 * (y * y + z * z),
                    2.0 * (x * y - w * z),
                    2.0 * (x * z + w * y),
                ],
                [
                    2.0 * (x * y + w * z),
                    1.0 - 2.0 * (x * x + z * z),
                    2.0 * (y * z - w * x),
                ],
                [
                    2.0 * (x * z - w * y),
                    2.0 * (y * z + w * x),
                    1.0 - 2.0 * (x * x + y * y),
                ],
            ]
        };
        let centre = [
            translation[0] + offset[0],
            translation[1] + offset[1],
            translation[2] + offset[2],
        ];
        let depth = e3[0] * centre[0] + e3[1] * centre[1] + e3[2] * centre[2] + e3[3];
        let product = (depth - fade_start) * fade_multiplier;
        if !product.is_finite() || product.abs() > f32::MAX as f64 {
            return Err(GroupZeroError::UnqualifiedFloat("fade product"));
        }
        placements.push(InstancePlacement {
            rotation,
            scale: sigma * (1.0 - product.clamp(0.0, 1.0)),
            offset,
        });
    }
    Ok(ValidatedGroupZero {
        placements,
        frequency: f(i.wind[0]) * f(i.material[1][0]),
        amplitudes: [
            f(i.material[0][0]) * f(i.wind[2]),
            f(i.material[0][1]) * f(i.wind[3]),
        ],
        phase: f(i.wind[4]),
        pool_length,
    })
}

/// The deformed `(x, y, z)` of one source vertex under one placement, before narrowing.
pub fn deform_vertex(
    v: &ValidatedGroupZero,
    placement: &InstancePlacement,
    position: [u32; 4],
    normal: [u32; 4],
    weight: [u32; 4],
) -> [f64; 3] {
    let f = |word: u32| f32::from_bits(word) as f64;
    let (ka1, kb1, kc) = (KA1 as f64, KB1 as f64, KC as f64);
    let p = [0, 1, 2].map(|l| f(position[l]));
    let n = [0, 1, 2].map(|l| f(normal[l]));
    let w3 = f(weight[3]);
    let placed: [f64; 3] = std::array::from_fn(|r| {
        placement.scale * (0..3).map(|c| placement.rotation[r][c] * p[c]).sum::<f64>()
            + placement.offset[r]
    });
    let alpha = ka1 * (v.phase + placed[0] + placed[1]);
    let beta = kb1 * (v.phase + kc * (placed[0] + placed[1] + placed[2]));
    let displacement = (v.amplitudes[0] * wave(beta, v.frequency)
        + v.amplitudes[1] * wave(alpha, v.frequency))
        * (1.0 - w3);
    [
        placed[0] + displacement * n[0],
        placed[1] + displacement * n[1],
        placed[2] + displacement * n[0],
    ]
}

/// The slot-12 colour row of a pool entry: `byte / 255` per channel.
pub fn colour_row(entry: [u8; 4]) -> [u32; 4] {
    entry.map(|byte| (byte as f32 / 255.0).to_bits())
}

/// Expands and deforms every instance. All-or-nothing.
pub fn deform_group_zero(
    i: &GroupZeroInputs<'_>,
    max_vertices: u64,
) -> Result<GroupZeroOutput, GroupZeroError> {
    let v = validate_group_zero(i, max_vertices)?;
    let vertices = i.positions.len() as u64;
    let total = v.placements.len() * i.positions.len();
    let mut out = GroupZeroOutput {
        positions: Vec::with_capacity(total),
        colours: Vec::with_capacity(total),
        indices: Vec::with_capacity(i.instances.len() * i.indices.len()),
    };
    for (instance, placement) in v.placements.iter().enumerate() {
        for vertex in 0..i.positions.len() {
            let lanes = deform_vertex(
                &v,
                placement,
                i.positions[vertex],
                i.normals[vertex],
                i.weights[vertex],
            );
            let mut position = [0u32; 4];
            for (lane, value) in lanes.iter().enumerate() {
                let value = *value as f32;
                if !value.is_finite() {
                    return Err(GroupZeroError::UnqualifiedFloat("output position"));
                }
                position[lane] = value.to_bits();
            }
            position[3] = 1f32.to_bits();
            out.positions.push(position);
            let index = colour_index(
                instance as u64,
                vertex as u64,
                vertices,
                i.colour_frequency as u64,
                i.batch_max as u64,
            );
            out.colours.push(colour_row(i.colour_pool[index as usize]));
        }
        let base = instance as u32 * vertices as u32;
        out.indices
            .extend(i.indices.iter().map(|k| base + *k as u32));
    }
    Ok(out)
}

/// The allowed absolute error of output position lane `lane` (`group-zero-deformation.md`,
/// Tolerance): `2^-18·M + (|A1| + |A2|)·|1 − w3|·|N_lane|·(2^-18 + 6·|f|·δ)` with
/// `M = |s|·(|P.x| + |P.y| + |P.z|) + |o_lane|` and `δ = 2^-20·(1 + |θ| + 4·M)`. `N_lane` is
/// `N.x` for the x and z lanes and `N.y` for the y lane.
pub fn position_tolerance(
    v: &ValidatedGroupZero,
    placement: &InstancePlacement,
    position: [u32; 4],
    normal: [u32; 4],
    weight: [u32; 4],
    lane: usize,
) -> f64 {
    let f = |word: u32| f32::from_bits(word) as f64;
    let size = placement.scale.abs() * (0..3).map(|l| f(position[l]).abs()).sum::<f64>()
        + placement.offset[lane].abs();
    let delta = 2f64.powi(-20) * (1.0 + v.phase.abs() + 4.0 * size);
    let n = f(normal[if lane == 1 { 1 } else { 0 }]).abs();
    2f64.powi(-18) * size
        + (v.amplitudes[0].abs() + v.amplitudes[1].abs())
            * (1.0 - f(weight[3])).abs()
            * n
            * (2f64.powi(-18) + 6.0 * v.frequency.abs() * delta)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn gz8_instance_row_address() {
        assert_eq!(instance_rows_address(0x1000_0000, 0x8000_1230), 0x1000_1230);
        let mut memory = crate::guest::SparseMemory::new();
        memory.put(0x1000_1230, &[0x3c, 0x00].repeat(8));
        assert_eq!(
            read_instance_rows(&memory, 0x1000_0000, 0x8000_1230, 1),
            Ok(vec![[0x3c00; 8]])
        );
        assert!(read_instance_rows(&memory, 0x1000_0000, 0x8000_1230, 2).is_err());
        // Bit 31 clear is refused by admission, before any read.
        let mut admission = GroupZeroAdmission {
            draw_batch_byte0: 0,
            pass_control: 3,
            pass_mode: 8,
            pass_workers: 1,
            material_flags: 1,
            output_selector: 0,
            instance_count: 1,
            instance_location: 0x0000_1230,
            auxiliary_slot12: false,
            colour_frequency: 2,
            batch_max: 1,
        };
        assert_eq!(
            admit_group_zero(&admission),
            Err(GroupZeroError::InstanceLocation)
        );
        admission.instance_location = 0x8000_1230;
        assert_eq!(admit_group_zero(&admission), Ok(()));
    }
    use crate::testutil::Rng;

    const ONE: u16 = 0x3c00;
    const IDENTITY_ENV: [[u32; 4]; 4] = [
        [0x3f80_0000, 0, 0, 0],
        [0, 0x3f80_0000, 0, 0],
        [0, 0, 0x3f80_0000, 0],
        [0, 0, 0, 0x3f80_0000],
    ];

    fn bits(values: [f32; 4]) -> [u32; 4] {
        values.map(f32::to_bits)
    }

    fn object(t: [f32; 3]) -> [u32; 8] {
        [0.0, 0.0, 0.0, 1.0, t[0], t[1], t[2], 1.0].map(f32::to_bits)
    }

    struct Case {
        wind: [f32; 5],
        material: [[f32; 4]; 2],
        object: [u32; 8],
        fade: [u16; 2],
        environment: [[u32; 4]; 4],
        instances: Vec<[u16; 8]>,
        frequency: u32,
        batch_max: u32,
        pool: Vec<[u8; 4]>,
        positions: Vec<[u32; 4]>,
        normals: Vec<[u32; 4]>,
        weights: Vec<[u32; 4]>,
        indices: Vec<u16>,
    }

    impl Case {
        fn new(positions: &[[f32; 3]]) -> Self {
            let v = positions.len();
            Self {
                wind: [0.0; 5],
                material: [[0.0; 4]; 2],
                object: object([0.0; 3]),
                fade: [0, 0],
                environment: IDENTITY_ENV,
                instances: vec![[0, 0, 0, ONE, 0, 0, 0, ONE]],
                frequency: 3,
                batch_max: 255,
                pool: vec![[255, 0, 0, 255]; 16],
                positions: positions
                    .iter()
                    .map(|p| bits([p[0], p[1], p[2], 1.0]))
                    .collect(),
                normals: vec![bits([0.0, 0.0, 1.0, 0.0]); v],
                weights: vec![bits([0.0, 0.0, 0.0, 1.0]); v],
                indices: (0..v as u16).collect(),
            }
        }

        fn inputs(&self) -> GroupZeroInputs<'_> {
            GroupZeroInputs {
                wind: self.wind.map(f32::to_bits),
                material: self.material.map(bits),
                object: self.object,
                fade: self.fade,
                environment: self.environment,
                instances: &self.instances,
                colour_frequency: self.frequency,
                batch_max: self.batch_max,
                colour_pool: &self.pool,
                positions: &self.positions,
                normals: &self.normals,
                weights: &self.weights,
                indices: &self.indices,
            }
        }

        fn run(&self) -> Result<GroupZeroOutput, GroupZeroError> {
            deform_group_zero(&self.inputs(), DEFAULT_MAX_VERTICES)
        }
    }

    fn floats(out: &GroupZeroOutput) -> Vec<[f32; 4]> {
        out.positions
            .iter()
            .map(|p| p.map(f32::from_bits))
            .collect()
    }

    #[test]
    fn gz1_placement_and_rotation() {
        let mut case = Case::new(&[[1.0, 0.0, 0.0], [0.0, 2.0, 0.0]]);
        case.indices = vec![0, 1, 1];
        case.batch_max = 1;
        case.pool = vec![[10, 0, 0, 0], [20, 0, 0, 0]];
        // Instance 0: offset (3, 0, 0), scale 1. Instance 1: offset (0, 0, -1), scale 0.5,
        // quaternion (0, 0, 1, 1).
        case.instances = vec![
            [0x4200, 0, 0, ONE, 0, 0, 0, ONE],
            [0, 0, 0xbc00, 0x3800, 0, 0, ONE, ONE],
        ];
        let out = case.run().unwrap();
        let expected = [
            [4.0, 0.0, 0.0, 1.0],
            [3.0, 2.0, 0.0, 1.0],
            [0.0, 0.5, -1.0, 1.0],
            [-1.0, 0.0, -1.0, 1.0],
        ];
        for (got, want) in floats(&out).iter().zip(expected) {
            for lane in 0..4 {
                assert!((got[lane] - want[lane]).abs() < 1e-6, "{got:?} vs {want:?}");
            }
        }
        assert_eq!(out.indices, vec![0, 1, 1, 2, 3, 3]);
        let reds: Vec<u32> = out
            .colours
            .iter()
            .map(|c| (f32::from_bits(c[0]) * 255.0).round() as u32)
            .collect();
        assert_eq!(reds, vec![10, 10, 20, 20]);
        assert_eq!(required_pool_length(2, 2, 3, 1), 2);
    }

    #[test]
    fn gz2_fade() {
        for (tz, expected) in [(0.0, 2.0), (3.0, 1.0), (10.0, 0.0)] {
            let mut case = Case::new(&[[1.0, 1.0, 1.0]]);
            case.instances = vec![[0, 0, 0, 0x4000, 0, 0, 0, ONE]];
            case.environment[3] = bits([0.0, 0.0, 1.0, 0.0]);
            case.object = object([0.0, 0.0, tz]);
            case.fade = [0x3800, 0x4000];
            assert_eq!(
                floats(&case.run().unwrap())[0],
                [expected, expected, expected, 1.0]
            );
        }
    }

    #[test]
    fn gz3_wind_free_of_constants() {
        for (m1x, expected) in [
            (0.5, [0.375, 0.75, 0.375, 1.0]),
            (1.0, [0.0, 0.0, 0.0, 1.0]),
        ] {
            let mut case = Case::new(&[[0.0, 0.0, 0.0]]);
            case.normals = vec![bits([1.0, 2.0, 3.0, 0.0])];
            case.weights = vec![bits([0.0, 0.0, 0.0, 0.5])];
            case.material = [[0.5, 1.0, 0.0, 0.0], [m1x, 0.0, 0.0, 0.0]];
            case.wind = [1.0, 0.0, 0.5, 0.5, 0.0];
            assert_eq!(floats(&case.run().unwrap())[0], expected);
        }
    }

    #[test]
    fn gz5_colour_batches() {
        let indices: Vec<Vec<u64>> = (0..5)
            .map(|i| (0..3).map(|v| colour_index(i, v, 3, 4, 2)).collect())
            .collect();
        assert_eq!(
            indices,
            vec![
                vec![0, 0, 0],
                vec![0, 1, 1],
                vec![2, 2, 2],
                vec![2, 3, 3],
                vec![4, 4, 4]
            ]
        );
        assert_eq!(required_pool_length(5, 3, 4, 2), 5);
        let indices: Vec<Vec<u64>> = (0..3)
            .map(|i| (0..4).map(|v| colour_index(i, v, 4, 6, 255)).collect())
            .collect();
        assert_eq!(
            indices,
            vec![vec![0, 0, 0, 0], vec![0, 0, 1, 1], vec![1, 1, 1, 1]]
        );
        assert_eq!(required_pool_length(3, 4, 6, 255), 2);
    }

    #[test]
    fn gz6_closed_form_with_tuning_values() {
        for (frequency, d) in [(0.5, 0.413_344_1), (1.0, 1.228_352_1)] {
            let mut case = Case::new(&[[1.0, 0.0, 0.0]]);
            case.normals = vec![bits([1.0, 0.0, 0.0, 0.0])];
            case.material = [[1.0, 1.0, 0.0, 0.0], [frequency, 0.0, 0.0, 0.0]];
            case.wind = [1.0, 0.0, 1.0, 1.0, 0.0];
            case.weights = vec![bits([0.0; 4])];
            let got = floats(&case.run().unwrap())[0];
            let expected = [1.0 + d, 0.0, d, 1.0];
            for lane in 0..4 {
                assert!(
                    (got[lane] - expected[lane]).abs() <= 1e-6,
                    "f {frequency}: {got:?}"
                );
            }
        }
    }

    #[test]
    fn gz7_refusals() {
        let mut case = Case::new(&[[0.0; 3]]);
        case.instances.clear();
        assert_eq!(case.run(), Err(GroupZeroError::InstancePopulation));
        let mut case = Case::new(&[[0.0; 3], [1.0; 3]]);
        case.normals.pop();
        assert_eq!(case.run(), Err(GroupZeroError::NormalPopulation));
        let mut case = Case::new(&[[0.0; 3], [1.0; 3]]);
        case.indices = vec![2];
        assert_eq!(case.run(), Err(GroupZeroError::Index(2)));
        let mut case = Case::new(&[[0.0; 3]]);
        case.frequency = 1;
        assert_eq!(case.run(), Err(GroupZeroError::ColourFrequency(1)));
        let mut case = Case::new(&[[0.0; 3]]);
        case.environment[1][1] = f32::NAN.to_bits();
        assert_eq!(
            case.run(),
            Err(GroupZeroError::UnqualifiedFloat("environment"))
        );
        let mut case = Case::new(&[[0.0; 3]]);
        case.pool.clear();
        assert_eq!(case.run(), Err(GroupZeroError::Source));
        let case = Case::new(&[[0.0; 3]]);
        assert_eq!(
            deform_group_zero(&case.inputs(), 0),
            Err(GroupZeroError::Budget)
        );
    }

    #[test]
    fn half_decodings() {
        assert_eq!(rebased_half(0x3c00), 1.0);
        assert_eq!(rebased_half(0x8000).to_bits(), (-0.0f64).to_bits());
        assert_eq!(rebased_half(0x0001), 2f64.powi(-15) * (1.0 + 1.0 / 1024.0));
        assert_eq!(
            rebased_half(0x7fff),
            2f64.powi(16) * (1.0 + 1023.0 / 1024.0)
        );
        assert_eq!(fade_half(0x3800), 0.5);
        assert_eq!(fade_half(0x0001), 2f64.powi(-25));
        assert_eq!(fade_half(0x7c00), 65536.0);
    }

    #[test]
    fn admission() {
        let ok = GroupZeroAdmission {
            draw_batch_byte0: 0x00,
            pass_control: 3,
            pass_mode: 0x28,
            pass_workers: 1,
            material_flags: 0,
            output_selector: 0,
            instance_count: 2,
            instance_location: 0x8000_0010,
            auxiliary_slot12: false,
            colour_frequency: 2,
            batch_max: 1,
        };
        assert_eq!(admit_group_zero(&ok), Ok(()));
        let cases: [(GroupZeroAdmission, GroupZeroError); 10] = [
            (
                GroupZeroAdmission {
                    draw_batch_byte0: 0x10,
                    ..ok
                },
                GroupZeroError::Group,
            ),
            (
                GroupZeroAdmission {
                    pass_control: 2,
                    ..ok
                },
                GroupZeroError::UpdateOnlyPass,
            ),
            (
                GroupZeroAdmission { pass_mode: 8, ..ok },
                GroupZeroError::GeometryMode(8),
            ),
            (
                GroupZeroAdmission {
                    pass_workers: 2,
                    ..ok
                },
                GroupZeroError::WorkerControl(2),
            ),
            (
                GroupZeroAdmission {
                    output_selector: 1,
                    ..ok
                },
                GroupZeroError::OutputSelector(1),
            ),
            (
                GroupZeroAdmission {
                    instance_count: 0,
                    ..ok
                },
                GroupZeroError::InstancePopulation,
            ),
            (
                GroupZeroAdmission {
                    instance_location: 0x10,
                    ..ok
                },
                GroupZeroError::InstanceLocation,
            ),
            (
                GroupZeroAdmission {
                    auxiliary_slot12: true,
                    ..ok
                },
                GroupZeroError::SlotTwelveWriter,
            ),
            (
                GroupZeroAdmission {
                    colour_frequency: 1,
                    ..ok
                },
                GroupZeroError::ColourFrequency(1),
            ),
            (
                GroupZeroAdmission { batch_max: 0, ..ok },
                GroupZeroError::BatchMaximum,
            ),
        ];
        for (admission, error) in cases {
            assert_eq!(admit_group_zero(&admission), Err(error));
        }
        // Material bit 0 sets mode bit 0x20 and caps workers at 1.
        assert_eq!(
            admit_group_zero(&GroupZeroAdmission {
                pass_mode: 0x8,
                material_flags: 1,
                pass_workers: 2,
                ..ok
            }),
            Ok(())
        );
    }

    /// Property: the wind displacement stays within `[0, |A1| + |A2|]·|1 − w3|` along the normal
    /// lanes, and identical inputs give identical bits.
    #[test]
    fn property_envelope_and_determinism() {
        let mut rng = Rng::new(63);
        for _ in 0..2_000 {
            let mut r = |lo: f32, hi: f32| rng.range_f32(lo, hi);
            let mut case = Case::new(&[[r(-5.0, 5.0), r(-5.0, 5.0), r(-5.0, 5.0)]]);
            let n = [r(-1.0, 1.0), r(-1.0, 1.0), r(-1.0, 1.0)];
            case.normals = vec![bits([n[0], n[1], n[2], 0.0])];
            let w3 = r(0.0, 1.0);
            case.weights = vec![bits([0.0, 0.0, 0.0, w3])];
            case.material = [
                [r(0.0, 2.0), r(0.0, 2.0), 0.0, 0.0],
                [r(0.1, 2.0), 0.0, 0.0, 0.0],
            ];
            case.wind = [r(0.1, 2.0), 0.0, r(0.0, 2.0), r(0.0, 2.0), r(-50.0, 50.0)];
            let still = {
                let mut calm = Case::new(&[[0.0; 3]]);
                calm.positions = case.positions.clone();
                calm.normals = case.normals.clone();
                calm.weights = case.weights.clone();
                floats(&calm.run().unwrap())[0]
            };
            let out = case.run().unwrap();
            assert_eq!(out, case.run().unwrap());
            let got = floats(&out)[0];
            let a = (case.material[0][0] * case.wind[2]).abs()
                + (case.material[0][1] * case.wind[3]).abs();
            let bound = a as f64 * (1.0 - w3 as f64).abs() + 1e-5;
            for (lane, normal) in [(0, n[0]), (1, n[1]), (2, n[0])] {
                let delta = (got[lane] - still[lane]) as f64;
                assert!(
                    delta.abs() <= bound * normal.abs() as f64 + 1e-5,
                    "lane {lane}"
                );
            }
        }
    }
}
