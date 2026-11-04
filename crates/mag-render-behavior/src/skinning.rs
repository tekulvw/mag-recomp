//! Group-11 palette skinning (`skinning-deformation.md`).

use crate::guest::be_u32;

/// The guest fields that decide whether an item is admitted.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct SkinningAdmission {
    /// Mesh record `+0xBC`.
    pub mesh_group: u16,
    /// Draw-batch byte `+0x00`; its high nibble is the batch group.
    pub draw_batch_byte0: u8,
    /// Mesh record `+0x9C`.
    pub mesh_flags: u32,
    /// Object transform byte `+0x6F`.
    pub object_byte_6f: u8,
    /// Primary material `+0x58`.
    pub material_flags: u32,
    /// The item's pass record address.
    pub pass_address: u32,
    /// Draw-batch byte `+0x18`.
    pub draw_batch_byte18: u8,
    /// Draw-batch byte `+0x1B`.
    pub draw_batch_byte1b: u8,
    /// Draw-batch `u32 +0x14`.
    pub draw_batch_palette: u32,
    /// Pass `+0x230`.
    pub pass_mode: u32,
    /// Pass `+0x22C`.
    pub pass_workers: u32,
    /// The partition's configuration stream (stream 19).
    pub configuration: Vec<u8>,
    /// Partition `u16 +0x14` and `u16 +0x16`.
    pub segment_offsets: [u16; 2],
    /// Partition `u32 +0x48` and `u32 +0x50`.
    pub segment_lengths: [u32; 2],
}

/// Where the two palette segments are in guest memory. A zero-length segment is not read.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PaletteLocation {
    /// (address, length) of segments 0 and 1.
    pub segments: [(u32, u32); 2],
}

/// A guest word that is NaN or infinite.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum NonFiniteWord {
    Position {
        vertex: usize,
        lane: usize,
    },
    Normal {
        vertex: usize,
        lane: usize,
    },
    Tangent {
        vertex: usize,
        lane: usize,
    },
    Palette {
        matrix: usize,
        element: usize,
    },
    /// Byte offset in the object transform record.
    Object {
        offset: u32,
    },
    Output {
        bank: u32,
        vertex: usize,
        lane: usize,
    },
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SkinningError {
    Group,
    Static,
    /// Not a skinned source, or an empty palette.
    PaletteInputs,
    PassMissing,
    PostProcessing,
    GeometryMode(u32),
    WorkerControl(u32),
    Configuration,
    /// A palette length does not fit 16 bits.
    UnsupportedControl {
        slot: usize,
    },
    /// Draw-batch byte `+0x1B` bit 0 set: the palette base is unconfirmed (open question 2), so
    /// the case is refused.
    PaletteBaseUnconfirmed,
    Population {
        semantic: u32,
    },
    SourceExtent,
    Budget,
    NonFinite(NonFiniteWord),
    Joint {
        vertex: usize,
        lane: usize,
        index: u32,
    },
}

/// Checks admission and locates the palette.
pub fn admit_skinning(a: &SkinningAdmission) -> Result<PaletteLocation, SkinningError> {
    if a.mesh_group != 11 || u16::from(a.draw_batch_byte0 >> 4) != a.mesh_group {
        return Err(SkinningError::Group);
    }
    if a.mesh_flags & 0x1000 != 0 {
        return Err(SkinningError::Static);
    }
    if a.object_byte_6f == 1 || a.material_flags & 0x20 == 0 {
        return Err(SkinningError::PaletteInputs);
    }
    if a.pass_address == 0 {
        return Err(SkinningError::PassMissing);
    }
    let byte1b_bit0 = a.draw_batch_byte1b & 1 != 0;
    if a.draw_batch_byte18 >> 2 != 0 && !byte1b_bit0 {
        return Err(SkinningError::PostProcessing);
    }
    let material_bit0 = a.material_flags & 1 != 0;
    let mode = (a.pass_mode | if material_bit0 { 0x20 } else { 0 }) & !0x2;
    if mode != 8 && mode != 10 {
        return Err(SkinningError::GeometryMode(mode));
    }
    let workers = a.pass_workers.min(if material_bit0 { 1 } else { 2 });
    if workers != 2 {
        return Err(SkinningError::WorkerControl(workers));
    }
    let c = &a.configuration;
    if c.len() < 16 || c[0] != 0x83 || c[6] != 17 || c[7] != 0 || c[12..16] != [0xff; 4] {
        return Err(SkinningError::Configuration);
    }
    for (slot, length) in a.segment_lengths.iter().enumerate() {
        if length >> 16 != 0 {
            return Err(SkinningError::UnsupportedControl { slot });
        }
    }
    if byte1b_bit0 {
        return Err(SkinningError::PaletteBaseUnconfirmed);
    }
    let base = a.draw_batch_palette & !0xf;
    Ok(PaletteLocation {
        segments: [0, 1].map(|slot| {
            (
                base.wrapping_add(a.segment_offsets[slot] as u32),
                a.segment_lengths[slot],
            )
        }),
    })
}

/// Plain inputs of the skinning computation.
#[derive(Clone, Copy, Debug)]
pub struct SkinningInputs<'a> {
    /// Palette segment bytes, exactly `L0` and `L1` long.
    pub segments: [&'a [u8]; 2],
    /// Stream 5: one 8-byte record per vertex.
    pub influences: &'a [u8],
    pub positions: &'a [[f32; 4]],
    pub normals: &'a [[f32; 4]],
    pub tangents: &'a [[f32; 4]],
    /// Object transform words `+0x00..+0x1C`: quaternion, translation, scale.
    pub object: [u32; 8],
}

/// Skinned banks, in vertex order. There is no weight bank; a consumer asking for semantic 9 on
/// this item must refuse.
#[derive(Clone, Debug, PartialEq)]
pub struct SkinnedBanks {
    pub positions: Vec<[f32; 4]>,
    pub normals: Vec<[f32; 4]>,
    pub tangents: Vec<[f32; 4]>,
}

/// Default vertex budget. Its size is not behaviour.
pub const DEFAULT_MAX_VERTICES: usize = 1 << 20;

type Palette = Vec<[[f64; 4]; 3]>;

fn check_bank(
    bank: &[[f32; 4]],
    word: fn(usize, usize) -> NonFiniteWord,
) -> Result<(), SkinningError> {
    for (vertex, row) in bank.iter().enumerate() {
        if let Some(lane) = row.iter().position(|v| !v.is_finite()) {
            return Err(SkinningError::NonFinite(word(vertex, lane)));
        }
    }
    Ok(())
}

/// The object direction matrix `σ·R̃`, with `R̃` built literally from the stored quaternion.
pub fn object_direction_matrix(quaternion: [f64; 4], scale: f64) -> [[f64; 3]; 3] {
    let [x, y, z, w] = quaternion;
    let r = [
        [
            1.0 - 2.0 * (y * y + z * z),
            2.0 * (x * y - z * w),
            2.0 * (x * z + y * w),
        ],
        [
            2.0 * (x * y + z * w),
            1.0 - 2.0 * (x * x + z * z),
            2.0 * (y * z - x * w),
        ],
        [
            2.0 * (x * z - y * w),
            2.0 * (y * z + x * w),
            1.0 - 2.0 * (x * x + y * y),
        ],
    ];
    r.map(|row| row.map(|v| v * scale))
}

/// Skins every vertex. All-or-nothing.
pub fn skin(
    inputs: &SkinningInputs<'_>,
    max_vertices: usize,
) -> Result<SkinnedBanks, SkinningError> {
    let vertices = inputs.positions.len();
    if inputs.normals.len() != vertices {
        return Err(SkinningError::Population { semantic: 2 });
    }
    if inputs.tangents.len() != vertices {
        return Err(SkinningError::Population { semantic: 3 });
    }
    if inputs.influences.len() < 8 * vertices {
        return Err(SkinningError::SourceExtent);
    }
    let joined: Vec<u8> = inputs.segments[0]
        .iter()
        .chain(inputs.segments[1])
        .copied()
        .collect();
    let joints = joined.len() / 48;
    if joints == 0 {
        return Err(SkinningError::PaletteInputs);
    }
    if vertices > max_vertices {
        return Err(SkinningError::Budget);
    }

    check_bank(inputs.positions, |vertex, lane| NonFiniteWord::Position {
        vertex,
        lane,
    })?;
    check_bank(inputs.normals, |vertex, lane| NonFiniteWord::Normal {
        vertex,
        lane,
    })?;
    check_bank(inputs.tangents, |vertex, lane| NonFiniteWord::Tangent {
        vertex,
        lane,
    })?;
    let mut palette: Palette = Vec::with_capacity(joints);
    for matrix in 0..joints {
        let mut rows = [[0.0f64; 4]; 3];
        for element in 0..12 {
            let value = f32::from_bits(be_u32(&joined, 48 * matrix + 4 * element));
            if !value.is_finite() {
                return Err(SkinningError::NonFinite(NonFiniteWord::Palette {
                    matrix,
                    element,
                }));
            }
            rows[element / 4][element % 4] = value as f64;
        }
        palette.push(rows);
    }
    let object = inputs.object.map(f32::from_bits);
    if let Some(index) = object.iter().position(|v| !v.is_finite()) {
        return Err(SkinningError::NonFinite(NonFiniteWord::Object {
            offset: 4 * index as u32,
        }));
    }
    for vertex in 0..vertices {
        for lane in 0..4 {
            let index = inputs.influences[8 * vertex + 2 * lane + 1] as u32;
            if index as usize >= joints {
                return Err(SkinningError::Joint {
                    vertex,
                    lane,
                    index,
                });
            }
        }
    }

    let direction = object_direction_matrix(
        [object[0], object[1], object[2], object[3]].map(f64::from),
        object[7] as f64,
    );
    let mut out = SkinnedBanks {
        positions: Vec::with_capacity(vertices),
        normals: Vec::with_capacity(vertices),
        tangents: Vec::with_capacity(vertices),
    };
    for vertex in 0..vertices {
        let record = &inputs.influences[8 * vertex..8 * vertex + 8];
        let mut blended = [[0.0f64; 4]; 3];
        for k in 0..4 {
            let weight = record[2 * k] as f64 / 255.0;
            let matrix = &palette[record[2 * k + 1] as usize];
            for r in 0..3 {
                for c in 0..4 {
                    blended[r][c] += weight * matrix[r][c];
                }
            }
        }
        let linear = |v: &[f32; 4]| -> [f64; 3] {
            std::array::from_fn(|r| (0..3).map(|c| blended[r][c] * v[c] as f64).sum())
        };
        let p = &inputs.positions[vertex];
        let moved = linear(p);
        let position = [
            (moved[0] + blended[0][3]) as f32,
            (moved[1] + blended[1][3]) as f32,
            (moved[2] + blended[2][3]) as f32,
            p[3],
        ];
        let turn = |v: &[f32; 4]| -> [f32; 4] {
            let first = linear(v);
            let o: [f64; 3] =
                std::array::from_fn(|r| (0..3).map(|c| direction[r][c] * first[c]).sum());
            [o[0] as f32, o[1] as f32, o[2] as f32, v[3]]
        };
        let normal = turn(&inputs.normals[vertex]);
        let tangent = turn(&inputs.tangents[vertex]);
        for (bank, row) in [(1, &position), (2, &normal), (3, &tangent)] {
            if let Some(lane) = row.iter().position(|v| !v.is_finite()) {
                return Err(SkinningError::NonFinite(NonFiniteWord::Output {
                    bank,
                    vertex,
                    lane,
                }));
            }
        }
        out.positions.push(position);
        out.normals.push(normal);
        out.tangents.push(tangent);
    }
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::testutil::Rng;

    fn matrix_bytes(rows: [[f32; 4]; 3]) -> Vec<u8> {
        rows.iter()
            .flatten()
            .flat_map(|v| v.to_bits().to_be_bytes())
            .collect()
    }

    const IDENTITY: [[f32; 4]; 3] = [
        [1.0, 0.0, 0.0, 0.0],
        [0.0, 1.0, 0.0, 0.0],
        [0.0, 0.0, 1.0, 0.0],
    ];

    fn object(quaternion: [f32; 4], scale: f32) -> [u32; 8] {
        [
            quaternion[0],
            quaternion[1],
            quaternion[2],
            quaternion[3],
            0.0,
            0.0,
            0.0,
            scale,
        ]
        .map(f32::to_bits)
    }

    struct Case {
        palette: Vec<u8>,
        influences: Vec<u8>,
        positions: Vec<[f32; 4]>,
        normals: Vec<[f32; 4]>,
        tangents: Vec<[f32; 4]>,
        object: [u32; 8],
    }

    impl Case {
        fn new(
            palette: Vec<u8>,
            influence: [u8; 8],
            p: [f32; 4],
            n: [f32; 4],
            t: [f32; 4],
        ) -> Self {
            Self {
                palette,
                influences: influence.to_vec(),
                positions: vec![p],
                normals: vec![n],
                tangents: vec![t],
                object: object([0.0, 0.0, 0.0, 1.0], 1.0),
            }
        }

        fn run(&self) -> Result<SkinnedBanks, SkinningError> {
            skin(
                &SkinningInputs {
                    segments: [&self.palette, &[]],
                    influences: &self.influences,
                    positions: &self.positions,
                    normals: &self.normals,
                    tangents: &self.tangents,
                    object: self.object,
                },
                DEFAULT_MAX_VERTICES,
            )
        }
    }

    fn close(a: [f32; 4], b: [f32; 4]) {
        for lane in 0..4 {
            assert!(
                (a[lane] - b[lane]).abs() <= 1e-5 * (1.0 + b[lane].abs()),
                "{a:?} vs {b:?}"
            );
        }
    }

    #[test]
    fn sk1_single_rotation_translation() {
        let m = matrix_bytes([
            [0.0, -1.0, 0.0, 1.0],
            [1.0, 0.0, 0.0, 2.0],
            [0.0, 0.0, 1.0, 3.0],
        ]);
        let case = Case::new(
            m,
            [255, 0, 0, 0, 0, 0, 0, 0],
            [1.0, 0.0, 0.0, 1.0],
            [1.0, 0.0, 0.0, 0.0],
            [0.0, 1.0, 0.0, -1.0],
        );
        let out = case.run().unwrap();
        assert_eq!(out.positions[0], [1.0, 3.0, 3.0, 1.0]);
        assert_eq!(out.normals[0], [0.0, 1.0, 0.0, 0.0]);
        assert_eq!(out.tangents[0], [-1.0, 0.0, 0.0, -1.0]);
    }

    #[test]
    fn sk2_two_matrix_blend() {
        let mut palette = matrix_bytes(IDENTITY);
        palette.extend(matrix_bytes([
            [2.0, 0.0, 0.0, 4.0],
            [0.0, 2.0, 0.0, 0.0],
            [0.0, 0.0, 2.0, 0.0],
        ]));
        let case = Case::new(
            palette,
            [51, 0, 204, 1, 0, 0, 0, 0],
            [1.0; 4],
            [0.0, 0.0, 1.0, 0.0],
            [0.0; 4],
        );
        let out = case.run().unwrap();
        close(out.positions[0], [5.0, 1.8, 1.8, 1.0]);
        close(out.normals[0], [0.0, 0.0, 1.8, 0.0]);
    }

    #[test]
    fn sk3_weights_are_not_renormalised() {
        let palette = matrix_bytes([
            [1.0, 0.0, 0.0, 255.0],
            [0.0, 1.0, 0.0, 0.0],
            [0.0, 0.0, 1.0, 0.0],
        ]);
        let case = Case::new(
            palette,
            [100, 0, 0, 0, 0, 0, 0, 0],
            [1.0, 0.0, 0.0, 1.0],
            [0.0; 4],
            [0.0; 4],
        );
        close(
            case.run().unwrap().positions[0],
            [100.0 / 255.0 * 256.0, 0.0, 0.0, 1.0],
        );
    }

    #[test]
    fn sk4_non_unit_quaternion() {
        let mut case = Case::new(
            matrix_bytes(IDENTITY),
            [255, 0, 0, 0, 0, 0, 0, 0],
            [1.0, 2.0, 3.0, 1.0],
            [1.0, 0.0, 0.0, 0.0],
            [0.0, 1.0, 0.0, 1.0],
        );
        case.object = object([0.0, 0.0, 1.0, 1.0], 1.0);
        let out = case.run().unwrap();
        assert_eq!(out.positions[0], [1.0, 2.0, 3.0, 1.0]);
        assert_eq!(out.normals[0], [-1.0, 2.0, 0.0, 0.0]);
        assert_eq!(out.tangents[0], [-2.0, -1.0, 0.0, 1.0]);
    }

    #[test]
    fn sk5_unit_quaternion_and_scale() {
        let h = std::f32::consts::FRAC_1_SQRT_2;
        let mut case = Case::new(
            matrix_bytes(IDENTITY),
            [255, 0, 0, 0, 0, 0, 0, 0],
            [0.0, 0.0, 0.0, 1.0],
            [1.0, 0.0, 0.0, 0.0],
            [0.0, 1.0, 0.0, -1.0],
        );
        case.object = object([0.0, 0.0, h, h], 3.0);
        let out = case.run().unwrap();
        close(out.normals[0], [0.0, 3.0, 0.0, 0.0]);
        close(out.tangents[0], [-3.0, 0.0, 0.0, -1.0]);
    }

    #[test]
    fn sk6_palette_straddling_the_join() {
        let mut joined = matrix_bytes([[9.0; 4]; 3]);
        joined.extend(matrix_bytes([
            [1.0, 0.0, 0.0, 1.0],
            [0.0, 1.0, 0.0, 2.0],
            [0.0, 0.0, 1.0, 3.0],
        ]));
        joined.extend([0xff; 4]);
        assert_eq!(joined.len(), 100);
        let (segment0, segment1) = joined.split_at(40);
        let positions = [[0.0, 0.0, 0.0, 1.0]];
        let zero = [[0.0f32; 4]];
        let out = skin(
            &SkinningInputs {
                segments: [segment0, segment1],
                influences: &[255, 1, 0, 0, 0, 0, 0, 0],
                positions: &positions,
                normals: &zero,
                tangents: &zero,
                object: object([0.0, 0.0, 0.0, 1.0], 1.0),
            },
            DEFAULT_MAX_VERTICES,
        )
        .unwrap();
        assert_eq!(out.positions[0], [1.0, 2.0, 3.0, 1.0]);
    }

    #[test]
    fn sk7_refusals() {
        let base = || {
            Case::new(
                matrix_bytes(IDENTITY),
                [255, 0, 0, 1, 0, 0, 0, 0],
                [0.0, 0.0, 0.0, 1.0],
                [0.0; 4],
                [0.0; 4],
            )
        };
        assert_eq!(
            base().run(),
            Err(SkinningError::Joint {
                vertex: 0,
                lane: 1,
                index: 1
            })
        );
        let mut case = base();
        case.influences[3] = 0;
        assert!(case.run().is_ok());
        let mut empty_tangents = base();
        empty_tangents.tangents.clear();
        assert_eq!(
            empty_tangents.run(),
            Err(SkinningError::Population { semantic: 3 })
        );
        let mut empty_normals = base();
        empty_normals.normals.clear();
        assert_eq!(
            empty_normals.run(),
            Err(SkinningError::Population { semantic: 2 })
        );
        let mut short = base();
        short.influences.pop();
        assert_eq!(short.run(), Err(SkinningError::SourceExtent));
        let mut nan = base();
        nan.influences[3] = 0;
        nan.object[0] = f32::NAN.to_bits();
        assert_eq!(
            nan.run(),
            Err(SkinningError::NonFinite(NonFiniteWord::Object {
                offset: 0
            }))
        );
        let mut infinite = base();
        infinite.influences[3] = 0;
        infinite.object[4] = f32::INFINITY.to_bits();
        assert_eq!(
            infinite.run(),
            Err(SkinningError::NonFinite(NonFiniteWord::Object {
                offset: 0x10
            }))
        );
        let mut no_palette = base();
        no_palette.palette.truncate(47);
        assert_eq!(no_palette.run(), Err(SkinningError::PaletteInputs));
        let mut nan_palette = base();
        nan_palette.influences[3] = 0;
        nan_palette.palette[4..8].copy_from_slice(&f32::NAN.to_bits().to_be_bytes());
        assert_eq!(
            nan_palette.run(),
            Err(SkinningError::NonFinite(NonFiniteWord::Palette {
                matrix: 0,
                element: 1
            }))
        );
        let mut overflow = base();
        overflow.influences[3] = 0;
        overflow.palette[..4].copy_from_slice(&f32::MAX.to_bits().to_be_bytes());
        overflow.positions[0] = [f32::MAX, 0.0, 0.0, 1.0];
        assert_eq!(
            overflow.run(),
            Err(SkinningError::NonFinite(NonFiniteWord::Output {
                bank: 1,
                vertex: 0,
                lane: 0
            }))
        );
        let budget = base();
        assert_eq!(
            skin(
                &SkinningInputs {
                    segments: [&budget.palette, &[]],
                    influences: &budget.influences,
                    positions: &budget.positions,
                    normals: &budget.normals,
                    tangents: &budget.tangents,
                    object: budget.object,
                },
                0
            ),
            Err(SkinningError::Budget)
        );
    }

    fn admission() -> SkinningAdmission {
        let mut configuration = vec![0u8; 16];
        configuration[0] = 0x83;
        configuration[6] = 17;
        configuration[12..16].fill(0xff);
        SkinningAdmission {
            mesh_group: 11,
            draw_batch_byte0: 0xb0,
            mesh_flags: 0,
            object_byte_6f: 0,
            material_flags: 0x20,
            pass_address: 0x1000,
            draw_batch_byte18: 0,
            draw_batch_byte1b: 0,
            draw_batch_palette: 0x0010_000f,
            pass_mode: 8,
            pass_workers: 2,
            configuration,
            segment_offsets: [0x40, 0x100],
            segment_lengths: [96, 0],
        }
    }

    #[test]
    fn admission_conditions() {
        assert_eq!(
            admit_skinning(&admission()),
            Ok(PaletteLocation {
                segments: [(0x0010_0040, 96), (0x0010_0100, 0)]
            })
        );
        let refuse = |change: fn(&mut SkinningAdmission), error| {
            let mut a = admission();
            change(&mut a);
            assert_eq!(admit_skinning(&a), Err(error));
        };
        refuse(|a| a.mesh_group = 3, SkinningError::Group);
        refuse(|a| a.draw_batch_byte0 = 0xa0, SkinningError::Group);
        refuse(|a| a.mesh_flags = 0x1000, SkinningError::Static);
        refuse(|a| a.object_byte_6f = 1, SkinningError::PaletteInputs);
        refuse(|a| a.material_flags = 0, SkinningError::PaletteInputs);
        refuse(|a| a.pass_address = 0, SkinningError::PassMissing);
        refuse(|a| a.draw_batch_byte18 = 4, SkinningError::PostProcessing);
        refuse(|a| a.pass_mode = 0x28, SkinningError::GeometryMode(0x28));
        refuse(
            |a| a.material_flags = 0x21,
            SkinningError::GeometryMode(0x28),
        );
        refuse(|a| a.pass_workers = 1, SkinningError::WorkerControl(1));
        refuse(|a| a.configuration[6] = 16, SkinningError::Configuration);
        refuse(
            |a| a.configuration.truncate(15),
            SkinningError::Configuration,
        );
        refuse(
            |a| a.segment_lengths[1] = 0x1_0000,
            SkinningError::UnsupportedControl { slot: 1 },
        );
        refuse(
            |a| a.draw_batch_byte1b = 1,
            SkinningError::PaletteBaseUnconfirmed,
        );
        // Mode bit 1 is ignored: 10 masks to 8.
        let mut a = admission();
        a.pass_mode = 10;
        assert!(admit_skinning(&a).is_ok());
        // Post-processing is allowed when byte +0x1B bit 0 is set, but that case is refused for
        // its palette base.
        let mut a = admission();
        a.draw_batch_byte18 = 4;
        a.draw_batch_byte1b = 1;
        assert_eq!(
            admit_skinning(&a),
            Err(SkinningError::PaletteBaseUnconfirmed)
        );
    }

    /// Property: random palettes, weights and vertices meet the stated position and direction
    /// tolerances against an independent per-matrix reference, and w lanes are exact copies.
    #[test]
    fn property_tolerances() {
        let mut rng = Rng::new(51);
        for _ in 0..500 {
            let joints = 1 + rng.below(6) as usize;
            let matrices: Vec<[[f32; 4]; 3]> = (0..joints)
                .map(|_| {
                    std::array::from_fn(|_| std::array::from_fn(|_| rng.range_f32(-10.0, 10.0)))
                })
                .collect();
            let palette: Vec<u8> = matrices.iter().flat_map(|m| matrix_bytes(*m)).collect();
            let split = rng.below(palette.len() as u32 + 1) as usize;
            let vertices = 1 + rng.below(16) as usize;
            let mut influences = Vec::new();
            for _ in 0..vertices {
                for _ in 0..4 {
                    influences.push(rng.below(256) as u8);
                    influences.push(rng.below(joints as u32) as u8);
                }
            }
            let mut bank = || -> Vec<[f32; 4]> {
                (0..vertices)
                    .map(|_| std::array::from_fn(|_| rng.range_f32(-100.0, 100.0)))
                    .collect()
            };
            let (positions, normals, tangents) = (bank(), bank(), bank());
            let q: [f32; 4] = std::array::from_fn(|_| rng.range_f32(-1.0, 1.0));
            let sigma = rng.range_f32(0.1, 4.0);
            let out = skin(
                &SkinningInputs {
                    segments: [&palette[..split], &palette[split..]],
                    influences: &influences,
                    positions: &positions,
                    normals: &normals,
                    tangents: &tangents,
                    object: object(q, sigma),
                },
                DEFAULT_MAX_VERTICES,
            )
            .unwrap();
            let o = object_direction_matrix(q.map(f64::from), sigma as f64);
            for v in 0..vertices {
                let record = &influences[8 * v..8 * v + 8];
                // Reference: transform by each matrix, then blend.
                let mut p_ref = [0.0f64; 3];
                let mut p_tol = [0.0f64; 3];
                let mut dirs = [[0.0f64; 3]; 2];
                let mut sizes = [[0.0f64; 3]; 2];
                for k in 0..4 {
                    let w = record[2 * k] as f64 / 255.0;
                    let m = &matrices[record[2 * k + 1] as usize];
                    for r in 0..3 {
                        let row = m[r].map(f64::from);
                        let p = positions[v].map(f64::from);
                        p_ref[r] += w * (row[0] * p[0] + row[1] * p[1] + row[2] * p[2] + row[3]);
                        p_tol[r] += w
                            * ((0..3).map(|c| row[c].abs() * p[c].abs()).sum::<f64>()
                                + row[3].abs());
                        for (d, bank) in [&normals, &tangents].into_iter().enumerate() {
                            let n = bank[v].map(f64::from);
                            dirs[d][r] += w * (0..3).map(|c| row[c] * n[c]).sum::<f64>();
                            sizes[d][r] +=
                                w * (0..3).map(|c| row[c].abs() * n[c].abs()).sum::<f64>();
                        }
                    }
                }
                for r in 0..3 {
                    let allowed = 2f64.powi(-18) * p_tol[r];
                    assert!(
                        (out.positions[v][r] as f64 - p_ref[r]).abs()
                            <= allowed.max(f64::MIN_POSITIVE)
                    );
                }
                for (d, got) in [&out.normals, &out.tangents].into_iter().enumerate() {
                    for r in 0..3 {
                        let exact: f64 = (0..3).map(|c| o[r][c] * dirs[d][c]).sum();
                        let size: f64 = (0..3).map(|c| o[r][c].abs() * sizes[d][c]).sum();
                        assert!(
                            (got[v][r] as f64 - exact).abs()
                                <= 2f64.powi(-18) * size + f64::MIN_POSITIVE
                        );
                    }
                }
                assert_eq!(out.positions[v][3].to_bits(), positions[v][3].to_bits());
                assert_eq!(out.normals[v][3].to_bits(), normals[v][3].to_bits());
                assert_eq!(out.tangents[v][3].to_bits(), tangents[v][3].to_bits());
            }
        }
    }
}
