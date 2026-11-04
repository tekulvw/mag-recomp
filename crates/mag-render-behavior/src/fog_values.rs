//! Main fog constants and depth bounds (`main-fog-values.md`, the specified parts).
//!
//! The seven prepared rows are still derived by the game (open question 1 of the
//! specification), so they are an input here, supplied by the caller as data.

use crate::guest::{GuestMemory, be_u32};
use crate::lighting::window_depth;
use crate::producers::{AcquisitionError, ProducerInputs};

/// Displacement below `TOC` of the global holding the camera pointer variable's address.
pub const CAMERA_GLOBAL: u32 = 0x223c;

/// The seven rows the game prepares for the fog pass, as bit patterns.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PreparedFogRows {
    /// Fog vertex constant rows 0 and 1.
    pub vertex: [[u32; 4]; 2],
    /// Fog fragment constant rows 0–4.
    pub fragment: [[u32; 4]; 5],
}

/// The two camera fields the fog bound uses, as bit patterns.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct FogCamera {
    pub address: u32,
    /// `camera+0x60`: vertical extent slope.
    pub slope: u32,
    /// `camera+0xA24`: aspect ratio.
    pub aspect: u32,
}

/// Reads the camera through the doubly indirect pointer at `TOC−0x223C`.
pub fn acquire_fog_camera<M: GuestMemory + ?Sized>(
    memory: &M,
    toc: u32,
) -> Result<FogCamera, AcquisitionError> {
    if !toc.is_multiple_of(4) || toc < crate::producers::globals::LARGEST {
        return Err(AcquisitionError::InvalidToc(toc));
    }
    let word = |address: u32| -> Result<u32, AcquisitionError> {
        if address == 0 || address as u64 + 4 > 1 << 32 {
            return Err(AcquisitionError::OutOfRange { address, length: 4 });
        }
        let bytes = memory.read(address, 4).map_err(AcquisitionError::Source)?;
        if bytes.len() != 4 {
            return Err(AcquisitionError::Short { address, length: 4 });
        }
        Ok(be_u32(&bytes, 0))
    };
    let variable = word(toc - CAMERA_GLOBAL)?;
    if variable == 0 {
        return Err(AcquisitionError::NullPointer {
            global: "camera pointer variable",
        });
    }
    let camera = word(variable)?;
    if camera == 0 {
        return Err(AcquisitionError::NullPointer { global: "camera" });
    }
    let field = |offset: u32| {
        camera
            .checked_add(offset)
            .ok_or(AcquisitionError::OutOfRange {
                address: camera,
                length: offset,
            })
            .and_then(word)
    };
    Ok(FogCamera {
        address: camera,
        slope: field(0x60)?,
        aspect: field(0xa24)?,
    })
}

/// Every input of the fog values.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct FogValueInputs {
    pub prepared: PreparedFogRows,
    /// Fog parameter block `B[0..15]` at `RENDERER+0x50`.
    pub block: [u32; 16],
    pub camera_slope: u32,
    pub camera_aspect: u32,
    pub z0: u32,
    pub z1: u32,
    pub pa: u32,
    pub pb: u32,
    pub pc: u32,
    pub pd: u32,
    pub zh: u32,
    pub ftop: u32,
}

impl FogValueInputs {
    /// Assembles the inputs from the acquired set, the camera and the prepared rows.
    pub fn of(inputs: &ProducerInputs, camera: &FogCamera, prepared: PreparedFogRows) -> Self {
        let s = &inputs.scalars;
        Self {
            prepared,
            block: inputs.renderer.fog_block.value,
            camera_slope: camera.slope,
            camera_aspect: camera.aspect,
            z0: s.z0.value,
            z1: s.z1.value,
            pa: s.pa.value,
            pb: s.pb.value,
            pc: s.pc.value,
            pd: s.pd.value,
            zh: s.zh.value,
            ftop: s.ftop.value,
        }
    }
}

/// The fog producer's values.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct FogValues {
    /// Selects fragment program 4 (tinted) instead of 3.
    pub tinted: bool,
    pub vertex_rows: [[u32; 4]; 2],
    /// Seven rows, or eight when tinted.
    pub fragment_rows: Vec<[u32; 4]>,
    /// `[lower, FTOP]` as bit patterns.
    pub bounds: [u32; 2],
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum FogValuesError {
    NonFinite(&'static str),
    BoundAboveTop,
}

/// Computes the fog values.
pub fn fog_values(i: &FogValueInputs) -> Result<FogValues, FogValuesError> {
    let f = f32::from_bits;
    let b = |k: usize| i.block[k];
    let tint = f(i.prepared.fragment[0][3]);
    let atm = f(i.prepared.fragment[2][3]);
    let checked = [
        ("tint", tint),
        ("atmosphere limit", atm),
        ("fog distance", f(b(4))),
        ("fog range", f(b(5))),
        ("camera slope", f(i.camera_slope)),
        ("camera aspect", f(i.camera_aspect)),
        ("Z0", f(i.z0)),
        ("Z1", f(i.z1)),
        ("PA", f(i.pa)),
        ("PB", f(i.pb)),
        ("PC", f(i.pc)),
        ("PD", f(i.pd)),
        ("ZH", f(i.zh)),
        ("FTOP", f(i.ftop)),
    ];
    if let Some((name, _)) = checked.iter().find(|(_, value)| !value.is_finite()) {
        return Err(FogValuesError::NonFinite(name));
    }
    let ramp = f(i.z1) / f(b(5));
    if !ramp.is_finite() {
        return Err(FogValuesError::NonFinite("Z1 / fog range"));
    }
    let distance = f(b(4));
    let d = if distance < atm { distance } else { atm } as f64;
    let (cs, ca, z1) = (
        f(i.camera_slope) as f64,
        f(i.camera_aspect) as f64,
        f(i.z1) as f64,
    );
    let z = d / (cs * cs * (ca * ca + z1) + z1).sqrt();
    let lower = window_depth(
        z,
        f(i.zh) as f64,
        f(i.pa) as f64,
        f(i.pb) as f64,
        f(i.pc) as f64,
        f(i.pd) as f64,
    ) as f32;
    if !lower.is_finite() {
        return Err(FogValuesError::NonFinite("lower bound"));
    }
    if lower > f(i.ftop) {
        return Err(FogValuesError::BoundAboveTop);
    }
    let tinted = tint > f(i.z0);
    let mut fragment_rows: Vec<[u32; 4]> = i.prepared.fragment.to_vec();
    fragment_rows.push([b(4), b(12), ramp.to_bits(), b(13)]);
    fragment_rows.push([b(0), b(1), b(2), b(14)]);
    if tinted {
        fragment_rows.push([b(9), b(10), b(11), b(3)]);
    }
    Ok(FogValues {
        tinted,
        vertex_rows: i.prepared.vertex,
        fragment_rows,
        bounds: [lower.to_bits(), i.ftop],
    })
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;
    use crate::guest::SparseMemory;
    use crate::testutil::Rng;

    pub(crate) struct Case {
        pub(crate) b4: f32,
        pub(crate) b5: f32,
        pub(crate) atm: f32,
        pub(crate) cs: f32,
        pub(crate) ca: f32,
        pub(crate) tint: f32,
    }

    pub(crate) fn inputs(case: Case) -> FogValueInputs {
        let f = f32::to_bits;
        let mut block = [f(1.0); 16];
        block[4] = f(case.b4);
        block[5] = f(case.b5);
        let mut fragment = [[f(1.0); 4]; 5];
        fragment[0][3] = f(case.tint);
        fragment[2][3] = f(case.atm);
        FogValueInputs {
            prepared: PreparedFogRows {
                vertex: [[f(1.0); 4]; 2],
                fragment,
            },
            block,
            camera_slope: f(case.cs),
            camera_aspect: f(case.ca),
            z0: f(0.0),
            z1: f(1.0),
            pa: f(-2.0),
            pb: f(1.0),
            pc: f(0.0),
            pd: f(1.0),
            zh: f(0.5),
            ftop: f(1.0),
        }
    }

    pub(crate) const F1: Case = Case {
        b4: 4.0,
        b5: 2.0,
        atm: 2.0,
        cs: 0.0,
        ca: 0.0,
        tint: 0.0,
    };
    pub(crate) const F2: Case = Case {
        b4: 4.0,
        b5: 2.0,
        atm: 2.5,
        cs: 0.75,
        ca: 0.0,
        tint: 0.25,
    };

    fn lower(values: &FogValues) -> f32 {
        f32::from_bits(values.bounds[0])
    }

    #[test]
    fn f1_to_f3_table() {
        let v = fog_values(&inputs(F1)).unwrap();
        assert_eq!(v.bounds, [0x3f00_0000, 0x3f80_0000]);
        assert_eq!(f32::from_bits(v.fragment_rows[5][2]), 0.5);
        assert!(!v.tinted);
        assert_eq!(v.fragment_rows.len(), 7);

        let v = fog_values(&inputs(F2)).unwrap();
        assert_eq!(lower(&v), 0.5);
        assert!(v.tinted);
        assert_eq!(v.fragment_rows.len(), 8);

        let v = fog_values(&inputs(Case {
            b4: 3.0,
            b5: 8.0,
            atm: 10.0,
            cs: 0.5,
            ca: 0.75,
            tint: 0.0,
        }))
        .unwrap();
        let exact = {
            let z = 3.0 / (0.25f64 * (0.5625 + 1.0) + 1.0).sqrt();
            0.5 * (-2.0 / z + 1.0) + 0.5
        };
        assert!((lower(&v) as f64 - exact).abs() <= 2f64.powi(-22));
        assert!((lower(&v) as f64 - 0.606_917).abs() < 1e-6);
        assert!(crate::testutil::ulps(lower(&v), f32::from_bits(0x3f1b_5ef1)) <= 4);
        assert_eq!(f32::from_bits(v.fragment_rows[5][2]), 0.125);
        assert!(!v.tinted);
    }

    #[test]
    fn f4_row_layout() {
        let mut i = inputs(F2);
        i.block[9] = 0.75f32.to_bits();
        for k in [0, 1, 2, 3, 10, 11, 12, 13, 14] {
            i.block[k] = (100.0 + k as f32).to_bits();
        }
        let v = fog_values(&i).unwrap();
        let b = |k: usize| i.block[k];
        assert_eq!(v.fragment_rows[7], [0.75f32.to_bits(), b(10), b(11), b(3)]);
        assert_eq!(v.fragment_rows[6], [b(0), b(1), b(2), b(14)]);
        assert_eq!(
            v.fragment_rows[5],
            [4f32.to_bits(), b(12), 0.5f32.to_bits(), b(13)]
        );
        assert_eq!(&v.fragment_rows[..5], &i.prepared.fragment[..]);
        assert_eq!(v.vertex_rows, i.prepared.vertex);
    }

    #[test]
    fn f5_copy_of_nan() {
        let mut i = inputs(F1);
        i.prepared.vertex[0][0] = 0x7fc1_2345;
        i.block[15] = f32::NAN.to_bits();
        let v = fog_values(&i).unwrap();
        assert_eq!(v.vertex_rows[0][0], 0x7fc1_2345);
    }

    #[test]
    fn f6_to_f9_declines() {
        assert_eq!(
            fog_values(&inputs(Case { b5: 0.0, ..F1 })),
            Err(FogValuesError::NonFinite("Z1 / fog range"))
        );
        assert_eq!(
            fog_values(&inputs(Case { atm: 0.0, ..F1 })),
            Err(FogValuesError::NonFinite("lower bound"))
        );
        assert_eq!(
            fog_values(&inputs(Case { cs: f32::NAN, ..F1 })),
            Err(FogValuesError::NonFinite("camera slope"))
        );
        let mut i = inputs(F1);
        i.ftop = 0.25f32.to_bits();
        assert_eq!(fog_values(&i), Err(FogValuesError::BoundAboveTop));
        let mut i = inputs(F1);
        i.pd = f32::INFINITY.to_bits();
        assert_eq!(fog_values(&i), Err(FogValuesError::NonFinite("PD")));
        assert_eq!(
            fog_values(&inputs(Case {
                tint: f32::NAN,
                ..F1
            })),
            Err(FogValuesError::NonFinite("tint"))
        );
    }

    #[test]
    fn f10_minimum_rule_and_negative_distance() {
        let a = fog_values(&inputs(Case {
            b4: 2.0,
            atm: 2.0,
            ..F1
        }))
        .unwrap();
        let b = fog_values(&inputs(Case {
            b4: 4.0,
            atm: 2.0,
            ..F1
        }))
        .unwrap();
        assert_eq!(a.bounds, b.bounds);
        // d = 1.5 with cs = 0: z = 1.5, lower = 0.5·(1 − 2/1.5) + 0.5.
        let c = fog_values(&inputs(Case {
            b4: 1.5,
            atm: 2.0,
            ..F1
        }))
        .unwrap();
        assert_eq!(lower(&c), (0.5f64 * (1.0 - 2.0 / 1.5) + 0.5) as f32);
        // A negative distance is computed as written: z = -2 gives lower = 1.5.
        let mut i = inputs(Case {
            b4: -2.0,
            atm: 2.0,
            ..F1
        });
        i.ftop = 2.0f32.to_bits();
        assert_eq!(lower(&fog_values(&i).unwrap()), 1.5);
    }

    #[test]
    fn camera_acquisition() {
        const TOC: u32 = 0x10_0000;
        let mut m = SparseMemory::new();
        m.put_u32(TOC - CAMERA_GLOBAL, 0x20_0000);
        m.put_u32(0x20_0000, 0x30_0000);
        m.put_f32(0x30_0060, 0.75);
        m.put_f32(0x30_0a24, 1.5);
        let camera = acquire_fog_camera(&m, TOC).unwrap();
        assert_eq!(camera.address, 0x30_0000);
        assert_eq!(f32::from_bits(camera.slope), 0.75);
        assert_eq!(f32::from_bits(camera.aspect), 1.5);
        m.put_u32(0x20_0000, 0);
        assert_eq!(
            acquire_fog_camera(&m, TOC),
            Err(AcquisitionError::NullPointer { global: "camera" })
        );
        assert_eq!(
            acquire_fog_camera(&m, TOC + 1),
            Err(AcquisitionError::InvalidToc(TOC + 1))
        );
    }

    /// Property: `lower` within 2^-22 and the ramp within 1 ulp across random finite inputs.
    #[test]
    fn property_tolerances() {
        let mut rng = Rng::new(21);
        for _ in 0..10_000 {
            let case = Case {
                b4: rng.range_f32(0.5, 500.0),
                b5: rng.range_f32(0.1, 100.0),
                atm: rng.range_f32(0.5, 500.0),
                cs: rng.range_f32(0.0, 2.0),
                ca: rng.range_f32(0.5, 2.5),
                tint: rng.range_f32(-1.0, 1.0),
            };
            let i = inputs(Case { ..case });
            let Ok(v) = fog_values(&i) else { continue };
            let d = case.b4.min(case.atm) as f64;
            let (cs, ca) = (case.cs as f64, case.ca as f64);
            let z = d / (cs * cs * (ca * ca + 1.0) + 1.0).sqrt();
            let exact = 0.5 * (-2.0 / z + 1.0) + 0.5;
            assert!((lower(&v) as f64 - exact).abs() <= 2f64.powi(-22));
            let ramp = f32::from_bits(v.fragment_rows[5][2]);
            assert!(crate::testutil::ulps(ramp, (1.0 / case.b5 as f64) as f32) <= 1);
            assert_eq!(v.tinted, case.tint > 0.0);
        }
    }
}
