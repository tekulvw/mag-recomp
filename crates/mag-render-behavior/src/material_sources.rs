//! Main item mesh, material, technique and microcode sources (`material-sources.md`).
//!
//! [`PassSources::read`] checks the Main pass object once. [`item_sources`] then finds one item's
//! mesh record, primary material, technique chunk and fragment microcode, and chooses its geometry
//! source. An item error never stops other items.
//!
//! The technique chunk walk here follows the links to both program objects and checks, as one
//! pairwise overlap check, all ten regions this specification names, including the vertex
//! object's constant registers, values and instructions. Decoding both objects is the published
//! `material_programs` contract, which the caller runs on the same chunk bytes.

use crate::guest::{GuestMemory, SourceError, be_u16, be_u32};

/// Size of a mesh record.
pub const MESH_BYTES: usize = 192;

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum MaterialSourceError {
    /// `PASS` zero or misaligned, or not its own binding payload.
    InvalidPass,
    /// `PASS+0x190` is nonzero; override materials are not specified.
    UnqualifiedOverride(u32),
    NullMesh,
    NullResource,
    MaterialBounds,
    /// A technique chunk link or region is invalid.
    Program(&'static str),
    /// A fragment storage tag other than 2.
    UnqualifiedFragmentReference(u32),
    ParameterBounds,
    Read(SourceError),
}

fn read<M: GuestMemory + ?Sized>(
    memory: &M,
    address: u32,
    length: u32,
) -> Result<Vec<u8>, MaterialSourceError> {
    let short = |reason: String| {
        MaterialSourceError::Read(SourceError {
            address,
            length,
            reason,
        })
    };
    if address as u64 + length as u64 > 1 << 32 {
        return Err(short("range passes 2^32".into()));
    }
    let bytes = memory
        .read(address, length)
        .map_err(MaterialSourceError::Read)?;
    if bytes.len() != length as usize {
        return Err(short(format!("short read of {} bytes", bytes.len())));
    }
    Ok(bytes)
}

/// The Main pass fields this specification reads.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PassSources {
    pub address: u32,
    /// `i32 +0xDC`.
    pub render_mode: i32,
    /// `u32 +0x190`; must be 0 for Main items.
    pub override_mode: u32,
    /// `u32 +0x194`.
    pub override_mask: u32,
    /// `u32 +0x284`: the asset-bank base, which is also the geometry resource base.
    pub asset_bank: u32,
}

impl PassSources {
    /// Checks the pass and reads its fields.
    pub fn read<M: GuestMemory + ?Sized>(
        memory: &M,
        pass: u32,
    ) -> Result<Self, MaterialSourceError> {
        if pass == 0 || !pass.is_multiple_of(4) {
            return Err(MaterialSourceError::InvalidPass);
        }
        let word = |offset: u32| -> Result<u32, MaterialSourceError> {
            Ok(be_u32(&read(memory, pass.wrapping_add(offset), 4)?, 0))
        };
        if word(0x1fc)? != pass {
            return Err(MaterialSourceError::InvalidPass);
        }
        Ok(Self {
            address: pass,
            render_mode: word(0xdc)? as i32,
            override_mode: word(0x190)?,
            override_mask: word(0x194)?,
            asset_bank: word(0x284)?,
        })
    }

    /// The geometry resource base `G`.
    pub fn geometry_base(&self) -> u32 {
        self.asset_bank
    }
}

/// Where an item's geometry comes from.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum GeometrySource {
    GroupZero { first: u32 },
    Static { first: u32 },
    Skinned { first: u32, palette: u32 },
    Direct { instanced: bool },
}

/// One item's sources.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ItemSources {
    pub mesh_address: u32,
    pub mesh: [u8; MESH_BYTES],
    /// `u16 mesh+0xBC`.
    pub mesh_group: u16,
    pub material_address: u32,
    pub material: Vec<u8>,
    pub selector: u32,
    /// The technique table index `t`, also the parameter-membership bit index.
    pub table_index: u8,
    pub chunk_address: u32,
    /// The chunk's transfer bytes (declared length rounded up to 16).
    pub chunk: Vec<u8>,
    pub microcode_address: u32,
    /// The first `length` bytes of the microcode transfer.
    pub microcode: Vec<u8>,
    /// `u32 material+0x58`.
    pub flags: u32,
    /// `u8 material+0x5C`.
    pub alpha: u8,
    pub geometry: GeometrySource,
    /// `u8 mesh+0xA2`, metadata only.
    pub recorded_partitions: u8,
}

/// The mesh record address: `(u32 MBI+0x0C) · 16`, modulo `2^32`.
pub fn mesh_address(mbi: &[u8; 32]) -> u32 {
    be_u32(mbi, 0x0c).wrapping_mul(16)
}

/// The primary material address and length from the item record.
pub fn primary_material(mbi: &[u8; 32]) -> (u32, u32) {
    let address = u32::from_be_bytes([mbi[6], mbi[7], mbi[8], 0]);
    let length = (be_u32(mbi, 0x1c) & 0x1ff) * 16;
    (address, length)
}

/// The initial technique selector before the override mask.
pub fn initial_selector(selector_byte: u8, mesh_group: u16, render_mode: i32) -> u32 {
    if mesh_group == 15 {
        (selector_byte & 7) as u32
    } else if render_mode <= 15 {
        (selector_byte >> 3) as u32
    } else {
        0
    }
}

fn field(bytes: &[u8], at: u64, length: u64) -> Result<&[u8], MaterialSourceError> {
    let end = at
        .checked_add(length)
        .ok_or(MaterialSourceError::MaterialBounds)?;
    bytes
        .get(at as usize..end as usize)
        .ok_or(MaterialSourceError::MaterialBounds)
}

/// Fragment program object fields reached through the chunk.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct FragmentObject {
    /// Byte offset of the object in the chunk.
    pub offset: usize,
    /// `u32 FP+0`: the microcode length.
    pub length: u32,
    /// `u32 FP+12`: the storage word.
    pub storage: u32,
}

/// Walks the technique chunk to the fragment program object.
pub fn walk_chunk(chunk: &[u8]) -> Result<FragmentObject, MaterialSourceError> {
    let link = |holder: usize, at: usize, size: usize| -> Result<usize, MaterialSourceError> {
        let word = chunk
            .get(holder + at..holder + at + 4)
            .ok_or(MaterialSourceError::Program("link outside the chunk"))?;
        let relative = u32::from_be_bytes(word.try_into().expect("four bytes")) as usize;
        if relative == 0 {
            return Err(MaterialSourceError::Program("zero link"));
        }
        if !relative.is_multiple_of(4) {
            return Err(MaterialSourceError::Program("misaligned link"));
        }
        let target = holder + relative;
        if target + size > chunk.len() {
            return Err(MaterialSourceError::Program("record outside the chunk"));
        }
        Ok(target)
    };
    let a = link(0, 0, 16)?;
    let b = link(a, 12, 20)?;
    let vd = link(b, 12, 20)?;
    let vp = link(vd, 16, 24)?;
    let fd = link(b, 16, 20)?;
    let fp = link(fd, 16, 24)?;
    // The vertex object's own regions (the object layout of `material-producers.md`):
    // `k` constant registers of 4 bytes after the header, their 16-byte values from the next
    // 16-byte boundary, and `n` 16-byte instructions at byte offset `o`. All offsets are from
    // the object's start.
    let half = |at: usize| u16::from_be_bytes([chunk[at], chunk[at + 1]]) as usize;
    let (n, o, k) = (half(vp), half(vp + 18), half(vp + 22));
    let registers_end = 24 + 4 * k;
    let values_start = registers_end.next_multiple_of(16);
    let mut regions = vec![
        (0, 4),
        (a, 16),
        (b, 20),
        (vd, 20),
        (fd, 20),
        (vp, 24),
        (fp, 24),
        (vp + 24, 4 * k),
        (vp + values_start, 16 * k),
        (vp + o, 16 * n),
    ];
    for &(start, size) in &regions[7..] {
        if size != 0 && start + size > chunk.len() {
            return Err(MaterialSourceError::Program(
                "vertex object outside the chunk",
            ));
        }
    }
    // One check over all ten regions together; empty regions are ignored.
    regions.retain(|&(_, size)| size != 0);
    for (i, &(start, size)) in regions.iter().enumerate() {
        for &(other, other_size) in &regions[i + 1..] {
            if start < other + other_size && other < start + size {
                return Err(MaterialSourceError::Program("overlapping records"));
            }
        }
    }
    let length = be_u32(chunk, fp);
    if length == 0 {
        return Err(MaterialSourceError::Program("empty fragment microcode"));
    }
    Ok(FragmentObject {
        offset: fp,
        length,
        storage: be_u32(chunk, fp + 12),
    })
}

/// Finds one item's sources. `transform_byte_6f` is the item's transform record byte `+0x6F`.
pub fn item_sources<M: GuestMemory + ?Sized>(
    memory: &M,
    pass: &PassSources,
    mbi: &[u8; 32],
    transform_byte_6f: u8,
) -> Result<ItemSources, MaterialSourceError> {
    if pass.override_mode != 0 {
        return Err(MaterialSourceError::UnqualifiedOverride(pass.override_mode));
    }

    // Mesh.
    let mesh_address = mesh_address(mbi);
    if mesh_address == 0 {
        return Err(MaterialSourceError::NullMesh);
    }
    let mesh: [u8; MESH_BYTES] = read(memory, mesh_address, MESH_BYTES as u32)?
        .try_into()
        .expect("exact read");
    let mesh_group = be_u16(&mesh, 0xbc);

    // Primary material.
    let (material_address, length) = primary_material(mbi);
    if material_address == 0 || length == 0 {
        return Err(MaterialSourceError::NullResource);
    }
    let material = read(memory, material_address, length)?;
    let half =
        |at| -> Result<u64, MaterialSourceError> { Ok(be_u16(field(&material, at, 2)?, 0) as u64) };
    let word = |at| -> Result<u32, MaterialSourceError> { Ok(be_u32(field(&material, at, 4)?, 0)) };

    // Technique selection.
    let selector = initial_selector(mbi[4], mesh_group, pass.render_mode) & !pass.override_mask;
    let map = field(&material, half(0x48)? + 32 * selector as u64, 32)?;
    let table_index = map[1];
    let entry = field(&material, half(0x4a)? + 8 * table_index as u64, 8)?;
    let (relative, declared) = (be_u32(entry, 0), be_u32(entry, 4));
    let transfer = declared
        .checked_add(15)
        .ok_or(MaterialSourceError::MaterialBounds)?
        & !15;
    let base = word(0x38)?;
    if base == 0 {
        return Err(MaterialSourceError::NullResource);
    }
    let chunk_address = base.wrapping_add(relative);
    let chunk = read(memory, chunk_address, transfer)?;

    // Fragment microcode.
    let fragment = walk_chunk(&chunk)?;
    let tag = fragment.storage & 3;
    if tag != 2 {
        return Err(MaterialSourceError::UnqualifiedFragmentReference(tag));
    }
    let microcode_address = pass
        .asset_bank
        .checked_add(fragment.storage & !3)
        .ok_or(MaterialSourceError::ParameterBounds)?;
    let padded = fragment
        .length
        .checked_add(15)
        .ok_or(MaterialSourceError::ParameterBounds)?
        & !15;
    let mut microcode = read(memory, microcode_address, padded)?;
    microcode.truncate(fragment.length as usize);

    // Primary flags and alpha.
    let flags = word(0x58)?;
    let alpha = field(&material, 0x5c, 1)?[0];

    // Geometry source.
    let first = (be_u32(&mesh, 0x50) & 0x7fff_ffff).wrapping_add(pass.geometry_base());
    let geometry = if mesh_group == 0 {
        GeometrySource::GroupZero { first }
    } else if be_u32(&mesh, 0x9c) & 0x1000 != 0 {
        GeometrySource::Static { first }
    } else if transform_byte_6f != 1 && flags & 0x20 != 0 {
        let palette = if mbi[0x1b] & 1 == 0 {
            be_u32(mbi, 0x14) & !15
        } else {
            0
        };
        GeometrySource::Skinned { first, palette }
    } else {
        GeometrySource::Direct {
            instanced: transform_byte_6f == 1,
        }
    };

    Ok(ItemSources {
        mesh_address,
        mesh,
        mesh_group,
        material_address,
        material,
        selector,
        table_index,
        chunk_address,
        chunk,
        microcode_address,
        microcode,
        flags,
        alpha,
        geometry,
        recorded_partitions: mesh[0xa2],
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::guest::SparseMemory;

    const PASS: u32 = 0x0100_0000;

    fn pass(memory: &mut SparseMemory, render_mode: i32, mask: u32, bank: u32) {
        memory.put(PASS, &[0; 0x290]);
        memory.put_u32(PASS + 0xdc, render_mode as u32);
        memory.put_u32(PASS + 0x194, mask);
        memory.put_u32(PASS + 0x1fc, PASS);
        memory.put_u32(PASS + 0x284, bank);
    }

    /// A chunk with every record placed apart; the fragment object at `+0xC0`.
    fn chunk(length: u32, storage: u32) -> Vec<u8> {
        let mut c = vec![0u8; 0x130];
        let mut put = |at: usize, w: u32| c[at..at + 4].copy_from_slice(&w.to_be_bytes());
        put(0, 0x10);
        put(0x10 + 12, 0x10);
        put(0x20 + 12, 0x20);
        put(0x20 + 16, 0x40);
        put(0x40 + 16, 0x40);
        put(0x60 + 16, 0x60);
        put(0xc0, length);
        put(0xc0 + 12, storage);
        c
    }

    struct Item {
        memory: SparseMemory,
        mbi: [u8; 32],
    }

    /// Vector 4's material: selector 2, map base 0x100, table base 0x200, t = 5.
    fn item(group: u16) -> Item {
        let mut memory = SparseMemory::new();
        pass(&mut memory, 3, 0, 0x0900_0000);
        let mut mesh = [0u8; MESH_BYTES];
        mesh[0xbc..0xbe].copy_from_slice(&group.to_be_bytes());
        mesh[0x50..0x54].copy_from_slice(&0x8000_0400u32.to_be_bytes());
        memory.put(0x0012_3450, &mesh);
        let material = 0x1234_5000;
        let mut m = vec![0u8; 0x300];
        m[0x38..0x3c].copy_from_slice(&0x0800_0000u32.to_be_bytes());
        m[0x48..0x4a].copy_from_slice(&0x100u16.to_be_bytes());
        m[0x4a..0x4c].copy_from_slice(&0x200u16.to_be_bytes());
        m[0x58..0x5d].copy_from_slice(&[0, 0, 8, 1, 0x40]);
        m[0x140 + 1] = 5;
        m[0x228..0x230].copy_from_slice(&[0, 0, 0x40, 0, 0, 0, 1, 0x23]);
        memory.put(material, &m);
        memory.put(0x0800_4000, &chunk(0x44, 0x1002));
        memory.put(0x0900_1000, &[0xab; 80]);
        let mut mbi = [0u8; 32];
        mbi[0] = (group as u8) << 4;
        mbi[4] = 2 << 3;
        mbi[6..9].copy_from_slice(&[0x12, 0x34, 0x50]);
        mbi[0x0c..0x10].copy_from_slice(&0x0001_2345u32.to_be_bytes());
        mbi[0x1c..0x20].copy_from_slice(&0x30u32.to_be_bytes());
        Item { memory, mbi }
    }

    fn sources(item: &Item, byte_6f: u8) -> Result<ItemSources, MaterialSourceError> {
        let pass = PassSources::read(&item.memory, PASS)?;
        item_sources(&item.memory, &pass, &item.mbi, byte_6f)
    }

    #[test]
    fn v1_mesh() {
        let mut mbi = [0u8; 32];
        mbi[0x0c..0x10].copy_from_slice(&0x0001_2345u32.to_be_bytes());
        assert_eq!(mesh_address(&mbi), 0x0012_3450);
        mbi[0x0c..0x10].copy_from_slice(&0x1000_0001u32.to_be_bytes());
        assert_eq!(mesh_address(&mbi), 0x10);
        let mut i = item(5);
        i.mbi[0x0c..0x10].fill(0);
        assert_eq!(sources(&i, 0), Err(MaterialSourceError::NullMesh));
    }

    #[test]
    fn v2_material() {
        let mut mbi = [0u8; 32];
        mbi[6..9].copy_from_slice(&[0x12, 0x34, 0x50]);
        mbi[0x1c..0x20].copy_from_slice(&6u32.to_be_bytes());
        assert_eq!(primary_material(&mbi), (0x1234_5000, 96));
        mbi[0x1c..0x20].copy_from_slice(&0x8006u32.to_be_bytes());
        assert_eq!(primary_material(&mbi), (0x1234_5000, 96));
        for group in [0, 5, 11] {
            let mut i = item(group);
            i.mbi[0x1c..0x20].copy_from_slice(&0x200u32.to_be_bytes());
            assert_eq!(sources(&i, 0), Err(MaterialSourceError::NullResource));
        }
    }

    #[test]
    fn v3_selector() {
        assert_eq!(initial_selector(0x1a, 15, 3), 2);
        assert_eq!(initial_selector(0x1a, 5, 3), 3);
        assert_eq!(initial_selector(0x1a, 5, 16), 0);
        assert_eq!(initial_selector(0x1a, 5, -1), 3);
        let mut i = item(5);
        i.mbi[4] = 0x1a;
        pass(&mut i.memory, 3, 1, 0x0900_0000);
        // 3 AND NOT 1 = 2, which is vector 4's map record.
        assert_eq!(sources(&i, 0).unwrap().selector, 2);
    }

    #[test]
    fn v4_v6_chunk_and_microcode() {
        let mut i = item(5);
        i.mbi[4] = 2 << 3;
        let s = sources(&i, 0).unwrap();
        assert_eq!((s.selector, s.table_index), (2, 5));
        assert_eq!((s.chunk_address, s.chunk.len()), (0x0800_4000, 0x130));
        assert_eq!(s.microcode_address, 0x0900_1000);
        assert_eq!(s.microcode, vec![0xab; 0x44]);

        // Declared length 0xFFFFFFF8 overflows the rounding.
        let mut bad = item(5);
        bad.mbi[4] = 2 << 3;
        bad.memory
            .put(0x1234_5228, &[0, 0, 0x40, 0, 0xff, 0xff, 0xff, 0xf8]);
        assert_eq!(sources(&bad, 0), Err(MaterialSourceError::MaterialBounds));
        // Technique resource base 0.
        let mut null = item(5);
        null.mbi[4] = 2 << 3;
        null.memory.put_u32(0x1234_5038, 0);
        assert_eq!(sources(&null, 0), Err(MaterialSourceError::NullResource));
        // Tag 1 and an overflowing asset-bank address.
        let mut tag = item(5);
        tag.mbi[4] = 2 << 3;
        tag.memory.put(0x0800_4000, &chunk(0x44, 0x1001));
        assert_eq!(
            sources(&tag, 0),
            Err(MaterialSourceError::UnqualifiedFragmentReference(1))
        );
        let mut overflow = item(5);
        overflow.mbi[4] = 2 << 3;
        pass(&mut overflow.memory, 3, 0, 0xffff_f000);
        assert_eq!(
            sources(&overflow, 0),
            Err(MaterialSourceError::ParameterBounds)
        );
    }

    #[test]
    fn v5_material_bounds() {
        let mut i = item(5);
        i.mbi[4] = 2 << 3;
        i.mbi[0x1c..0x20].copy_from_slice(&0x14u32.to_be_bytes());
        assert_eq!(sources(&i, 0), Err(MaterialSourceError::MaterialBounds));
    }

    #[test]
    fn v7_primary_state() {
        let mut i = item(5);
        i.mbi[4] = 2 << 3;
        let s = sources(&i, 0).unwrap();
        assert_eq!((s.flags, s.alpha), (0x801, 0x40));
    }

    #[test]
    fn v8_geometry_dispatch() {
        let with = |group: u16, flags_9c: u32, flags: u32, byte_6f: u8| {
            let mut i = item(group);
            i.mbi[4] = if group == 15 { 2 } else { 2 << 3 };
            pass(&mut i.memory, 3, 0, 0x1000_0000);
            // The microcode moves with the asset bank.
            i.memory.put(0x1000_1000, &[0xab; 80]);
            i.memory.put_u32(0x0012_3450 + 0x9c, flags_9c);
            i.memory.put_u32(0x1234_5058, flags);
            i.mbi[0x14..0x18].copy_from_slice(&0x00ab_cde7u32.to_be_bytes());
            sources(&i, byte_6f).unwrap().geometry
        };
        assert_eq!(
            with(0, 0x1000, 0, 0),
            GeometrySource::GroupZero { first: 0x1000_0400 }
        );
        assert_eq!(
            with(5, 0x1000, 0, 0),
            GeometrySource::Static { first: 0x1000_0400 }
        );
        assert_eq!(
            with(11, 0, 0x20, 0),
            GeometrySource::Skinned {
                first: 0x1000_0400,
                palette: 0x00ab_cde0
            }
        );
        assert_eq!(
            with(11, 0, 0x20, 1),
            GeometrySource::Direct { instanced: true }
        );
        assert_eq!(
            with(5, 0, 0, 0),
            GeometrySource::Direct { instanced: false }
        );
        // Palette-in-place: palette 0.
        let mut i = item(11);
        i.mbi[4] = 2 << 3;
        i.mbi[0x1b] = 1;
        i.memory.put_u32(0x1234_5058, 0x20);
        assert!(matches!(
            sources(&i, 0).unwrap().geometry,
            GeometrySource::Skinned { palette: 0, .. }
        ));
    }

    #[test]
    fn v9_override_and_pass_checks() {
        let mut i = item(5);
        i.memory.put_u32(PASS + 0x190, 2);
        assert_eq!(
            sources(&i, 0),
            Err(MaterialSourceError::UnqualifiedOverride(2))
        );
        let memory = SparseMemory::new();
        assert_eq!(
            PassSources::read(&memory, 0),
            Err(MaterialSourceError::InvalidPass)
        );
        assert_eq!(
            PassSources::read(&memory, PASS + 2),
            Err(MaterialSourceError::InvalidPass)
        );
        let mut other = item(5);
        other.memory.put_u32(PASS + 0x1fc, PASS + 4);
        assert_eq!(
            PassSources::read(&other.memory, PASS),
            Err(MaterialSourceError::InvalidPass)
        );
    }

    #[test]
    fn chunk_links_are_checked() {
        let good = chunk(0x44, 2);
        assert_eq!(walk_chunk(&good).unwrap().offset, 0xc0);
        let mut zero = good.clone();
        zero[0..4].fill(0);
        assert_eq!(
            walk_chunk(&zero),
            Err(MaterialSourceError::Program("zero link"))
        );
        let mut odd = good.clone();
        odd[0..4].copy_from_slice(&0x12u32.to_be_bytes());
        assert_eq!(
            walk_chunk(&odd),
            Err(MaterialSourceError::Program("misaligned link"))
        );
        let mut outside = good.clone();
        outside[0x60 + 16..0x60 + 20].copy_from_slice(&0x1000u32.to_be_bytes());
        assert_eq!(
            walk_chunk(&outside),
            Err(MaterialSourceError::Program("record outside the chunk"))
        );
        // The fragment descriptor pointing back over the vertex descriptor.
        let mut overlap = good.clone();
        overlap[0x20 + 16..0x20 + 20].copy_from_slice(&0x24u32.to_be_bytes());
        overlap[0x44 + 16..0x44 + 20].copy_from_slice(&0x7cu32.to_be_bytes());
        assert_eq!(
            walk_chunk(&overlap),
            Err(MaterialSourceError::Program("overlapping records"))
        );
        // Vector 10 (cross-overlap): a vertex instruction region overlapping a walked record,
        // with every record and header otherwise disjoint. Links point forward, so the record
        // after the vertex object is the fragment descriptor, moved here to 0xA0..0xB4.
        let vp = 0x80;
        let mut cross = good.clone();
        cross[0x20 + 16..0x20 + 20].copy_from_slice(&0x80u32.to_be_bytes());
        cross[0xa0 + 16..0xa0 + 20].copy_from_slice(&0x20u32.to_be_bytes());
        assert!(walk_chunk(&cross).is_ok());
        // One instruction at o = 0x18: 0x98..0xA8 overlaps the descriptor.
        cross[vp..vp + 2].copy_from_slice(&1u16.to_be_bytes());
        cross[vp + 18..vp + 20].copy_from_slice(&0x18u16.to_be_bytes());
        assert_eq!(
            walk_chunk(&cross),
            Err(MaterialSourceError::Program("overlapping records"))
        );
        // Instructions over the fragment header (o = 0x40: 0xC0..0xD0).
        let mut cross = good.clone();
        cross[vp..vp + 2].copy_from_slice(&1u16.to_be_bytes());
        cross[vp + 18..vp + 20].copy_from_slice(&0x40u16.to_be_bytes());
        assert_eq!(
            walk_chunk(&cross),
            Err(MaterialSourceError::Program("overlapping records"))
        );
        // The same instructions at o = 0x18 (0x98..0xA8) overlap nothing.
        cross[vp + 18..vp + 20].copy_from_slice(&0x18u16.to_be_bytes());
        assert!(walk_chunk(&cross).is_ok());
        // Constant registers and values: k = 2 gives registers 0x98..0xA0 and values
        // 0xA0..0xC0, which run into the fragment header only with k = 3 (values to 0xD0).
        let mut constants = good.clone();
        constants[vp + 22..vp + 24].copy_from_slice(&2u16.to_be_bytes());
        assert!(walk_chunk(&constants).is_ok());
        constants[vp + 22..vp + 24].copy_from_slice(&3u16.to_be_bytes());
        assert_eq!(
            walk_chunk(&constants),
            Err(MaterialSourceError::Program("overlapping records"))
        );
        // A vertex object region past the chunk.
        let mut long = good.clone();
        long[vp..vp + 2].copy_from_slice(&64u16.to_be_bytes());
        long[vp + 18..vp + 20].copy_from_slice(&0x18u16.to_be_bytes());
        assert_eq!(
            walk_chunk(&long),
            Err(MaterialSourceError::Program(
                "vertex object outside the chunk"
            ))
        );
        let empty = chunk(0, 2);
        assert_eq!(
            walk_chunk(&empty),
            Err(MaterialSourceError::Program("empty fragment microcode"))
        );
    }
}
