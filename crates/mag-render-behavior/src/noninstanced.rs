//! Group-3 wind and group-4 ripple deformation (`noninstanced-deformation.md`).
//!
//! This is the single deformation for these groups. Every pass whose depth must coincide with
//! Main (the depth pre-pass in particular) must use these positions, or call this function with
//! the same inputs, so that depth-equal tests hold.

use crate::waveform::{KA1, KA2, KB1, KB2, KC, frac, ripple, wave};

/// The guest fields that decide whether an item is admitted.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct DeformationAdmission {
    /// Mesh record `+0xBC`.
    pub mesh_group: u16,
    /// Draw-batch byte `+0x00`.
    pub draw_batch_byte0: u8,
    pub pass_address: u32,
    /// Draw-batch byte `+0x18`.
    pub draw_batch_byte18: u8,
    /// Draw-batch byte `+0x1B`.
    pub draw_batch_byte1b: u8,
    /// Partition `u32 +0x48` and `u32 +0x50`.
    pub palette_lengths: [u32; 2],
    /// Pass `+0x230`.
    pub pass_mode: u32,
    /// Pass `+0x22C`.
    pub pass_workers: u32,
    /// Primary material `+0x58`.
    pub material_flags: u32,
    /// A tangent bank (semantic 3) exists.
    pub has_tangents: bool,
}

/// A guest word that is NaN or infinite.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DeformWord {
    /// Pass record `+0x260 + 4·index`.
    Wind {
        index: usize,
    },
    /// Material row `M{row}`, lane.
    Material {
        row: usize,
        lane: usize,
    },
    Origin {
        lane: usize,
    },
    Position {
        vertex: usize,
        lane: usize,
    },
    Normal {
        vertex: usize,
        lane: usize,
    },
    Weight {
        vertex: usize,
        lane: usize,
    },
    Tangent {
        vertex: usize,
        lane: usize,
    },
    /// `1 / M1.z` (k = 1) or `1 / M2.z` (k = 2).
    Reciprocal {
        k: usize,
    },
    Output {
        vertex: usize,
        lane: usize,
    },
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DeformError {
    Group,
    PassMissing,
    PostProcessing,
    Palette,
    GeometryMode(u32),
    WorkerControl(u32),
    MissingTangent,
    Population { semantic: u32 },
    Budget,
    NonFinite(DeformWord),
}

/// Checks admission. Returns the group (3 or 4).
pub fn admit_noninstanced(a: &DeformationAdmission) -> Result<u32, DeformError> {
    if !matches!(a.mesh_group, 3 | 4) || u16::from(a.draw_batch_byte0 >> 4) != a.mesh_group {
        return Err(DeformError::Group);
    }
    if a.pass_address == 0 {
        return Err(DeformError::PassMissing);
    }
    if a.draw_batch_byte1b & 1 == 0 && a.draw_batch_byte18 >> 2 != 0 {
        return Err(DeformError::PostProcessing);
    }
    if a.palette_lengths != [0, 0] {
        return Err(DeformError::Palette);
    }
    let material_bit0 = a.material_flags & 1 != 0;
    let mode = (a.pass_mode | if material_bit0 { 0x20 } else { 0 }) & !0x2;
    if !matches!(mode, 8 | 0x28) {
        return Err(DeformError::GeometryMode(mode));
    }
    let workers = a.pass_workers.min(if material_bit0 { 1 } else { 2 });
    if workers != if mode == 0x28 { 1 } else { 2 } {
        return Err(DeformError::WorkerControl(workers));
    }
    if a.mesh_group == 4 && !a.has_tangents {
        return Err(DeformError::MissingTangent);
    }
    Ok(a.mesh_group as u32)
}

/// Plain inputs, as bit patterns so pass-through banks and unread lanes stay exact.
#[derive(Clone, Copy, Debug)]
pub struct DeformationInputs<'a> {
    pub group: u32,
    /// Pass record `+0x260..+0x270`: `ω0`, `ω1` (unused), `ω2`, `ω3`, `θ`.
    pub wind: [u32; 5],
    /// Primary material rows `M0`, `M1`, `M2`.
    pub material: [[u32; 4]; 3],
    /// Object transform `+0x10..+0x18`.
    pub origin: [u32; 3],
    pub positions: &'a [[u32; 4]],
    pub normals: &'a [[u32; 4]],
    pub weights: &'a [[u32; 4]],
    pub tangents: Option<&'a [[u32; 4]]>,
}

/// Deformed positions and the pass-through banks.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct DeformedBanks {
    pub positions: Vec<[u32; 4]>,
    pub normals: Vec<[u32; 4]>,
    pub weights: Vec<[u32; 4]>,
    /// Group 4 only.
    pub tangents: Option<Vec<[u32; 4]>>,
}

/// Default vertex budget. Its size is not behaviour.
pub const DEFAULT_MAX_VERTICES: usize = 1 << 20;

fn f(bits: u32) -> f32 {
    f32::from_bits(bits)
}

fn check_lanes(
    bank: &[[u32; 4]],
    lanes: &[usize],
    word: fn(usize, usize) -> DeformWord,
) -> Result<(), DeformError> {
    for (vertex, row) in bank.iter().enumerate() {
        for &lane in lanes {
            if !f(row[lane]).is_finite() {
                return Err(DeformError::NonFinite(word(vertex, lane)));
            }
        }
    }
    Ok(())
}

/// Group 3: displacement along normal lanes x and y and a fixed vertical lane.
fn wind(i: &DeformationInputs<'_>) -> Result<DeformedBanks, DeformError> {
    let w = |index: usize| f(i.wind[index]) as f64;
    let m = |row: usize, lane: usize| f(i.material[row][lane]) as f64;
    let (frequency, a1, a2, theta) = (m(1, 0) * w(0), m(0, 0) * w(2), m(0, 1) * w(3), w(4));
    let sum: f64 = i.origin.iter().map(|word| f(*word) as f64).sum();
    let k = |c: f32| c as f64;
    let mut positions = Vec::with_capacity(i.positions.len());
    for vertex in 0..i.positions.len() {
        let lane = |bank: &[[u32; 4]], l: usize| f(bank[vertex][l]) as f64;
        let p = [0, 1, 2].map(|l| lane(i.positions, l));
        let (nx, ny) = (lane(i.normals, 0), lane(i.normals, 1));
        let weights = [0, 1, 2].map(|l| lane(i.weights, l));
        let t = weights[1] + sum;
        let a = theta + t;
        let b = theta + (k(KC) + t) * (p[0] + p[1] + p[2]);
        let wb = wave(k(KB1) * b, frequency) + wave(k(KB2) * b, frequency);
        let wa = wave(k(KA1) * a, frequency) + wave(k(KA2) * a, frequency);
        let g = weights[0] * a1;
        let out = [
            p[0] + wb * g * nx,
            p[1] + wa * (1.0 - weights[2]) * a2,
            p[2] + wb * g * ny,
        ];
        let mut row = [0u32; 4];
        for l in 0..3 {
            let value = out[l] as f32;
            if !value.is_finite() {
                return Err(DeformError::NonFinite(DeformWord::Output {
                    vertex,
                    lane: l,
                }));
            }
            row[l] = value.to_bits();
        }
        row[3] = i.positions[vertex][3];
        positions.push(row);
    }
    Ok(DeformedBanks {
        positions,
        normals: i.normals.to_vec(),
        weights: i.weights.to_vec(),
        tangents: None,
    })
}

/// Deforms one item. All-or-nothing.
pub fn deform(
    i: &DeformationInputs<'_>,
    max_vertices: usize,
) -> Result<DeformedBanks, DeformError> {
    if !matches!(i.group, 3 | 4) {
        return Err(DeformError::Group);
    }
    let vertices = i.positions.len();
    if i.normals.len() != vertices {
        return Err(DeformError::Population { semantic: 2 });
    }
    if i.weights.len() != vertices {
        return Err(DeformError::Population { semantic: 9 });
    }
    if i.group == 4 && i.tangents.is_none() {
        return Err(DeformError::MissingTangent);
    }
    if let Some(tangents) = i.tangents
        && tangents.len() != vertices
    {
        return Err(DeformError::Population { semantic: 3 });
    }
    if vertices > max_vertices {
        return Err(DeformError::Budget);
    }

    // Lanes read as numbers, per group.
    let group4 = i.group == 4;
    let wind_used: &[usize] = if group4 { &[4] } else { &[0, 2, 3, 4] };
    let material_used: [&[usize]; 3] = if group4 {
        [&[0, 1, 2, 3], &[0, 1, 2], &[0, 1, 2]]
    } else {
        [&[0, 1], &[0], &[]]
    };
    let normal_used: &[usize] = if group4 { &[0, 1, 2] } else { &[0, 1] };
    for &index in wind_used {
        if !f(i.wind[index]).is_finite() {
            return Err(DeformError::NonFinite(DeformWord::Wind { index }));
        }
    }
    for (row, lanes) in material_used.iter().enumerate() {
        for &lane in *lanes {
            if !f(i.material[row][lane]).is_finite() {
                return Err(DeformError::NonFinite(DeformWord::Material { row, lane }));
            }
        }
    }
    if let Some(lane) = i.origin.iter().position(|w| !f(*w).is_finite()) {
        return Err(DeformError::NonFinite(DeformWord::Origin { lane }));
    }
    check_lanes(i.positions, &[0, 1, 2, 3], |vertex, lane| {
        DeformWord::Position { vertex, lane }
    })?;
    check_lanes(i.normals, normal_used, |vertex, lane| DeformWord::Normal {
        vertex,
        lane,
    })?;
    check_lanes(i.weights, &[0, 1, 2], |vertex, lane| DeformWord::Weight {
        vertex,
        lane,
    })?;
    if group4 {
        let tangents = i.tangents.expect("checked above");
        check_lanes(tangents, &[0, 1, 2, 3], |vertex, lane| {
            DeformWord::Tangent { vertex, lane }
        })?;
    }

    if !group4 {
        return wind(i);
    }
    let tangents = i.tangents.expect("checked above");
    let m = |row: usize, lane: usize| f(i.material[row][lane]) as f64;
    let mut reciprocal = [0.0f64; 2];
    for (index, row) in [1usize, 2].into_iter().enumerate() {
        if !(1.0f32 / f(i.material[row][2])).is_finite() {
            return Err(DeformError::NonFinite(DeformWord::Reciprocal { k: row }));
        }
        reciprocal[index] = 1.0 / m(row, 2);
    }
    let theta = f(i.wind[4]) as f64;
    let sum: f64 = i.origin.iter().map(|w| f(*w) as f64).sum();
    let offset = frac(sum);

    let mut positions = Vec::with_capacity(vertices);
    for vertex in 0..vertices {
        let lane = |bank: &[[u32; 4]], l: usize| f(bank[vertex][l]) as f64;
        let p = [0, 1, 2].map(|l| lane(i.positions, l));
        let n = [0, 1, 2].map(|l| lane(i.normals, l));
        let w = [0, 1, 2].map(|l| lane(i.weights, l));
        let t = [0, 1, 2].map(|l| lane(tangents, l));
        let h = lane(tangents, 3);
        let cross = [
            n[1] * t[2] - n[2] * t[1],
            n[2] * t[0] - n[0] * t[2],
            n[0] * t[1] - n[1] * t[0],
        ];
        let bt = cross.map(|c| h * c);
        let mut displacement = 0.0;
        for k in 0..2 {
            let row = k + 1;
            let (scale_t, scale_bt) = (m(0, 2 * k), m(0, 2 * k + 1));
            let direction: [f64; 3] = std::array::from_fn(|l| scale_t * t[l] + scale_bt * bt[l]);
            let along: f64 = (0..3).map(|l| p[l] * direction[l]).sum();
            let phase = reciprocal[k] * along + offset - m(row, 1) * theta + w[2] + 0.5;
            displacement += ripple(phase) * m(row, 0) * w[k];
        }
        let mut out = [0u32; 4];
        for l in 0..3 {
            let value = (p[l] + displacement * n[l]) as f32;
            if !value.is_finite() {
                return Err(DeformError::NonFinite(DeformWord::Output {
                    vertex,
                    lane: l,
                }));
            }
            out[l] = value.to_bits();
        }
        out[3] = i.positions[vertex][3];
        positions.push(out);
    }
    Ok(DeformedBanks {
        positions,
        normals: i.normals.to_vec(),
        weights: i.weights.to_vec(),
        tangents: Some(tangents.to_vec()),
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::testutil::Rng;

    fn bits(values: [f32; 4]) -> [u32; 4] {
        values.map(f32::to_bits)
    }

    struct Item {
        group: u32,
        wind: [f32; 5],
        material: [[f32; 4]; 3],
        origin: [f32; 3],
        position: [f32; 4],
        normal: [f32; 4],
        weight: [f32; 4],
        tangent: Option<[f32; 4]>,
    }

    impl Item {
        fn ni3() -> Self {
            Self {
                group: 4,
                wind: [0.0, 0.0, 0.0, 0.0, 0.25],
                material: [
                    [1.0, 0.0, 0.0, 1.0],
                    [0.5, 0.0, 2.0, 0.0],
                    [0.25, 1.0, 4.0, 0.0],
                ],
                origin: [0.0; 3],
                position: [0.5, 1.0, 0.0, 1.0],
                normal: [0.0, 0.0, 1.0, 0.0],
                weight: [1.0, 1.0, 0.0, 0.0],
                tangent: Some([1.0, 0.0, 0.0, 1.0]),
            }
        }

        fn run_raw(
            &self,
            overrides: impl Fn(&mut DeformationInputsOwned),
        ) -> Result<DeformedBanks, DeformError> {
            let mut owned = DeformationInputsOwned {
                wind: self.wind.map(f32::to_bits),
                material: self.material.map(bits),
                origin: self.origin.map(f32::to_bits),
                positions: vec![bits(self.position)],
                normals: vec![bits(self.normal)],
                weights: vec![bits(self.weight)],
                tangents: self.tangent.map(|t| vec![bits(t)]),
            };
            overrides(&mut owned);
            deform(
                &DeformationInputs {
                    group: self.group,
                    wind: owned.wind,
                    material: owned.material,
                    origin: owned.origin,
                    positions: &owned.positions,
                    normals: &owned.normals,
                    weights: &owned.weights,
                    tangents: owned.tangents.as_deref(),
                },
                DEFAULT_MAX_VERTICES,
            )
        }

        fn run(&self) -> Result<DeformedBanks, DeformError> {
            self.run_raw(|_| {})
        }
    }

    struct DeformationInputsOwned {
        wind: [u32; 5],
        material: [[u32; 4]; 3],
        origin: [u32; 3],
        positions: Vec<[u32; 4]>,
        normals: Vec<[u32; 4]>,
        weights: Vec<[u32; 4]>,
        tangents: Option<Vec<[u32; 4]>>,
    }

    fn position(out: &DeformedBanks) -> [f32; 4] {
        out.positions[0].map(f32::from_bits)
    }

    #[test]
    fn ni3_simple_ripple() {
        assert_eq!(
            position(&Item::ni3().run().unwrap()),
            [0.5, 1.0, -0.25, 1.0]
        );
        let flipped = Item {
            tangent: Some([1.0, 0.0, 0.0, -1.0]),
            ..Item::ni3()
        };
        assert_eq!(position(&flipped.run().unwrap()), [0.5, 1.0, 0.25, 1.0]);
    }

    #[test]
    fn ni4_general_case() {
        let item = Item {
            group: 4,
            wind: [0.0, 0.0, 0.0, 0.0, 1.5],
            material: [
                [2.0, 1.0, 0.0, 3.0],
                [0.5, 0.25, 4.0, 0.0],
                [-1.0, 0.0, 0.5, 0.0],
            ],
            origin: [0.3, 0.5, 0.45],
            position: [2.0, 0.125, -0.25, 1.0],
            normal: [1.0, 0.0, 0.0, 0.0],
            weight: [0.6, 0.2, 0.1, 0.0],
            tangent: Some([0.0, 1.0, 0.0, 1.0]),
        };
        let p = position(&item.run().unwrap());
        assert!((p[0] - 1.817_95).abs() <= 1e-5, "{p:?}");
        assert_eq!(&p[1..], &[0.125, -0.25, 1.0]);
    }

    #[test]
    fn pass_through_banks_are_bit_exact() {
        let out = Item::ni3()
            .run_raw(|o| {
                o.normals[0][3] = 0xffff_ffff;
                o.weights[0][3] = 0x7fc0_0001;
                o.material[1][3] = 0xffff_ffff;
                o.material[2][3] = 0xffff_ffff;
                o.wind[..4].fill(0xffff_ffff);
            })
            .unwrap();
        assert_eq!(position(&out), [0.5, 1.0, -0.25, 1.0]);
        assert_eq!(out.normals[0][3], 0xffff_ffff);
        assert_eq!(out.weights[0][3], 0x7fc0_0001);
        assert_eq!(out.tangents.unwrap()[0], bits([1.0, 0.0, 0.0, 1.0]));
    }

    #[test]
    fn ni6_refusals() {
        let group11 = Item {
            group: 11,
            ..Item::ni3()
        };
        assert_eq!(group11.run(), Err(DeformError::Group));
        let no_tangent = Item {
            tangent: None,
            ..Item::ni3()
        };
        assert_eq!(no_tangent.run(), Err(DeformError::MissingTangent));
        let group3 = Item {
            group: 3,
            tangent: None,
            ..Item::ni3()
        };
        assert_eq!(
            group3.run_raw(|o| o.normals.clear()),
            Err(DeformError::Population { semantic: 2 })
        );
        assert_eq!(
            group3.run_raw(|o| o.weights.clear()),
            Err(DeformError::Population { semantic: 9 })
        );
        assert_eq!(
            Item::ni3().run_raw(|o| o.tangents.as_mut().unwrap().push([0; 4])),
            Err(DeformError::Population { semantic: 3 })
        );
        let nan_theta = |o: &mut DeformationInputsOwned| o.wind[4] = f32::NAN.to_bits();
        assert_eq!(
            Item::ni3().run_raw(nan_theta),
            Err(DeformError::NonFinite(DeformWord::Wind { index: 4 }))
        );
        assert_eq!(
            group3.run_raw(nan_theta),
            Err(DeformError::NonFinite(DeformWord::Wind { index: 4 }))
        );
        assert_eq!(
            Item::ni3().run_raw(|o| o.material[1][2] = 0),
            Err(DeformError::NonFinite(DeformWord::Reciprocal { k: 1 }))
        );
        assert_eq!(
            Item::ni3().run_raw(|o| o.material[2][2] = 1e-45f32.to_bits()),
            Err(DeformError::NonFinite(DeformWord::Reciprocal { k: 2 }))
        );
        assert_eq!(
            Item::ni3().run_raw(|o| o.origin[1] = f32::INFINITY.to_bits()),
            Err(DeformError::NonFinite(DeformWord::Origin { lane: 1 }))
        );
        // Output overflow.
        assert_eq!(
            Item::ni3().run_raw(|o| {
                o.positions[0][2] = f32::MIN.to_bits();
                o.material[2][0] = f32::MAX.to_bits();
            }),
            Err(DeformError::NonFinite(DeformWord::Output {
                vertex: 0,
                lane: 2
            }))
        );
        let budget = deform(
            &DeformationInputs {
                group: 4,
                wind: [0; 5],
                material: [[0; 4]; 3],
                origin: [0; 3],
                positions: &[[0; 4]],
                normals: &[[0; 4]],
                weights: &[[0; 4]],
                tangents: Some(&[[0; 4]]),
            },
            0,
        );
        assert_eq!(budget, Err(DeformError::Budget));
    }

    fn group3(
        position: [f32; 4],
        normal: [f32; 4],
        weight: [f32; 4],
        m0: [f32; 2],
        m1x: f32,
        wind: [f32; 5],
    ) -> Item {
        Item {
            group: 3,
            wind,
            material: [[m0[0], m0[1], 0.0, 0.0], [m1x, 0.0, 0.0, 0.0], [0.0; 4]],
            origin: [0.0; 3],
            position,
            normal,
            weight,
            tangent: None,
        }
    }

    #[test]
    fn ni1_group_three_phases_free_of_constants() {
        for (m1x, expected) in [(0.5, [1.5, -0.75, -1.0, 1.0]), (1.0, [1.0, -1.0, 0.0, 1.0])] {
            let item = group3(
                [1.0, -1.0, 0.0, 1.0],
                [0.5, -1.0, 9.0, 0.0],
                [1.0, 0.0, 0.5, 0.0],
                [0.5, 1.0],
                m1x,
                [1.0, 0.0, 1.0, 0.25, 0.0],
            );
            let out = item.run().unwrap();
            assert_eq!(position(&out), expected);
            assert!(out.tangents.is_none());
        }
    }

    #[test]
    fn ni2_group_three_closed_form() {
        for (frequency, expected) in [
            (0.5, [1.704_297_7, 2.0, 0.0, 1.0]),
            (1.0, [2.737_702_3, 0.0, 0.0, 1.0]),
        ] {
            let item = group3(
                [1.0, 0.0, 0.0, 1.0],
                [1.0, 0.0, 0.0, 0.0],
                [1.0, 0.0, 0.0, 0.0],
                [1.0, 1.0],
                frequency,
                [1.0, 0.0, 1.0, 1.0, 0.0],
            );
            let got = position(&item.run().unwrap());
            for lane in 0..4 {
                assert!(
                    (got[lane] - expected[lane]).abs() <= 1e-6,
                    "f {frequency}: {got:?}"
                );
            }
        }
    }

    #[test]
    fn group_three_unread_lanes_do_not_refuse() {
        let item = group3(
            [0.3, 0.2, 0.1, 1.0],
            [0.6, 0.8, 0.0, 0.0],
            [0.7, 0.2, 0.4, 0.0],
            [0.5, 0.25],
            0.8,
            [1.0, 0.0, 0.5, 0.75, 12.5],
        );
        let clean = item.run().unwrap();
        let noisy = item
            .run_raw(|o| {
                o.normals[0][2] = 0xffff_ffff;
                o.normals[0][3] = 0xffff_ffff;
                o.weights[0][3] = 0xffff_ffff;
                o.material[0][2] = 0xffff_ffff;
                o.material[0][3] = 0xffff_ffff;
                o.material[1][1..].fill(0xffff_ffff);
                o.material[2] = [0xffff_ffff; 4];
                o.wind[1] = 0xffff_ffff;
            })
            .unwrap();
        assert_eq!(noisy.positions, clean.positions);
        assert_eq!(noisy.normals[0][2], 0xffff_ffff);
        assert_eq!(noisy.weights[0][3], 0xffff_ffff);
        assert_ne!(
            clean.positions[0],
            [0.3f32, 0.2, 0.1, 1.0].map(f32::to_bits)
        );
    }

    /// Property: group-3 displacement stays inside its envelopes, `[0, 2]·|g·N|` horizontally and
    /// `[0, 2]·|(1 − W.z)·A2|` vertically.
    #[test]
    fn property_group_three_envelopes() {
        let mut rng = Rng::new(62);
        for _ in 0..5_000 {
            let mut r = |lo: f32, hi: f32| rng.range_f32(lo, hi);
            let item = group3(
                [r(-10.0, 10.0), r(-10.0, 10.0), r(-10.0, 10.0), 1.0],
                [r(-1.0, 1.0), r(-1.0, 1.0), r(-1.0, 1.0), 0.0],
                [r(0.0, 1.0), r(0.0, 1.0), r(0.0, 1.0), 0.0],
                [r(-2.0, 2.0), r(-2.0, 2.0)],
                r(-2.0, 2.0),
                [
                    r(-2.0, 2.0),
                    0.0,
                    r(-2.0, 2.0),
                    r(-2.0, 2.0),
                    r(-100.0, 100.0),
                ],
            );
            let got = position(&item.run().unwrap());
            let (p, n, w, m, wind) = (
                item.position,
                item.normal,
                item.weight,
                item.material,
                item.wind,
            );
            let g = (w[0] * m[0][0] * wind[2]) as f64;
            let vertical = ((1.0 - w[2]) * m[0][1] * wind[3]) as f64;
            let slack = 1e-5 * (1.0 + p.iter().map(|v| v.abs() as f64).sum::<f64>());
            let within = |delta: f64, scale: f64| {
                let (lo, hi) = if scale >= 0.0 {
                    (0.0, 2.0 * scale)
                } else {
                    (2.0 * scale, 0.0)
                };
                delta >= lo - slack && delta <= hi + slack
            };
            assert!(within((got[0] - p[0]) as f64, g * n[0] as f64));
            assert!(within((got[1] - p[1]) as f64, vertical));
            assert!(within((got[2] - p[2]) as f64, g * n[1] as f64));
        }
    }

    fn admission() -> DeformationAdmission {
        DeformationAdmission {
            mesh_group: 4,
            draw_batch_byte0: 0x40,
            pass_address: 0x1000,
            draw_batch_byte18: 0,
            draw_batch_byte1b: 0,
            palette_lengths: [0, 0],
            pass_mode: 8,
            pass_workers: 2,
            material_flags: 0,
            has_tangents: true,
        }
    }

    #[test]
    fn admission_conditions() {
        assert_eq!(admit_noninstanced(&admission()), Ok(4));
        let three = DeformationAdmission {
            mesh_group: 3,
            draw_batch_byte0: 0x30,
            has_tangents: false,
            ..admission()
        };
        assert_eq!(admit_noninstanced(&three), Ok(3));
        let refuse = |change: fn(&mut DeformationAdmission), error| {
            let mut a = admission();
            change(&mut a);
            assert_eq!(admit_noninstanced(&a), Err(error));
        };
        refuse(|a| a.mesh_group = 11, DeformError::Group);
        refuse(|a| a.draw_batch_byte0 = 0x30, DeformError::Group);
        refuse(|a| a.pass_address = 0, DeformError::PassMissing);
        refuse(|a| a.draw_batch_byte18 = 4, DeformError::PostProcessing);
        refuse(|a| a.palette_lengths = [48, 0], DeformError::Palette);
        refuse(|a| a.pass_mode = 4, DeformError::GeometryMode(4));
        refuse(|a| a.pass_workers = 1, DeformError::WorkerControl(1));
        refuse(|a| a.has_tangents = false, DeformError::MissingTangent);
        // Mode 0x28 needs one worker.
        let mut a = admission();
        a.material_flags = 1;
        a.pass_mode = 8;
        assert_eq!(admit_noninstanced(&a), Ok(4));
        a.material_flags = 0;
        a.pass_mode = 0x28;
        assert_eq!(admit_noninstanced(&a), Err(DeformError::WorkerControl(2)));
        a.pass_workers = 1;
        assert_eq!(admit_noninstanced(&a), Ok(4));
        // Byte +0x1B bit 0 set allows a nonzero +0x18 count field.
        let mut a = admission();
        a.draw_batch_byte18 = 4;
        a.draw_batch_byte1b = 1;
        assert_eq!(admit_noninstanced(&a), Ok(4));
    }

    /// Property: group-4 positions meet the stated tolerance against a binary32 evaluation of
    /// the same formulas, and inline and repeated evaluation agree bit for bit.
    #[test]
    fn property_group_four_tolerance_and_determinism() {
        let mut rng = Rng::new(61);
        for _ in 0..5_000 {
            let mut r = |lo: f32, hi: f32| rng.range_f32(lo, hi);
            let item = Item {
                group: 4,
                wind: [0.0, 0.0, 0.0, 0.0, r(-50.0, 50.0)],
                material: [
                    [r(-2.0, 2.0), r(-2.0, 2.0), r(-2.0, 2.0), r(-2.0, 2.0)],
                    [r(-1.0, 1.0), r(-1.0, 1.0), r(0.5, 4.0), 0.0],
                    [r(-1.0, 1.0), r(-1.0, 1.0), r(0.5, 4.0), 0.0],
                ],
                origin: [r(-100.0, 100.0), r(-100.0, 100.0), r(-100.0, 100.0)],
                position: [r(-20.0, 20.0), r(-20.0, 20.0), r(-20.0, 20.0), 1.0],
                normal: [r(-1.0, 1.0), r(-1.0, 1.0), r(-1.0, 1.0), 0.0],
                weight: [r(0.0, 1.0), r(0.0, 1.0), r(0.0, 1.0), 0.0],
                tangent: Some([
                    r(-1.0, 1.0),
                    r(-1.0, 1.0),
                    r(-1.0, 1.0),
                    if r(0.0, 1.0) < 0.5 { -1.0 } else { 1.0 },
                ]),
            };
            let got = position(&item.run().unwrap());
            assert_eq!(item.run().unwrap(), item.run().unwrap());
            // A binary32 evaluation is a different but admissible implementation; both must lie
            // within the tolerance of each other's exact value up to twice the allowance.
            let (p, n, w, tg, m) = (
                item.position,
                item.normal,
                item.weight,
                item.tangent.unwrap(),
                item.material,
            );
            let theta = item.wind[4];
            let o32 = {
                let s = item.origin[0] + item.origin[1] + item.origin[2];
                s - s.floor()
            };
            let t = [tg[0], tg[1], tg[2]];
            let cross = [
                n[1] * t[2] - n[2] * t[1],
                n[2] * t[0] - n[0] * t[2],
                n[0] * t[1] - n[1] * t[0],
            ];
            let bt = cross.map(|c| tg[3] * c);
            let mut d32 = 0.0f32;
            let mp = p[0].abs() + p[1].abs() + p[2].abs();
            let nmax = n[0].abs().max(n[1].abs()).max(n[2].abs()) as f64;
            let tmax = t[0].abs().max(t[1].abs()).max(t[2].abs()) as f64;
            let mut slack = 0.0f64;
            for k in 0..2 {
                let row = m[k + 1];
                let dir: [f32; 3] =
                    std::array::from_fn(|l| m[0][2 * k] * t[l] + m[0][2 * k + 1] * bt[l]);
                let along = p[0] * dir[0] + p[1] * dir[1] + p[2] * dir[2];
                let phase = (1.0 / row[2]) * along + o32 - row[1] * theta + w[2] + 0.5;
                d32 += ripple(phase as f64) as f32 * row[0] * w[k];
                let g =
                    (m[0][2 * k].abs() as f64 + 2.0 * m[0][2 * k + 1].abs() as f64 * nmax) * tmax;
                let phi = (1.0 / row[2] as f64).abs() * mp as f64 * g
                    + (row[1] * theta).abs() as f64
                    + w[2].abs() as f64
                    + 2.0;
                // The origin sum carries its own binary32 rounding into the phase.
                let delta = 2f64.powi(-20) * phi
                    + 2f64.powi(-23) * (item.origin.iter().map(|x| x.abs()).sum::<f32>() as f64);
                slack += (row[0] * w[k]).abs() as f64 * (2f64.powi(-18) + 6.0 * delta);
            }
            for l in 0..3 {
                let other = p[l] + d32 * n[l];
                let allowed =
                    2.0 * (2f64.powi(-18) * p[l].abs() as f64 + n[l].abs() as f64 * slack);
                assert!(
                    ((got[l] - other) as f64).abs() <= allowed + 1e-12,
                    "lane {l}: {} vs {}",
                    got[l],
                    other
                );
            }
        }
    }
}
