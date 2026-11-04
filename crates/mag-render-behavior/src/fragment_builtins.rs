//! Main fragment built-in rows (`fragment-builtins.md`).
//!
//! [`builtin_rows`] turns a material's built-in records into the ordered list of fragment row
//! writes that `material_values` takes as its built-in input. Pass values are read only when a
//! request needs them.

use crate::guest::{GuestMemory, be_u16, be_u32};

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum BuiltinError {
    /// A read outside the material or chunk bytes.
    Bounds(&'static str),
    /// The membership bit or a slot range is out of range.
    Mapping(&'static str),
    /// Kind 31 or 32 with a missing role.
    UnqualifiedAnimated { kind: u32, role: usize },
    /// An all-ones exponent in an arithmetic input.
    Domain(&'static str),
    /// A computed result whose exact value is not a finite binary32.
    Overflow(&'static str),
    /// A pass value could not be read, including `t` at `PASS+0x1E0` when a kind-31/32 request
    /// needs it. This is the item's read failure, not an unqualified kind.
    PassRead(String),
}

/// Kind 31's two scale factors, as the binary32 values nearest the decimals.
pub const K1: f32 = 0.91;
pub const K2: f32 = 0.95;

fn span<'a>(
    bytes: &'a [u8],
    at: u64,
    length: u64,
    what: &'static str,
) -> Result<&'a [u8], BuiltinError> {
    let end = at.checked_add(length).ok_or(BuiltinError::Bounds(what))?;
    bytes
        .get(at as usize..end as usize)
        .ok_or(BuiltinError::Bounds(what))
}

fn half(bytes: &[u8], at: u64, what: &'static str) -> Result<u64, BuiltinError> {
    Ok(be_u16(span(bytes, at, 2, what)?, 0) as u64)
}

fn word(bytes: &[u8], at: u64, what: &'static str) -> Result<u32, BuiltinError> {
    Ok(be_u32(span(bytes, at, 4, what)?, 0))
}

/// The domain rule for an arithmetic input: subnormal is zero, an all-ones exponent fails.
fn input(bits: u32, what: &'static str) -> Result<f64, BuiltinError> {
    if (bits >> 23) & 0xff == 0xff {
        return Err(BuiltinError::Domain(what));
    }
    let value = f32::from_bits(bits);
    Ok(if value.is_subnormal() {
        0.0
    } else {
        value as f64
    })
}

fn output(value: f64, what: &'static str) -> Result<u32, BuiltinError> {
    if !value.is_finite() || value.abs() > f32::MAX as f64 {
        return Err(BuiltinError::Overflow(what));
    }
    Ok((value as f32).to_bits())
}

/// Lazily read pass values; every request in one call sees the same value.
struct Pass<'a, M: ?Sized> {
    memory: &'a M,
    address: u32,
}

impl<M: GuestMemory + ?Sized> Pass<'_, M> {
    fn words<const N: usize>(&self, offset: u32) -> Result<[u32; N], BuiltinError> {
        let address = self.address.wrapping_add(offset);
        let length = 4 * N as u32;
        if address as u64 + length as u64 > 1 << 32 {
            return Err(BuiltinError::PassRead(format!("{address:#x} passes 2^32")));
        }
        let bytes = self
            .memory
            .read(address, length)
            .map_err(|e| BuiltinError::PassRead(e.to_string()))?;
        if bytes.len() != length as usize {
            return Err(BuiltinError::PassRead(format!(
                "short read at {address:#x}"
            )));
        }
        Ok(std::array::from_fn(|i| be_u32(&bytes, 4 * i)))
    }
}

/// Role values `R_0 … R_7`, three words each; `None` for a role no record carries.
fn roles(material: &[u8]) -> Result<[Option<[u32; 3]>; 8], BuiltinError> {
    let count = span(material, 0x52, 1, "role count")?[0] as u64;
    let records = half(material, 0x44, "parameter records")?;
    let values = half(material, 0x46, "parameter values")?;
    let mut roles = [None; 8];
    for i in 0..count {
        let record = span(material, records + 16 * i, 16, "parameter record")?;
        let role = be_u16(record, 14) as usize;
        if role >= 8 {
            continue;
        }
        let index = be_u16(record, 8) as u64;
        let words = match role {
            0 => 2,
            6 => 3,
            _ => 1,
        };
        let mut value = [0u32; 3];
        for (w, slot) in value.iter_mut().enumerate().take(words) {
            *slot = word(material, values + 4 * (index + w as u64), "role value")?;
        }
        roles[role] = Some(value);
    }
    Ok(roles)
}

/// The fragment built-in rows of one item.
///
/// `material` is the material's copied bytes, `chunk` the selected technique's program chunk,
/// `selector` the technique selector and `pass` the Main pass object's address.
pub fn builtin_rows<M: GuestMemory + ?Sized>(
    memory: &M,
    pass: u32,
    material: &[u8],
    chunk: &[u8],
    selector: u32,
) -> Result<Vec<(u32, [u32; 4])>, BuiltinError> {
    let pass = Pass {
        memory,
        address: pass,
    };
    let technique = half(material, 0x48, "technique table")? + 32 * selector as u64;
    let membership = span(material, technique, 32, "technique record")?[1] as u32;
    if membership >= 32 {
        return Err(BuiltinError::Mapping("membership bit"));
    }
    let count = span(material, 0x53, 1, "built-in count")?[0] as u64;
    if count == 0 {
        return Ok(Vec::new());
    }
    let table = half(material, 0x42, "built-in table")?;

    let mut rows = Vec::new();
    let mut role_values = None;
    let mut time = None;
    for r in 0..count {
        let record = span(material, table + 16 * r, 16, "built-in record")?;
        let kind = be_u32(record, 4);
        if be_u32(record, 8) & (1 << membership) == 0 {
            continue;
        }
        if !matches!(kind, 3 | 4 | 31 | 32) {
            continue;
        }
        let destination = word(chunk, be_u32(record, 12) as u64, "destination word")?;
        if (destination >> 24) & 0xf != 3 {
            continue;
        }
        let slot = (destination >> 14) & 0x3ff;
        match kind {
            3 => {
                let alpha = span(material, 0x5c, 1, "alpha")?[0] as f64;
                let [mu] = pass.words::<1>(0xd8)?;
                let mu = input(mu, "alpha modulation")?;
                let a = alpha / 255.0;
                let modulated = a * mu;
                rows.push((
                    slot,
                    [
                        if alpha > 0.0 {
                            output(1.0 / a, "alpha reciprocal")?
                        } else {
                            1f32.to_bits()
                        },
                        if modulated < 1.0 {
                            output(1.0 / (1.0 - modulated), "transmission reciprocal")?
                        } else {
                            1f32.to_bits()
                        },
                        output(modulated, "modulated alpha")?,
                        1f32.to_bits(),
                    ],
                ));
            }
            4 => {
                if slot > 1020 {
                    return Err(BuiltinError::Mapping("pass matrix slot"));
                }
                let x = pass.words::<16>(0)?;
                for j in 0..4 {
                    rows.push((slot + j as u32, std::array::from_fn(|l| x[4 * j + l])));
                }
            }
            _ => {
                let span_rows = if kind == 31 { 4 } else { 2 };
                if slot + span_rows > 1024 {
                    return Err(BuiltinError::Mapping("animated slot"));
                }
                if role_values.is_none() {
                    role_values = Some(roles(material)?);
                }
                let roles = role_values.as_ref().expect("read above");
                let role = |r: usize| -> Result<[u32; 3], BuiltinError> {
                    roles[r].ok_or(BuiltinError::UnqualifiedAnimated { kind, role: r })
                };
                let needed: &[usize] = if kind == 31 {
                    &[0, 1, 2]
                } else {
                    &[3, 4, 5, 6, 7]
                };
                for &r in needed {
                    role(r)?;
                }
                if time.is_none() {
                    let [t] = pass.words::<1>(0x1e0)?;
                    time = Some(t);
                }
                let t = input(time.expect("read above"), "time")?;
                let value = |r: usize, lane: usize| input(role(r).expect("checked")[lane], "role");
                if kind == 31 {
                    let (p, q) = (value(0, 0)?, value(0, 1)?);
                    let (d, w) = (value(1, 0)?, value(2, 0)?);
                    let u = t * p / d;
                    let s = t * q / d;
                    let theta = std::f64::consts::PI * w;
                    let (a, b) = (s * theta.cos(), s * theta.sin());
                    let half_root3 = 3f64.sqrt() / 2.0;
                    let (k1, k2) = (K1 as f64, K2 as f64);
                    let values = [
                        [-half_root3 * u + a, u / 2.0 + b, a, -u + b],
                        [half_root3 * u + a, u / 2.0 + b, a, -u + b],
                        [d, d, -k1 * d, k1 * d],
                        [-k2 * d, k2 * d, -d, -d],
                    ];
                    for (j, lanes) in values.iter().enumerate() {
                        let mut row = [0u32; 4];
                        for (lane, v) in lanes.iter().enumerate() {
                            row[lane] = output(*v, "animated scroll")?;
                        }
                        rows.push((slot + j as u32, row));
                    }
                } else {
                    let r7 = value(7, 0)?;
                    rows.push((
                        slot,
                        [
                            output(value(3, 0)? * value(4, 0)?, "animated product")?,
                            output(value(5, 0)? * t, "animated product")?,
                            0,
                            0,
                        ],
                    ));
                    rows.push((
                        slot + 1,
                        [
                            output(value(6, 0)? * r7, "animated product")?,
                            output(value(6, 1)? * r7, "animated product")?,
                            output(value(6, 2)? * r7, "animated product")?,
                            0,
                        ],
                    ));
                }
            }
        }
    }
    Ok(rows)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::guest::SparseMemory;

    const PASS: u32 = 0x1000;

    struct Setup {
        material: Vec<u8>,
        chunk: Vec<u8>,
        memory: SparseMemory,
    }

    /// `m = 0`; built-in records at `+0x100`; parameter records at `+0x200`, values at `+0x300`.
    fn setup() -> Setup {
        let mut material = vec![0u8; 0x400];
        material[0x42..0x44].copy_from_slice(&0x100u16.to_be_bytes());
        material[0x44..0x46].copy_from_slice(&0x200u16.to_be_bytes());
        material[0x46..0x48].copy_from_slice(&0x300u16.to_be_bytes());
        material[0x48..0x4a].copy_from_slice(&0x80u16.to_be_bytes());
        let mut memory = SparseMemory::new();
        memory.put(PASS, &[0; 0x200]);
        Setup {
            material,
            chunk: vec![0; 0x100],
            memory,
        }
    }

    impl Setup {
        /// Adds a built-in record of `kind` with mask 1, its destination word at chunk `+4·n`.
        fn record(&mut self, kind: u32, slot: u32) {
            self.record_with(kind, 1, (3 << 24) | (slot << 14));
        }
        fn record_with(&mut self, kind: u32, mask: u32, destination: u32) {
            let n = self.material[0x53] as usize;
            let at = 0x100 + 16 * n;
            self.material[at + 4..at + 8].copy_from_slice(&kind.to_be_bytes());
            self.material[at + 8..at + 12].copy_from_slice(&mask.to_be_bytes());
            self.material[at + 12..at + 16].copy_from_slice(&(4 * n as u32).to_be_bytes());
            self.chunk[4 * n..4 * n + 4].copy_from_slice(&destination.to_be_bytes());
            self.material[0x53] += 1;
        }
        fn role(&mut self, role: u16, values: &[f32]) {
            let i = self.material[0x52] as usize;
            let at = 0x200 + 16 * i;
            let index = 4 * i as u16;
            self.material[at + 8..at + 10].copy_from_slice(&index.to_be_bytes());
            self.material[at + 14..at + 16].copy_from_slice(&role.to_be_bytes());
            for (w, v) in values.iter().enumerate() {
                let v_at = 0x300 + 4 * (index as usize + w);
                self.material[v_at..v_at + 4].copy_from_slice(&v.to_bits().to_be_bytes());
            }
            self.material[0x52] += 1;
        }
        fn rows(&self) -> Result<Vec<(u32, [u32; 4])>, BuiltinError> {
            builtin_rows(&self.memory, PASS, &self.material, &self.chunk, 0)
        }
    }

    fn close(actual: u32, expected: f64, ulps: f64) {
        let a = f32::from_bits(actual);
        let ulp = (a.abs().next_up() - a.abs()) as f64;
        assert!(
            (a as f64 - expected).abs() <= ulps * ulp,
            "{a} vs {expected}"
        );
    }

    #[test]
    fn v1_alpha_transfer() {
        for (alpha, mu, expected) in [
            (51u8, 0.5f32, [5.0, 1.0 / 0.9, 0.1, 1.0]),
            (0, 0.5, [1.0, 1.0, 0.0, 1.0]),
            (255, 1.0, [1.0, 1.0, 1.0, 1.0]),
            (255, 2.0, [1.0, 1.0, 2.0, 1.0]),
            (102, 0.25, [2.5, 1.0 / 0.9, 0.1, 1.0]),
        ] {
            let mut s = setup();
            s.material[0x5c] = alpha;
            s.memory.put_f32(PASS + 0xd8, mu);
            s.record(3, 8);
            let rows = s.rows().unwrap();
            assert_eq!(rows.len(), 1);
            assert_eq!(rows[0].0, 8);
            for lane in 0..3 {
                close(rows[0].1[lane], expected[lane], 2.0);
            }
            assert_eq!(rows[0].1[3], 1f32.to_bits());
        }
    }

    #[test]
    fn v2_pass_matrix() {
        let mut s = setup();
        for i in 0..16 {
            s.memory.put_f32(PASS + 4 * i, (i + 1) as f32);
        }
        s.record(4, 40);
        let rows = s.rows().unwrap();
        assert_eq!(rows.len(), 4);
        for (j, (slot, row)) in rows.iter().enumerate() {
            assert_eq!(*slot, 40 + j as u32);
            let first = (4 * j + 1) as f32;
            assert_eq!(
                *row,
                [first, first + 1.0, first + 2.0, first + 3.0].map(f32::to_bits)
            );
        }
        let mut s = setup();
        s.record(4, 1021);
        assert_eq!(s.rows(), Err(BuiltinError::Mapping("pass matrix slot")));
    }

    fn scroll(w: f32) -> Vec<(u32, [u32; 4])> {
        let mut s = setup();
        s.role(0, &[2.0, 6.0]);
        s.role(1, &[4.0]);
        s.role(2, &[w]);
        s.memory.put_f32(PASS + 0x1e0, 2.0);
        s.record(31, 20);
        s.rows().unwrap()
    }

    fn check_scroll(rows: &[(u32, [u32; 4])], expected: [[f64; 4]; 2], u: f64, s: f64, theta: f64) {
        let bound =
            2f64.powi(-18) * (u.abs() + s.abs()) + 2f64.powi(-20) * s.abs() * (1.0 + theta.abs());
        for (j, lanes) in expected.iter().enumerate() {
            assert_eq!(rows[j].0, 20 + j as u32);
            for lane in 0..4 {
                let a = f32::from_bits(rows[j].1[lane]) as f64;
                assert!((a - lanes[lane]).abs() <= bound, "row {j} lane {lane}: {a}");
            }
        }
        let k1 = K1 as f64 * 4.0;
        let k2 = K2 as f64 * 4.0;
        for (j, lanes) in [[4.0, 4.0, -k1, k1], [-k2, k2, -4.0, -4.0]]
            .iter()
            .enumerate()
        {
            for lane in 0..4 {
                close(rows[2 + j].1[lane], lanes[lane], 1.0);
            }
        }
        // Visual check of the scale rows against the decimals.
        assert!((f32::from_bits(rows[2].1[2]) + 3.64).abs() < 1e-5);
        assert!((f32::from_bits(rows[3].1[0]) + 3.8).abs() < 1e-5);
    }

    #[test]
    fn v3_v4_animated_scroll() {
        let h = 3f64.sqrt() / 2.0;
        let rows = scroll(0.5);
        assert_eq!(rows.len(), 4);
        check_scroll(
            &rows,
            [[-h, 3.5, 0.0, 2.0], [h, 3.5, 0.0, 2.0]],
            1.0,
            3.0,
            std::f64::consts::FRAC_PI_2,
        );
        let rows = scroll(0.0);
        check_scroll(
            &rows,
            [[-h + 3.0, 0.5, 3.0, -1.0], [h + 3.0, 0.5, 3.0, -1.0]],
            1.0,
            3.0,
            0.0,
        );
    }

    #[test]
    fn v5_animated_products() {
        let mut s = setup();
        s.role(3, &[1.5]);
        s.role(4, &[4.0]);
        s.role(5, &[-2.0]);
        s.role(6, &[0.5, -1.0, 2.0]);
        s.role(7, &[3.0]);
        s.memory.put_f32(PASS + 0x1e0, 0.25);
        s.record(32, 10);
        let rows = s.rows().unwrap();
        assert_eq!(
            rows,
            vec![
                (10, [6.0f32, -0.5, 0.0, 0.0].map(f32::to_bits)),
                (11, [1.5f32, -3.0, 6.0, 0.0].map(f32::to_bits)),
            ]
        );
        let mut s = setup();
        s.record(32, 1023);
        assert_eq!(s.rows(), Err(BuiltinError::Mapping("animated slot")));
    }

    #[test]
    fn v6_role_precedence() {
        let mut s = setup();
        s.role(0, &[2.0, 6.0]);
        s.role(1, &[9.0]);
        s.role(1, &[2.0]);
        s.role(9, &[100.0]);
        s.role(2, &[0.0]);
        s.memory.put_f32(PASS + 0x1e0, 2.0);
        s.record(31, 20);
        let rows = s.rows().unwrap();
        // d = 2: row L+2 lane 0 is d.
        assert_eq!(f32::from_bits(rows[2].1[0]), 2.0);
        let mut s = setup();
        s.role(0, &[2.0, 6.0]);
        s.role(1, &[4.0]);
        s.memory.put_f32(PASS + 0x1e0, 2.0);
        s.record(31, 20);
        assert_eq!(
            s.rows(),
            Err(BuiltinError::UnqualifiedAnimated { kind: 31, role: 2 })
        );
    }

    #[test]
    fn v7_membership() {
        let mut s = setup();
        s.memory.put_f32(PASS + 0xd8, 0.5);
        s.material[0x5c] = 51;
        s.record_with(3, 1, (3 << 24) | (1 << 14));
        s.record_with(3, 2, (3 << 24) | (2 << 14));
        s.record_with(3, 1, (3 << 24) | (3 << 14));
        let slots: Vec<u32> = s.rows().unwrap().iter().map(|r| r.0).collect();
        assert_eq!(slots, vec![1, 3]);
        s.material[0x80 + 1] = 32;
        assert_eq!(s.rows(), Err(BuiltinError::Mapping("membership bit")));
    }

    #[test]
    fn v8_v9_ignored_kinds_and_destinations() {
        let mut s = setup();
        s.record(5, 0);
        let at = 0x100 + 12;
        s.material[at..at + 4].copy_from_slice(&0xffff_ffffu32.to_be_bytes());
        assert_eq!(s.rows(), Ok(Vec::new()));
        let mut s = setup();
        s.record(3, 0);
        s.material[at..at + 4].copy_from_slice(&0xffff_ffffu32.to_be_bytes());
        assert_eq!(s.rows(), Err(BuiltinError::Bounds("destination word")));
        // Kind 4 aimed at vertex constants: no rows, and X is never read (the pass is unmapped).
        let mut s = setup();
        s.memory = SparseMemory::new();
        s.record_with(4, 1, (2 << 24) | (8 << 14));
        assert_eq!(s.rows(), Ok(Vec::new()));
    }

    #[test]
    fn v10_order_against_ordinary_rows() {
        // The built-in rows follow the ordinary rows; material-values applies them in order.
        let mut s = setup();
        s.material[0x5c] = 51;
        s.memory.put_f32(PASS + 0xd8, 0.5);
        s.record(3, 8);
        let builtins = s.rows().unwrap();
        let mut writes = vec![(8u32, [7f32.to_bits(); 4])];
        writes.extend(builtins);
        let last = writes.iter().rev().find(|w| w.0 == 8).unwrap();
        close(last.1[0], 5.0, 2.0);
    }

    #[test]
    fn v11_domain() {
        let mut s = setup();
        s.role(0, &[2.0, 6.0]);
        s.role(1, &[0.0]);
        s.role(2, &[0.5]);
        s.memory.put_f32(PASS + 0x1e0, 2.0);
        s.record(31, 20);
        assert_eq!(s.rows(), Err(BuiltinError::Overflow("animated scroll")));

        let mut s = setup();
        s.material[0x5c] = 51;
        s.memory.put_f32(PASS + 0xd8, f32::NAN);
        s.record(3, 8);
        assert_eq!(s.rows(), Err(BuiltinError::Domain("alpha modulation")));

        let mut s = setup();
        s.role(3, &[1.5]);
        s.role(4, &[4.0]);
        s.role(5, &[f32::from_bits(1)]);
        s.role(6, &[0.5, -1.0, 2.0]);
        s.role(7, &[3.0]);
        s.memory.put_f32(PASS + 0x1e0, 0.25);
        s.record(32, 10);
        assert_eq!(s.rows().unwrap()[0].1[1], 0);
    }

    #[test]
    fn no_records_reads_nothing_else() {
        let mut s = setup();
        s.memory = SparseMemory::new();
        s.material[0x42..0x44].copy_from_slice(&0xffffu16.to_be_bytes());
        assert_eq!(s.rows(), Ok(Vec::new()));
        // A pass that cannot be read fails the item when a request needs it.
        s.material[0x42..0x44].copy_from_slice(&0x100u16.to_be_bytes());
        s.record(3, 0);
        assert!(matches!(s.rows(), Err(BuiltinError::PassRead(_))));
    }
}
