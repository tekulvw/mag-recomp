//! Direct-draw vertex and index reads from the item's own descriptors. No
//! buffers prepared by another stage are used.
//! Canonical rows expand the actual RSX frequency operation; the real vertex
//! program still performs any instance transform. Admission separately requires
//! the genuine inherited vertex base-offset/index snapshot to be zero.
use super::{AuxiliaryError, DecodedAuxiliaryAttribute, OwnedAuxiliaryAttributes};
use crate::{
    geometry_bindings::{self, AuxiliaryBinding, BindingError, VertexBinding},
    material_textures::{LocatedTextureRequest, TextureLocation},
};
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum DirectDrawError {
    Layout,
    Slot(u32),
    Stream(u32),
    Format(u32),
    Components(u32),
    MissingStream { slot: u32, stream: u32 },
    Primitive(u32),
    IndexFormat(u32),
    Empty,
    Range,
    Budget,
    Allocation,
    Read(AuxiliaryError),
    Decode(BindingError),
}
#[derive(Debug)]
pub struct OwnedDirectDraw {
    pub layout_address: u32,
    pub instanced: bool,
    pub vertex_count: usize,
    pub source_index_count: u32,
    pub primitive: u32,
    pub indices: Vec<u32>,
    pub attributes: OwnedAuxiliaryAttributes,
}
impl OwnedDirectDraw {
    pub fn owned_bytes(&self) -> usize {
        self.indices.len() * 4
            + self
                .attributes
                .attributes
                .iter()
                .map(|a| a.words.len() * 16)
                .sum::<usize>()
    }
}
fn word(b: &[u8], at: usize) -> u32 {
    u32::from_be_bytes(b[at..at + 4].try_into().expect("bounded source field"))
}
fn half(b: &[u8], at: usize) -> u16 {
    u16::from_be_bytes(b[at..at + 2].try_into().expect("bounded source field"))
}
fn triangles(source: &[u32], primitive: u32) -> Result<Vec<u32>, DirectDrawError> {
    let mut out = Vec::new();
    let capacity = match primitive {
        5 => source.len(),
        6 | 7 => source.len().saturating_sub(2) * 3,
        8 => source.len() / 4 * 6,
        other => return Err(DirectDrawError::Primitive(other)),
    };
    out.try_reserve_exact(capacity)
        .map_err(|_| DirectDrawError::Allocation)?;
    match primitive {
        5 => {
            if !source.len().is_multiple_of(3) {
                return Err(DirectDrawError::Primitive(primitive));
            }
            out.extend_from_slice(source)
        }
        6 => {
            for i in 2..source.len() {
                let a = source[i - 2];
                let b = source[i - 1];
                let triangle = if i & 1 == 0 {
                    [a, b, source[i]]
                } else {
                    [b, a, source[i]]
                };
                out.extend_from_slice(&triangle);
            }
        }
        7 => {
            for i in 2..source.len() {
                out.extend_from_slice(&[source[0], source[i - 1], source[i]])
            }
        }
        8 => {
            if !source.len().is_multiple_of(4) {
                return Err(DirectDrawError::Primitive(primitive));
            }
            for q in source.as_chunks::<4>().0 {
                out.extend_from_slice(&[q[0], q[1], q[2], q[0], q[2], q[3]])
            }
        }
        _ => unreachable!(),
    }
    if out.is_empty() {
        return Err(DirectDrawError::Empty);
    }
    Ok(out)
}
pub fn copy_direct_draw(
    mesh: &[u8; 192],
    layout: &[u8; 656],
    transform: &[u8; 112],
    instanced: bool,
    technique_mask: u32,
    budget: usize,
    read: &mut impl FnMut(LocatedTextureRequest) -> Result<Vec<u8>, AuxiliaryError>,
) -> Result<OwnedDirectDraw, DirectDrawError> {
    use DirectDrawError as E;
    let packed = instanced && transform[0x6f] != 0;
    let base_vertices = word(mesh, 0x34);
    let indexed = word(mesh, 0x38) != 0;
    let count = if instanced {
        u32::from(half(transform, 0x6c))
    } else if indexed {
        word(mesh, 0x38)
    } else {
        base_vertices
    };
    let count = if instanced && !indexed {
        count.checked_mul(base_vertices).ok_or(E::Range)?
    } else {
        count
    };
    if count == 0 {
        return Err(E::Empty);
    }
    let count_usize = count as usize;
    // A bounded worst-case triangulation reserve before any source copy.
    let index_budget = count_usize
        .checked_mul(12)
        .filter(|&n| n <= budget)
        .ok_or(E::Budget)?;
    let mut charge = index_budget;
    let raw_indices = if indexed {
        let flags = word(mesh, 0x48);
        if flags & !0x10 != 0 {
            return Err(E::IndexFormat(flags));
        }
        let bytes = count_usize
            .checked_mul(if flags == 0x10 { 2 } else { 4 })
            .ok_or(E::Range)?;
        charge = charge
            .checked_add(bytes)
            .filter(|&n| n <= budget)
            .ok_or(E::Budget)?;
        let index = if instanced {
            word(transform, 0x68)
        } else {
            word(mesh, 0x5c)
        };
        let offset = index & 0x7fffffff;
        if u64::from(offset) + bytes as u64 > 0x80000000 {
            return Err(E::Range);
        }
        let raw = read(LocatedTextureRequest {
            location: if index >> 31 == 0 {
                TextureLocation::Local
            } else {
                TextureLocation::Main
            },
            offset,
            bytes,
        })
        .map_err(E::Read)?;
        if raw.len() != bytes {
            return Err(E::Layout);
        }
        if flags == 0x10 {
            raw.as_chunks::<2>()
                .0
                .iter()
                .map(|v| u32::from(u16::from_be_bytes(*v)))
                .collect::<Vec<_>>()
        } else {
            raw.as_chunks::<4>()
                .0
                .iter()
                .map(|v| u32::from_be_bytes(*v))
                .collect::<Vec<_>>()
        }
    } else {
        (0..count).collect::<Vec<_>>()
    };
    // Indexed fetch has a20-bit address domain. Native u32 indices retain every
    // logical index within it; no truncation to16 bits or population filtering.
    if raw_indices.iter().any(|&v| v > 0xfffff) {
        return Err(E::Range);
    }
    let vertex_count = raw_indices.iter().max().copied().ok_or(E::Empty)? as usize + 1;
    let indices = triangles(&raw_indices, word(mesh, 0x44))?;
    let frequency_mask = if packed {
        0xc7ff
    } else if instanced {
        u32::from(half(mesh, 0x9c))
    } else {
        if word(layout, 0x80) != 0x00041fc0 {
            return Err(E::Layout);
        }
        word(layout, 0x84)
    };
    let mut plans: [Option<AuxiliaryBinding>; 16] = core::array::from_fn(|_| None);
    for declaration in 0..16 {
        let at = 0x110 + declaration * 20;
        let slot = word(layout, at);
        if slot >= 16 {
            return Err(E::Slot(slot));
        }
        let components = word(layout, at + 16);
        if components == 0 || (!instanced && technique_mask & (1 << (2 * (15 - slot))) == 0) {
            continue;
        }
        if components > 4 {
            return Err(E::Components(components));
        }
        let stream = word(layout, at + 8);
        if stream >= 4 {
            return Err(E::Stream(stream));
        }
        let source = word(mesh, 0x4c + stream as usize * 4);
        if source == 0 {
            return Err(E::MissingStream { slot, stream });
        }
        let offset = (source & 0x7fffffff)
            .checked_add(word(layout, at + 4))
            .filter(|&v| v < 0x80000000)
            .ok_or(E::Range)?;
        let kind = word(layout, at + 12);
        if !(1..=7).contains(&kind) {
            return Err(E::Format(kind));
        }
        let stride = half(layout, 0x250 + stream as usize * 2);
        if stride > 255 {
            return Err(E::Layout);
        }
        let frequency = if instanced {
            if frequency_mask & (1 << declaration) != 0 {
                base_vertices
            } else {
                0
            }
        } else {
            if word(layout, slot as usize * 8) != 0x00041740 + slot * 4 {
                return Err(E::Layout);
            }
            let state = word(layout, slot as usize * 8 + 4);
            if state & 0xffff != (u32::from(stride) << 8) | (components << 4) | kind {
                return Err(E::Layout);
            }
            state >> 16
        };
        if frequency > 65535 {
            return Err(E::Range);
        }
        plans[slot as usize] = Some(AuxiliaryBinding {
            binding: VertexBinding {
                declaration,
                semantic: 0,
                slot: slot as u8,
                format: kind as u8,
                components: components as u8,
                stride: stride as u8,
                byte_offset: 0,
                encoded_bytes: 0,
            },
            source_declaration: [0; 8],
            source_format_word: kind
                | (components << 4)
                | (u32::from(stride) << 8)
                | (frequency << 16),
            frequency: frequency as u16,
            modulo: frequency_mask & (1 << slot) != 0,
            location: source >> 31,
            offset,
        });
    }
    if instanced {
        let base = word(transform, 0x60);
        let overrides = if packed {
            [
                (11, 2, 3, 0),
                (12, 2, 3, u32::from(transform[0x64])),
                (13, 2, 3, u32::from(transform[0x65])),
            ]
        } else {
            [(11, 3, 3, 0), (12, 4, 4, 14), (13, 3, 4, 6)]
        };
        let stride = if packed {
            u32::from(half(transform, 0x66))
        } else {
            18
        };
        if stride > 255 || base_vertices > 65535 {
            return Err(E::Range);
        }
        for (slot, kind, components, delta) in overrides {
            let source = base.wrapping_add(delta) | if packed { 0 } else { 0x80000000 };
            plans[slot as usize] = Some(AuxiliaryBinding {
                binding: VertexBinding {
                    declaration: 16 + slot as usize,
                    semantic: 0,
                    slot,
                    format: kind,
                    components,
                    stride: stride as u8,
                    byte_offset: 0,
                    encoded_bytes: 0,
                },
                source_declaration: [0; 8],
                source_format_word: u32::from(kind)
                    | (u32::from(components) << 4)
                    | (stride << 8)
                    | (base_vertices << 16),
                frequency: base_vertices as u16,
                modulo: frequency_mask & (1 << slot) != 0,
                location: source >> 31,
                offset: source & 0x7fffffff,
            });
        }
    }
    let mut attributes = OwnedAuxiliaryAttributes::default();
    for plan in plans.into_iter().flatten() {
        let bytes =
            geometry_bindings::auxiliary_source_bytes(&plan, vertex_count).map_err(E::Decode)?;
        let decoded = vertex_count.checked_mul(16).ok_or(E::Range)?;
        charge = charge
            .checked_add(bytes)
            .and_then(|n| n.checked_add(decoded))
            .filter(|&n| n <= budget)
            .ok_or(E::Budget)?;
        if u64::from(plan.offset) + bytes as u64 > 0x80000000 {
            return Err(E::Range);
        }
        let raw = read(LocatedTextureRequest {
            location: if plan.location == 0 {
                TextureLocation::Local
            } else {
                TextureLocation::Main
            },
            offset: plan.offset,
            bytes,
        })
        .map_err(E::Read)?;
        let words = crate::decoded_geometry_cache::auxiliary(&plan, &raw, vertex_count, decoded)
            .map(std::sync::Arc::unwrap_or_clone)
            .map_err(E::Decode)?;
        attributes.attributes.push(DecodedAuxiliaryAttribute {
            source: plan,
            words,
        });
    }
    Ok(OwnedDirectDraw {
        layout_address: word(mesh, 0x3c),
        instanced,
        vertex_count,
        source_index_count: count,
        primitive: word(mesh, 0x44),
        indices,
        attributes,
    })
}
