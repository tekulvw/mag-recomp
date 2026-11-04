//! Source-qualified fragment builtin requests of one material. Every active
//! record survives as a request/no-op/unqualified operation. Their rows are
//! prepared by the scene crate's `fragment_builtin_rows`.
use super::ParameterTarget;

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum BuiltinError {
    Bounds,
    Mapping,
    Allocation,
    MissingSource,
    UnqualifiedSemantic(u32),
    UnqualifiedDestination {
        semantic: u32,
        target: ParameterTarget,
    },
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum FragmentBuiltinRequest {
    AlphaTransfer {
        record_index: usize,
        logical_slot: u16,
        primary_alpha: u8,
    },
    Matrix4 {
        record_index: usize,
        logical_slot: u16,
    },
    NoOp {
        record_index: usize,
    },
    OtherDestination {
        record_index: usize,
        semantic: u32,
        target: ParameterTarget,
    },
    Unqualified {
        record_index: usize,
        semantic: u32,
    },
    /// Kinds 31/32: a block of rows derived from the selected material's
    /// role-tagged parameter values and the pass time scalar.
    Animated {
        record_index: usize,
        semantic: u32,
        logical_slot: u16,
        roles: RoleValues,
    },
}
/// Last value per parameter role (record half +14) of the selected material,
/// up to three words each; `None` when no record carries that role.
pub type RoleValues = [Option<[u32; 3]>; 8];
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct BuiltinPassValues {
    pub matrix_words: Option<[u32; 16]>,
    pub alpha_modulation: Option<u32>,
    /// Pass binding payload scalar at +0x1E0, read by kinds 31/32.
    pub animation_time: Option<u32>,
}
/// Role values come from every record of the material's parameter table
/// (records at half(0x44), values at half(0x46), byte(0x52) records), not only
/// the technique's ordinary run. A later record with the same role wins.
fn role_values(material: &[u8]) -> Result<RoleValues, BuiltinError> {
    let records = usize::from(half(material, 0x44)?);
    let values = usize::from(half(material, 0x46)?);
    let count = usize::from(*material.get(0x52).ok_or(BuiltinError::Bounds)?);
    let mut roles: RoleValues = [None; 8];
    for index in 0..count {
        let record = records
            .checked_add(index * 16)
            .ok_or(BuiltinError::Bounds)?;
        let role = usize::from(half(material, record + 14)?);
        if role >= roles.len() {
            continue;
        }
        let at = values
            .checked_add(usize::from(half(material, record + 8)?) * 4)
            .ok_or(BuiltinError::Bounds)?;
        // Roles read one word except 0 (two) and 6 (three); unread words stay 0.
        let needed = match role {
            0 => 2,
            6 => 3,
            _ => 1,
        };
        let mut words = [0; 3];
        for (i, w) in words.iter_mut().take(needed).enumerate() {
            *w = word(material, at + 4 * i)?;
        }
        roles[role] = Some(words);
    }
    Ok(roles)
}
fn word(b: &[u8], at: usize) -> Result<u32, BuiltinError> {
    Ok(u32::from_be_bytes(
        b.get(at..at.checked_add(4).ok_or(BuiltinError::Bounds)?)
            .ok_or(BuiltinError::Bounds)?
            .try_into()
            .map_err(|_| BuiltinError::Bounds)?,
    ))
}
fn half(b: &[u8], at: usize) -> Result<u16, BuiltinError> {
    Ok(u16::from_be_bytes(
        b.get(at..at.checked_add(2).ok_or(BuiltinError::Bounds)?)
            .ok_or(BuiltinError::Bounds)?
            .try_into()
            .map_err(|_| BuiltinError::Bounds)?,
    ))
}
pub fn decode_fragment_builtin_requests(
    selected: &[u8],
    primary: &[u8],
    selector: u32,
    chunk: &[u8],
) -> Result<Vec<FragmentBuiltinRequest>, BuiltinError> {
    let technique = usize::from(half(selected, 0x48)?)
        .checked_add(
            (selector as usize)
                .checked_mul(32)
                .ok_or(BuiltinError::Bounds)?,
        )
        .ok_or(BuiltinError::Bounds)?;
    let map = *selected.get(technique + 1).ok_or(BuiltinError::Bounds)?;
    if map >= 32 {
        return Err(BuiltinError::Mapping);
    }
    let count = usize::from(*selected.get(0x53).ok_or(BuiltinError::Bounds)?);
    if count == 0 {
        return Ok(Vec::new());
    }
    let table = usize::from(half(selected, 0x42)?);
    let records = selected
        .get(table..table + count * 16)
        .ok_or(BuiltinError::Bounds)?;
    let mut requests = Vec::new();
    requests
        .try_reserve_exact(count)
        .map_err(|_| BuiltinError::Allocation)?;
    for (record_index, record) in records.as_chunks::<16>().0.iter().enumerate() {
        if word(record, 8)? & (1u32 << map) == 0 {
            continue;
        }
        let semantic = word(record, 4)?;
        // Only kinds 3, 4, 31 and 32 are built-ins. Every other kind,
        // including 5..=30 and anything above 32, is a no-op whose
        // destination word is never read.
        if !matches!(semantic, 3 | 4 | 31 | 32) {
            requests.push(FragmentBuiltinRequest::NoOp { record_index });
            continue;
        }
        let descriptor = word(chunk, word(record, 12)? as usize)?;
        let slot = ((descriptor >> 14) & 1023) as u16;
        let target = match ((descriptor >> 24) & 15) as u8 {
            1 => ParameterTarget::ConstantVertexAttribute(slot),
            2 => ParameterTarget::VertexConstants(slot),
            3 => ParameterTarget::FragmentConstants(slot),
            kind => ParameterTarget::IgnoredKind(kind),
        };
        if !matches!(target, ParameterTarget::FragmentConstants(_)) {
            requests.push(FragmentBuiltinRequest::OtherDestination {
                record_index,
                semantic,
                target,
            });
            continue;
        }
        if matches!(semantic, 31 | 32) {
            let ParameterTarget::FragmentConstants(logical_slot) = target else {
                unreachable!("checked fragment destination")
            };
            requests.push(FragmentBuiltinRequest::Animated {
                record_index,
                semantic,
                logical_slot,
                roles: role_values(selected)?,
            });
            continue;
        }
        requests.push(if semantic == 3 {
            FragmentBuiltinRequest::AlphaTransfer {
                record_index,
                logical_slot: slot,
                primary_alpha: *primary.get(0x5c).ok_or(BuiltinError::Bounds)?,
            }
        } else {
            FragmentBuiltinRequest::Matrix4 {
                record_index,
                logical_slot: slot,
            }
        });
    }
    Ok(requests)
}
#[cfg(test)]
mod tests {
    use super::*;
    fn word(b: &mut [u8], at: usize, value: u32) {
        b[at..at + 4].copy_from_slice(&value.to_be_bytes());
    }
    #[test]
    fn source_masks_do_not_terminate_builtin_scan_and_noop_has_no_descriptor_read() {
        let mut selected = vec![0; 192];
        let mut primary = selected.clone();
        let mut chunk = vec![0; 8];
        selected[0x42..0x44].copy_from_slice(&96u16.to_be_bytes());
        selected[0x48..0x4a].copy_from_slice(&160u16.to_be_bytes());
        selected[0x53] = 4;
        primary[0x5c] = 7;
        for (i, semantic) in [3, 4, 5, 3].into_iter().enumerate() {
            let at = 96 + i * 16;
            word(&mut selected, at + 4, semantic);
            word(&mut selected, at + 8, if i == 1 { 0 } else { 1 });
            word(
                &mut selected,
                at + 12,
                if i == 0 {
                    0
                } else if i == 2 {
                    u32::MAX
                } else {
                    4
                },
            );
        }
        word(&mut chunk, 0, (3 << 24) | (8 << 14));
        word(&mut chunk, 4, (3 << 24) | (9 << 14));
        let requests = decode_fragment_builtin_requests(&selected, &primary, 0, &chunk).unwrap();
        assert_eq!(
            requests,
            [
                FragmentBuiltinRequest::AlphaTransfer {
                    record_index: 0,
                    logical_slot: 8,
                    primary_alpha: 7
                },
                FragmentBuiltinRequest::NoOp { record_index: 2 },
                FragmentBuiltinRequest::AlphaTransfer {
                    record_index: 3,
                    logical_slot: 9,
                    primary_alpha: 7
                }
            ]
        );
    }
}
